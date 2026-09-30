#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <ETH.h>
#include <LittleFS.h>
#include <freertos/task.h>
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "ConfigStore.h"
#include "FskTimer.h"
#include "TxManager.h"
#include "WebConfigApi.h"
#include "WebInterface.h"

EspMock ESP;
ETHClass ETH;
LittleFSMock LittleFS;

namespace WebServerMock {
AsyncWebServer *server = nullptr;
AsyncWebSocket *socket = nullptr;
} // namespace WebServerMock

namespace TaskMock {
TaskFunction_t function = nullptr;
std::string name;
uint32_t stackDepth = 0;
UBaseType_t priority = 0;
BaseType_t core = -1;
size_t createCount = 0;
size_t delayCount = 0;
bool stopOnDelay = false;
} // namespace TaskMock

namespace {

enum class TxCallKind { KeyUp, Byte, BufferedEnd, Abort };

struct TxCall {
  TxCallKind kind;
  uint8_t byte;
  TxManager::Source source;
};

struct DependencyMock {
  Config config;
  TxManager::Status status;
  size_t freeSlots = 128;
  std::vector<TxCall> txCalls;
  size_t queueAttempt = 0;
  size_t failQueueAttempt = 0;
  bool endResult = true;
  bool abortResult = true;
  uint32_t underruns = 0;
  int configApiStatus = 200;
  bool configApiOk = true;
  size_t configApiCalls = 0;
  std::string configApiInput;

  bool acceptQueueAttempt() {
    queueAttempt++;
    return failQueueAttempt == 0 || queueAttempt != failQueueAttempt;
  }

  void reset() { *this = DependencyMock{}; }
};

DependencyMock mock;
uint32_t currentMillis = 0;

JsonDocument responseJson(const AsyncWebServerRequest &request) {
  JsonDocument doc;
  deserializeJson(doc, request.body);
  return doc;
}

AsyncWebServer &server() { return *WebServerMock::server; }

void runOnePushTaskIteration(uint32_t now) {
  currentMillis = now;
  TaskMock::stopOnDelay = true;
  try {
    TaskMock::function(nullptr);
  } catch (const TaskMock::StopTask &) {
  }
  TaskMock::stopOnDelay = false;
}

} // namespace

uint32_t millis() { return currentMillis; }

namespace ConfigStore {

Config get() { return mock.config; }

} // namespace ConfigStore

void configToJson(const Config &config, JsonObject out) {
  out["callsign"] = config.callsign;
  out["baudRate"] = config.baudRate;
}

namespace WebConfigApi {

int apply(JsonVariantConst input, JsonObject out) {
  mock.configApiCalls++;
  serializeJson(input, mock.configApiInput);
  out["ok"] = mock.configApiOk;
  out["transportTest"] = true;
  return mock.configApiStatus;
}

} // namespace WebConfigApi

namespace TxManager {

Status getStatus() { return mock.status; }
size_t commandQueueFreeSlots() { return mock.freeSlots; }

bool enqueueKeyUp(Source source) {
  mock.txCalls.push_back({TxCallKind::KeyUp, 0, source});
  return mock.acceptQueueAttempt();
}

bool enqueueByte(uint8_t byte, Source source) {
  mock.txCalls.push_back({TxCallKind::Byte, byte, source});
  return mock.acceptQueueAttempt();
}

bool enqueueBufferedEnd() {
  mock.txCalls.push_back({TxCallKind::BufferedEnd, 0, Source::Web});
  mock.queueAttempt++;
  if (mock.failQueueAttempt != 0 && mock.queueAttempt == mock.failQueueAttempt) {
    return false;
  }
  return mock.endResult;
}

bool enqueueAbort() {
  mock.txCalls.push_back({TxCallKind::Abort, 0, Source::Web});
  return mock.abortResult;
}

} // namespace TxManager

namespace FskTimer {

uint32_t underrunCount() { return mock.underruns; }

} // namespace FskTimer

