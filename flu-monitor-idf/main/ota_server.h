#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts an HTTP server with a single authenticated POST /ota endpoint that
// streams its request body into the next OTA partition via esp_ota_ops,
// then reboots into it. Call once, after WiFi STA is connected.
//
// Push an update with: ./ota_flash.sh <device-ip-or-hostname>
void ota_server_start(void);

#ifdef __cplusplus
}
#endif
