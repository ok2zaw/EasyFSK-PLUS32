#pragma once

#include <ArduinoJson.h>

// Hardware-independent mapping from ConfigStore errors to the HTTP response
// emitted by POST /api/config and /api/config/restore. Keeping the status and
// JSON shape here prevents the two endpoints from drifting apart and makes the
// externally visible contract host-testable without ESPAsyncWebServer.
namespace WebConfigResponse {

enum class Kind {
  Deferred,
  StorageError,
  ValidationError,
};

inline Kind classify(JsonObjectConst errors) {
  // A persistence error is a server failure and must not be hidden even if a
  // future caller accidentally supplies more than one category.
  if (errors["storage"].is<const char *>()) {
    return Kind::StorageError;
  }
  if (errors["_"].is<const char *>()) {
    return Kind::Deferred;
  }
  return Kind::ValidationError;
}

inline int statusCode(Kind kind) {
  switch (kind) {
    case Kind::Deferred: return 200;
    case Kind::StorageError: return 500;
    case Kind::ValidationError: return 400;
  }
  return 500;
}

inline void buildBody(Kind kind, JsonObjectConst errors, JsonObject out) {
  out["ok"] = false;
  switch (kind) {
    case Kind::Deferred:
      out["deferred"] = true;
      out["message"] = errors["_"];
      return;
    case Kind::StorageError:
      out["reason"] = "storage_error";
      out["message"] = errors["storage"];
      return;
    case Kind::ValidationError:
      out["errors"] = errors;
      return;
  }
}

} // namespace WebConfigResponse
