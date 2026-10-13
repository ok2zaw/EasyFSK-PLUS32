#include <unity.h>

#include "AdcLevel.h"

namespace {

void feed(AdcLevel::WindowAnalyzer &analyzer, uint16_t sample,
          AdcLevel::Reading &reading) {
  bool complete = false;
  for (uint16_t i = 0; i < AdcLevel::WindowAnalyzer::kWindowSamples; ++i) {
    complete = analyzer.addSample(sample, reading);
  }
  TEST_ASSERT_TRUE(complete);
}

} // namespace

void setUp() {}
void tearDown() {}

void test_quiet_centered_input_has_low_level_without_clipping() {
  AdcLevel::WindowAnalyzer analyzer;
  AdcLevel::Reading reading;
  feed(analyzer, 2048, reading);
  TEST_ASSERT_TRUE(reading.valid);
  TEST_ASSERT_EQUAL_UINT8(0, reading.levelPct);
  TEST_ASSERT_FALSE(reading.clipped);
}

void test_quiet_noncentered_bias_is_not_mistaken_for_audio() {
  AdcLevel::WindowAnalyzer analyzer;
  AdcLevel::Reading reading;
  feed(analyzer, 1900, reading);
  TEST_ASSERT_EQUAL_UINT8(0, reading.levelPct);
  TEST_ASSERT_FALSE(reading.clipped);
}

void test_five_percent_noise_reference_is_approximately_five_percent() {
  AdcLevel::WindowAnalyzer analyzer;
  AdcLevel::Reading reading;
  bool complete = false;
  for (uint16_t i = 0; i < AdcLevel::WindowAnalyzer::kWindowSamples; ++i) {
    complete = analyzer.addSample((i & 1U) ? 2149 : 1947, reading);
  }
  TEST_ASSERT_TRUE(complete);
  TEST_ASSERT_EQUAL_UINT8(5, reading.levelPct);
  TEST_ASSERT_FALSE(reading.clipped);
}

void test_samples_near_adc_rails_flag_overload() {
  AdcLevel::WindowAnalyzer analyzer;
  AdcLevel::Reading reading;
  bool complete = false;
  for (uint16_t i = 0; i < AdcLevel::WindowAnalyzer::kWindowSamples; ++i) {
    complete = analyzer.addSample(i == 0 ? 4095 : 2048, reading);
  }
  TEST_ASSERT_TRUE(complete);
  TEST_ASSERT_TRUE(reading.clipped);
  TEST_ASSERT_EQUAL_UINT8(100, reading.levelPct);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_quiet_centered_input_has_low_level_without_clipping);
  RUN_TEST(test_quiet_noncentered_bias_is_not_mistaken_for_audio);
  RUN_TEST(test_five_percent_noise_reference_is_approximately_five_percent);
  RUN_TEST(test_samples_near_adc_rails_flag_overload);
  return UNITY_END();
}
