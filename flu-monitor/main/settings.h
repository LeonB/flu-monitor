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
  float zone_optimal_max_c;
  float fast_rise_c_per_min;
  float thermocouple_deadband_c;
  char google_sheets_webhook_url[SETTINGS_SHEETS_URL_MAX_LEN];
  char google_sheets_secret[SETTINGS_SHEETS_SECRET_MAX_LEN];
  // LED/glow pulse-rendering tuning -- see config.h's DEFAULT_* comment.
  // Not used by flu-monitor itself; distributed from here so flu-display's
  // LED ring and this device's own dashboard.js glow read one live source
  // instead of each hardcoding their own copy.
  uint16_t idle_pulse_period_ms;
  uint16_t fast_pulse_period_ms;
  float rate_deadband_c_per_min;
  float color_transition_exponent;
  // CSS-compatible #RRGGBB, shared by the web UI and LED ring. Append only:
  // settings_init migrates the previous NVS layout without losing settings.
  char zone_cold_color[8];
  char zone_optimal_color[8];
  char zone_hot_color[8];
  float breathing_exponent;  // 0.3..3.0; 1.0 preserves the original envelope
  uint32_t maximum_brightness; // 0..255, must be >= minimum
  uint32_t minimum_brightness; // 0..255, LED channel floor
} settings_t;

// Loads settings from NVS, seeding config.h's compile-time defaults if NVS
// doesn't have a usable copy yet (first boot, or a corrupted/old-layout
// entry). The preceding layout migrates with default colours, preserving
// all existing values. Call once at startup before settings_get()/settings_set() are used
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