void setUp() {
  mock.reset();
  currentMillis = 0;
  ESP.flashChipSize = 4u * 1024u * 1024u;
  ESP.freeHeap = 123456;
  ETH.mac = "AA:BB:CC:DD:EE:FF";
  ETH.ip.value = "192.168.1.50";
  ETH.linked = true;
}

void tearDown() {}

void test_begin_registers_complete_http_surface() {
  TEST_ASSERT_NOT_NULL(WebServerMock::server);
  TEST_ASSERT_NOT_NULL(WebServerMock::socket);
  TEST_ASSERT_EQUAL_UINT16(80, server().port);
  TEST_ASSERT_TRUE(server().begun);
  TEST_ASSERT_EQUAL_UINT32(6, server().routes.size());
  TEST_ASSERT_EQUAL_UINT32(3, server().jsonHandlers.size());
  TEST_ASSERT_EQUAL_STRING("/ws", WebServerMock::socket->path.c_str());
  TEST_ASSERT_EQUAL_STRING("/", server().staticHandler.uri.c_str());
  TEST_ASSERT_EQUAL_STRING("/", server().staticHandler.filesystemPath.c_str());
  TEST_ASSERT_EQUAL_STRING("index.html", server().staticHandler.defaultFile.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, TaskMock::createCount);
  TEST_ASSERT_NOT_NULL(TaskMock::function);
  TEST_ASSERT_EQUAL_STRING("WebPush", TaskMock::name.c_str());
  TEST_ASSERT_EQUAL_UINT32(4096, TaskMock::stackDepth);
  TEST_ASSERT_EQUAL_UINT32(1, TaskMock::priority);
  TEST_ASSERT_EQUAL_INT(0, TaskMock::core);
}

void test_get_config_returns_serialized_live_config() {
  std::strcpy(mock.config.callsign, "OK2ZAW");
  mock.config.baudRate = 50.0f;
  AsyncWebServerRequest request;

  TEST_ASSERT_TRUE(server().dispatch("/api/config", HTTP_GET, request));

  TEST_ASSERT_TRUE(request.sent);
  TEST_ASSERT_EQUAL_INT(200, request.status);
  TEST_ASSERT_EQUAL_STRING("application/json", request.contentType.c_str());
  JsonDocument body = responseJson(request);
  TEST_ASSERT_EQUAL_STRING("OK2ZAW", body["callsign"].as<const char *>());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, body["baudRate"].as<float>());
}

void test_config_backup_adds_download_header() {
  std::strcpy(mock.config.callsign, "BACKUP");
  AsyncWebServerRequest request;

  TEST_ASSERT_TRUE(server().dispatch("/api/config/backup", HTTP_GET, request));

  TEST_ASSERT_EQUAL_INT(200, request.status);
  TEST_ASSERT_EQUAL_STRING(
      "attachment; filename=\"easyfsk-config.json\"",
      request.headers["Content-Disposition"].c_str());
  JsonDocument body = responseJson(request);
  TEST_ASSERT_EQUAL_STRING("BACKUP", body["callsign"].as<const char *>());
}

void test_config_and_restore_posts_share_controller_and_transport() {
  mock.configApiStatus = 422;
  mock.configApiOk = false;
  JsonDocument input;
  input["callsign"] = "NEW";
  JsonVariant json = input.as<JsonVariant>();
  AsyncWebServerRequest configRequest;

  TEST_ASSERT_TRUE(server().dispatchJson("/api/config", configRequest, json));
  TEST_ASSERT_EQUAL_INT(422, configRequest.status);
  TEST_ASSERT_EQUAL_STRING("{\"callsign\":\"NEW\"}", mock.configApiInput.c_str());
  JsonDocument configBody = responseJson(configRequest);
  TEST_ASSERT_FALSE(configBody["ok"].as<bool>());
  TEST_ASSERT_TRUE(configBody["transportTest"].as<bool>());

  AsyncWebServerRequest restoreRequest;
  TEST_ASSERT_TRUE(server().dispatchJson("/api/config/restore", restoreRequest, json));
  TEST_ASSERT_EQUAL_INT(422, restoreRequest.status);
  TEST_ASSERT_EQUAL_UINT32(2, mock.configApiCalls);
}

