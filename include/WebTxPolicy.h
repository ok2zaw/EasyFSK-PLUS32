#pragma once

#include <stddef.h>

// Pure, hardware-independent admission policy for POST /api/tx/send.
// Keeping this calculation outside ESPAsyncWebServer makes the safety-
// relevant queue preflight directly host-testable.
namespace WebTxPolicy {

constexpr size_t MAX_TEXT_LENGTH = 120;

enum class Decision {
  Accept,
  Inhibited,
  TextTooLong,
  QueueFull,
};

struct Plan {
  Decision decision = Decision::Accept;
  size_t requiredSlots = 0;
  bool startSession = false;
  bool endSession = false;
};

// Whether web text can simply be appended to the transmission in progress
// (like text typed into an active N1MM "[...]" session). Not during the
// tail -- that session is already over and its buffer is about to be
// cleared -- and not into a CW session, whose engine can't send RTTY text.
// In those cases the web request brings its own TX_ON/TX_END and TxManager
// starts it as the next session.
inline bool canAppendToActive(bool txActive, bool ending, bool cwSession) {
  return txActive && !ending && !cwSession;
}

// `txActive` here means "append to the active session" -- pass the result
// of canAppendToActive(), not the raw TX state.
inline Plan planSend(bool inhibited, bool txActive, size_t textLength,
                     size_t freeSlots) {
  Plan plan;
  if (inhibited) {
    plan.decision = Decision::Inhibited;
    return plan;
  }
  if (textLength > MAX_TEXT_LENGTH) {
    plan.decision = Decision::TextTooLong;
    return plan;
  }

  plan.startSession = !txActive;
  plan.endSession = !txActive;
  plan.requiredSlots = textLength + (txActive ? 0u : 2u);
  if (plan.requiredSlots > freeSlots) {
    plan.decision = Decision::QueueFull;
  }
  return plan;
}

} // namespace WebTxPolicy
