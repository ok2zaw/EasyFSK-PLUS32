#include "TxManager.h"
#include "Baudot.h"
#include "FskTimer.h"
#include "CwTimer.h"
#include "StatusDisplay.h"
#include "Pins.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

namespace TxManager {

// Defined before the anonymous namespace below (rather than after, as its
// sibling functions are) because taskFn() calls it via argument-dependent
// lookup on a Source value -- ADL still requires the declaration to
// precede the call within this translation unit for an ordinary
// (non-template) function, so it can't come later in the file.
const char *sourceLabel(Source src) {
  switch (src) {
    case Source::Uart2Fsk: return "FSK2";
    case Source::Rts: return "RTS";
    case Source::Web: return "WEB";
    case Source::Winkey: return "CWK";
    default: return "FSK1";
  }
}

namespace {

struct TxMsg {
  enum class Type : uint8_t {
    KeyUp, BufferedEnd, Abort, AppendByte, // RTTY/Baudot path
    CwAppendChar, CwMergeMark, CwSideEffect, CwBackspace, CwClearPending, CwTuneKey, // CW/Winkey path
  } type;
  uint8_t byte;
  Source src;
  Morse::SideEffect cwEffect;
};

enum class State { Idle, LeadPa, LeadPtt, Sending, TailPtt, TailPa };

constexpr UBaseType_t COMMAND_QUEUE_DEPTH = 128;

QueueHandle_t s_queue = nullptr;
portMUX_TYPE s_statusMux = portMUX_INITIALIZER_UNLOCKED;

Config s_cfg;
Baudot::SendBuffer s_sendBuffer;
Morse::CwBuffer s_cwBuffer;
State s_state = State::Idle;
uint32_t s_stateEnteredMs = 0;
bool s_rtsSession = false;
bool s_tuneActive = false; // Winkey 0x0B "Key Immediate" -- continuous carrier for antenna tuning
bool s_pttOutputActive = false;
bool s_paOutputActive = false;
Source s_activeSource = Source::SerialLink;
Status s_status;

// Winkey's PTT lead/tail command (0x04) -- a single relay's lead/tail pair,
// unlike RTTY's two-stage PA+PTT sequence. Mapped onto this sequencer
// (design doc: "maps directly onto parameters this sequencer already has")
// by putting all of the lead time into the PTT-lead stage (PA-lead stage
// skipped, i.e. 0) and all of the tail time into the PTT-tail stage, with
// no additional PA-tail delay. Defaults to 0/0 (immediate) until a Winkey
// host actually sends 0x04.
uint16_t s_cwLeadMs = 0;
uint16_t s_cwTailMs = 0;

inline bool isCwSource(Source src) { return src == Source::Winkey; }

inline uint16_t currentPaLeadMs() { return isCwSource(s_activeSource) ? 0 : s_cfg.paLeadMs; }
inline uint16_t currentPttLeadMs() { return isCwSource(s_activeSource) ? s_cwLeadMs : s_cfg.pttLeadMs; }
inline uint16_t currentPaTailMs() { return isCwSource(s_activeSource) ? 0 : s_cfg.paTailMs; }
inline uint16_t currentPttTailMs() { return isCwSource(s_activeSource) ? s_cwTailMs : s_cfg.pttTailMs; }

inline void setPttOutput(bool active) {
  digitalWrite(PTT_PIN, active ? HIGH : LOW);
  s_pttOutputActive = active;
}

inline void setPaOutput(bool active) {
  digitalWrite(PTT_PA_PIN, active ? HIGH : LOW);
  s_paOutputActive = active;
}

void updateStatus() {
  portENTER_CRITICAL(&s_statusMux);
  s_status.txActive = (s_state != State::Idle);
  // Physical output levels. txActive intentionally remains true through
  // both tail states, until PA has been released and cleanup is complete.
  s_status.pttActive = s_pttOutputActive;
  s_status.paActive = s_paOutputActive;
  s_status.pttSource = s_activeSource;
  s_status.bufferPending = isCwSource(s_activeSource)
                                ? static_cast<uint16_t>(s_cwBuffer.pending())
                                : static_cast<uint16_t>(s_sendBuffer.pending());
  s_status.revision++;
  portEXIT_CRITICAL(&s_statusMux);
}

void noteCharStarted(uint8_t asciiByte) {
  portENTER_CRITICAL(&s_statusMux);
  s_status.lastChar = static_cast<char>(asciiByte);
  s_status.charSeq++;
  s_status.revision++;
  portEXIT_CRITICAL(&s_statusMux);
}

void setInhibitedStatus(bool inhibited) {
  portENTER_CRITICAL(&s_statusMux);
  s_status.inhibited = inhibited;
  s_status.revision++;
  portEXIT_CRITICAL(&s_statusMux);
}

// Immediate, no-delay shutdown -- mirrors loop()'s hardware-inhibit force
// stop in the AVR original: bypasses PA/PTT tail delays entirely, since
// this is a safety cutoff, not a graceful end of transmission. Silent on
// the serial port either way, same as the AVR original. Resets BOTH
// engines/buffers unconditionally (harmless on whichever one was idle) --
// simpler and safer than branching on s_activeSource in a code path whose
// whole point is "stop everything right now".
void forceStopImmediate() {
  setPaOutput(false);
  setPttOutput(false);
  digitalWrite(LED_RX_PIN, HIGH);
  FskTimer::setActive(false, false);
  CwTimer::setActive(false);
  if (s_rtsSession) {
    FskTimer::holdLow();
  } else if (isCwSource(s_activeSource)) {
    CwTimer::holdKeyUp();
  } else {
    FskTimer::holdSpace();
  }
  s_sendBuffer.reset();
  s_cwBuffer.reset();
  FskTimer::resetForNewTx();
  CwTimer::resetForNewTx();
  s_state = State::Idle;
  s_rtsSession = false;
  s_tuneActive = false;
  updateStatus();
}

// Begins the graceful nested shutdown sequence. After pttTailMs the main
// PTT output drops first; PTT_PA_PIN remains active for paTailMs and drops
// last. Called
// once the active engine naturally reaches end-of-data, once an
// RTS-sourced session's RTS line deasserts and its (possibly empty)
// buffer has drained, or once TX_ABORT is processed mid-transmission.
void beginTailSequence() {
  s_state = State::TailPtt;
  s_stateEnteredMs = millis();
  updateStatus();
}

void beginKeyUp(Source src, bool inhibitedNow) {
  if (s_state != State::Idle) {
    // Redundant TX_ON/first-char-while-active -- ignored, matches the AVR
    // original's explicit guard (see loop()'s TX_ON handling). Also exactly
    // what lets WinkeyEmulator unconditionally call this before every
    // character without tracking its own session-open state.
    return;
  }
  if (inhibitedNow) {
    StatusDisplay::showStatus("INHIBIT"); // mirrors setPTT()'s own inhibit refusal
    return;
  }
  s_rtsSession = false;
  s_activeSource = src;
  if (!isCwSource(src)) {
    s_sendBuffer.endWhenBufferEmpty = false; // waits for an explicit TX_END, per the RTTY protocol
  }
  // Deliberately NOT resetting s_cwBuffer here: doing so would drop any
  // characters WinkeyEmulator already queued (it calls this unconditionally
  // before every character/side-effect, relying on the "redundant TX_ON"
  // no-op above when a session is already open). s_cwBuffer is only ever
  // reset explicitly -- Abort, the hardware-inhibit force-stop, Winkey's
  // own Clear Buffer (0x0A), or once a session's tail sequence confirms
  // the buffer is truly, finally empty (see the TailPa case below).
  setPaOutput(true);
  s_state = State::LeadPa;
  s_stateEnteredMs = millis();
  updateStatus();
}

void handleMessage(const TxMsg &msg, bool inhibitedNow) {
  switch (msg.type) {
    case TxMsg::Type::KeyUp:
      beginKeyUp(msg.src, inhibitedNow);
      break;

    case TxMsg::Type::BufferedEnd:
      s_sendBuffer.endWhenBufferEmpty = true; // unconditional, matches the AVR original
      break;

    case TxMsg::Type::Abort:
      // AVR TX_ABORT always dumps pending text, even if PTT has not keyed yet
      // (for example bytes received before a leading '[' command).
      s_sendBuffer.reset();
      s_cwBuffer.reset();
      s_tuneActive = false;
      if (s_state != State::Idle) {
        if (s_state == State::Sending) {
          if (isCwSource(s_activeSource)) {
            CwTimer::setActive(false);
          } else if (!s_rtsSession) {
            FskTimer::setActive(false, false);
          }
        }
        // If already in a tail state, do not restart either delay. Abort only
        // guarantees that queued content is gone and the existing shutdown
        // continues toward the safe state.
        if (s_state != State::TailPtt && s_state != State::TailPa) {
          beginTailSequence();
        }
      } else {
        FskTimer::resetForNewTx();
        CwTimer::resetForNewTx();
        updateStatus();
      }
      break;

    case TxMsg::Type::AppendByte:
      if (!inhibitedNow) s_sendBuffer.addByte(msg.byte);
      break;

    // --- CW/Winkey path ---
    case TxMsg::Type::CwAppendChar:
      if (inhibitedNow) break;
      beginKeyUp(Source::Winkey, inhibitedNow); // no-op if already active
      s_cwBuffer.addChar(msg.byte);
      break;

    case TxMsg::Type::CwMergeMark:
      s_cwBuffer.addMergeMark();
      break;

    case TxMsg::Type::CwSideEffect:
      if (inhibitedNow) break;
      // Starts a session for a standalone side effect too (matters for
      // PttOn/PttOff, which are meant to control the relay independent of
      // keying). Minor known imperfection: a lone buffered Wait or Nop with
      // no surrounding text -- an unusual thing for a host to send -- would
      // also briefly engage PTT via the lead sequence before the tail
      // sequence drops it again; not worth the extra state to special-case
      // given how rare that pattern is in practice.
      beginKeyUp(Source::Winkey, inhibitedNow);
      s_cwBuffer.addSideEffect(msg.cwEffect, msg.byte);
      break;

    case TxMsg::Type::CwBackspace:
      s_cwBuffer.backspace();
      break;

    case TxMsg::Type::CwClearPending:
      s_cwBuffer.reset();
      break;

    case TxMsg::Type::CwTuneKey:
      if (msg.byte != 0) {
        if (inhibitedNow) break;
        beginKeyUp(Source::Winkey, inhibitedNow); // no-op if already active
        s_tuneActive = true;
      } else {
        s_tuneActive = false;
        if (s_state == State::Sending && isCwSource(s_activeSource)) {
          CwTimer::setActive(false);
          beginTailSequence();
        }
      }
      break;
  }
}

// Advances the CW/Winkey engine by one TxManager task iteration: keeps
// CwTimer's ring fed while there's Element content ready, and executes a
// buffered side-effect (PTT on/off, buffered speed, wait, nop) once
// CwTimer has FULLY drained everything queued ahead of it -- preserving
// the host's original ordering without needing the ISR/ring layer to know
// about anything but plain key-down/key-up Runs.
void pumpCwEngine() {
  if (s_tuneActive) {
    // Winkey 0x0B "Key Immediate" (antenna-tune carrier): hold the key
    // down continuously until the host sends key-up, bypassing CwBuffer
    // entirely. Implemented as repeated bounded-duration key-down Runs
    // (re-issued whenever CwTimer goes idle) rather than a true indefinite
    // level, so it reuses the same ring/ISR machinery as normal keying
    // instead of needing a separate raw-pin code path.
    if (CwTimer::isIdle() && CwTimer::ringHasFreeSlot()) {
      Morse::Run tuneRun;
      tuneRun.keyDown = true;
      tuneRun.durationMs = 5000;
      tuneRun.asciiByte = 0;
      CwTimer::pushRun(tuneRun);
    }
    return;
  }

  for (;;) {
    Morse::NextKind kind = s_cwBuffer.peekKind();
    if (kind == Morse::NextKind::None) break;

    if (kind == Morse::NextKind::SideEffect) {
      if (!CwTimer::isIdle()) break; // let everything already queued finish first
      uint8_t value = 0;
      Morse::SideEffect effect = s_cwBuffer.takeSideEffect(value);
      switch (effect) {
        case Morse::SideEffect::PttOn: setPttOutput(true); break;
        case Morse::SideEffect::PttOff: setPttOutput(false); break;
        case Morse::SideEffect::BufferedSpeed: s_cwBuffer.setSpeedWpm(value); break;
        case Morse::SideEffect::Wait: {
          // Coarse (0-99s) pause. capped at 60s to keep the ring's
          // uint16_t-millisecond duration field valid -- multi-second
          // Winkey buffered waits are a rare corner; TODO if this proves
          // too coarse for a real host. Pushing once is guaranteed to
          // succeed: CwTimer::isIdle() above means the ring is empty and
          // this task is the ring's only producer.
          uint32_t waitMs = static_cast<uint32_t>(value) * 1000u;
          if (waitMs > 60000u) waitMs = 60000u;
          Morse::Run wait;
          wait.keyDown = false;
          wait.durationMs = static_cast<uint16_t>(waitMs);
          wait.asciiByte = 0;
          CwTimer::pushRun(wait);
          break;
        }
        case Morse::SideEffect::KeyBuffered: {
          // Winkey 0x19 "Key Buffered": timed key-DOWN (0-99s), same
          // capping/ring-safety reasoning as Wait above, just keyDown=true.
          uint32_t onMs = static_cast<uint32_t>(value) * 1000u;
          if (onMs > 60000u) onMs = 60000u;
          Morse::Run keyOn;
          keyOn.keyDown = true;
          keyOn.durationMs = static_cast<uint16_t>(onMs);
          keyOn.asciiByte = 0;
          CwTimer::pushRun(keyOn);
          break;
        }
        case Morse::SideEffect::Nop:
          break;
      }
      continue; // side effect handled -- loop back for whatever's next
    }

    // kind == Element
    if (!CwTimer::ringHasFreeSlot()) break;
    Morse::Run run;
    if (!s_cwBuffer.nextElementRun(run)) break; // shouldn't happen given peekKind() above, but stay safe
    CwTimer::pushRun(run);
  }

  uint8_t startedAscii;
  while (CwTimer::takeCharStarted(startedAscii)) {
    StatusDisplay::appendTxChar(startedAscii);
    noteCharStarted(startedAscii);
  }

  if (s_cwBuffer.peekKind() == Morse::NextKind::None && CwTimer::isIdle()) {
    CwTimer::setActive(false);
    beginTailSequence();
  }
}

void taskFn(void *) {
  bool inhibitShown = false;

  for (;;) {
    TxMsg msg;
    bool haveMsg = xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(2)) == pdTRUE;

    // (0) Hardware inhibit -- edge-triggered, mirrors loop()'s handling.
    bool inhibitedNow = (digitalRead(CPU_INH_PIN) == LOW);
    if (inhibitedNow && !inhibitShown) {
      // A hardware interlock cancels the whole request stream, including
      // commands that have not reached the state machine yet. Otherwise a
      // queued KeyUp could fire immediately after the interlock is released.
      xQueueReset(s_queue);
      forceStopImmediate();
      inhibitShown = true;
      setInhibitedStatus(true);
      StatusDisplay::showStatus("INHIBIT");
    } else if (!inhibitedNow && inhibitShown) {
      inhibitShown = false;
      setInhibitedStatus(false);
      if (s_state == State::Idle) StatusDisplay::showStatus("RX");
    }

    // (1) External hardware PTT-request (RTS-style) input poll.
    bool rtsAsserted = (digitalRead(PTT_USB_RTS_PIN) == LOW);
    if (!inhibitedNow) {
      if (rtsAsserted && s_state == State::Idle) {
        s_rtsSession = true;
        s_activeSource = Source::Rts;
        s_sendBuffer.endWhenBufferEmpty = false;
        setPaOutput(true);
        s_state = State::LeadPa;
        s_stateEnteredMs = millis();
        updateStatus();
      } else if (!rtsAsserted && s_rtsSession && s_state == State::Sending) {
        // Hand off to the normal Baudot end-of-data path -- if any text
        // was queued via serial while RTS was asserted, it now gets sent
        // for real before the transmission ends, exactly like the AVR
        // original (which achieves this the same way: clearing rtsKeyed
        // un-suppresses processHalfBit(), which then runs the ordinary
        // buffer-drain / TX_END_FLAG logic on its own).
        s_rtsSession = false;
        s_sendBuffer.endWhenBufferEmpty = true;
        FskTimer::setActive(true, false);
      }
    }

    // (2) Drain one queued command, if any arrived within this iteration's wait.
    if (haveMsg) {
      handleMessage(msg, inhibitedNow);
    }

    // (3) Advance the non-blocking PTT/PA lead-tail state machine.
    uint32_t nowMs = millis();
    switch (s_state) {
      case State::Idle:
        break;

      case State::LeadPa:
        if (nowMs - s_stateEnteredMs >= currentPaLeadMs()) {
          if (s_rtsSession) {
            FskTimer::holdLow();
          } else if (isCwSource(s_activeSource)) {
            CwTimer::holdKeyUp();
          } else {
            FskTimer::holdMark();
          }
          setPttOutput(true);
          digitalWrite(LED_RX_PIN, LOW);
          StatusDisplay::refreshLine1(s_cfg.callsign, sourceLabel(s_activeSource), s_cfg.markHigh);
          s_state = State::LeadPtt;
          s_stateEnteredMs = nowMs;
          updateStatus();
        }
        break;

      case State::LeadPtt:
        if (nowMs - s_stateEnteredMs >= currentPttLeadMs()) {
          if (s_rtsSession) {
            FskTimer::setActive(true, true);
          } else if (isCwSource(s_activeSource)) {
            CwTimer::resetForNewTx();
            CwTimer::setActive(true);
          } else {
            FskTimer::resetForNewTx();
            FskTimer::setActive(true, false);
          }
          StatusDisplay::resetTxLine();
          s_state = State::Sending;
          s_stateEnteredMs = nowMs;
          updateStatus();
        }
        break;

      case State::Sending:
        if (isCwSource(s_activeSource)) {
          pumpCwEngine();
          updateStatus();
        } else if (!s_rtsSession) {
          // Feed the ISR's lookahead ring buffer -- this is the "compute
          // next character's bit pattern one full character-period ahead"
          // decision from the design doc.
          while (FskTimer::ringHasFreeSlot()) {
            Baudot::QueuedSymbol sym = s_sendBuffer.nextSymbol();
            if (!FskTimer::pushSymbol(sym)) break;
          }
          uint8_t startedAscii;
          while (FskTimer::takeCharStarted(startedAscii)) {
            StatusDisplay::appendTxChar(startedAscii);
            noteCharStarted(startedAscii);
          }
          if (FskTimer::takeEndOfData()) {
            FskTimer::setActive(false, false);
            beginTailSequence();
          }
          updateStatus(); // cheap; keeps bufferPending fresh for the web/LCD feed
        }
        // else: RTS-sourced session, just waiting on the RTS-poll block above
        break;

      case State::TailPtt:
        if (nowMs - s_stateEnteredMs >= currentPttTailMs()) {
          setPttOutput(false);
          digitalWrite(LED_RX_PIN, HIGH);
          if (s_rtsSession) {
            FskTimer::holdLow();
          } else if (isCwSource(s_activeSource)) {
            CwTimer::holdKeyUp();
          } else {
            FskTimer::holdSpace();
          }
          s_state = State::TailPa;
          s_stateEnteredMs = nowMs;
          updateStatus();
        }
        break;

      case State::TailPa:
        if (nowMs - s_stateEnteredMs >= currentPaTailMs()) {
          setPaOutput(false);
          bool stillInhibited = (digitalRead(CPU_INH_PIN) == LOW);
          StatusDisplay::showStatus(stillInhibited ? "INHIBIT" : "RX");
          StatusDisplay::resetTxLine();
          FskTimer::resetForNewTx();
          CwTimer::resetForNewTx();
          bool wasCw = isCwSource(s_activeSource);
          if (!wasCw) {
            s_sendBuffer.reset();
            Serial.write("\ncmd:\n"); // tells N1MM that TX is finished -- UART1 only; Winkey hosts don't speak this
          }
          s_state = State::Idle;
          s_rtsSession = false;
          updateStatus();
          // CW only: if WinkeyEmulator queued more text/side-effects DURING
          // this tail delay (a real host streams continuously, it doesn't
          // wait for our PTT timing), don't drop it as a dead session --
          // start a fresh lead-in for it immediately, same as a real Winkey
          // keyer keeping PTT engaged across closely-spaced text rather
          // than chopping it into fragments. Only truly reset the CW buffer
          // once it's confirmed empty.
          if (wasCw) {
            if (s_cwBuffer.peekKind() != Morse::NextKind::None) {
              beginKeyUp(Source::Winkey, digitalRead(CPU_INH_PIN) == LOW);
            } else {
              s_cwBuffer.reset();
            }
          }
        }
        break;
    }
  }
}

} // namespace

