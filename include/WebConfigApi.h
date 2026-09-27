#pragma once

#include <ArduinoJson.h>

// Controller shared by POST /api/config and POST /api/config/restore.
// It executes the real ConfigStore transaction and builds the complete JSON
// response, while leaving only transport/serialization to WebInterface.
namespace WebConfigApi {

// Returns the HTTP status code and writes the response object to `out`.
int apply(JsonVariantConst input, JsonObject out);

} // namespace WebConfigApi
