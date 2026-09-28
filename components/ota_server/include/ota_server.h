#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers an authenticated POST /ota endpoint on the caller's own
// already-running httpd_handle_t (e.g. flu-monitor's shared server, see
// rest_api.h) that streams its request body into the next OTA partition
// via esp_ota_ops, then reboots into it. Call once, after WiFi STA is
// connected and the server is up.
//
// `secret` is compared against the request's own "secret" query param --
// each caller passes its own per-project OTA_SECRET (from its own
// git-ignored secrets.h, see secrets.h.example), so this shared component
// itself carries no secret of its own.
//
// Push an update with: ./ota_flash.sh <device-ip-or-hostname>
void ota_server_register(httpd_handle_t server, const char *secret);

// For a caller with no HTTP server of its own yet (e.g. flu-display, which
// otherwise runs none): starts a dedicated httpd instance sized for OTA
// (8192-byte stack -- esp_ota_* calls + logging need more than the 4096
// default) and registers the endpoint on it. Call once, after WiFi STA is
// connected.
void ota_server_start(const char *secret);

#ifdef __cplusplus
}
#endif
