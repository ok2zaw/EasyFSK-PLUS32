#include <unity.h>

#include <vector>

#include "AfskModulator.h"

namespace {

float estimateFrequency(const std::vector<int16_t> &samples) {
  size_t crossings = 0;
  size_t firstCrossing = 0;
  size_t lastCrossing = 0;
  for (size_t i = 1; i < samples.size(); ++i) {
    if (samples[i - 1] <= 0 && samples[i] > 0) {
      if (crossings == 0) firstCrossing = i;
      lastCrossing = i;
      ++crossings;
    }
  }
  if (crossings < 2) return 0.0f;
  return static_cast<float>(crossings - 1) * 48000.0f /
         static_cast<float>(lastCrossing - firstCrossing);
}

void resetModulator() {
  AfskModulator::configure(48000, 45.45f, 2125, 2295);
}

} // namespace

void setUp() { resetModulator(); }
void tearDown() {}

void test_start_bit_uses_space_tone_and_data_uses_mark_tone() {
  Baudot::QueuedSymbol symbol{1, 'E'};
  TEST_ASSERT_TRUE(AfskModulator::pushSymbol(symbol));
  AfskModulator::setActive(true);

  std::vector<int16_t> startBit(800);
  for (int16_t &sample : startBit) sample = AfskModulator::nextSample();
  TEST_ASSERT_FLOAT_WITHIN(40.0f, 2295.0f, estimateFrequency(startBit));

  for (size_t i = 0; i < 300; ++i) AfskModulator::nextSample();
  std::vector<int16_t> firstDataBit(800);
  for (int16_t &sample : firstDataBit) sample = AfskModulator::nextSample();
  TEST_ASSERT_FLOAT_WITHIN(40.0f, 2125.0f, estimateFrequency(firstDataBit));
}

void test_end_marker_stops_frames_and_queue_is_bounded() {
  Baudot::QueuedSymbol symbol{1, 'E'};
  for (size_t i = 0; i < AfskModulator::SYMBOL_QUEUE_CAPACITY - 1; ++i) {
    TEST_ASSERT_TRUE(AfskModulator::pushSymbol(symbol));
  }
  TEST_ASSERT_FALSE(AfskModulator::pushSymbol(symbol));

  resetModulator();
  Baudot::QueuedSymbol end{Baudot::TX_END_FLAG, 0};
  TEST_ASSERT_TRUE(AfskModulator::pushSymbol(end));
  AfskModulator::setActive(true);
  for (size_t i = 0; i < 32; ++i) AfskModulator::nextSample();
  TEST_ASSERT_EQUAL_UINT32(0, AfskModulator::underrunCount());
}

void test_empty_symbol_queue_uses_the_fsk_idle_diddle() {
  AfskModulator::setActive(true);
  std::vector<int16_t> start(800);
  for (int16_t &sample : start) sample = AfskModulator::nextSample();
  TEST_ASSERT_FLOAT_WITHIN(40.0f, 2295.0f, estimateFrequency(start));
  for (size_t i = 0; i < 300; ++i) AfskModulator::nextSample();
  std::vector<int16_t> lettersBit(800);
  for (int16_t &sample : lettersBit) sample = AfskModulator::nextSample();
  TEST_ASSERT_FLOAT_WITHIN(40.0f, 2125.0f, estimateFrequency(lettersBit));
  TEST_ASSERT_EQUAL_UINT32(1, AfskModulator::underrunCount());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_start_bit_uses_space_tone_and_data_uses_mark_tone);
  RUN_TEST(test_end_marker_stops_frames_and_queue_is_bounded);
  RUN_TEST(test_empty_symbol_queue_uses_the_fsk_idle_diddle);
  return UNITY_END();
}
