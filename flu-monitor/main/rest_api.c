#include "rest_api.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "sensors.h"
#include "settings.h"
#include "sheets_logger.h"
#include "ws_server.h"

static const char *TAG = "rest_api";

// Embedded web UI assets (EMBED_FILES in CMakeLists.txt) -- served below by
// web_ui_*_handler, same extern-symbol pattern as captive_portal.c's own
// embedded root.html/styles.css/alpine.min.js.
extern const char web_ui_html_start[] asm("_binary_dashboard_html_start");
extern const char web_ui_html_end[] asm("_binary_dashboard_html_end");
extern const char web_ui_js_start[] asm("_binary_dashboard_js_start");
extern const char web_ui_js_end[] asm("_binary_dashboard_js_end");
extern const char web_ui_css_start[] asm("_binary_dashboard_css_start");
extern const char web_ui_css_end[] asm("_binary_dashboard_css_end");
extern const char web_ui_alpine_js_start[] asm("_binary_alpinejs_min_js_start");
extern const char web_ui_alpine_js_end[] asm("_binary_alpinejs_min_js_end");

static esp_err_t web_ui_html_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, web_ui_html_start, web_ui_html_end - web_ui_html_start);
  return ESP_OK;
}
static const httpd_uri_t web_ui_html_uri = {.uri = "/", .method = HTTP_GET, .handler = web_ui_html_get_handler};

static esp_err_t web_ui_js_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_send(req, web_ui_js_start, web_ui_js_end - web_ui_js_start);
  return ESP_OK;
}
static const httpd_uri_t web_ui_js_uri = {.uri = "/dashboard.js", .method = HTTP_GET, .handler = web_ui_js_get_handler};

static esp_err_t web_ui_css_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/css");
  httpd_resp_send(req, web_ui_css_start, web_ui_css_end - web_ui_css_start);
  return ESP_OK;
}
static const httpd_uri_t web_ui_css_uri = {.uri = "/dashboard.css", .method = HTTP_GET, .handler = web_ui_css_get_handler};

static esp_err_t web_ui_alpine_js_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_send(req, web_ui_alpine_js_start, web_ui_alpine_js_end - web_ui_alpine_js_start);
  return ESP_OK;
}
static const httpd_uri_t web_ui_alpine_js_uri = {
    .uri = "/alpinejs.min.js", .method = HTTP_GET, .handler = web_ui_alpine_js_get_handler};

// cJSON_AddNumberToObject widens our floats to double and prints the
// shortest round-tripping decimal for *that* double -- which surfaces the
// float's own binary-representation noise (e.g. 24.188323974609375 instead
// of 24.2). Rounding to a sensible decimal precision first, matching what
// the ESPHome sidecar's own str_sprintf precision already used for these
// same values, gets a clean API response instead.
static double round_to(double value, double step) {
  return round(value / step) * step;
}

char *rest_api_reading_json(const sensor_reading_t *r) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "thermocouple_ok", r->thermocouple_ok);
  cJSON_AddNumberToObject(root, "thermocouple_c", round_to(r->thermocouple_c, 0.1));
  cJSON_AddNumberToObject(root, "cold_junction_c", round_to(r->cold_junction_c, 0.1));
  cJSON_AddNumberToObject(root, "thermocouple_rate_c_per_min", round_to(r->thermocouple_rate_c_per_min, 0.01));
  cJSON_AddStringToObject(root, "thermocouple_zone", thermocouple_zone_name(r->thermocouple_zone));

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

static esp_err_t reading_get_handler(httpd_req_t *req) {
  sensor_reading_t r;
  sensors_get_last_reading(&r);

  char *json = rest_api_reading_json(&r);
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

// Live WiFi link quality -- esp_wifi_sta_get_ap_info() queries the current
// association fresh on every call (unlike RSSI, which the WiFi driver only
// ever logs once, at the moment it associates -- see CLAUDE.md's own
// writeup of chasing an apparent REST API hang that turned out to be a
// weak signal after roaming to a farther AP within the same SSID). No
// caching here: this is meant to answer "how's the signal right now," not
// "how was it at boot."
static esp_err_t wifi_get_handler(httpd_req_t *req) {
  wifi_ap_record_t info;
  esp_err_t err = esp_wifi_sta_get_ap_info(&info);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "connected", err == ESP_OK);
  if (err == ESP_OK) {
    cJSON_AddStringToObject(root, "ssid", (const char *) info.ssid);
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", info.bssid[0], info.bssid[1], info.bssid[2],
             info.bssid[3], info.bssid[4], info.bssid[5]);
    cJSON_AddStringToObject(root, "bssid", bssid);
    cJSON_AddNumberToObject(root, "channel", info.primary);
    cJSON_AddNumberToObject(root, "rssi", info.rssi);
  }

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

static const httpd_uri_t wifi_get_uri = {
    .uri = "/api/wifi",
    .method = HTTP_GET,
    .handler = wifi_get_handler,
};

