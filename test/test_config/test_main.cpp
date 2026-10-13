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
  TEST_ASSERT_FALSE(cfg.tci.enabled);
  TEST_ASSERT_EQUAL_UINT16(2125, cfg.tci.markHz);
  TEST_ASSERT_EQUAL_UINT16(2295, cfg.tci.spaceHz);
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

void test_validation_rejects_present_fields_with_wrong_types() {
  Config cfg;
  JsonDocument input;
  input["callsign"] = 123;
  input["baudRate"] = "50";
  input["polarity"] = false;
  input["pttLeadMs"] = 1.5;
  input["liveLcdText"] = "true";
  input["uart2Mode"] = 1;
  input["cwSpeedWpm"] = 20.5;
  input["network"] = "not-an-object";
  input["tci"] = "not-an-object";
  JsonDocument errorDoc;
  JsonObject errors = errorDoc.to<JsonObject>();

  TEST_ASSERT_FALSE(
      configValidate(input.as<JsonVariantConst>(), cfg, errors));
  TEST_ASSERT_TRUE(errors["callsign"].is<const char *>());
  TEST_ASSERT_TRUE(errors["baudRate"].is<const char *>());
  TEST_ASSERT_TRUE(errors["polarity"].is<const char *>());
  TEST_ASSERT_TRUE(errors["pttLeadMs"].is<const char *>());
  TEST_ASSERT_TRUE(errors["liveLcdText"].is<const char *>());
  TEST_ASSERT_TRUE(errors["uart2Mode"].is<const char *>());
  TEST_ASSERT_TRUE(errors["cwSpeedWpm"].is<const char *>());
  TEST_ASSERT_TRUE(errors["network"].is<const char *>());
  TEST_ASSERT_TRUE(errors["tci"].is<const char *>());
}

void test_hostname_must_start_and_end_alphanumeric() {
  const char *invalidHostnames[] = {"-radio", "radio-", "-"};
  for (const char *hostname : invalidHostnames) {
    Config cfg;
    JsonDocument input;
    input["network"]["hostname"] = hostname;
    JsonDocument errorDoc;
    JsonObject errors = errorDoc.to<JsonObject>();
    TEST_ASSERT_FALSE(
        configValidate(input.as<JsonVariantConst>(), cfg, errors));
    TEST_ASSERT_TRUE(errors["network.hostname"].is<const char *>());
  }

  Config cfg;
  JsonDocument input;
  input["network"]["hostname"] = "r";
  JsonDocument errorDoc;
  TEST_ASSERT_TRUE(configValidate(input.as<JsonVariantConst>(), cfg,
                                  errorDoc.to<JsonObject>()));
  TEST_ASSERT_EQUAL_STRING("r", cfg.network.hostname);
}

void test_save_writes_complete_round_trippable_json() {
  Config original;
  strcpy(original.callsign, "OK2ZAW");
  original.baudRate = 50.0f;
  original.markHigh = false;
  original.uart2Mode = Uart2Mode::Cw;
  original.cwSpeedWpm = 27;
  original.tci.enabled = true;
  strcpy(original.tci.serverIp, "10.0.0.25");
  original.tci.port = 50002;
  original.tci.markHz = 2125;
  original.tci.spaceHz = 2295;
  original.tci.sampleRate = 24000;
  original.tci.sampleFormat = 3;
  original.tci.channels = 2;
  original.tci.samples = 480;
  original.tci.bufferTimeoutMs = 100;

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
  TEST_ASSERT_TRUE(loaded.tci.enabled);
  TEST_ASSERT_EQUAL_STRING(original.tci.serverIp, loaded.tci.serverIp);
  TEST_ASSERT_EQUAL_UINT16(original.tci.port, loaded.tci.port);
  TEST_ASSERT_EQUAL_UINT16(original.tci.markHz, loaded.tci.markHz);
  TEST_ASSERT_EQUAL_UINT16(original.tci.spaceHz, loaded.tci.spaceHz);
  TEST_ASSERT_EQUAL_UINT32(original.tci.sampleRate, loaded.tci.sampleRate);
  TEST_ASSERT_EQUAL_UINT8(original.tci.sampleFormat, loaded.tci.sampleFormat);
  TEST_ASSERT_EQUAL_UINT8(original.tci.channels, loaded.tci.channels);
  TEST_ASSERT_EQUAL_UINT16(original.tci.samples, loaded.tci.samples);
  TEST_ASSERT_EQUAL_UINT16(original.tci.bufferTimeoutMs,
                            loaded.tci.bufferTimeoutMs);
}

