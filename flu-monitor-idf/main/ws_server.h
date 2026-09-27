#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the /ws WebSocket endpoint on the caller's already-running
// shared server (see rest_api.h). Broadcast-only from the server's side --
// this device never expects a meaningful client->server frame.
void ws_server_register(httpd_handle_t server);

// Broadcasts a reading to every connected WS client, wrapped as
// {"type":"reading","reading":<reading_json>}. `reading_json` is consumed
// as-is (via cJSON's raw-value support, not re-parsed) -- pass the exact
// string rest_api_reading_json() returns, so REST and WS agree on one
// schema. Safe to call from any task; a no-op if nothing's connected.
void ws_server_broadcast_reading(const char *reading_json);

// Broadcasts {"type":"settings_changed"} -- lets a connected client (e.g.
// flu-display) know its own cached copy of the zone thresholds is stale,
// without waiting for its next poll cycle. Safe to call from any task.
void ws_server_broadcast_settings_changed(void);

#ifdef __cplusplus
}
#endif
