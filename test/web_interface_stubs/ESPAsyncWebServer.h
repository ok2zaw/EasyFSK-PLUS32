#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

using WebRequestMethodComposite = uint8_t;
constexpr WebRequestMethodComposite HTTP_GET = 1;
constexpr WebRequestMethodComposite HTTP_POST = 2;

enum AwsEventType { WS_EVT_CONNECT, WS_EVT_DISCONNECT, WS_EVT_DATA, WS_EVT_PONG, WS_EVT_ERROR };

class AsyncWebServer;
class AsyncWebSocket;
class AsyncWebSocketClient {};

class AsyncWebServerResponse {
public:
  AsyncWebServerResponse(int statusCode, const char *contentTypeValue,
                         const String &bodyValue)
      : status(statusCode), contentType(contentTypeValue), body(bodyValue.c_str()) {}

  void addHeader(const char *name, const char *value) { headers[name] = value; }

  int status;
  std::string contentType;
  std::string body;
  std::map<std::string, std::string> headers;
};

class AsyncWebServerRequest {
public:
  void send(int statusCode, const char *contentTypeValue, const String &bodyValue) {
    capture(statusCode, contentTypeValue, bodyValue.c_str(), {});
  }

  void send(int statusCode, const char *contentTypeValue, const char *bodyValue) {
    capture(statusCode, contentTypeValue, bodyValue, {});
  }

  AsyncWebServerResponse *beginResponse(int statusCode, const char *contentTypeValue,
                                        const String &bodyValue) {
    return new AsyncWebServerResponse(statusCode, contentTypeValue, bodyValue);
  }

  void send(AsyncWebServerResponse *response) {
    capture(response->status, response->contentType.c_str(), response->body.c_str(),
            response->headers);
    delete response;
  }

  bool sent = false;
  int status = 0;
  std::string contentType;
  std::string body;
  std::map<std::string, std::string> headers;

private:
  void capture(int statusCode, const char *contentTypeValue, const char *bodyValue,
               const std::map<std::string, std::string> &headerValues) {
    sent = true;
    status = statusCode;
    contentType = contentTypeValue == nullptr ? "" : contentTypeValue;
    body = bodyValue == nullptr ? "" : bodyValue;
    headers = headerValues;
  }
};

class AsyncWebHandler {
public:
  virtual ~AsyncWebHandler() = default;
};

using ArRequestHandlerFunction = std::function<void(AsyncWebServerRequest *)>;
using ArJsonRequestHandlerFunction =
    std::function<void(AsyncWebServerRequest *, JsonVariant &)>;
using AwsEventHandler = std::function<void(AsyncWebSocket *, AsyncWebSocketClient *,
                                           AwsEventType, void *, uint8_t *, size_t)>;

class AsyncCallbackJsonWebHandler : public AsyncWebHandler {
public:
  AsyncCallbackJsonWebHandler(const char *pathValue,
                              ArJsonRequestHandlerFunction callbackValue)
      : path(pathValue), callback(callbackValue) {}

  std::string path;
  ArJsonRequestHandlerFunction callback;
};

class AsyncStaticWebHandler : public AsyncWebHandler {
public:
  AsyncStaticWebHandler &setDefaultFile(const char *value) {
    defaultFile = value;
    return *this;
  }

  std::string uri;
  std::string filesystemPath;
  std::string defaultFile;
};

struct MockRoute {
  std::string path;
  WebRequestMethodComposite method;
  ArRequestHandlerFunction callback;
};

namespace WebServerMock {
extern AsyncWebServer *server;
extern AsyncWebSocket *socket;
} // namespace WebServerMock

class AsyncWebSocket : public AsyncWebHandler {
public:
  explicit AsyncWebSocket(const char *pathValue) : path(pathValue) {
    WebServerMock::socket = this;
  }

  void onEvent(AwsEventHandler callbackValue) { callback = callbackValue; }
  void textAll(const String &body) { sentBodies.emplace_back(body.c_str()); }
  void cleanupClients() { cleanupCount++; }

  std::string path;
  AwsEventHandler callback;
  std::vector<std::string> sentBodies;
  size_t cleanupCount = 0;
};

class AsyncWebServer {
public:
  explicit AsyncWebServer(uint16_t portValue) : port(portValue) {
    WebServerMock::server = this;
  }

  void on(const char *path, WebRequestMethodComposite method,
          ArRequestHandlerFunction callback) {
    routes.push_back({path, method, callback});
  }

  void addHandler(AsyncWebHandler *handler) {
    handlers.push_back(handler);
    if (auto *json = dynamic_cast<AsyncCallbackJsonWebHandler *>(handler)) {
      jsonHandlers.push_back(json);
    }
  }

  template <typename FileSystem>
  AsyncStaticWebHandler &serveStatic(const char *uri, FileSystem &, const char *path) {
    staticHandler.uri = uri;
    staticHandler.filesystemPath = path;
    return staticHandler;
  }

  void begin() { begun = true; }

  bool dispatch(const char *path, WebRequestMethodComposite method,
                AsyncWebServerRequest &request) const {
    for (const MockRoute &route : routes) {
      if (route.path == path && route.method == method) {
        route.callback(&request);
        return true;
      }
    }
    return false;
  }

  bool dispatchJson(const char *path, AsyncWebServerRequest &request,
                    JsonVariant &json) const {
    for (const AsyncCallbackJsonWebHandler *handler : jsonHandlers) {
      if (handler->path == path) {
        handler->callback(&request, json);
        return true;
      }
    }
    return false;
  }

  uint16_t port;
  bool begun = false;
  std::vector<MockRoute> routes;
  std::vector<AsyncWebHandler *> handlers;
  std::vector<AsyncCallbackJsonWebHandler *> jsonHandlers;
  AsyncStaticWebHandler staticHandler;
};