void test_tci_validation_and_sample_duration() {
  Config cfg;
  JsonDocument input;
  JsonObject tci = input["tci"].to<JsonObject>();
  tci["enabled"] = true;
  tci["serverIp"] = "10.0.0.25";
  tci["port"] = 50001;
  tci["receiver"] = 2;
  tci["markHz"] = 2125;
  tci["spaceHz"] = 2295;
  tci["sampleRate"] = 48000;
  tci["sampleFormat"] = 0;
  tci["channels"] = 1;
  tci["samples"] = 480;
  tci["bufferTimeoutMs"] = 50;
  JsonDocument errors;

  TEST_ASSERT_TRUE(configValidate(input.as<JsonVariantConst>(), cfg,
                                  errors.to<JsonObject>()));
  TEST_ASSERT_TRUE(cfg.tci.enabled);
  TEST_ASSERT_EQUAL_STRING("10.0.0.25", cfg.tci.serverIp);
  TEST_ASSERT_EQUAL_UINT16(50001, cfg.tci.port);
  TEST_ASSERT_EQUAL_UINT8(2, cfg.tci.receiver);

  tci["samples"] = 479;
  TEST_ASSERT_FALSE(configValidate(input.as<JsonVariantConst>(), cfg,
                                   errors.to<JsonObject>()));
  TEST_ASSERT_TRUE(errors["tci.samples"].is<const char *>());
}

void test_tci_validation_rejects_invalid_values() {
  Config cfg;
  JsonDocument input;
  JsonObject tci = input["tci"].to<JsonObject>();
  tci["serverIp"] = "radio.local";
  tci["port"] = 0;
  tci["receiver"] = 16;
  tci["markHz"] = 299;
  tci["spaceHz"] = 3000;
  tci["sampleRate"] = 44100;
  tci["sampleFormat"] = 4;
  tci["channels"] = 3;
  tci["samples"] = 2049;
  tci["bufferTimeoutMs"] = 501;
  JsonDocument errors;
  JsonObject errorObject = errors.to<JsonObject>();

  TEST_ASSERT_FALSE(configValidate(input.as<JsonVariantConst>(), cfg,
                                   errorObject));
  TEST_ASSERT_TRUE(errorObject["tci.serverIp"].is<const char *>());
  TEST_ASSERT_TRUE(errorObject["tci.port"].is<const char *>());
  TEST_ASSERT_TRUE(errorObject["tci.receiver"].is<const char *>());
  TEST_ASSERT_TRUE(errorObject["tci.markHz"].is<const char *>());
  TEST_ASSERT_TRUE(errorObject["tci.sampleRate"].is<const char *>());
  TEST_ASSERT_TRUE(errorObject["tci.sampleFormat"].is<const char *>());
  TEST_ASSERT_TRUE(errorObject["tci.channels"].is<const char *>());
  TEST_ASSERT_TRUE(errorObject["tci.samples"].is<const char *>());
  TEST_ASSERT_TRUE(errorObject["tci.bufferTimeoutMs"].is<const char *>());
}

void test_save_failure_is_reported() {
  LittleFS.allowWrite = false;
  Config cfg;
  TEST_ASSERT_FALSE(configSave(cfg));
}

void test_partial_write_failure_preserves_previous_config() {
  LittleFS.fileExists = true;
  LittleFS.contents = R"({"callsign":"OLD","cwSpeedWpm":20})";
  LittleFS.writeLimit = 5;
  Config replacement;
  strcpy(replacement.callsign, "NEW");

  TEST_ASSERT_FALSE(configSave(replacement));
  TEST_ASSERT_FALSE(LittleFS.tempExists);

  Config loaded;
  TEST_ASSERT_TRUE(configLoad(loaded));
  TEST_ASSERT_EQUAL_STRING("OLD", loaded.callsign);
}

void test_rename_failure_preserves_previous_config() {
  LittleFS.fileExists = true;
  LittleFS.contents = R"({"callsign":"OLD","cwSpeedWpm":20})";
  LittleFS.allowRename = false;
  Config replacement;
  strcpy(replacement.callsign, "NEW");

  TEST_ASSERT_FALSE(configSave(replacement));
  TEST_ASSERT_FALSE(LittleFS.tempExists);

  Config loaded;
  TEST_ASSERT_TRUE(configLoad(loaded));
  TEST_ASSERT_EQUAL_STRING("OLD", loaded.callsign);
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
  RUN_TEST(test_validation_rejects_present_fields_with_wrong_types);
  RUN_TEST(test_hostname_must_start_and_end_alphanumeric);
  RUN_TEST(test_save_writes_complete_round_trippable_json);
  RUN_TEST(test_tci_validation_and_sample_duration);
  RUN_TEST(test_tci_validation_rejects_invalid_values);
  RUN_TEST(test_save_failure_is_reported);
  RUN_TEST(test_partial_write_failure_preserves_previous_config);
  RUN_TEST(test_rename_failure_preserves_previous_config);
  return UNITY_END();
}
