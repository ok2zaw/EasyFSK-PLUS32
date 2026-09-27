#include <unity.h>

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstring>

#include "ConfigStore.h"
#include "FskTimer.h"
#include "ModeSelect.h"
#include "TxManager.h"
#include "WebConfigApi.h"
#include "WinkeyEmulator.h"

LittleFSMock LittleFS;

namespace {

struct DependencyMock {
  TxManager::Status status;
  int txApplyCount = 0;
  int fskReconfigureCount = 0;
  int winkeyApplyCount = 0;
  int modeSelectCount = 0;
  Config txConfig;
  Config winkeyConfig;
  float baudRate = 0;
  bool markHigh = false;
  bool cwMode = false;

  void reset() { *this = DependencyMock{}; }
};

DependencyMock mock;

JsonObject errorsFrom(JsonDocument &doc) { return doc.to<JsonObject>(); }

} // namespace

namespace TxManager {

Status getStatus() { return mock.status; }

void applyConfig(const Config &cfg) {
  mock.txApplyCount++;
  mock.txConfig = cfg;
}

} // namespace TxManager

namespace FskTimer {

void reconfigure(float baudRate, bool markHigh) {
  mock.fskReconfigureCount++;
  mock.baudRate = baudRate;
  mock.markHigh = markHigh;
}

} // namespace FskTimer

namespace WinkeyEmulator {

void applyConfig(const Config &cfg) {
  mock.winkeyApplyCount++;
  mock.winkeyConfig = cfg;
}

} // namespace WinkeyEmulator

namespace ModeSelect {

void setActive(bool cwActive) {
  mock.modeSelectCount++;
  mock.cwMode = cwActive;
}

} // namespace ModeSelect

void setUp() {
  LittleFS.reset();
  mock.reset();
  ConfigStore::begin();
}

void tearDown() {}

void test_begin_loads_defaults_when_file_is_missing() {
  Config cfg = ConfigStore::get();
  TEST_ASSERT_EQUAL_STRING("", cfg.callsign);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 45.45f, cfg.baudRate);
  TEST_ASSERT_EQUAL_UINT8(20, cfg.cwSpeedWpm);
}

void test_idle_valid_update_is_saved_then_applied_everywhere() {
  JsonDocument input;
  input["callsign"] = "OK2ZAW";
  input["baudRate"] = 50;
  input["polarity"] = "markLow";
  input["uart2Mode"] = "cw";
  input["cwSpeedWpm"] = 32;
  JsonDocument errorDoc;

  TEST_ASSERT_TRUE(ConfigStore::applyAndSave(
      input.as<JsonVariantConst>(), errorsFrom(errorDoc)));

  Config current = ConfigStore::get();
  TEST_ASSERT_EQUAL_STRING("OK2ZAW", current.callsign);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, current.baudRate);
  TEST_ASSERT_FALSE(current.markHigh);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Uart2Mode::Cw),
                          static_cast<uint8_t>(current.uart2Mode));
  TEST_ASSERT_EQUAL_UINT8(32, current.cwSpeedWpm);
  TEST_ASSERT_TRUE(LittleFS.fileExists);

  TEST_ASSERT_EQUAL_INT(1, mock.txApplyCount);
  TEST_ASSERT_EQUAL_INT(1, mock.fskReconfigureCount);
  TEST_ASSERT_EQUAL_INT(1, mock.winkeyApplyCount);
  TEST_ASSERT_EQUAL_INT(1, mock.modeSelectCount);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, mock.baudRate);
  TEST_ASSERT_FALSE(mock.markHigh);
  TEST_ASSERT_TRUE(mock.cwMode);
  TEST_ASSERT_EQUAL_UINT8(32, mock.txConfig.cwSpeedWpm);
  TEST_ASSERT_EQUAL_UINT8(32, mock.winkeyConfig.cwSpeedWpm);

  Config persisted;
  TEST_ASSERT_TRUE(configLoad(persisted));
  TEST_ASSERT_EQUAL_STRING("OK2ZAW", persisted.callsign);
  TEST_ASSERT_EQUAL_UINT8(32, persisted.cwSpeedWpm);
}

void test_active_tx_rejects_update_without_save_or_live_change() {
  mock.status.txActive = true;
  JsonDocument input;
  input["callsign"] = "BUSY";
  JsonDocument errorDoc;
  JsonObject errors = errorsFrom(errorDoc);

  TEST_ASSERT_FALSE(
      ConfigStore::applyAndSave(input.as<JsonVariantConst>(), errors));
  TEST_ASSERT_TRUE(errors["_"].is<const char *>());
  TEST_ASSERT_EQUAL_STRING("", ConfigStore::get().callsign);
  TEST_ASSERT_FALSE(LittleFS.fileExists);
  TEST_ASSERT_EQUAL_INT(0, mock.txApplyCount);
  TEST_ASSERT_EQUAL_INT(0, mock.fskReconfigureCount);
  TEST_ASSERT_EQUAL_INT(0, mock.winkeyApplyCount);
  TEST_ASSERT_EQUAL_INT(0, mock.modeSelectCount);
}

