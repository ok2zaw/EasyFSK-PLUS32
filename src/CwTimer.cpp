#include "CwTimer.h"
#include "Pins.h"

namespace CwTimer {

namespace {

constexpr size_t RING_CAPACITY = 4; // same lookahead depth as FskTimer's ring

hw_timer_t *s_timer = nullptr;

// --- shared state: TxManager (producer) writes, ISR (consumer) reads,
// except where noted -- same single-producer/single-consumer discipline as
// FskTimer, plain `volatile` is sufficient. ---
volatile bool s_active = false;

Morse::Run s_ring[RING_CAPACITY];
volatile size_t s_ringHead = 0; // ISR reads from here
volatile size_t s_ringTail = 0; // TxManager writes here

volatile bool s_charStartedFlag = false;
volatile uint8_t s_charStartedAscii = 0;

// --- ISR-local state ---
bool s_haveCurrent = false;   // true while a Run is actively counting down
bool s_currentKeyDown = false;
uint32_t s_remainingMs = 0;

inline bool ringEmpty() { return s_ringHead == s_ringTail; }
inline bool ringFull() { return ((s_ringTail + 1) % RING_CAPACITY) == s_ringHead; }

bool ringPop(Morse::Run &out) {
  if (ringEmpty()) return false;
  out = s_ring[s_ringHead];
  s_ringHead = (s_ringHead + 1) % RING_CAPACITY;
  return true;
}

void ARDUINO_ISR_ATTR onTick() {
  if (!s_active) return;

  if (s_haveCurrent) {
    if (s_remainingMs > 1) {
      s_remainingMs--;
      return;
    }
    s_remainingMs = 0;
    s_haveCurrent = false; // this Run just finished; fall through to start the next one
  }

  Morse::Run run;
  if (!ringPop(run)) {
    // Ring empty: nothing queued right now. Hold the key up and wait --
    // NOT an "underrun" in the RTTY sense (CW naturally has gaps between
    // characters/words while the host is thinking), so no counter/fallback
    // symbol needed, just idle.
    digitalWrite(FSK_PIN, LOW);
    return;
  }

  s_currentKeyDown = run.keyDown;
  s_remainingMs = run.durationMs;
  s_haveCurrent = (s_remainingMs > 0);
  digitalWrite(FSK_PIN, run.keyDown ? HIGH : LOW);

  if (run.asciiByte != 0) {
    s_charStartedAscii = run.asciiByte;
    s_charStartedFlag = true;
  }

  if (!s_haveCurrent) {
    // Zero-length Run (shouldn't normally happen -- CwBuffer guards against
    // it -- but don't wedge the ISR if it does): try the next one right away
    // on the following tick rather than waiting a full extra ms.
  }
}

} // namespace

void begin() {
  pinMode(FSK_PIN, OUTPUT);
  digitalWrite(FSK_PIN, LOW);

  // Arduino-ESP32 2.x timer API. Run the counter at 1MHz and fire every
  // 1000 ticks, which gives the CW engine its 1ms scheduling period.
  s_timer = timerBegin(1, 80, true);
  timerAttachInterrupt(s_timer, &onTick, true);
  timerAlarmWrite(s_timer, 1000, true);
  timerAlarmEnable(s_timer);
}

void setActive(bool active) {
  s_active = active;
}

void resetForNewTx() {
  s_ringHead = 0;
  s_ringTail = 0;
  s_haveCurrent = false;
  s_remainingMs = 0;
  s_charStartedFlag = false;
}

void holdKeyUp() { digitalWrite(FSK_PIN, LOW); }

bool ringHasFreeSlot() { return !ringFull(); }

bool pushRun(const Morse::Run &run) {
  if (ringFull()) return false;
  s_ring[s_ringTail] = run;
  s_ringTail = (s_ringTail + 1) % RING_CAPACITY;
  return true;
}

bool isIdle() {
  return ringEmpty() && !s_haveCurrent;
}

bool takeCharStarted(uint8_t &asciiByte) {
  if (!s_charStartedFlag) return false;
  asciiByte = s_charStartedAscii;
  s_charStartedFlag = false;
  return true;
}

} // namespace CwTimer