void test_status_route_serializes_all_sources_and_runtime_fields() {
  struct SourceCase {
    TxManager::Source source;
    const char *name;
  } cases[] = {
      {TxManager::Source::SerialLink, "serial"},
      {TxManager::Source::Uart2Fsk, "uart2fsk"},
      {TxManager::Source::Rts, "rts"},
      {TxManager::Source::Web, "web"},
      {TxManager::Source::Winkey, "winkey"},
  };
  std::strcpy(mock.config.callsign, "OK2ZAW");
  mock.status.txActive = true;
  mock.status.pttActive = true;
  mock.status.paActive = false;
  mock.status.inhibited = true;
  mock.status.lastChar = 'K';
  mock.status.bufferPending = 17;

  for (const SourceCase &item : cases) {
    mock.status.pttSource = item.source;
    AsyncWebServerRequest request;
    TEST_ASSERT_TRUE(server().dispatch("/api/status", HTTP_GET, request));
    JsonDocument body = responseJson(request);
    TEST_ASSERT_EQUAL_STRING("status", body["type"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("tx", body["state"].as<const char *>());
    TEST_ASSERT_TRUE(body["pttActive"].as<bool>());
    TEST_ASSERT_FALSE(body["paActive"].as<bool>());
    TEST_ASSERT_TRUE(body["inhibited"].as<bool>());
    TEST_ASSERT_EQUAL_STRING(item.name, body["pttSource"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("OK2ZAW", body["callsign"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("K", body["char"].as<const char *>());
    TEST_ASSERT_EQUAL_UINT16(17, body["bufferPending"].as<uint16_t>());
  }
}

void test_idle_status_uses_empty_character() {
  mock.status.lastChar = 0;
  AsyncWebServerRequest request;

  TEST_ASSERT_TRUE(server().dispatch("/api/status", HTTP_GET, request));
  JsonDocument body = responseJson(request);
  TEST_ASSERT_EQUAL_STRING("idle", body["state"].as<const char *>());
  TEST_ASSERT_EQUAL_STRING("", body["char"].as<const char *>());
}

void test_system_route_reports_firmware_network_and_diagnostics() {
  mock.underruns = 9;
  AsyncWebServerRequest request;

  TEST_ASSERT_TRUE(server().dispatch("/api/system", HTTP_GET, request));

  JsonDocument body = responseJson(request);
  TEST_ASSERT_EQUAL_STRING("EasyFSK-PLUS32 0.1.0-dev",
                           body["firmware"].as<const char *>());
  TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", body["mac"].as<const char *>());
  TEST_ASSERT_EQUAL_STRING("192.168.1.50", body["ip"].as<const char *>());
  TEST_ASSERT_TRUE(body["linkUp"].as<bool>());
  TEST_ASSERT_EQUAL_UINT32(4u * 1024u * 1024u, body["flashSize"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(123456, body["heapFree"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(9, body["fskUnderruns"].as<uint32_t>());
}

void test_system_route_tracks_link_loss_and_recovery() {
  ETH.linked = false;
  AsyncWebServerRequest lost;
  TEST_ASSERT_TRUE(server().dispatch("/api/system", HTTP_GET, lost));
  JsonDocument lostBody = responseJson(lost);
  TEST_ASSERT_FALSE(lostBody["linkUp"].as<bool>());

  ETH.linked = true;
  AsyncWebServerRequest recovered;
  TEST_ASSERT_TRUE(server().dispatch("/api/system", HTTP_GET, recovered));
  JsonDocument recoveredBody = responseJson(recovered);
  TEST_ASSERT_TRUE(recoveredBody["linkUp"].as<bool>());
}

void test_heartbeat_tracks_link_loss_and_recovery() {
  mock.status.revision = 42;
  WebServerMock::socket->sentBodies.clear();
  WebServerMock::socket->cleanupCount = 0;

  ETH.linked = false;
  runOnePushTaskIteration(1000);
  TEST_ASSERT_EQUAL_UINT32(2, WebServerMock::socket->sentBodies.size());
  JsonDocument lostHeartbeat;
  deserializeJson(lostHeartbeat, WebServerMock::socket->sentBodies[1]);
  TEST_ASSERT_EQUAL_STRING("heartbeat", lostHeartbeat["type"].as<const char *>());
  TEST_ASSERT_FALSE(lostHeartbeat["linkUp"].as<bool>());

  WebServerMock::socket->sentBodies.clear();
  ETH.linked = true;
  runOnePushTaskIteration(2000);
  TEST_ASSERT_EQUAL_UINT32(2, WebServerMock::socket->sentBodies.size());
  JsonDocument recoveredHeartbeat;
  deserializeJson(recoveredHeartbeat, WebServerMock::socket->sentBodies[1]);
  TEST_ASSERT_TRUE(recoveredHeartbeat["linkUp"].as<bool>());
  TEST_ASSERT_EQUAL_UINT32(2, WebServerMock::socket->cleanupCount);
  TEST_ASSERT_EQUAL_UINT32(2, TaskMock::delayCount);
}

void test_idle_send_queues_complete_web_session() {
  JsonDocument input;
  input["text"] = "AB";
  JsonVariant json = input.as<JsonVariant>();
  AsyncWebServerRequest request;

  TEST_ASSERT_TRUE(server().dispatchJson("/api/tx/send", request, json));

  TEST_ASSERT_EQUAL_INT(200, request.status);
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", request.body.c_str());
  TEST_ASSERT_EQUAL_UINT32(4, mock.txCalls.size());
  TEST_ASSERT_EQUAL(TxCallKind::KeyUp, mock.txCalls[0].kind);
  TEST_ASSERT_EQUAL(TxManager::Source::Web, mock.txCalls[0].source);
  TEST_ASSERT_EQUAL(TxCallKind::Byte, mock.txCalls[1].kind);
  TEST_ASSERT_EQUAL_UINT8('A', mock.txCalls[1].byte);
  TEST_ASSERT_EQUAL_UINT8('B', mock.txCalls[2].byte);
  TEST_ASSERT_EQUAL(TxCallKind::BufferedEnd, mock.txCalls[3].kind);
}

void test_send_appends_only_bytes_to_active_rtty_session() {
  mock.status.txActive = true;
  mock.status.pttSource = TxManager::Source::SerialLink;
  JsonDocument input;
  input["text"] = "AB";
  JsonVariant json = input.as<JsonVariant>();
  AsyncWebServerRequest request;

  TEST_ASSERT_TRUE(server().dispatchJson("/api/tx/send", request, json));

  TEST_ASSERT_EQUAL_INT(200, request.status);
  TEST_ASSERT_EQUAL_UINT32(2, mock.txCalls.size());
  TEST_ASSERT_EQUAL(TxCallKind::Byte, mock.txCalls[0].kind);
  TEST_ASSERT_EQUAL_UINT8('A', mock.txCalls[0].byte);
  TEST_ASSERT_EQUAL_UINT8('B', mock.txCalls[1].byte);
}

void test_send_preflight_rejections_do_not_touch_queue() {
  JsonDocument input;
  input["text"] = "AB";
  JsonVariant json = input.as<JsonVariant>();

  mock.status.inhibited = true;
  AsyncWebServerRequest inhibited;
  TEST_ASSERT_TRUE(server().dispatchJson("/api/tx/send", inhibited, json));
  TEST_ASSERT_EQUAL_INT(200, inhibited.status);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, inhibited.body.find("inhibited"));
  TEST_ASSERT_TRUE(mock.txCalls.empty());

  mock.status.inhibited = false;
  std::string oversized(121, 'X');
  input["text"] = oversized.c_str();
  AsyncWebServerRequest tooLong;
  TEST_ASSERT_TRUE(server().dispatchJson("/api/tx/send", tooLong, json));
  TEST_ASSERT_EQUAL_INT(400, tooLong.status);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, tooLong.body.find("text_too_long"));
  TEST_ASSERT_TRUE(mock.txCalls.empty());

  input["text"] = "AB";
  mock.freeSlots = 3;
  AsyncWebServerRequest full;
  TEST_ASSERT_TRUE(server().dispatchJson("/api/tx/send", full, json));
  TEST_ASSERT_EQUAL_INT(503, full.status);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, full.body.find("tx_queue_full"));
  TEST_ASSERT_TRUE(mock.txCalls.empty());
}

void test_mid_send_queue_failure_aborts_without_queuing_remainder() {
  mock.failQueueAttempt = 3; // KeyUp and A succeed; B fails.
  JsonDocument input;
  input["text"] = "ABC";
  JsonVariant json = input.as<JsonVariant>();
  AsyncWebServerRequest request;

  TEST_ASSERT_TRUE(server().dispatchJson("/api/tx/send", request, json));

  TEST_ASSERT_EQUAL_INT(503, request.status);
  TEST_ASSERT_EQUAL_UINT32(4, mock.txCalls.size());
  TEST_ASSERT_EQUAL(TxCallKind::KeyUp, mock.txCalls[0].kind);
  TEST_ASSERT_EQUAL_UINT8('A', mock.txCalls[1].byte);
  TEST_ASSERT_EQUAL_UINT8('B', mock.txCalls[2].byte);
  TEST_ASSERT_EQUAL(TxCallKind::Abort, mock.txCalls[3].kind);
}

void test_end_and_abort_routes_report_queue_results() {
  AsyncWebServerRequest endOk;
  TEST_ASSERT_TRUE(server().dispatch("/api/tx/end", HTTP_POST, endOk));
  TEST_ASSERT_EQUAL_INT(200, endOk.status);
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", endOk.body.c_str());

  mock.endResult = false;
  AsyncWebServerRequest endFailed;
  TEST_ASSERT_TRUE(server().dispatch("/api/tx/end", HTTP_POST, endFailed));
  TEST_ASSERT_EQUAL_INT(503, endFailed.status);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, endFailed.body.find("tx_queue_full_aborted"));

  mock.abortResult = true;
  AsyncWebServerRequest abortOk;
  TEST_ASSERT_TRUE(server().dispatch("/api/tx/abort", HTTP_POST, abortOk));
  TEST_ASSERT_EQUAL_INT(200, abortOk.status);

  mock.abortResult = false;
  AsyncWebServerRequest abortFailed;
  TEST_ASSERT_TRUE(server().dispatch("/api/tx/abort", HTTP_POST, abortFailed));
  TEST_ASSERT_EQUAL_INT(503, abortFailed.status);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, abortFailed.body.find("tx_not_ready"));
}

int main(int, char **) {
  WebInterface::begin();
  UNITY_BEGIN();
  RUN_TEST(test_begin_registers_complete_http_surface);
  RUN_TEST(test_get_config_returns_serialized_live_config);
  RUN_TEST(test_config_backup_adds_download_header);
  RUN_TEST(test_config_and_restore_posts_share_controller_and_transport);
  RUN_TEST(test_status_route_serializes_all_sources_and_runtime_fields);
  RUN_TEST(test_idle_status_uses_empty_character);
  RUN_TEST(test_system_route_reports_firmware_network_and_diagnostics);
  RUN_TEST(test_system_route_tracks_link_loss_and_recovery);
  RUN_TEST(test_heartbeat_tracks_link_loss_and_recovery);
  RUN_TEST(test_idle_send_queues_complete_web_session);
  RUN_TEST(test_send_appends_only_bytes_to_active_rtty_session);
  RUN_TEST(test_send_preflight_rejections_do_not_touch_queue);
  RUN_TEST(test_mid_send_queue_failure_aborts_without_queuing_remainder);
  RUN_TEST(test_end_and_abort_routes_report_queue_results);
  return UNITY_END();
}
