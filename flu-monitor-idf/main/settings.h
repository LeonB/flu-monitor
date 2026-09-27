#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_SHEETS_URL_MAX_LEN     128
#define SETTINGS_SHEETS_SECRET_MAX_LEN  64

// Runtime-editable tunables -- replaces the ESPHome sidecar's YAML
// `substitutions:` block. NVS-backed (see settings.c), with config.h's
// DEFAULT_* macros used only as first-boot seed values.
typedef struct {
  uint16_t log_heartbeat_min;
  float zone_cold_max_c;
  float zone_good_max_c;
  float fast_rise_c_per_min;
  float thermocouple_deadband_c;
  char google_sheets_webhook_url[SETTINGS_SHEETS_URL_MAX_LEN];
  char google_sheets_secret[SETTINGS_SHEETS_SECRET_MAX_LEN];
} settings_t;

// Loads settings from NVS, seeding config.h's compile-time defaults if NVS
// doesn't have a usable copy yet (first boot, or a corrupted/old-layout
// entry). Call once at startup before settings_get()/settings_set() are used
// anywhere else.
void settings_init(void);

// Copies the current settings into *out. Safe to call from any task --
// internally mutex-guarded against a concurrent settings_set().
void settings_get(settings_t *out);

// Validates and persists new settings, replacing the current ones entirely
// (matches the REST API's POST /api/settings, which always sends the whole
// object -- the planned web UI's single sticky Save bar has no per-field
// PATCH semantics to preserve). Returns ESP_ERR_INVALID_ARG without changing
// anything, NVS included, if validation fails.
esp_err_t settings_set(const settings_t *new_settings);

#ifdef __cplusplus
}
#endif
