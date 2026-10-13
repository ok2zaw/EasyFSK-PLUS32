#include "AdcLevel.h"

namespace AdcLevel {

namespace {

constexpr uint16_t kAdcMax = 4095;
constexpr uint16_t kClipMargin = 12;
constexpr uint16_t kFullScaleAmplitude = 2047;

} // namespace

bool WindowAnalyzer::addSample(uint16_t raw, Reading &reading) {
  if (raw > kAdcMax) return false;

  if (raw < minimum_) minimum_ = raw;
  if (raw > maximum_) maximum_ = raw;
  sum_ += raw;
  if (raw <= kClipMargin || raw >= kAdcMax - kClipMargin) clipped_ = true;

  if (++samples_ < kWindowSamples) return false;

  const uint16_t dcOffset = static_cast<uint16_t>(
      (sum_ + kWindowSamples / 2U) / kWindowSamples);
  const uint16_t peak = maximum_ - dcOffset > dcOffset - minimum_
                            ? maximum_ - dcOffset
                            : dcOffset - minimum_;
  uint32_t percent =
      (static_cast<uint32_t>(peak) * 100U + kFullScaleAmplitude / 2U) /
      kFullScaleAmplitude;
  if (percent > 100U) percent = 100U;
  reading.levelPct = static_cast<uint8_t>(percent);
  reading.clipped = clipped_;
  reading.valid = true;

  samples_ = 0;
  minimum_ = kAdcMax;
  maximum_ = 0;
  sum_ = 0;
  clipped_ = false;
  return true;
}

} // namespace AdcLevel
