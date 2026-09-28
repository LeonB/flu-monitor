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
//   GET  /api/settings -- current runtime settings
//   POST /api/settings -- replace settings wholesale (JSON body)
//   GET  /api/events   -- the fixed woodstove-event taxonomy (slug + label)
//   POST /api/event    -- log an immediate, un-gated Sheets row for one of
//                         those events (JSON body {"event": "<slug>"})
// Returns the server handle so other modules (ota_server, ws_server) can
// register their own endpoints onto the same server instead of each
// starting their own (only one httpd can bind port 80 at a time).
httpd_handle_t rest_api_start(void);

// Serializes a reading into the exact same JSON shape GET /api/reading
// returns -- shared with ws_server's own reading broadcast (via main.c) so
// both surfaces agree on one schema. Caller must free() the returned string.
char *rest_api_reading_json(const sensor_reading_t *r);

#ifdef __cplusplus
}
#endif
