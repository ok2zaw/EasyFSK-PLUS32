#include <unity.h>

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstring>

#include "Config.h"

LittleFSMock LittleFS;

void setUp() { LittleFS.reset(); }
void tearDown() {}

void test_missing_file_returns_defaults() {
  Config cfg;
  strcpy(cfg.callsign, "OLD");
  cfg.baudRate = 75.0f;

  TEST_ASSERT_FALSE(configLoad(cfg));
  TEST_ASSERT_EQUAL_STRING("", cfg.callsign);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 45.45f, cfg.baudRate);
  TEST_ASSERT_EQUAL_UINT8(20, cfg.cwSpeedWpm);
}

void test_corrupt_json_returns_defaults() {
  LittleFS.fileExists = true;
  LittleFS.contents = "{not-json";
  Config cfg;
  strcpy(cfg.callsign, "OLD");

  TEST_ASSERT_FALSE(configLoad(cfg));
  TEST_ASSERT_EQUAL_STRING("", cfg.callsign);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 45.45f, cfg.baudRate);
}

void test_partial_valid_file_overrides_only_present_fields() {
  LittleFS.fileExists = true;
  LittleFS.contents = R"({"callsign":"OK2ZAW","baudRate":50,"cwSpeedWpm":31})";
  Config cfg;

  TEST_ASSERT_TRUE(configLoad(cfg));
  TEST_ASSERT_EQUAL_STRING("OK2ZAW", cfg.callsign);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, cfg.baudRate);
  TEST_ASSERT_EQUAL_UINT8(31, cfg.cwSpeedWpm);
  TEST_ASSERT_TRUE(cfg.markHigh); // absent field kept its default
}

void test_load_keeps_valid_fields_when_another_field_is_invalid() {
  LittleFS.fileExists = true;
  LittleFS.contents =
      R"({"callsign":"OK2","baudRate":123,"cwSpeedWpm":35,"network":{"hostname":"radio-1","staticIp":"999.1.1.1"}})";
  Config cfg;

  TEST_ASSERT_TRUE(configLoad(cfg));
  TEST_ASSERT_EQUAL_STRING("OK2", cfg.callsign);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 45.45f, cfg.baudRate);
  TEST_ASSERT_EQUAL_UINT8(35, cfg.cwSpeedWpm);
  TEST_ASSERT_EQUAL_STRING("radio-1", cfg.network.hostname);
  TEST_ASSERT_EQUAL_STRING("192.168.1.50", cfg.network.staticIp);
}

void test_api_validation_is_transactional_on_error() {
  Config cfg;
  strcpy(cfg.callsign, "BEFORE");
  cfg.cwSpeedWpm = 20;
  JsonDocument input;
  input["callsign"] = "AFTER";
  input["cwSpeedWpm"] = 100;
  JsonDocument errorDoc;
  JsonObject errors = errorDoc.to<JsonObject>();

  TEST_ASSERT_FALSE(
      configValidate(input.as<JsonVariantConst>(), cfg, errors));
  TEST_ASSERT_EQUAL_STRING("BEFORE", cfg.callsign);
  TEST_ASSERT_EQUAL_UINT8(20, cfg.cwSpeedWpm);
  TEST_ASSERT_TRUE(errors["cwSpeedWpm"].is<const char *>());
}

