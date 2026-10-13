#pragma once

#include <stdint.h>

namespace AdcLevel {

struct Reading {
  uint8_t levelPct = 0;
  bool clipped = false;
  bool valid = false;
};

class WindowAnalyzer {
 public:
  static constexpr uint16_t kWindowSamples = 400;

  bool addSample(uint16_t raw, Reading &reading);

 private:
  uint16_t samples_ = 0;
  uint16_t minimum_ = 4095;
  uint16_t maximum_ = 0;
  uint32_t sum_ = 0;
  bool clipped_ = false;
};

} // namespace AdcLevel
