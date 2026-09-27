#include <unity.h>

#include "Arduino.h"
#include "WebTxPolicy.h"

SerialMock Serial;

void setUp() { Serial.reset(); }
void tearDown() {}

void test_inhibit_takes_priority_over_other_rejections() {
  WebTxPolicy::Plan plan = WebTxPolicy::planSend(true, false, 121, 0);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(WebTxPolicy::Decision::Inhibited),
      static_cast<uint8_t>(plan.decision));
  TEST_ASSERT_EQUAL_size_t(0, plan.requiredSlots);
}

void test_text_length_boundary_is_accepted() {
  WebTxPolicy::Plan plan = WebTxPolicy::planSend(false, true, 120, 120);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WebTxPolicy::Decision::Accept),
                          static_cast<uint8_t>(plan.decision));
  TEST_ASSERT_EQUAL_size_t(120, plan.requiredSlots);
}

void test_text_above_limit_is_rejected_before_slot_calculation() {
  WebTxPolicy::Plan plan = WebTxPolicy::planSend(false, false, 121, 1000);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(WebTxPolicy::Decision::TextTooLong),
      static_cast<uint8_t>(plan.decision));
  TEST_ASSERT_EQUAL_size_t(0, plan.requiredSlots);
}

void test_idle_send_reserves_keyup_text_and_end() {
  WebTxPolicy::Plan plan = WebTxPolicy::planSend(false, false, 5, 7);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WebTxPolicy::Decision::Accept),
                          static_cast<uint8_t>(plan.decision));
  TEST_ASSERT_EQUAL_size_t(7, plan.requiredSlots);
  TEST_ASSERT_TRUE(plan.startSession);
  TEST_ASSERT_TRUE(plan.endSession);
}

void test_active_send_only_reserves_text_bytes() {
  WebTxPolicy::Plan plan = WebTxPolicy::planSend(false, true, 5, 5);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WebTxPolicy::Decision::Accept),
                          static_cast<uint8_t>(plan.decision));
  TEST_ASSERT_EQUAL_size_t(5, plan.requiredSlots);
  TEST_ASSERT_FALSE(plan.startSession);
  TEST_ASSERT_FALSE(plan.endSession);
}

void test_one_missing_queue_slot_rejects_entire_request() {
  WebTxPolicy::Plan plan = WebTxPolicy::planSend(false, false, 5, 6);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(WebTxPolicy::Decision::QueueFull),
      static_cast<uint8_t>(plan.decision));
  TEST_ASSERT_EQUAL_size_t(7, plan.requiredSlots);
}

void test_empty_idle_send_still_reserves_session_brackets() {
  WebTxPolicy::Plan accepted = WebTxPolicy::planSend(false, false, 0, 2);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WebTxPolicy::Decision::Accept),
                          static_cast<uint8_t>(accepted.decision));
  TEST_ASSERT_EQUAL_size_t(2, accepted.requiredSlots);

  WebTxPolicy::Plan rejected = WebTxPolicy::planSend(false, false, 0, 1);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(WebTxPolicy::Decision::QueueFull),
      static_cast<uint8_t>(rejected.decision));
}

void test_append_only_to_a_running_rtty_session() {
  TEST_ASSERT_TRUE(WebTxPolicy::canAppendToActive(true, false, false));
  TEST_ASSERT_FALSE(WebTxPolicy::canAppendToActive(false, false, false)); // idle
  TEST_ASSERT_FALSE(WebTxPolicy::canAppendToActive(true, true, false));  // tail
  TEST_ASSERT_FALSE(WebTxPolicy::canAppendToActive(true, false, true));  // CW on air
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_inhibit_takes_priority_over_other_rejections);
  RUN_TEST(test_text_length_boundary_is_accepted);
  RUN_TEST(test_text_above_limit_is_rejected_before_slot_calculation);
  RUN_TEST(test_idle_send_reserves_keyup_text_and_end);
  RUN_TEST(test_active_send_only_reserves_text_bytes);
  RUN_TEST(test_one_missing_queue_slot_rejects_entire_request);
  RUN_TEST(test_empty_idle_send_still_reserves_session_brackets);
  RUN_TEST(test_append_only_to_a_running_rtty_session);
  return UNITY_END();
}
