#include "WebConfigApi.h"

#include "Config.h"
#include "ConfigStore.h"
#include "WebConfigResponse.h"

namespace WebConfigApi {

int apply(JsonVariantConst input, JsonObject out) {
  JsonDocument errorDoc;
  JsonObject errors = errorDoc.to<JsonObject>();

  if (ConfigStore::applyAndSave(input, errors)) {
    out["ok"] = true;
    configToJson(ConfigStore::get(), out["config"].to<JsonObject>());
    return 200;
  }

  WebConfigResponse::Kind kind = WebConfigResponse::classify(errors);
  WebConfigResponse::buildBody(kind, errors, out);
  return WebConfigResponse::statusCode(kind);
}

} // namespace WebConfigApi
