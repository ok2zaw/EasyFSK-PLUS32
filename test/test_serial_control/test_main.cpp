#include <Arduino.h>
#include <ArduinoJson.h>
#include <unity.h>

#include <string>
#include <vector>

#include "ConfigStore.h"
#include "SerialControl.h"
#include "TxManager.h"

SerialMock Serial;

namespace {

enum class TxCallKind { KeyUp, BufferedEnd, Abort, Byte };

struct TxCall {
  TxCallKind kind;
  uint8_t byte;
  TxManager::Source source;
};

std::vector<TxCall> txCalls;
bool txQueueAccepts = true;
Config storedConfig;
std::vector<std::string> appliedDocuments;
bool configApplySucceeds = true;

void pollBytes(size_t count) {
  for (size_t i = 0; i < count; ++i) SerialControl::poll();
}

void sendBytes(const std::string &bytes) {
  Serial.pushRx(bytes);
  pollBytes(bytes.size());
}

void expectDocument(size_t index, const char *expected) {
  TEST_ASSERT_LESS_THAN(appliedDocuments.size(), index);
  TEST_ASSERT_EQUAL_STRING(expected, appliedDocuments[index].c_str());
}

} // namespace

namespace ConfigStore {

Config get() { return storedConfig; }

bool applyAndSave(JsonVariantConst in, JsonObject errors) {
  std::string rendered;
  serializeJson(in, rendered);
  appliedDocuments.push_back(rendered);
  if (!configApplySucceeds) errors["mock"] = "save failed";
  return configApplySucceeds;
}

} // namespace ConfigStore

namespace TxManager {

bool enqueueKeyUp(Source source) {
  txCalls.push_back({TxCallKind::KeyUp, 0, source});
  return txQueueAccepts;
}

bool enqueueBufferedEnd() {
  txCalls.push_back({TxCallKind::BufferedEnd, 0, Source::SerialLink});
  return txQueueAccepts;
}

bool enqueueAbort() {
  txCalls.push_back({TxCallKind::Abort, 0, Source::SerialLink});
  return txQueueAccepts;
}

bool enqueueByte(uint8_t byte, Source source) {
  txCalls.push_back({TxCallKind::Byte, byte, source});
  return txQueueAccepts;
}

} // namespace TxManager

void setUp() {
  Serial.reset();
  txCalls.clear();
  txQueueAccepts = true;
  storedConfig = Config{};
  appliedDocuments.clear();
  configApplySucceeds = true;
  SerialControl::begin();
}

void tearDown() {}

void test_poll_consumes_at_most_one_byte() {
  Serial.pushRx("AB");

  SerialControl::poll();

  TEST_ASSERT_EQUAL_UINT32(1, txCalls.size());
  TEST_ASSERT_EQUAL(TxCallKind::Byte, txCalls[0].kind);
  TEST_ASSERT_EQUAL_UINT8('A', txCalls[0].byte);
  TEST_ASSERT_EQUAL(TxManager::Source::SerialLink, txCalls[0].source);
  TEST_ASSERT_EQUAL_INT(1, Serial.available());

  SerialControl::poll();
  TEST_ASSERT_EQUAL_UINT32(2, txCalls.size());
  TEST_ASSERT_EQUAL_UINT8('B', txCalls[1].byte);
}

void test_empty_poll_does_nothing() {
  SerialControl::poll();
  TEST_ASSERT_TRUE(txCalls.empty());
  TEST_ASSERT_TRUE(appliedDocuments.empty());
  TEST_ASSERT_TRUE(Serial.tx.empty());
}

void test_tx_control_bytes_map_to_queue_calls() {
  sendBytes("[]\\");

  TEST_ASSERT_EQUAL_UINT32(3, txCalls.size());
  TEST_ASSERT_EQUAL(TxCallKind::KeyUp, txCalls[0].kind);
  TEST_ASSERT_EQUAL(TxManager::Source::SerialLink, txCalls[0].source);
  TEST_ASSERT_EQUAL(TxCallKind::BufferedEnd, txCalls[1].kind);
  TEST_ASSERT_EQUAL(TxCallKind::Abort, txCalls[2].kind);
}

void test_queue_rejection_does_not_reinterpret_input() {
  txQueueAccepts = false;
  sendBytes("[X]\\");

  TEST_ASSERT_EQUAL_UINT32(4, txCalls.size());
  TEST_ASSERT_EQUAL(TxCallKind::KeyUp, txCalls[0].kind);
  TEST_ASSERT_EQUAL(TxCallKind::Byte, txCalls[1].kind);
  TEST_ASSERT_EQUAL_UINT8('X', txCalls[1].byte);
  TEST_ASSERT_EQUAL(TxCallKind::BufferedEnd, txCalls[2].kind);
  TEST_ASSERT_EQUAL(TxCallKind::Abort, txCalls[3].kind);
  TEST_ASSERT_TRUE(Serial.tx.empty());
}

