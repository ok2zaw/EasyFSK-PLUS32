#include "WebInterface.h"
#include "TxManager.h"
#include "ConfigStore.h"
#include "FskTimer.h"
#include "Version.h"
#include "WebConfigResponse.h"
#include "WebTxPolicy.h"

#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ETH.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

namespace WebInterface {

namespace {

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

const char *sourceToString(TxManager::Source src) {
  switch (src) {
    case TxManager::Source::Rts: return "rts";
    case TxManager::Source::Web: return "web";
    case TxManager::Source::Uart2Fsk: return "uart2fsk"; // added 2026-09-27, UART1/UART2 split
    case TxManager::Source::Winkey: return "winkey";     // added 2026-09-27, CW/Winkey keying
    default: return "serial"; // Source::SerialLink (UART1)
  }
}

void buildStatusJson(JsonObject obj) {
  TxManager::Status st = TxManager::getStatus();
  Config cfg = ConfigStore::get();
  obj["type"] = "status";
  obj["state"] = st.txActive ? "tx" : "idle";
  obj["pttActive"] = st.pttActive;
  obj["paActive"] = st.paActive;
  obj["pttSource"] = sourceToString(st.pttSource);
  obj["inhibited"] = st.inhibited;
  obj["callsign"] = cfg.callsign;
  char ch[2] = {st.lastChar, '\0'};
  obj["char"] = st.lastChar != 0 ? String(ch) : String("");
  obj["bufferPending"] = st.bufferPending;
}

void buildErrorsResponse(JsonObject errors, AsyncWebServerRequest *request) {
  WebConfigResponse::Kind kind = WebConfigResponse::classify(errors);
  JsonDocument doc;
  WebConfigResponse::buildBody(kind, errors, doc.to<JsonObject>());
  String body;
  serializeJson(doc, body);
  request->send(WebConfigResponse::statusCode(kind), "application/json", body);
}

void handleGetConfig(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  configToJson(ConfigStore::get(), root);
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void handlePostConfig(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonDocument errDoc;
  JsonObject errs = errDoc.to<JsonObject>();
  if (ConfigStore::applyAndSave(json, errs)) {
    JsonDocument doc;
    doc["ok"] = true;
    JsonObject cfgObj = doc["config"].to<JsonObject>();
    configToJson(ConfigStore::get(), cfgObj);
    String body;
    serializeJson(doc, body);
    request->send(200, "application/json", body);
  } else {
    buildErrorsResponse(errs, request);
  }
}

void handleConfigBackup(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  configToJson(ConfigStore::get(), root);
  String body;
  serializeJson(doc, body);
  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", body);
  response->addHeader("Content-Disposition", "attachment; filename=\"easyfsk-config.json\"");
  request->send(response);
}

void handleGetStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  buildStatusJson(root);
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void handleGetSystem(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["firmware"] = FW_VERSION;
  root["mac"] = ETH.macAddress();
  root["ip"] = ETH.localIP().toString();
  root["linkUp"] = ETH.linkUp();
  root["flashSize"] = ESP.getFlashChipSize();
  root["heapFree"] = ESP.getFreeHeap();
  root["fskUnderruns"] = FskTimer::underrunCount();
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void handleTxSend(AsyncWebServerRequest *request, JsonVariant &json) {
  TxManager::Status st = TxManager::getStatus();
  const char *text = json["text"] | "";
  size_t textLen = strlen(text);
  WebTxPolicy::Plan plan = WebTxPolicy::planSend(
      st.inhibited, st.txActive, textLen, TxManager::commandQueueFreeSlots());
  if (plan.decision == WebTxPolicy::Decision::Inhibited) {
    request->send(200, "application/json", "{\"ok\":false,\"reason\":\"inhibited\"}");
    return;
  }
  if (plan.decision == WebTxPolicy::Decision::TextTooLong) {
    request->send(400, "application/json", "{\"ok\":false,\"reason\":\"text_too_long\",\"maxLength\":120}");
    return;
  }
  if (plan.decision == WebTxPolicy::Decision::QueueFull) {
    request->send(503, "application/json", "{\"ok\":false,\"reason\":\"tx_queue_full\"}");
    return;
  }

  bool queued = !plan.startSession || TxManager::enqueueKeyUp(TxManager::Source::Web);
  for (const char *p = text; *p != '\0'; p++) {
    queued = queued && TxManager::enqueueByte(static_cast<uint8_t>(*p), TxManager::Source::Web);
  }
  if (plan.endSession) {
    queued = queued && TxManager::enqueueBufferedEnd(); // idle Send behaves like a self-contained [text]
  }
  if (!queued) {
    TxManager::enqueueAbort();
    request->send(503, "application/json", "{\"ok\":false,\"reason\":\"tx_queue_full\"}");
    return;
  }
  request->send(200, "application/json", "{\"ok\":true}");
}

void handleTxEnd(AsyncWebServerRequest *request) {
  bool queued = TxManager::enqueueBufferedEnd();
  request->send(queued ? 200 : 503, "application/json",
                queued ? "{\"ok\":true}" : "{\"ok\":false,\"reason\":\"tx_queue_full_aborted\"}");
}

void handleTxAbort(AsyncWebServerRequest *request) {
  bool queued = TxManager::enqueueAbort();
  request->send(queued ? 200 : 503, "application/json",
                queued ? "{\"ok\":true}" : "{\"ok\":false,\"reason\":\"tx_not_ready\"}");
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len) {
  // Server -> client push only, per the design doc: incoming client
  // messages (if any ever arrive) are deliberately ignored -- TX actions
  // always go through the REST endpoints above instead, so every action
  // gets a definite HTTP success/failure response.
  (void)server;
  (void)client;
  (void)type;
  (void)arg;
  (void)data;
  (void)len;
}

void pushTask(void *) {
  uint32_t lastRevision = 0xFFFFFFFF; // force an initial push
  uint32_t lastHeartbeatMs = 0;

  for (;;) {
    TxManager::Status st = TxManager::getStatus();
    if (st.revision != lastRevision) {
      lastRevision = st.revision;
      JsonDocument doc;
      JsonObject root = doc.to<JsonObject>();
      buildStatusJson(root);
      String body;
      serializeJson(doc, body);
      ws.textAll(body);
    }

    uint32_t now = millis();
    if (now - lastHeartbeatMs >= 1000) {
      lastHeartbeatMs = now;
      JsonDocument doc;
      doc["type"] = "heartbeat";
      doc["uptimeMs"] = now;
      doc["heapFree"] = ESP.getFreeHeap();
      doc["linkUp"] = ETH.linkUp();
      String body;
      serializeJson(doc, body);
      ws.textAll(body);
      ws.cleanupClients();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

} // namespace

void begin() {
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/config", handlePostConfig));
  server.on("/api/config/backup", HTTP_GET, handleConfigBackup);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/config/restore", handlePostConfig));
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/system", HTTP_GET, handleGetSystem);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/tx/send", handleTxSend));
  server.on("/api/tx/end", HTTP_POST, handleTxEnd);
  server.on("/api/tx/abort", HTTP_POST, handleTxAbort);

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.begin();

  // Pinned to core 0, per the design doc's dual-core decision (TxManager +
  // the timer ISR own core 1; heavier/less time-critical work lives here).
  xTaskCreatePinnedToCore(pushTask, "WebPush", 4096, nullptr, 1, nullptr, 0);
}

} // namespace WebInterface