static void settings_to_json(const settings_t *s, cJSON *root) {
  cJSON_AddNumberToObject(root, "log_heartbeat_min", s->log_heartbeat_min);
  cJSON_AddNumberToObject(root, "zone_cold_max_c", round_to(s->zone_cold_max_c, 0.1));
  cJSON_AddNumberToObject(root, "zone_optimal_max_c", round_to(s->zone_optimal_max_c, 0.1));
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

  item = cJSON_GetObjectItem(root, "zone_optimal_max_c");
  s.zone_optimal_max_c = cJSON_IsNumber(item) ? (float) item->valuedouble : 0.0f;

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

  // Lets flu-display (and any other WS client) know its cached zone
  // thresholds are stale, without waiting for its own next poll cycle.
  ws_server_broadcast_settings_changed();

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  return ESP_OK;
}

static const httpd_uri_t settings_post_uri = {
    .uri = "/api/settings",
    .method = HTTP_POST,
    .handler = settings_post_handler,
};

// The fixed woodstove-event taxonomy, ported verbatim from the original
// ESPHome sidecar's own template buttons (same slugs, so any future
// analysis joining against old Sheets data doesn't need a mapping table).
// A closed set rather than free-text event labels, deliberately: this is
// the data-gathering phase's whole point (see the repo root CLAUDE.md) --
// a consistent, known taxonomy is what makes burns comparable to each
// other later, which free-text annotations from a rushed tap at the stove
// would not reliably give.
typedef struct {
  const char *slug;
  const char *label;
} woodstove_event_t;

static const woodstove_event_t EVENTS[] = {
    {"cold_start", "Cold Start"},
    {"opened_stove", "Opened Stove"},
    {"added_wood", "Added Wood"},
    {"damper_up", "Damper Up"},
    {"damper_down", "Damper Down"},
    {"burning_optimally", "Burning Optimally"},
    {"roaring", "Stove Roaring"},
    {"dying_down", "Dying Down"},
    {"fire_out", "Fire Out"},
    {"stove_off", "Stove Off"},
};
#define EVENT_COUNT (sizeof(EVENTS) / sizeof(EVENTS[0]))

// Returns EVENTS[]'s own canonical slug pointer on a match (so callers can
// hang onto it, e.g. in the event log ring buffer below, without copying),
// or NULL if `slug` isn't one of the fixed taxonomy.
static const char *event_slug_canonical(const char *slug) {
  for (size_t i = 0; i < EVENT_COUNT; i++) {
    if (strcmp(EVENTS[i].slug, slug) == 0) {
      return EVENTS[i].slug;
    }
  }
  return NULL;
}

// A small separate ring buffer of *logged* events (as opposed to EVENTS[]
// above, the fixed menu of what's loggable) -- GET /api/history includes
// these as markers on the 24h graph, matching the design mockup's dots on
// the curve. 64 comfortably covers a burn's worth of taps; older ones just
// age out, same as the sensor history ring buffer.
#define EVENT_LOG_CAPACITY 64
typedef struct {
  uint32_t uptime_s;
  const char *slug;  // points into EVENTS[]'s own static strings, never freed
} logged_event_t;
static logged_event_t s_event_log[EVENT_LOG_CAPACITY];
static size_t s_event_log_count = 0;
static size_t s_event_log_head = 0;

static void event_log_push(const char *slug) {
  s_event_log[s_event_log_head] = (logged_event_t){
      .uptime_s = (uint32_t) (esp_timer_get_time() / 1000000),
      .slug = slug,
  };
  s_event_log_head = (s_event_log_head + 1) % EVENT_LOG_CAPACITY;
  if (s_event_log_count < EVENT_LOG_CAPACITY) {
    s_event_log_count++;
  }
}

static esp_err_t events_get_handler(httpd_req_t *req) {
  cJSON *root = cJSON_CreateArray();
  for (size_t i = 0; i < EVENT_COUNT; i++) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "slug", EVENTS[i].slug);
    cJSON_AddStringToObject(item, "label", EVENTS[i].label);
    cJSON_AddItemToArray(root, item);
  }

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

static const httpd_uri_t events_get_uri = {
    .uri = "/api/events",
    .method = HTTP_GET,
    .handler = events_get_handler,
};

// Queues an immediate, un-gated Sheets log tagged with the given event slug
// -- see sheets_logger_log_event()'s own doc comment for why this bypasses
// the periodic deadband/heartbeat check and doesn't reset its clock.
static esp_err_t event_post_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len >= 128) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
    return ESP_FAIL;
  }

  char body[128] = {0};
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

  cJSON *item = cJSON_GetObjectItem(root, "event");
  const char *canonical = cJSON_IsString(item) ? event_slug_canonical(item->valuestring) : NULL;
  cJSON_Delete(root);
  if (canonical == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown event -- see GET /api/events for valid slugs");
    return ESP_FAIL;
  }

  sheets_logger_log_event(canonical);
  event_log_push(canonical);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  return ESP_OK;
}

static const httpd_uri_t event_post_uri = {
    .uri = "/api/event",
    .method = HTTP_POST,
    .handler = event_post_handler,
};