void test_config_single_key_commands_submit_expected_json() {
  sendBytes("~0~1~4~5~7~D~d");

  TEST_ASSERT_EQUAL_UINT32(7, appliedDocuments.size());
  expectDocument(0, "{\"polarity\":\"markHigh\"}");
  expectDocument(1, "{\"polarity\":\"markLow\"}");
  expectDocument(2, "{\"baudRate\":45.45}");
  expectDocument(3, "{\"baudRate\":50}");
  expectDocument(4, "{\"baudRate\":75}");
  expectDocument(5, "{\"liveLcdText\":true}");
  expectDocument(6, "{\"liveLcdText\":false}");
  TEST_ASSERT_TRUE(txCalls.empty());
}

void test_numeric_commands_submit_expected_fields() {
  sendBytes("~L123\r~T456\n~l7\r~t0\n");

  TEST_ASSERT_EQUAL_UINT32(4, appliedDocuments.size());
  expectDocument(0, "{\"pttLeadMs\":123}");
  expectDocument(1, "{\"pttTailMs\":456}");
  expectDocument(2, "{\"paLeadMs\":7}");
  expectDocument(3, "{\"paTailMs\":0}");
}

void test_numeric_entry_accepts_only_first_four_digits() {
  sendBytes("~L12x345\r");

  TEST_ASSERT_EQUAL_UINT32(1, appliedDocuments.size());
  expectDocument(0, "{\"pttLeadMs\":1234}");
  TEST_ASSERT_NOT_EQUAL(std::string::npos, Serial.tx.find("1234"));
  TEST_ASSERT_EQUAL(std::string::npos, Serial.tx.find("12345"));
}

void test_empty_or_cancelled_numeric_entry_is_not_saved() {
  sendBytes("~L\r~T99~Z");

  TEST_ASSERT_TRUE(appliedDocuments.empty());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, Serial.tx.find("Cancelled."));
  TEST_ASSERT_EQUAL_UINT32(1, txCalls.size());
  TEST_ASSERT_EQUAL(TxCallKind::Byte, txCalls[0].kind);
  TEST_ASSERT_EQUAL_UINT8('Z', txCalls[0].byte);
}

void test_callsign_is_saved_and_limited_to_six_characters() {
  sendBytes("~COK2ZAW7\r");

  TEST_ASSERT_EQUAL_UINT32(1, appliedDocuments.size());
  expectDocument(0, "{\"callsign\":\"OK2ZAW\"}");
  TEST_ASSERT_NOT_EQUAL(std::string::npos, Serial.tx.find("OK2ZAW"));
  TEST_ASSERT_EQUAL(std::string::npos, Serial.tx.find("OK2ZAW7"));
}

void test_empty_or_cancelled_callsign_entry_is_not_saved() {
  sendBytes("~C\r~CNEW~Q");

  TEST_ASSERT_TRUE(appliedDocuments.empty());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, Serial.tx.find("Cancelled."));
  TEST_ASSERT_EQUAL_UINT32(1, txCalls.size());
  TEST_ASSERT_EQUAL_UINT8('Q', txCalls[0].byte);
}

void test_apply_failure_is_reported_and_parser_returns_to_normal() {
  configApplySucceeds = false;
  sendBytes("~0X");

  TEST_ASSERT_EQUAL_UINT32(1, appliedDocuments.size());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, Serial.tx.find("Not applied: save failed"));
  TEST_ASSERT_EQUAL_UINT32(1, txCalls.size());
  TEST_ASSERT_EQUAL_UINT8('X', txCalls[0].byte);
}

void test_query_and_unknown_config_commands_do_not_save() {
  sendBytes("~?~!X");

  TEST_ASSERT_TRUE(appliedDocuments.empty());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, Serial.tx.find("EasyFSK-PLUS32 configuration"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, Serial.tx.find("Not a recognized command"));
  TEST_ASSERT_EQUAL_UINT32(1, txCalls.size());
  TEST_ASSERT_EQUAL_UINT8('X', txCalls[0].byte);
}

void test_begin_resets_an_incomplete_config_command() {
  sendBytes("~L12");
  SerialControl::begin();
  sendBytes("X");

  TEST_ASSERT_TRUE(appliedDocuments.empty());
  TEST_ASSERT_EQUAL_UINT32(1, txCalls.size());
  TEST_ASSERT_EQUAL_UINT8('X', txCalls[0].byte);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_poll_consumes_at_most_one_byte);
  RUN_TEST(test_empty_poll_does_nothing);
  RUN_TEST(test_tx_control_bytes_map_to_queue_calls);
  RUN_TEST(test_queue_rejection_does_not_reinterpret_input);
  RUN_TEST(test_config_single_key_commands_submit_expected_json);
  RUN_TEST(test_numeric_commands_submit_expected_fields);
  RUN_TEST(test_numeric_entry_accepts_only_first_four_digits);
  RUN_TEST(test_empty_or_cancelled_numeric_entry_is_not_saved);
  RUN_TEST(test_callsign_is_saved_and_limited_to_six_characters);
  RUN_TEST(test_empty_or_cancelled_callsign_entry_is_not_saved);
  RUN_TEST(test_apply_failure_is_reported_and_parser_returns_to_normal);
  RUN_TEST(test_query_and_unknown_config_commands_do_not_save);
  RUN_TEST(test_begin_resets_an_incomplete_config_command);
  return UNITY_END();
}