void prepareSafePins() {
  // Set output latches before changing direction to avoid a brief active-HIGH
  // pulse as the GPIO switches from reset input mode to output mode.
  setPttOutput(false);
  setPaOutput(false);
  digitalWrite(LED_RX_PIN, HIGH);
  digitalWrite(ON_PIN, LOW);
  pinMode(PTT_PIN, OUTPUT);
  pinMode(PTT_PA_PIN, OUTPUT);
  pinMode(LED_RX_PIN, OUTPUT);
  pinMode(ON_PIN, OUTPUT);

  // GPIO35/GPIO39 are input-only and have no internal pull resistors.
  // External pull-ups are mandatory on both active-LOW inputs.
  pinMode(CPU_INH_PIN, INPUT);
  pinMode(PTT_USB_RTS_PIN, INPUT);
}

void begin(const Config &cfg) {
  s_cfg = cfg;

  prepareSafePins();

  s_cwBuffer.setSpeedWpm(cfg.cwSpeedWpm);

  s_queue = xQueueCreate(COMMAND_QUEUE_DEPTH, sizeof(TxMsg));
  if (s_queue == nullptr) {
    Serial.println(F("TxManager queue allocation failed; TX disabled"));
    prepareSafePins();
    return;
  }

  // Pinned to core 1 (same core as the timer ISRs by default), elevated
  // priority relative to the web server -- per the design doc's dual-core
  // decision.
  BaseType_t taskResult = xTaskCreatePinnedToCore(
      taskFn, "TxManager", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
  if (taskResult != pdPASS) {
    Serial.println(F("TxManager task creation failed; TX disabled"));
    vQueueDelete(s_queue);
    s_queue = nullptr;
    prepareSafePins();
  }
}

void applyConfig(const Config &cfg) {
  s_cfg = cfg;
  s_cwBuffer.setSpeedWpm(cfg.cwSpeedWpm);
}

bool enqueueKeyUp(Source src) {
  if (s_queue == nullptr) return false;
  TxMsg m{TxMsg::Type::KeyUp, 0, src, Morse::SideEffect::Nop};
  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}

bool enqueueBufferedEnd() {
  if (s_queue == nullptr) return false;
  TxMsg m{TxMsg::Type::BufferedEnd, 0, Source::SerialLink, Morse::SideEffect::Nop};
  if (xQueueSend(s_queue, &m, 0) == pdTRUE) return true;
  // Never leave a keyed transmitter waiting forever for an End command that
  // could not enter a full queue. Drop pending data and force a safe abort.
  enqueueAbort();
  return false;
}

bool enqueueAbort() {
  if (s_queue == nullptr) return false;
  TxMsg m{TxMsg::Type::Abort, 0, Source::SerialLink, Morse::SideEffect::Nop};
  // Abort has priority over all queued text/control traffic. Resetting the
  // command queue is intentional: none of that work may execute after abort.
  xQueueReset(s_queue);
  return xQueueSendToFront(s_queue, &m, 0) == pdTRUE;
}

bool enqueueByte(uint8_t b, Source src) {
  if (s_queue == nullptr) return false;
  TxMsg m{TxMsg::Type::AppendByte, b, src, Morse::SideEffect::Nop};
  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}

size_t commandQueueFreeSlots() {
  return s_queue == nullptr ? 0 : static_cast<size_t>(uxQueueSpacesAvailable(s_queue));
}

bool cwEnqueueChar(uint8_t asciiByte) {
  if (s_queue == nullptr) return false;
  TxMsg m{TxMsg::Type::CwAppendChar, asciiByte, Source::Winkey, Morse::SideEffect::Nop};
  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}

bool cwEnqueueMergeMark() {
  if (s_queue == nullptr) return false;
  TxMsg m{TxMsg::Type::CwMergeMark, 0, Source::Winkey, Morse::SideEffect::Nop};
  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}

bool cwEnqueueSideEffect(Morse::SideEffect effect, uint8_t value) {
  if (s_queue == nullptr) return false;
  TxMsg m{TxMsg::Type::CwSideEffect, value, Source::Winkey, effect};
  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}

bool cwEnqueueBufferedSpeed(uint8_t wpm) {
  return cwEnqueueSideEffect(Morse::SideEffect::BufferedSpeed, wpm);
}

void cwBackspace() {
  if (s_queue == nullptr) return;
  TxMsg m{TxMsg::Type::CwBackspace, 0, Source::Winkey, Morse::SideEffect::Nop};
  xQueueSend(s_queue, &m, 0);
}

void cwClearPendingBuffer() {
  if (s_queue == nullptr) return;
  TxMsg m{TxMsg::Type::CwClearPending, 0, Source::Winkey, Morse::SideEffect::Nop};
  xQueueSend(s_queue, &m, 0);
}

void cwSetPttLeadTail(uint16_t leadMs, uint16_t tailMs) {
  s_cwLeadMs = leadMs;
  s_cwTailMs = tailMs;
}

void cwSetTuneKeyDown(bool down) {
  if (s_queue == nullptr) return;
  TxMsg m{TxMsg::Type::CwTuneKey, static_cast<uint8_t>(down ? 1 : 0), Source::Winkey, Morse::SideEffect::Nop};
  xQueueSend(s_queue, &m, 0);
}

uint16_t cwBufferPending() { return getStatus().bufferPending; }

// The four timing setters below are called directly (not queued) since
// they only ever affect characters generated in the future -- CwBuffer's
// own setters already document "takes effect after the call", and reading
// s_cwBuffer's timing fields is never done from the ISR, only from
// TxManager's own task (pumpCwEngine()) and here, so there's no cross-task
// hazard needing the queue's ordering guarantee. (Contrast with
// cwEnqueueBufferedSpeed(), which is the intentionally-ordered/buffered
// Winkey 0x1C command, not this immediate 0x02/0x03/0x0D/0x10/0x11 path.)
void cwSetSpeedWpm(uint8_t wpm) { s_cwBuffer.setSpeedWpm(wpm); }
void cwSetWeightingPct(uint8_t pct) { s_cwBuffer.setWeightingPct(pct); }
void cwSetFarnsworthWpm(uint8_t wpm) { s_cwBuffer.setFarnsworthWpm(wpm); }
void cwSetKeyCompMs(uint8_t ms) { s_cwBuffer.setKeyCompMs(ms); }
void cwSetFirstExtensionMs(uint8_t ms) { s_cwBuffer.setFirstExtensionMs(ms); }

Status getStatus() {
  Status copy;
  portENTER_CRITICAL(&s_statusMux);
  copy = s_status;
  portEXIT_CRITICAL(&s_statusMux);
  return copy;
}

} // namespace TxManager
