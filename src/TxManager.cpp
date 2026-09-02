#include "TxManager.h"
#include "Baudot.h"
#include "FskTimer.h"
#include "StatusDisplay.h"
#include "Pins.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

namespace TxManager {

namespace {

struct TxMsg {
  enum class Type : uint8_t { KeyUp, BufferedEnd, Abort, AppendByte } type;
  uint8_t byte;
  Source src;
};

enum class State { Idle, LeadPa, LeadPtt, Sending, TailPa, TailPtt };

QueueHandle_t s_queue = nullptr;
portMUX_TYPE s_statusMux = portMUX_INITIALIZER_UNLOCKED;

Config s_cfg;
Baudot::SendBuffer s_sendBuffer;
State s_state = State::Idle;
uint32_t s_stateEnteredMs = 0;
bool s_rtsSession = false;
Source s_activeSource = Source::SerialLink;
Status s_status;

const char *sourceLabel(Source src) {
  switch (src) {
    case Source::Rts: return "RTS";
    case Source::Web: return "WEB";
    default: return "FSK";
  }
}

void updateStatus() {
  portENTER_CRITICAL(&s_statusMux);
  s_status.txActive = (s_state != State::Idle);
  s_status.paActive = (s_state == State::LeadPa || s_state == State::LeadPtt ||
                        s_state == State::Sending || s_state == State::TailPa);
  s_status.pttSource = s_activeSource;
  s_status.bufferPending = s_sendBuffer.pending();
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
// the serial port either way, same as the AVR original.
void forceStopImmediate() {
  if (s_state == State::Idle) return;
  digitalWrite(PTT_PA_PIN, LOW);
  digitalWrite(PTT_PIN, LOW);
  digitalWrite(LED_RX_PIN, HIGH);
  FskTimer::setActive(false, false);
  if (s_rtsSession) {
    FskTimer::holdLow();
  } else {
    FskTimer::holdSpace();
  }
  s_sendBuffer.reset();
  FskTimer::resetForNewTx();
  s_state = State::Idle;
  s_rtsSession = false;
  updateStatus();
}

// Begins the graceful (PA-tail, then PTT-tail) shutdown sequence. Called
// once the Baudot engine naturally reaches end-of-data, once an
// RTS-sourced session's RTS line deasserts and its (possibly empty)
// buffer has drained, or once TX_ABORT is processed mid-transmission.
void beginTailSequence() {
  digitalWrite(PTT_PA_PIN, LOW);
  s_state = State::TailPa;
  s_stateEnteredMs = millis();
}

void handleMessage(const TxMsg &msg, bool inhibitedNow) {
  switch (msg.type) {
    case TxMsg::Type::KeyUp:
      if (s_state == State::Idle) {
        if (inhibitedNow) {
          StatusDisplay::showStatus("INHIBIT"); // mirrors setPTT()'s own inhibit refusal
        } else {
          s_rtsSession = false;
          s_activeSource = msg.src;
          s_sendBuffer.endWhenBufferEmpty = false;
          digitalWrite(PTT_PA_PIN, HIGH);
          s_state = State::LeadPa;
          s_stateEnteredMs = millis();
          updateStatus();
        }
      }
      // else: redundant TX_ON while already active -- ignored, matches the
      // AVR original's explicit guard (see loop()'s TX_ON handling).
      break;

    case TxMsg::Type::BufferedEnd:
      s_sendBuffer.endWhenBufferEmpty = true; // unconditional, matches the AVR original
      break;

    case TxMsg::Type::Abort:
      if (s_state != State::Idle) {
        s_sendBuffer.reset();
        if (s_state == State::Sending && !s_rtsSession) {
          FskTimer::setActive(false, false);
        }
        beginTailSequence();
      }
      break;

    case TxMsg::Type::AppendByte:
      s_sendBuffer.addByte(msg.byte);
      break;
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
        digitalWrite(PTT_PA_PIN, HIGH);
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
        if (nowMs - s_stateEnteredMs >= s_cfg.paLeadMs) {
          if (s_rtsSession) {
            FskTimer::holdLow();
          } else {
            FskTimer::holdMark();
          }
          digitalWrite(PTT_PIN, HIGH);
          digitalWrite(LED_RX_PIN, LOW);
          StatusDisplay::refreshLine1(s_cfg.callsign, sourceLabel(s_activeSource), s_cfg.markHigh);
          s_state = State::LeadPtt;
          s_stateEnteredMs = nowMs;
        }
        break;

      case State::LeadPtt:
        if (nowMs - s_stateEnteredMs >= s_cfg.pttLeadMs) {
          if (s_rtsSession) {
            FskTimer::setActive(true, true);
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
        if (!s_rtsSession) {
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

      case State::TailPa:
        if (nowMs - s_stateEnteredMs >= s_cfg.paTailMs) {
          digitalWrite(PTT_PIN, LOW);
          digitalWrite(LED_RX_PIN, HIGH);
          bool stillInhibited = (digitalRead(CPU_INH_PIN) == LOW);
          StatusDisplay::showStatus(stillInhibited ? "INHIBIT" : "RX");
          StatusDisplay::resetTxLine();
          if (s_rtsSession) {
            FskTimer::holdLow();
          } else {
            FskTimer::holdSpace();
          }
          s_state = State::TailPtt;
          s_stateEnteredMs = nowMs;
        }
        break;

      case State::TailPtt:
        if (nowMs - s_stateEnteredMs >= s_cfg.pttTailMs) {
          s_sendBuffer.reset();
          FskTimer::resetForNewTx();
          Serial.write("\ncmd:\n"); // tells N1MM that TX is finished
          s_state = State::Idle;
          s_rtsSession = false;
          updateStatus();
        }
        break;
    }
  }
}

} // namespace

void begin(const Config &cfg) {
  s_cfg = cfg;

  pinMode(PTT_PIN, OUTPUT);
  digitalWrite(PTT_PIN, LOW);
  pinMode(PTT_PA_PIN, OUTPUT);
  digitalWrite(PTT_PA_PIN, LOW);
  pinMode(LED_RX_PIN, OUTPUT);
  digitalWrite(LED_RX_PIN, HIGH);
  pinMode(CPU_INH_PIN, INPUT_PULLUP);
  pinMode(PTT_USB_RTS_PIN, INPUT); // external pull-up expected on this net (input-only GPIO)
  pinMode(ON_PIN, OUTPUT);
  digitalWrite(ON_PIN, LOW);

  s_queue = xQueueCreate(64, sizeof(TxMsg));

  // Pinned to core 1 (same core as the timer ISR by default), elevated
  // priority relative to the web server -- per the design doc's dual-core
  // decision.
  xTaskCreatePinnedToCore(taskFn, "TxManager", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
}

void applyConfig(const Config &cfg) { s_cfg = cfg; }

bool enqueueKeyUp(Source src) {
  TxMsg m{TxMsg::Type::KeyUp, 0, src};
  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}

bool enqueueBufferedEnd() {
  TxMsg m{TxMsg::Type::BufferedEnd, 0, Source::SerialLink};
  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}

bool enqueueAbort() {
  TxMsg m{TxMsg::Type::Abort, 0, Source::SerialLink};
  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}

bool enqueueByte(uint8_t b, Source src) {
  TxMsg m{TxMsg::Type::AppendByte, b, src};
  return xQueueSend(s_queue, &m, 0) == pdTRUE;
}

Status getStatus() {
  Status copy;
  portENTER_CRITICAL(&s_statusMux);
  copy = s_status;
  portEXIT_CRITICAL(&s_statusMux);
  return copy;
}

} // namespace TxManager