// Backs the web UI's 24h graph. Samples are downsampled to ~4min
// resolution (see sensors.h's history_sample_t) and events are the last
// EVENT_LOG_CAPACITY logged annotations, both returned oldest-first as
// compact [age_s, ...] arrays rather than repeated-key objects, to keep
// this cheap to build even at (a still-small) 360+64 entries. age_s is
// seconds before "now" (this response's own timestamp), not a raw uptime
// value the client would otherwise need this device's boot time to
// interpret.
static esp_err_t history_get_handler(httpd_req_t *req) {
  uint32_t now_s = (uint32_t) (esp_timer_get_time() / 1000000);

  static history_sample_t samples[SENSORS_HISTORY_CAPACITY];
  size_t sample_count = sensors_get_history(samples, SENSORS_HISTORY_CAPACITY);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "now_s", now_s);

  cJSON *samples_json = cJSON_CreateArray();
  for (size_t i = 0; i < sample_count; i++) {
    cJSON *point = cJSON_CreateArray();
    cJSON_AddItemToArray(point, cJSON_CreateNumber(now_s - samples[i].uptime_s));
    cJSON_AddItemToArray(point, cJSON_CreateNumber(round_to(samples[i].thermocouple_c, 0.1)));
    cJSON_AddItemToArray(point, cJSON_CreateNumber(round_to(samples[i].thermocouple_rate_c_per_min, 0.01)));
    cJSON_AddItemToArray(point, cJSON_CreateNumber(samples[i].thermocouple_zone));
    cJSON_AddItemToArray(samples_json, point);
  }
  cJSON_AddItemToObject(root, "samples", samples_json);

  cJSON *events_json = cJSON_CreateArray();
  size_t event_start = s_event_log_count < EVENT_LOG_CAPACITY ? 0 : s_event_log_head;
  for (size_t i = 0; i < s_event_log_count; i++) {
    logged_event_t *e = &s_event_log[(event_start + i) % EVENT_LOG_CAPACITY];
    cJSON *point = cJSON_CreateArray();
    cJSON_AddItemToArray(point, cJSON_CreateNumber(now_s - e->uptime_s));
    cJSON_AddItemToArray(point, cJSON_CreateString(e->slug));
    cJSON_AddItemToArray(events_json, point);
  }
  cJSON_AddItemToObject(root, "events", events_json);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

static const httpd_uri_t history_get_uri = {
    .uri = "/api/history",
    .method = HTTP_GET,
    .handler = history_get_handler,
};

// Explicit, not just relying on HTTPD_DEFAULT_CONFIG()'s own default (also
// 7) -- rest_api_log_socket_usage() below needs to size its client_fds
// buffer to match whatever this is actually set to.
#define REST_API_MAX_OPEN_SOCKETS 7

static httpd_handle_t s_server = NULL;

httpd_handle_t rest_api_start(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;  // shared with ota_server's esp_ota_* calls, which need more than the 4096 default
  config.max_uri_handlers = 16;  // default (8) is too few once ota + web_ui + this project's own endpoints are all registered
  config.max_open_sockets = REST_API_MAX_OPEN_SOCKETS;

  ESP_LOGI(TAG, "Starting REST API server on port %d", config.server_port);
  ESP_ERROR_CHECK(httpd_start(&server, &config));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reading_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_get_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_get_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_post_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &events_get_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &event_post_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &history_get_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &web_ui_html_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &web_ui_js_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &web_ui_css_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &web_ui_alpine_js_uri));
  s_server = server;
  return server;
}

// Diagnostic for chasing an apparent httpd hang (see CLAUDE.md's "The REST
// API server is single-threaded..." section) -- logs the current open
// socket count against REST_API_MAX_OPEN_SOCKETS, plus each fd, so a
// leak/exhaustion pattern (count climbing to the max and staying pinned
// there) is visible in the serial log over time, distinguishing it from a
// single genuinely-wedged request (which would show a low, stable count).
// Called periodically from main.c's sensor_log_task -- no dedicated timer,
// piggybacking its existing ~30s cadence.
void rest_api_log_socket_usage(void) {
  if (s_server == NULL) {
    return;
  }
  int client_fds[REST_API_MAX_OPEN_SOCKETS];
  size_t count = REST_API_MAX_OPEN_SOCKETS;
  esp_err_t err = httpd_get_client_list(s_server, &count, client_fds);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "httpd_get_client_list failed: %s", esp_err_to_name(err));
    return;
  }

  char fds_str[REST_API_MAX_OPEN_SOCKETS * 5 + 1] = {0};
  size_t pos = 0;
  for (size_t i = 0; i < count && pos < sizeof(fds_str) - 5; i++) {
    pos += snprintf(fds_str + pos, sizeof(fds_str) - pos, "%d ", client_fds[i]);
  }
  ESP_LOGI(TAG, "Open sockets: %u/%d [%s]", (unsigned) count, REST_API_MAX_OPEN_SOCKETS, fds_str);
}
