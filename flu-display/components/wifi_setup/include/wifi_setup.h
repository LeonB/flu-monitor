#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max lengths match wifi_config_t's own ssid[32]/password[64] fields.
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64

// Loads previously-saved WiFi credentials from NVS. Returns true if found
// (buffers are filled and NUL-terminated), false if none are stored yet.
bool wifi_creds_load(char *ssid_out, char *pass_out);

// Saves WiFi credentials to NVS (namespace "wifi_cfg"), overwriting any
// previous values. Call this from the captive portal's save handler.
esp_err_t wifi_creds_save(const char *ssid, const char *pass);

// Attempts to join the given network in STA mode, waiting up to
// timeout_ms for either a successful IP acquisition or a connection
// failure. Returns true on success. Leaves WiFi running in STA mode either
// way (caller decides what to do next on failure, e.g. fall back to AP).
bool wifi_sta_try_connect(const char *ssid, const char *pass, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