void test_validation_accepts_all_documented_boundaries() {
  Config cfg;
  JsonDocument input;
  input["callsign"] = "N0CALL";
  input["baudRate"] = 75;
  input["polarity"] = "markLow";
  input["pttLeadMs"] = 0;
  input["pttTailMs"] = 9999;
  input["paLeadMs"] = 0;
  input["paTailMs"] = 9999;
  input["uart2Mode"] = "cw";
  input["cwSpeedWpm"] = 99;
  JsonObject network = input["network"].to<JsonObject>();
  network["hostname"] = "easy-fsk-32";
  network["dhcp"] = false;
  network["staticIp"] = "10.0.0.1";
  network["gateway"] = "10.0.0.254";
  network["subnet"] = "255.255.255.0";
  network["dns"] = "1.1.1.1";
  JsonDocument errorDoc;

  TEST_ASSERT_TRUE(configValidate(input.as<JsonVariantConst>(), cfg,
                                  errorDoc.to<JsonObject>()));
  TEST_ASSERT_EQUAL_STRING("N0CALL", cfg.callsign);
  TEST_ASSERT_FALSE(cfg.markHigh);
  TEST_ASSERT_EQUAL_UINT16(0, cfg.pttLeadMs);
  TEST_ASSERT_EQUAL_UINT16(9999, cfg.pttTailMs);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Uart2Mode::Cw),
                          static_cast<uint8_t>(cfg.uart2Mode));
  TEST_ASSERT_EQUAL_UINT8(99, cfg.cwSpeedWpm);
  TEST_ASSERT_EQUAL_STRING("10.0.0.1", cfg.network.staticIp);
}

void test_validation_reports_each_invalid_network_field() {
  Config cfg;
  JsonDocument input;
  JsonObject network = input["network"].to<JsonObject>();
  network["hostname"] = "bad_name";
  network["staticIp"] = "256.1.1.1";
  network["gateway"] = "1.2.3";
  network["subnet"] = "1..2.3";
  network["dns"] = "abc";
  JsonDocument errorDoc;
  JsonObject errors = errorDoc.to<JsonObject>();

  TEST_ASSERT_FALSE(
      configValidate(input.as<JsonVariantConst>(), cfg, errors));
  TEST_ASSERT_TRUE(errors["network.hostname"].is<const char *>());
  TEST_ASSERT_TRUE(errors["network.staticIp"].is<const char *>());
  TEST_ASSERT_TRUE(errors["network.gateway"].is<const char *>());
  TEST_ASSERT_TRUE(errors["network.subnet"].is<const char *>());
  TEST_ASSERT_TRUE(errors["network.dns"].is<const char *>());
}

void test_save_writes_complete_round_trippable_json() {
  Config original;
  strcpy(original.callsign, "OK2ZAW");
  original.baudRate = 50.0f;
  original.markHigh = false;
  original.uart2Mode = Uart2Mode::Cw;
  original.cwSpeedWpm = 27;

  TEST_ASSERT_TRUE(configSave(original));
  TEST_ASSERT_TRUE(LittleFS.fileExists);
  TEST_ASSERT_FALSE(LittleFS.contents.empty());

  Config loaded;
  TEST_ASSERT_TRUE(configLoad(loaded));
  TEST_ASSERT_EQUAL_STRING(original.callsign, loaded.callsign);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, original.baudRate, loaded.baudRate);
  TEST_ASSERT_EQUAL(original.markHigh, loaded.markHigh);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(original.uart2Mode),
                          static_cast<uint8_t>(loaded.uart2Mode));
  TEST_ASSERT_EQUAL_UINT8(original.cwSpeedWpm, loaded.cwSpeedWpm);
}

void test_save_failure_is_reported() {
  LittleFS.allowWrite = false;
  Config cfg;
  TEST_ASSERT_FALSE(configSave(cfg));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_missing_file_returns_defaults);
  RUN_TEST(test_corrupt_json_returns_defaults);
  RUN_TEST(test_partial_valid_file_overrides_only_present_fields);
  RUN_TEST(test_load_keeps_valid_fields_when_another_field_is_invalid);
  RUN_TEST(test_api_validation_is_transactional_on_error);
  RUN_TEST(test_validation_accepts_all_documented_boundaries);
  RUN_TEST(test_validation_reports_each_invalid_network_field);
  RUN_TEST(test_save_writes_complete_round_trippable_json);
  RUN_TEST(test_save_failure_is_reported);
  return UNITY_END();
}
