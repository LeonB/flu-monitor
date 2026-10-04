#include "settings.h"

#include <string.h>
#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "config.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "settings"
#define NVS_KEY_BLOB  "settings_v1"  // whole settings_t as one blob -- see settings_set()'s own doc comment for why

static settings_t s_settings;
static SemaphoreHandle_t s_mutex;

static void set_defaults(settings_t *s) {
  memset(s, 0, sizeof(*s));
  s->log_heartbeat_min = DEFAULT_LOG_HEARTBEAT_MIN;
  s->zone_cold_max_c = DEFAULT_ZONE_COLD_MAX_C;
  s->zone_optimal_max_c = DEFAULT_ZONE_OPTIMAL_MAX_C;
  s->fast_rise_c_per_min = DEFAULT_FAST_RISE_C_PER_MIN;
  s->thermocouple_deadband_c = DEFAULT_THERMOCOUPLE_DEADBAND_C;
  // google_sheets_webhook_url/secret left empty -- no safe compile-time
  // default; Milestone 5's Sheets logging has nothing to do until these are
  // set via POST /api/settings.
  s->idle_pulse_period_ms = DEFAULT_IDLE_PULSE_PERIOD_MS;
  s->fast_pulse_period_ms = DEFAULT_FAST_PULSE_PERIOD_MS;
  s->rate_deadband_c_per_min = DEFAULT_RATE_DEADBAND_C_PER_MIN;
  s->color_transition_exponent = DEFAULT_COLOR_TRANSITION_EXPONENT;
  strcpy(s->zone_cold_color, "#003cff");
  strcpy(s->zone_optimal_color, "#ff3700");
  strcpy(s->zone_hot_color, "#ff0000");
}

static bool valid_color(const char color[8]) {
  if (color[0] != '#' || color[7] != '\0') return false;
  for (int i = 1; i <= 6; i++) {
    char c = color[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

static bool validate(const settings_t *s) {
  if (!valid_color(s->zone_cold_color) || !valid_color(s->zone_optimal_color) || !valid_color(s->zone_hot_color)) return false;
  if (s->zone_cold_max_c >= s->zone_optimal_max_c) {
    return false;
  }
  if (s->fast_rise_c_per_min <= 0.0f) {
    return false;
  }
  if (s->log_heartbeat_min == 0) {
    return false;
  }
  if (s->thermocouple_deadband_c < 0.0f) {
    return false;
  }
  if (s->idle_pulse_period_ms == 0 || s->fast_pulse_period_ms == 0) {
    return false;
  }
  if (s->rate_deadband_c_per_min < 0.0f) {
    return false;
  }
  if (s->color_transition_exponent <= 0.0f) {
    return false;
  }
  return true;
}

void settings_init(void) {
  s_mutex = xSemaphoreCreateMutex();

  settings_t loaded;
  set_defaults(&loaded);

  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    settings_t from_nvs = loaded;
    size_t len = sizeof(from_nvs);
    // The old blob is the unchanged prefix ending before the new colours.
    // Seed first so an old blob retains all settings and gets default colours.
    if (nvs_get_blob(handle, NVS_KEY_BLOB, &from_nvs, &len) == ESP_OK &&
        (len == sizeof(from_nvs) || len == offsetof(settings_t, zone_cold_color)) &&
        validate(&from_nvs)) {
      loaded = from_nvs;
      ESP_LOGI(TAG, "Loaded settings from NVS");
    } else {
      ESP_LOGI(TAG, "No usable settings in NVS yet -- using compile-time defaults");
    }
    nvs_close(handle);
  } else {
    ESP_LOGI(TAG, "Settings NVS namespace not found yet -- using compile-time defaults");
  }

  s_settings = loaded;
}

void settings_get(settings_t *out) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  *out = s_settings;
  xSemaphoreGive(s_mutex);
}

esp_err_t settings_set(const settings_t *new_settings) {
  if (!validate(new_settings)) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_blob(handle, NVS_KEY_BLOB, new_settings, sizeof(*new_settings));
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    return err;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_settings = *new_settings;
  xSemaphoreGive(s_mutex);
  return ESP_OK;
}
