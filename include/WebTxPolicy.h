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
