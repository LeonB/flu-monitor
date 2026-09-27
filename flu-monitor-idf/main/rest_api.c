#include "rest_api.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "sensors.h"
#include "settings.h"

static const char *TAG = "rest_api";

// cJSON_AddNumberToObject widens our floats to double and prints the
// shortest round-tripping decimal for *that* double -- which surfaces the
// float's own binary-representation noise (e.g. 24.188323974609375 instead
// of 24.2). Rounding to a sensible decimal precision first, matching what
// the ESPHome sidecar's own str_sprintf precision already used for these
// same values, gets a clean API response instead.
static double round_to(double value, double step) {
  return round(value / step) * step;
}

static esp_err_t reading_get_handler(httpd_req_t *req) {
  sensor_reading_t r;
  sensors_get_last_reading(&r);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "bmp581_ok", r.bmp581_ok);
  cJSON_AddNumberToObject(root, "bmp581_temperature_c", round_to(r.bmp581_temperature_c, 0.1));
  cJSON_AddNumberToObject(root, "bmp581_pressure_pa", round_to(r.bmp581_pressure_pa, 1.0));
  cJSON_AddBoolToObject(root, "thermocouple_ok", r.thermocouple_ok);
  cJSON_AddNumberToObject(root, "thermocouple_c", round_to(r.thermocouple_c, 0.1));
  cJSON_AddNumberToObject(root, "cold_junction_c", round_to(r.cold_junction_c, 0.1));
  cJSON_AddNumberToObject(root, "thermocouple_rate_c_per_min", round_to(r.thermocouple_rate_c_per_min, 0.01));
  cJSON_AddStringToObject(root, "thermocouple_zone", thermocouple_zone_name(r.thermocouple_zone));

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

static const httpd_uri_t reading_uri = {
    .uri = "/api/reading",
    .method = HTTP_GET,
    .handler = reading_get_handler,
};

static void settings_to_json(const settings_t *s, cJSON *root) {
  cJSON_AddNumberToObject(root, "log_heartbeat_min", s->log_heartbeat_min);
  cJSON_AddNumberToObject(root, "zone_cold_max_c", round_to(s->zone_cold_max_c, 0.1));
  cJSON_AddNumberToObject(root, "zone_good_max_c", round_to(s->zone_good_max_c, 0.1));
  cJSON_AddNumberToObject(root, "fast_rise_c_per_min", round_to(s->fast_rise_c_per_min, 0.1));
  cJSON_AddNumberToObject(root, "thermocouple_deadband_c", round_to(s->thermocouple_deadband_c, 0.1));
  cJSON_AddStringToObject(root, "google_sheets_webhook_url", s->google_sheets_webhook_url);
  cJSON_AddStringToObject(root, "google_sheets_secret", s->google_sheets_secret);
}

static esp_err_t settings_get_handler(httpd_req_t *req) {
  settings_t s;
  settings_get(&s);

  cJSON *root = cJSON_CreateObject();
  settings_to_json(&s, root);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

static const httpd_uri_t settings_get_uri = {
    .uri = "/api/settings",
    .method = HTTP_GET,
    .handler = settings_get_handler,
};

// Full-replace semantics, matching the planned web UI's single sticky Save
// bar (no per-field PATCH to preserve) -- a field missing from the request
// body becomes that field's zero value (numeric) or empty string, not
// "leave whatever was there before".
static esp_err_t settings_post_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len >= 512) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
    return ESP_FAIL;
  }

  char body[512] = {0};
  int received = 0;
  while (received < req->content_len) {
    int ret = httpd_req_recv(req, body + received, req->content_len - received);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request body");
      return ESP_FAIL;
    }
    received += ret;
  }
  body[received] = '\0';

  cJSON *root = cJSON_Parse(body);
  if (root == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  settings_t s = {0};
  cJSON *item;

  item = cJSON_GetObjectItem(root, "log_heartbeat_min");
  s.log_heartbeat_min = cJSON_IsNumber(item) ? (uint16_t) item->valuedouble : 0;

  item = cJSON_GetObjectItem(root, "zone_cold_max_c");
  s.zone_cold_max_c = cJSON_IsNumber(item) ? (float) item->valuedouble : 0.0f;

  item = cJSON_GetObjectItem(root, "zone_good_max_c");
  s.zone_good_max_c = cJSON_IsNumber(item) ? (float) item->valuedouble : 0.0f;

  item = cJSON_GetObjectItem(root, "fast_rise_c_per_min");
  s.fast_rise_c_per_min = cJSON_IsNumber(item) ? (float) item->valuedouble : 0.0f;

  item = cJSON_GetObjectItem(root, "thermocouple_deadband_c");
  s.thermocouple_deadband_c = cJSON_IsNumber(item) ? (float) item->valuedouble : 0.0f;

  item = cJSON_GetObjectItem(root, "google_sheets_webhook_url");
  if (cJSON_IsString(item)) {
    strlcpy(s.google_sheets_webhook_url, item->valuestring, sizeof(s.google_sheets_webhook_url));
  }

  item = cJSON_GetObjectItem(root, "google_sheets_secret");
  if (cJSON_IsString(item)) {
    strlcpy(s.google_sheets_secret, item->valuestring, sizeof(s.google_sheets_secret));
  }

  cJSON_Delete(root);

  esp_err_t err = settings_set(&s);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Rejected settings update: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid settings values");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  return ESP_OK;
}

static const httpd_uri_t settings_post_uri = {
    .uri = "/api/settings",
    .method = HTTP_POST,
    .handler = settings_post_handler,
};

httpd_handle_t rest_api_start(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;  // shared with ota_server's esp_ota_* calls, which need more than the 4096 default
  config.max_uri_handlers = 12;  // default (8) is too few once ota + this milestone's 3 endpoints are all registered

  ESP_LOGI(TAG, "Starting REST API server on port %d", config.server_port);
  ESP_ERROR_CHECK(httpd_start(&server, &config));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reading_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_get_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_post_uri));
  return server;
}
