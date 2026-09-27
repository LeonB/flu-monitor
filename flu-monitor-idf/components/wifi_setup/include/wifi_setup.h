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

// Tests the given credentials by connecting as STA while the setup AP (and
// its captive portal) stays up, instead of blindly saving and rebooting to
// find out. Requires WIFI_MODE_APSTA to already be active with the STA
// netif already created and started -- true whenever this is called from
// inside the captive portal (see wifi_init_softap()), since the portal's
// own network scan needs the same thing. On failure, disconnects and
// unregisters its own temporary handlers before returning, leaving the
// AP/portal untouched so the caller can prompt for another attempt.
bool wifi_sta_test_connect(const char *ssid, const char *pass, uint32_t timeout_ms);

// The device reboots to attempt joining newly-saved credentials, so the
// captive portal that triggered the attempt can't observe its outcome
// directly -- this NVS-backed flag is how that outcome survives the
// reboot, letting the portal show a "that didn't take" failure state
// (with the offending SSID) instead of the plain setup form the next time
// it starts.
bool wifi_last_attempt_failed(char *failed_ssid_out, size_t failed_ssid_out_size);
esp_err_t wifi_mark_attempt_failed(const char *ssid);
esp_err_t wifi_clear_attempt_failed(void);

typedef struct {
  char ssid[WIFI_SSID_MAX_LEN + 1];
  int8_t rssi;
  bool secured;
} wifi_scan_result_t;

// Blocking scan for nearby WiFi networks, strongest signal first, with
// duplicate SSIDs (seen on multiple channels/BSSIDs) collapsed to their
// strongest showing. Requires an active STA interface (WIFI_MODE_APSTA is
// fine -- the captive portal's own AP doesn't need to be torn down to
// scan). Returns the number of results written into `out_results` (capped
// at `max_results`), or -1 on failure.
int wifi_scan(wifi_scan_result_t *out_results, size_t max_results);

#ifdef __cplusplus
}
#endif
