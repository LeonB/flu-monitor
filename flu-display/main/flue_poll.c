#include "flue_poll.h"

#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"

static const char *TAG = "flue_poll";

// flu-monitor's web_server exposes each sensor at /sensor/<exact entity
// NAME, URL-encoded>, not its object_id -- verified against the live
// device: GET .../sensor/Thermocouple%20Temperature returns
// {"id":"sensor/Thermocouple Temperature","value":25,"state":"25.0 °C"}.
#define THERMOCOUPLE_URL "http://" FLU_MONITOR_HOST "/sensor/Thermocouple%20Temperature"

// Same reasoning as flu-monitor.yaml's own sanity clamp (see CLAUDE.md): a
// stovepipe has no business reading outside this range, so anything outside
// it is a corrupted/failed reading, not real data -- and must not become the
// new rate-of-change baseline.
#define SANITY_MIN_C  -40.0f
#define SANITY_MAX_C  600.0f

#define HTTP_RESPONSE_BUF_SIZE  256
#define HTTP_TIMEOUT_MS         5000

static char s_response_buf[HTTP_RESPONSE_BUF_SIZE];
static int s_response_len;

static bool s_have_prev = false;
static float s_prev_temperature_c;
static int64_t s_prev_time_us;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && !esp_http_client_is_chunked_response(evt->client)) {
    int copy_len = evt->data_len;
    if (s_response_len + copy_len >= HTTP_RESPONSE_BUF_SIZE) {
      copy_len = HTTP_RESPONSE_BUF_SIZE - 1 - s_response_len;
    }
    if (copy_len > 0) {
      memcpy(s_response_buf + s_response_len, evt->data, copy_len);
      s_response_len += copy_len;
    }
  }
  return ESP_OK;
}

void flue_poll_init(void) {
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set("flu-display"));
  ESP_ERROR_CHECK(mdns_instance_name_set("flu-display"));
}

flue_reading_t flue_poll_once(void) {
  flue_reading_t result = {0};

  s_response_len = 0;

  esp_http_client_config_t config = {
      .url = THERMOCOUPLE_URL,
      .event_handler = http_event_handler,
      .timeout_ms = HTTP_TIMEOUT_MS,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return result;
  }

  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (status != 200) {
    ESP_LOGW(TAG, "HTTP GET returned status %d", status);
    return result;
  }

  cJSON *root = cJSON_ParseWithLength(s_response_buf, s_response_len);
  if (root == NULL) {
    ESP_LOGW(TAG, "Failed to parse JSON response: '%.*s'", s_response_len, s_response_buf);
    return result;
  }

  cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (!cJSON_IsNumber(value)) {
    ESP_LOGW(TAG, "JSON response had no numeric 'value' field: '%.*s'", s_response_len, s_response_buf);
    cJSON_Delete(root);
    return result;
  }

  float temperature_c = (float) value->valuedouble;
  cJSON_Delete(root);

  if (temperature_c < SANITY_MIN_C || temperature_c > SANITY_MAX_C) {
    ESP_LOGW(TAG, "Rejecting implausible reading: %.1f C", temperature_c);
    return result;
  }

  int64_t now_us = esp_timer_get_time();
  result.valid = true;
  result.temperature_c = temperature_c;

  if (s_have_prev) {
    float elapsed_min = (float) (now_us - s_prev_time_us) / 1000000.0f / 60.0f;
    if (elapsed_min > 0.0f) {
      result.rate_c_per_min = (temperature_c - s_prev_temperature_c) / elapsed_min;
    }
  }

  s_have_prev = true;
  s_prev_temperature_c = temperature_c;
  s_prev_time_us = now_us;

  return result;
}
