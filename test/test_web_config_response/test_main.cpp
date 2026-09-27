#include <unity.h>

#include <ArduinoJson.h>
#include <Arduino.h>

#include "WebConfigResponse.h"

SerialMock Serial;

namespace {

JsonObject errorsFrom(JsonDocument &doc) {
  return doc.to<JsonObject>();
}

JsonObject buildResponse(WebConfigResponse::Kind kind,
                         JsonObjectConst errors,
                         JsonDocument &response) {
  JsonObject body = response.to<JsonObject>();
  WebConfigResponse::buildBody(kind, errors, body);
  return body;
}

void test_active_tx_is_a_successful_deferred_response() {
  JsonDocument errorDoc;
  JsonObject errors = errorsFrom(errorDoc);
  errors["_"] = "configuration cannot change during transmission";

  WebConfigResponse::Kind kind = WebConfigResponse::classify(errors);
  JsonDocument responseDoc;
  JsonObject response = buildResponse(kind, errors, responseDoc);

  TEST_ASSERT_EQUAL_INT(200, WebConfigResponse::statusCode(kind));
  TEST_ASSERT_FALSE(response["ok"].as<bool>());
  TEST_ASSERT_TRUE(response["deferred"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("configuration cannot change during transmission",
                           response["message"].as<const char *>());
  TEST_ASSERT_FALSE(response["reason"].is<const char *>());
  TEST_ASSERT_FALSE(response["errors"].is<JsonObjectConst>());
}

void test_storage_failure_is_an_internal_server_error() {
  JsonDocument errorDoc;
  JsonObject errors = errorsFrom(errorDoc);
  errors["storage"] = "failed to persist configuration";

  WebConfigResponse::Kind kind = WebConfigResponse::classify(errors);
  JsonDocument responseDoc;
  JsonObject response = buildResponse(kind, errors, responseDoc);

  TEST_ASSERT_EQUAL_INT(500, WebConfigResponse::statusCode(kind));
  TEST_ASSERT_FALSE(response["ok"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("storage_error", response["reason"].as<const char *>());
  TEST_ASSERT_EQUAL_STRING("failed to persist configuration",
                           response["message"].as<const char *>());
  TEST_ASSERT_FALSE(response["deferred"].is<bool>());
  TEST_ASSERT_FALSE(response["errors"].is<JsonObjectConst>());
}

void test_validation_failure_preserves_per_field_errors() {
  JsonDocument errorDoc;
  JsonObject errors = errorsFrom(errorDoc);
  errors["cwSpeedWpm"] = "must be from 5 to 99";
  errors["network.hostname"] = "invalid hostname";

  WebConfigResponse::Kind kind = WebConfigResponse::classify(errors);
  JsonDocument responseDoc;
  JsonObject response = buildResponse(kind, errors, responseDoc);

  TEST_ASSERT_EQUAL_INT(400, WebConfigResponse::statusCode(kind));
  TEST_ASSERT_FALSE(response["ok"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("must be from 5 to 99",
                           response["errors"]["cwSpeedWpm"].as<const char *>());
  TEST_ASSERT_EQUAL_STRING("invalid hostname",
                           response["errors"]["network.hostname"].as<const char *>());
  TEST_ASSERT_FALSE(response["deferred"].is<bool>());
  TEST_ASSERT_FALSE(response["reason"].is<const char *>());
}

void test_storage_failure_has_priority_over_other_categories() {
  JsonDocument errorDoc;
  JsonObject errors = errorsFrom(errorDoc);
  errors["_"] = "transmission active";
  errors["storage"] = "write failed";

  WebConfigResponse::Kind kind = WebConfigResponse::classify(errors);

  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(WebConfigResponse::Kind::StorageError),
      static_cast<int>(kind));
  TEST_ASSERT_EQUAL_INT(500, WebConfigResponse::statusCode(kind));
}

} // namespace

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_active_tx_is_a_successful_deferred_response);
  RUN_TEST(test_storage_failure_is_an_internal_server_error);
  RUN_TEST(test_validation_failure_preserves_per_field_errors);
  RUN_TEST(test_storage_failure_has_priority_over_other_categories);
  return UNITY_END();
}
