#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers an authenticated POST /ota endpoint (on the caller's own
// already-running httpd_handle_t -- see rest_api.h, which owns the single
// shared server) that streams its request body into the next OTA partition
// via esp_ota_ops, then reboots into it. Call once, after WiFi STA is
// connected and the shared server is up.
//
// Push an update with: ./ota_flash.sh <device-ip-or-hostname>
void ota_server_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
