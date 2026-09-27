#pragma once

#include <stdint.h>

#include "TxSequencer.h"

// Pure, platform-independent decisions for handing the transmitter from one
// session to the next. TxManager has exactly one session at a time, but its
// producers don't wait for each other: N1MM may send the next "[text]" while
// the previous transmission is still in its PTT/PA tail, UART2's Winkey host
// may stream CW while an RTTY session is on air, and so on. These rules decide
// what happens to such a request instead of silently dropping it; TxManager
// applies them to the real buffers and pins. Kept header-only and free of
// Arduino/FreeRTOS calls so they are covered by host-side unit tests, the
// same way TxSequencer's timing transitions are.
namespace TxHandoff {

using State = TxSequencer::State;

// The two playback engines. Every RTTY source (UART1, UART2 in FSK2 mode, the
// RTS input, the web Send button) shares the Baudot engine; only the Winkey
// host drives the CW engine.
enum class Engine : uint8_t { Fsk, Cw };

enum class KeyUpAction : uint8_t {
  Start,  // idle: open a new session now
  Ignore, // same engine already transmitting: a redundant TX_ON (AVR behaviour)
  Defer,  // remember it and open a new session once the current one has ended
};

// An RTTY TX_ON ('[', or the web Send button) arriving while `state` is
// current and `active` is the engine of the session in progress.
inline KeyUpAction onKeyUp(State state, Engine active, Engine requested) {
  if (state == State::Idle) return KeyUpAction::Start;
  // During the tail the session is already ending: its buffer is cleared once
  // PA drops, so a new request must become its own session, not be merged.
  if (state == State::TailPtt || state == State::TailPa) return KeyUpAction::Defer;
  return (active == requested) ? KeyUpAction::Ignore : KeyUpAction::Defer;
}

enum class NextSession : uint8_t { None, Fsk, Cw };

// Chosen once PA has dropped and the transmitter is idle again. A deferred
// RTTY request goes first -- UART1/RTTY has priority over UART2 by design --
// and CW content still waiting in its buffer follows as the session after.
inline NextSession afterTail(bool fskKeyUpDeferred, bool cwContentPending) {
  if (fskKeyUpDeferred) return NextSession::Fsk;
  if (cwContentPending) return NextSession::Cw;
  return NextSession::None;
}

// CW text arriving during a CW session's PTT tail continues the SAME session,
// as a real WinKey does: PTT is still engaged, so keying resumes without
// dropping it and running the lead-in again. RTTY is not resumed this way --
// its session ended with an explicit TX_END, so a new request gets a new
// session (see onKeyUp()).
inline bool resumeCwFromTail(State state, Engine active, bool cwContentPending) {
  return state == State::TailPtt && active == Engine::Cw && cwContentPending;
}

} // namespace TxHandoff
