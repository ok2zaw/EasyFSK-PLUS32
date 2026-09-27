#include <unity.h>

#include <cstring>

#include "Arduino.h"
#include "Morse.h"

SerialMock Serial;

namespace {

void assertRun(const Morse::Run &run, bool keyDown, uint16_t durationMs,
               uint8_t asciiByte) {
  TEST_ASSERT_EQUAL(keyDown, run.keyDown);
  TEST_ASSERT_EQUAL_UINT16(durationMs, run.durationMs);
  TEST_ASSERT_EQUAL_UINT8(asciiByte, run.asciiByte);
}

Morse::Run nextRun(Morse::CwBuffer &buffer) {
  Morse::Run run;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::NextKind::Element),
                          static_cast<uint8_t>(buffer.peekKind()));
  TEST_ASSERT_TRUE(buffer.nextElementRun(run));
  return run;
}

} // namespace

void setUp() { Serial.reset(); }
void tearDown() {}

void test_lookup_is_case_insensitive_and_supports_punctuation() {
  TEST_ASSERT_EQUAL_STRING(".-", Morse::lookupPattern('A'));
  TEST_ASSERT_EQUAL_STRING(".-", Morse::lookupPattern('a'));
  TEST_ASSERT_EQUAL_STRING(".----", Morse::lookupPattern('1'));
  TEST_ASSERT_EQUAL_STRING("..--..", Morse::lookupPattern('?'));
  TEST_ASSERT_NULL(Morse::lookupPattern(' '));
  TEST_ASSERT_NULL(Morse::lookupPattern(0x80));
}

void test_a_expands_to_standard_twenty_wpm_runs() {
  Morse::CwBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addChar('A'));

  assertRun(nextRun(buffer), true, 60, 'A');
  assertRun(nextRun(buffer), false, 60, 0);
  assertRun(nextRun(buffer), true, 180, 0);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::NextKind::None),
                          static_cast<uint8_t>(buffer.peekKind()));
}

void test_consecutive_characters_have_one_three_unit_gap() {
  Morse::CwBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addChar('E'));
  TEST_ASSERT_TRUE(buffer.addChar('T'));

  assertRun(nextRun(buffer), true, 60, 'E');
  assertRun(nextRun(buffer), false, 180, 0);
  assertRun(nextRun(buffer), true, 180, 'T');
}

void test_space_produces_one_seven_unit_word_gap() {
  Morse::CwBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addChar('E'));
  TEST_ASSERT_TRUE(buffer.addChar(' '));
  TEST_ASSERT_TRUE(buffer.addChar('T'));

  assertRun(nextRun(buffer), true, 60, 'E');
  assertRun(nextRun(buffer), false, 420, 0);
  assertRun(nextRun(buffer), true, 180, 'T');
}

void test_merge_mark_removes_gap_between_next_two_characters() {
  Morse::CwBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addChar('X'));
  TEST_ASSERT_TRUE(buffer.addMergeMark());
  TEST_ASSERT_TRUE(buffer.addChar('A'));
  TEST_ASSERT_TRUE(buffer.addChar('R'));

  // Drain X (-..-) and its three intra-character gaps.
  for (int i = 0; i < 7; ++i) nextRun(buffer);

  assertRun(nextRun(buffer), false, 180, 0); // normal X -> A gap
  assertRun(nextRun(buffer), true, 60, 'A');
  assertRun(nextRun(buffer), false, 60, 0);
  assertRun(nextRun(buffer), true, 180, 0);
  assertRun(nextRun(buffer), true, 60, 'R'); // merged A -> R: no gap
}

void test_timing_controls_apply_to_new_character_runs() {
  Morse::CwBuffer buffer;
  buffer.reset();
  buffer.setSpeedWpm(10);        // dit = 120 ms
  buffer.setWeightingPct(60);    // mark x1.2, intra-gap x0.8
  buffer.setKeyCompMs(4);        // add 4 ms to mark, remove 4 from gap
  buffer.setFirstExtensionMs(5); // first element of each character only
  TEST_ASSERT_TRUE(buffer.addChar('A'));

  assertRun(nextRun(buffer), true, 153, 'A');  // 120*1.2 + 4 + 5
  assertRun(nextRun(buffer), false, 92, 0);    // 120*0.8 - 4
  assertRun(nextRun(buffer), true, 436, 0);    // 360*1.2 + 4
}

