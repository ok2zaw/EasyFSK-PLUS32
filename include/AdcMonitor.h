#pragma once

#include "AdcLevel.h"

#include <stdint.h>

namespace AdcMonitor {

struct Snapshot : AdcLevel::Reading {
  uint32_t updatedAtMs = 0;
};

// Starts DMA sampling of the RX audio input on GPIO34 and the worker task
// that publishes short-window peak-level and clipping measurements.
bool begin();
Snapshot getSnapshot();

} // namespace AdcMonitor
