#pragma once

#include <stdint.h>

// Pure, platform-independent timing decisions for the TX output sequencer.
// Keeping these transitions free of Arduino/FreeRTOS calls lets the exact
// PA/PTT ordering be covered by host-side unit tests while TxManager remains
// responsible for applying each event to the physical pins and TX engines.
namespace TxSequencer {

enum class State : uint8_t {
  Idle,
  LeadPa,
  LeadPtt,
  Sending,
  TailPtt,
  TailPa,
};

enum class Event : uint8_t {
  None,
  AssertPtt,
  StartSending,
  ReleasePtt,
  ReleasePa,
};

struct Timings {
  uint16_t paLeadMs;
  uint16_t pttLeadMs;
  uint16_t pttTailMs;
  uint16_t paTailMs;
};

struct Transition {
  Event event;
  State nextState;
};

inline Transition poll(State state, uint32_t nowMs, uint32_t enteredMs,
                       const Timings &timings) {
  // Unsigned subtraction intentionally preserves correct elapsed time when
  // millis() wraps around UINT32_MAX.
  const uint32_t elapsedMs = nowMs - enteredMs;

  switch (state) {
    case State::LeadPa:
      if (elapsedMs >= timings.paLeadMs) {
        return {Event::AssertPtt, State::LeadPtt};
      }
      break;

    case State::LeadPtt:
      if (elapsedMs >= timings.pttLeadMs) {
        return {Event::StartSending, State::Sending};
      }
      break;

    case State::TailPtt:
      if (elapsedMs >= timings.pttTailMs) {
        return {Event::ReleasePtt, State::TailPa};
      }
      break;

    case State::TailPa:
      if (elapsedMs >= timings.paTailMs) {
        return {Event::ReleasePa, State::Idle};
      }
      break;

    case State::Idle:
    case State::Sending:
      break;
  }

  return {Event::None, state};
}

} // namespace TxSequencer