void test_timing_change_does_not_split_an_in_flight_character() {
  Morse::CwBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addChar('A'));
  TEST_ASSERT_TRUE(buffer.addChar('E'));

  assertRun(nextRun(buffer), true, 60, 'A');
  buffer.setSpeedWpm(10); // must apply starting with E, not midway through A
  assertRun(nextRun(buffer), false, 60, 0);
  assertRun(nextRun(buffer), true, 180, 0);
  assertRun(nextRun(buffer), false, 360, 0);
  assertRun(nextRun(buffer), true, 120, 'E');
}

void test_speed_is_clamped_to_winkey_range() {
  Morse::CwBuffer slow;
  slow.reset();
  slow.setSpeedWpm(1);
  TEST_ASSERT_TRUE(slow.addChar('E'));
  assertRun(nextRun(slow), true, 240, 'E');

  Morse::CwBuffer fast;
  fast.reset();
  fast.setSpeedWpm(200);
  TEST_ASSERT_TRUE(fast.addChar('E'));
  assertRun(nextRun(fast), true, 12, 'E');
}

void test_backspace_removes_latest_unsent_item() {
  Morse::CwBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addChar('E'));
  TEST_ASSERT_TRUE(buffer.addChar('T'));
  buffer.backspace();

  TEST_ASSERT_EQUAL_size_t(1, buffer.pending());
  assertRun(nextRun(buffer), true, 60, 'E');
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::NextKind::None),
                          static_cast<uint8_t>(buffer.peekKind()));
  buffer.backspace(); // empty is a safe no-op
}

void test_full_buffer_rejects_additional_items() {
  Morse::CwBuffer buffer;
  buffer.reset();
  for (size_t i = 0; i < Morse::BUFFER_SIZE; ++i) {
    TEST_ASSERT_TRUE(buffer.addChar('E'));
  }

  TEST_ASSERT_TRUE(buffer.full());
  TEST_ASSERT_EQUAL_size_t(Morse::BUFFER_SIZE, buffer.pending());
  TEST_ASSERT_FALSE(buffer.addChar('T'));
  TEST_ASSERT_FALSE(buffer.addSideEffect(Morse::SideEffect::Wait, 3));
}

void test_side_effect_keeps_order_between_characters() {
  Morse::CwBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addChar('E'));
  TEST_ASSERT_TRUE(buffer.addSideEffect(Morse::SideEffect::Wait, 7));
  TEST_ASSERT_TRUE(buffer.addChar('T'));

  assertRun(nextRun(buffer), true, 60, 'E');
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::NextKind::SideEffect),
                          static_cast<uint8_t>(buffer.peekKind()));
  Morse::Run unused;
  TEST_ASSERT_FALSE(buffer.nextElementRun(unused));

  uint8_t value = 0;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::SideEffect::Wait),
                          static_cast<uint8_t>(buffer.takeSideEffect(value)));
  TEST_ASSERT_EQUAL_UINT8(7, value);
  assertRun(nextRun(buffer), false, 180, 0);
  assertRun(nextRun(buffer), true, 180, 'T');
}

void test_buffered_speed_is_exposed_as_ordered_side_effect() {
  Morse::CwBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addBufferedSpeed(35));

  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::NextKind::SideEffect),
                          static_cast<uint8_t>(buffer.peekKind()));
  uint8_t value = 0;
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(Morse::SideEffect::BufferedSpeed),
      static_cast<uint8_t>(buffer.takeSideEffect(value)));
  TEST_ASSERT_EQUAL_UINT8(35, value);
}

void test_unsupported_character_is_skipped_without_losing_following_text() {
  Morse::CwBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addChar(0x80));
  TEST_ASSERT_TRUE(buffer.addChar('E'));

  assertRun(nextRun(buffer), true, 60, 'E');
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::NextKind::None),
                          static_cast<uint8_t>(buffer.peekKind()));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_lookup_is_case_insensitive_and_supports_punctuation);
  RUN_TEST(test_a_expands_to_standard_twenty_wpm_runs);
  RUN_TEST(test_consecutive_characters_have_one_three_unit_gap);
  RUN_TEST(test_space_produces_one_seven_unit_word_gap);
  RUN_TEST(test_merge_mark_removes_gap_between_next_two_characters);
  RUN_TEST(test_timing_controls_apply_to_new_character_runs);
  RUN_TEST(test_timing_change_does_not_split_an_in_flight_character);
  RUN_TEST(test_speed_is_clamped_to_winkey_range);
  RUN_TEST(test_backspace_removes_latest_unsent_item);
  RUN_TEST(test_full_buffer_rejects_additional_items);
  RUN_TEST(test_side_effect_keeps_order_between_characters);
  RUN_TEST(test_buffered_speed_is_exposed_as_ordered_side_effect);
  RUN_TEST(test_unsupported_character_is_skipped_without_losing_following_text);
  return UNITY_END();
}
