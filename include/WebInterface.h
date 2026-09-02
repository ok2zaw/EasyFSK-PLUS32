#pragma once

// ---------------------------------------------------------------------------
// ESPAsyncWebServer + AsyncWebSocket backend, implementing the routes and
// JSON shapes from the design doc's "Web UI: routes and JSON shapes" section
// verbatim: GET/POST /api/config, GET /api/config/backup, POST
// /api/config/restore, GET /api/status, GET /api/system, POST /api/tx/send
// /end /abort, and a server->client-only WebSocket at /ws (status push on
// every change plus a ~1s heartbeat). Static files (the web UI itself) are
// served from LittleFS.
//
// No authentication (user's choice -- trusted wired LAN). Every TX action
// goes through TxManager's queue exactly like a serial byte would, so it's
// gated by CPU_INH_PIN the same as every other path.
// ---------------------------------------------------------------------------

namespace WebInterface {

// Starts the web server + WebSocket and the background task that pushes
// WebSocket status/heartbeat messages. Call once from setup(), after
// LittleFS and Ethernet are up.
void begin();

} // namespace WebInterface
