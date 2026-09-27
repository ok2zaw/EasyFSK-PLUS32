#include "FskTimer.h"
#include "Pins.h"

namespace FskTimer {

namespace {

// Bit-frame position constants, matching the AVR original.
constexpr int START_BIT_POS = -1;
constexpr int STOP_BIT_POS = 5;

// Ring buffer depth. 2-3 characters of lookahead is enormous slack at RTTY
// speeds (93-154ms/char vs microseconds of ISR work) -- 4 (power of two,
// cheap wrap-around) leaves headroom without wasting RAM.
constexpr size_t RING_CAPACITY = 4;

hw_timer_t *s_timer = nullptr;

// --- shared state: TxManager (producer) writes, ISR (consumer) reads,
// except where noted. All single-writer/single-reader per direction, so
// plain `volatile` is sufficient without needing a mutex -- matches the
// design doc's "small lock-free single-producer/single-consumer ring
// buffer" framing. ---
volatile bool s_active = false;    // TxManager writes; true = ISR should run
volatile bool s_rtsKeyed = false;  // TxManager writes; true = ISR leaves FSK_PIN alone
volatile uint8_t s_markLevel = HIGH;
volatile uint8_t s_spaceLevel = LOW;

Baudot::QueuedSymbol s_ring[RING_CAPACITY];
volatile size_t s_ringHead = 0; // ISR reads from here
volatile size_t s_ringTail = 0; // TxManager writes here

volatile bool s_charStartedFlag = false; // ISR writes; TxManager reads+clears
volatile uint8_t s_charStartedAscii = 0;
volatile bool s_endOfDataFlag = false; // ISR writes; TxManager reads+clears
volatile uint32_t s_underrunCount = 0;

// --- ISR-local state: touched ONLY inside the ISR, never from TxManager,
// so these are plain (non-volatile) statics. ---
int s_bitPos = START_BIT_POS;
int s_stopBitCounter = 0;
bool s_midBit = false;
uint8_t s_sendingChar = Baudot::LTRS_SHIFT;
bool s_halted = false; // true once TX_END_FLAG has been seen; waits for setActive(false)

inline bool ringEmpty() { return s_ringHead == s_ringTail; }
inline bool ringFull() { return ((s_ringTail + 1) % RING_CAPACITY) == s_ringHead; }

bool ringPop(Baudot::QueuedSymbol &out) {
  if (ringEmpty()) return false;
  out = s_ring[s_ringHead];
  s_ringHead = (s_ringHead + 1) % RING_CAPACITY;
  return true;
}

void ARDUINO_ISR_ATTR onHalfBit() {
  if (!s_active || s_rtsKeyed || s_halted) {
    return; // mirrors AVR processHalfBit()'s `if (!ptt) return;` / rtsKeyed early-return
  }

  if (s_midBit) {
    s_midBit = false;
    return;
  }

  if (s_bitPos == START_BIT_POS) {
    Baudot::QueuedSymbol sym;
    if (!ringPop(sym)) {
      // Underrun: ring was empty when we needed the next symbol. Fall
      // back to an idle LTRS diddle rather than glitching -- same spirit
      // as the AVR original's idle-diddle default, just triggered by a
      // production shortfall instead of an intentionally-idle buffer.
      sym.baudotCode = Baudot::LTRS_SHIFT;
      sym.asciiByte = 0;
      s_underrunCount++;
    }

    if (sym.baudotCode == Baudot::TX_END_FLAG) {
      s_endOfDataFlag = true;
      s_halted = true; // park here -- FSK_PIN stays at its current (mark) level
                        // until TxManager notices and drives the tail sequence
      return;
    }

    s_sendingChar = sym.baudotCode;
    if (sym.asciiByte != 0) {
      s_charStartedAscii = sym.asciiByte;
      s_charStartedFlag = true;
    }

    digitalWrite(FSK_PIN, s_spaceLevel); // start bit is always space
    s_bitPos++;
    s_midBit = true;

  } else if (s_bitPos == STOP_BIT_POS) {
    if (s_stopBitCounter == 0) {
      digitalWrite(FSK_PIN, s_markLevel);
      s_stopBitCounter = Baudot::STOP_BIT_HALF_PERIODS;
    } else {
      s_stopBitCounter--;
      if (s_stopBitCounter == 0) {
        s_bitPos = START_BIT_POS;
      }
    }
  } else {
    bool bit = s_sendingChar & (0x01 << s_bitPos);
    digitalWrite(FSK_PIN, bit ? s_markLevel : s_spaceLevel);
    s_bitPos++;
    s_midBit = true;
  }
}

uint64_t halfBitTicksFor(float baudRate) {
  // Timer runs at 1MHz (1 tick = 1us), so ticks-per-half-bit = 1e6 / (2*baud).
  return static_cast<uint64_t>((1000000.0 / (2.0 * baudRate)) + 0.5);
}

} // namespace

void begin(float baudRate, bool markHigh) {
  pinMode(FSK_PIN, OUTPUT);
  s_markLevel = markHigh ? HIGH : LOW;
  s_spaceLevel = markHigh ? LOW : HIGH;
  digitalWrite(FSK_PIN, s_markLevel);

  // Arduino-ESP32 2.x timer API. The ESP32 APB timer clock is 80MHz;
  // divider 80 gives a 1MHz counter (one tick per microsecond).
  s_timer = timerBegin(0, 80, true);
  timerAttachInterrupt(s_timer, &onHalfBit, true);
  timerAlarmWrite(s_timer, halfBitTicksFor(baudRate), true);
  timerAlarmEnable(s_timer);
}

void reconfigure(float baudRate, bool markHigh) {
  s_markLevel = markHigh ? HIGH : LOW;
  s_spaceLevel = markHigh ? LOW : HIGH;
  timerAlarmWrite(s_timer, halfBitTicksFor(baudRate), true);
}

void resetForNewTx() {
  s_bitPos = START_BIT_POS;
  s_stopBitCounter = 0;
  s_midBit = false;
  s_sendingChar = Baudot::LTRS_SHIFT;
  s_halted = false;
  s_ringHead = 0;
  s_ringTail = 0;
  s_charStartedFlag = false;
  s_endOfDataFlag = false;
}

void setActive(bool active, bool rtsKeyed) {
  s_rtsKeyed = rtsKeyed;
  s_active = active;
}

void holdMark() { digitalWrite(FSK_PIN, s_markLevel); }
void holdSpace() { digitalWrite(FSK_PIN, s_spaceLevel); }
void holdLow() { digitalWrite(FSK_PIN, LOW); }

bool ringHasFreeSlot() { return !ringFull(); }

bool pushSymbol(const Baudot::QueuedSymbol &sym) {
  if (ringFull()) return false;
  s_ring[s_ringTail] = sym;
  s_ringTail = (s_ringTail + 1) % RING_CAPACITY;
  return true;
}

bool takeCharStarted(uint8_t &asciiByte) {
  if (!s_charStartedFlag) return false;
  asciiByte = s_charStartedAscii;
  s_charStartedFlag = false;
  return true;
}

bool takeEndOfData() {
  if (!s_endOfDataFlag) return false;
  s_endOfDataFlag = false;
  return true;
}

uint32_t underrunCount() { return s_underrunCount; }

} // namespace FskTimer