void test_invalid_update_is_rejected_before_tx_or_storage_checks() {
  mock.status.txActive = true;
  JsonDocument input;
  input["cwSpeedWpm"] = 100;
  JsonDocument errorDoc;
  JsonObject errors = errorsFrom(errorDoc);

  TEST_ASSERT_FALSE(
      ConfigStore::applyAndSave(input.as<JsonVariantConst>(), errors));
  TEST_ASSERT_TRUE(errors["cwSpeedWpm"].is<const char *>());
  TEST_ASSERT_FALSE(errors["_"].is<const char *>());
  TEST_ASSERT_FALSE(LittleFS.fileExists);
  TEST_ASSERT_EQUAL_INT(0, mock.txApplyCount);
}

void test_storage_failure_does_not_change_or_apply_live_config() {
  LittleFS.allowWrite = false;
  JsonDocument input;
  input["callsign"] = "NEW";
  input["cwSpeedWpm"] = 40;
  JsonDocument errorDoc;
  JsonObject errors = errorsFrom(errorDoc);

  TEST_ASSERT_FALSE(
      ConfigStore::applyAndSave(input.as<JsonVariantConst>(), errors));
  TEST_ASSERT_TRUE(errors["storage"].is<const char *>());
  Config current = ConfigStore::get();
  TEST_ASSERT_EQUAL_STRING("", current.callsign);
  TEST_ASSERT_EQUAL_UINT8(20, current.cwSpeedWpm);
  TEST_ASSERT_EQUAL_INT(0, mock.txApplyCount);
  TEST_ASSERT_EQUAL_INT(0, mock.fskReconfigureCount);
  TEST_ASSERT_EQUAL_INT(0, mock.winkeyApplyCount);
  TEST_ASSERT_EQUAL_INT(0, mock.modeSelectCount);
}

void test_web_api_success_returns_saved_live_configuration() {
  JsonDocument input;
  input["callsign"] = "OK2ZAW";
  input["uart2Mode"] = "cw";
  input["cwSpeedWpm"] = 28;
  JsonDocument responseDoc;

  int status = WebConfigApi::apply(
      input.as<JsonVariantConst>(), responseDoc.to<JsonObject>());

  TEST_ASSERT_EQUAL_INT(200, status);
  TEST_ASSERT_TRUE(responseDoc["ok"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("OK2ZAW",
                           responseDoc["config"]["callsign"].as<const char *>());
  TEST_ASSERT_EQUAL_STRING("cw",
                           responseDoc["config"]["uart2Mode"].as<const char *>());
  TEST_ASSERT_EQUAL_UINT8(28, responseDoc["config"]["cwSpeedWpm"].as<uint8_t>());
  TEST_ASSERT_TRUE(LittleFS.fileExists);
  TEST_ASSERT_EQUAL_INT(1, mock.txApplyCount);
}

void test_web_api_validation_error_returns_field_errors_without_save() {
  JsonDocument input;
  input["cwSpeedWpm"] = 100;
  JsonDocument responseDoc;

  int status = WebConfigApi::apply(
      input.as<JsonVariantConst>(), responseDoc.to<JsonObject>());

  TEST_ASSERT_EQUAL_INT(400, status);
  TEST_ASSERT_FALSE(responseDoc["ok"].as<bool>());
  TEST_ASSERT_TRUE(responseDoc["errors"]["cwSpeedWpm"].is<const char *>());
  TEST_ASSERT_FALSE(LittleFS.fileExists);
  TEST_ASSERT_EQUAL_INT(0, mock.txApplyCount);
}

void test_web_api_active_tx_returns_deferred_response() {
  mock.status.txActive = true;
  JsonDocument input;
  input["callsign"] = "BUSY";
  JsonDocument responseDoc;

  int status = WebConfigApi::apply(
      input.as<JsonVariantConst>(), responseDoc.to<JsonObject>());

  TEST_ASSERT_EQUAL_INT(200, status);
  TEST_ASSERT_FALSE(responseDoc["ok"].as<bool>());
  TEST_ASSERT_TRUE(responseDoc["deferred"].as<bool>());
  TEST_ASSERT_TRUE(responseDoc["message"].is<const char *>());
  TEST_ASSERT_FALSE(LittleFS.fileExists);
  TEST_ASSERT_EQUAL_INT(0, mock.txApplyCount);
}

void test_web_api_storage_failure_returns_500_without_live_change() {
  LittleFS.allowWrite = false;
  JsonDocument input;
  input["callsign"] = "NEW";
  JsonDocument responseDoc;

  int status = WebConfigApi::apply(
      input.as<JsonVariantConst>(), responseDoc.to<JsonObject>());

  TEST_ASSERT_EQUAL_INT(500, status);
  TEST_ASSERT_FALSE(responseDoc["ok"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("storage_error",
                           responseDoc["reason"].as<const char *>());
  TEST_ASSERT_EQUAL_STRING("", ConfigStore::get().callsign);
  TEST_ASSERT_EQUAL_INT(0, mock.txApplyCount);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_begin_loads_defaults_when_file_is_missing);
  RUN_TEST(test_idle_valid_update_is_saved_then_applied_everywhere);
  RUN_TEST(test_active_tx_rejects_update_without_save_or_live_change);
  RUN_TEST(test_invalid_update_is_rejected_before_tx_or_storage_checks);
  RUN_TEST(test_storage_failure_does_not_change_or_apply_live_config);
  RUN_TEST(test_web_api_success_returns_saved_live_configuration);
  RUN_TEST(test_web_api_validation_error_returns_field_errors_without_save);
  RUN_TEST(test_web_api_active_tx_returns_deferred_response);
  RUN_TEST(test_web_api_storage_failure_returns_500_without_live_change);
  return UNITY_END();
}
