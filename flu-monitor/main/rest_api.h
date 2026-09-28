#pragma once

#include "esp_http_server.h"

#include "sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the single shared HTTP server for this device's "normal running"
// state (once connected to real WiFi -- the captive portal's own separate
// server covers initial setup) and registers this project's REST
// endpoints on it:
//   GET  /api/reading  -- latest sensor reading + derived rate/zone
//   GET  /api/wifi     -- live WiFi link info (SSID/BSSID/channel/RSSI),
//                         queried fresh on every call, not cached
//   GET  /api/settings -- current runtime settings
//   POST /api/settings -- replace settings wholesale (JSON body)
//   GET  /api/events   -- the fixed woodstove-event taxonomy (slug + label)
//   POST /api/event    -- log an immediate, un-gated Sheets row for one of
//                         those events (JSON body {"event": "<slug>"})
//   GET  /api/history  -- ~24h of downsampled readings + recent logged
//                         events, for the web UI's graph
//   GET  /                -- the embedded web UI (dashboard/graph/settings)
//   GET  /dashboard.js,
//        /dashboard.css,
//        /alpinejs.min.js  -- the web UI's own JS/CSS/Alpine.js assets
// Returns the server handle so other modules (ota_server, ws_server) can
// register their own endpoints onto the same server instead of each
// starting their own (only one httpd can bind port 80 at a time).
httpd_handle_t rest_api_start(void);

// Serializes a reading into the exact same JSON shape GET /api/reading
// returns -- shared with ws_server's own reading broadcast (via main.c) so
// both surfaces agree on one schema. Caller must free() the returned string.
char *rest_api_reading_json(const sensor_reading_t *r);

// Logs the REST API server's current open-socket count (see the .c file's
// own doc comment) -- diagnostic for chasing an apparent server hang.
// Call periodically; a no-op before rest_api_start() has run.
void rest_api_log_socket_usage(void);

#ifdef __cplusplus
}
#endif
