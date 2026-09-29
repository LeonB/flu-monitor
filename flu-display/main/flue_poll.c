#include "flue_poll.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_display.h"
#include "mdns.h"

static const char *TAG = "flue_poll";

// Same reasoning as the sidecar's own sanity clamp (see CLAUDE.md; the
// original ESPHome flu-monitor.yaml had the identical clamp before this
// project's ESP-IDF rewrite retired it): a stovepipe has no business
// reading outside this range, so anything outside it is a corrupted/failed
// reading, not real data -- and must not become the new rate-of-change
// baseline. The sidecar's own raw thermocouple_c is reported as-read
// regardless of plausibility (only its *derived* rate/zone are protected --
// see ../flu-monitor/CLAUDE.md), so this check is still this device's own
// responsibility.
#define SANITY_MIN_C  -40.0f
#define SANITY_MAX_C  600.0f

// The sanity clamp above only catches *wildly* wrong values -- it has to
// stay wide, since a real overfire could genuinely reach a few hundred
// degrees. A single corrupted/glitched reading landing inside that wide
// range sails straight through otherwise, briefly flashing the display to
// a misleading color (observed: an oven at ~150C spiking to a bogus 250C+
// reading, self-correcting on the very next poll a few seconds later).
// A jump bigger than this in a single broadcast isn't trusted immediately
// -- it's held pending, and only accepted once the *next* broadcast also
// jumps beyond SUSPICIOUS_JUMP_C (from the last confirmed value) in the
// *same direction*. A one-off glitch typically self-corrects back toward
// the prior value on the next broadcast -- the opposite direction -- so it
// gets discarded rather than confirmed; a real sustained rise or fall keeps
// going the same way and confirms within one extra cycle. (An earlier
// version compared the new reading's *magnitude* against the held pending
// value instead of checking direction -- that broke down for any steady,
// roughly-linear trend, since each newly-held reading differs from the
// previous one by close to the same per-tick delta every time, which never
// closes to within tolerance -- see REVIEW.md finding #2.)
#define SUSPICIOUS_JUMP_C     15.0f

#define HTTP_TIMEOUT_MS         5000
#define URL_BUF_SIZE            64
#define HTTP_RESPONSE_BUF_SIZE  512  // must comfortably fit a settings response with a full webhook URL + secret

#define WS_MSG_BUF_SIZE  512

static char s_response_buf[HTTP_RESPONSE_BUF_SIZE];
static int s_response_len;

static bool s_have_prev = false;
static float s_prev_temperature_c;

static bool s_have_pending = false;
static bool s_pending_rising;  // direction of the first held-pending jump, to confirm a continued trend

static bool s_have_ever_valid = false;
static int64_t s_last_valid_us = 0;

static char s_ws_msg_buf[WS_MSG_BUF_SIZE];
static int s_ws_msg_len = 0;

static SemaphoreHandle_t s_refetch_settings_sem;

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

// Resolves FLU_MONITOR_MDNS_NAME via an explicit mDNS query rather than
// relying on the implicit ".local" resolution hook that esp_http_client (or
// esp_websocket_client) would otherwise trigger on its own by parsing the
// hostname out of the URL. That implicit path worked in initial testing but
// proved unreliable on a later boot (consistent ESP_ERR_HTTP_CONNECT /
// getaddrinfo failures even though the Mac and other clients resolved the
// same hostname fine at the same time) -- see CLAUDE.md's "flu-display"
// section. mdns_query_a() itself is also somewhat flaky poll-to-poll on
// this network (weak/variable RSSI observed, plausibly lossy multicast),
// so this is only called once at boot to get an initial address -- not on
// every reconnect, unlike the reading itself the address rarely changes on
// a home network, and the WS client's own auto-reconnect handles a
// transient drop against the same address.
static bool resolve_sidecar(esp_ip4_addr_t *out_addr) {
  esp_err_t err = mdns_query_a(FLU_MONITOR_MDNS_NAME, MDNS_QUERY_TIMEOUT_MS, out_addr);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NOT_FOUND) {
      ESP_LOGW(TAG, "mDNS query for '%s' got no response", FLU_MONITOR_MDNS_NAME);
    } else {
      ESP_LOGW(TAG, "mDNS query for '%s' failed: %s", FLU_MONITOR_MDNS_NAME, esp_err_to_name(err));
    }
    return false;
  }
  return true;
}

// GETs flu-monitor's GET /api/settings and applies the zone/rate
// thresholds it returns -- see led_display_set_thresholds()'s own doc
// comment for why this matters (a threshold changed via the sidecar's REST
// API should actually move the ring's gradient, not just its own
// classification).
static void fetch_and_apply_settings(const esp_ip4_addr_t *ip) {
  char url[URL_BUF_SIZE];
  snprintf(url, sizeof(url), "http://" IPSTR "/api/settings", IP2STR(ip));

  s_response_len = 0;

  esp_http_client_config_t config = {
      .url = url,
      .event_handler = http_event_handler,
      .timeout_ms = HTTP_TIMEOUT_MS,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "GET /api/settings failed: %s", esp_err_to_name(err));
    return;
  }
  if (status != 200) {
    ESP_LOGW(TAG, "GET /api/settings returned status %d", status);
    return;
  }

  cJSON *root = cJSON_ParseWithLength(s_response_buf, s_response_len);
  if (root == NULL) {
    ESP_LOGW(TAG, "Failed to parse /api/settings response: '%.*s'", s_response_len, s_response_buf);
    return;
  }

  cJSON *zone_cold_max_c = cJSON_GetObjectItemCaseSensitive(root, "zone_cold_max_c");
  cJSON *zone_optimal_max_c = cJSON_GetObjectItemCaseSensitive(root, "zone_optimal_max_c");
  cJSON *fast_rise_c_per_min = cJSON_GetObjectItemCaseSensitive(root, "fast_rise_c_per_min");
  cJSON *rate_deadband_c_per_min = cJSON_GetObjectItemCaseSensitive(root, "rate_deadband_c_per_min");
  cJSON *idle_pulse_period_ms = cJSON_GetObjectItemCaseSensitive(root, "idle_pulse_period_ms");
  cJSON *fast_pulse_period_ms = cJSON_GetObjectItemCaseSensitive(root, "fast_pulse_period_ms");
  cJSON *color_transition_exponent = cJSON_GetObjectItemCaseSensitive(root, "color_transition_exponent");
  if (!cJSON_IsNumber(zone_cold_max_c) || !cJSON_IsNumber(zone_optimal_max_c) || !cJSON_IsNumber(fast_rise_c_per_min) ||
      !cJSON_IsNumber(rate_deadband_c_per_min) || !cJSON_IsNumber(idle_pulse_period_ms) ||
      !cJSON_IsNumber(fast_pulse_period_ms) || !cJSON_IsNumber(color_transition_exponent)) {
    ESP_LOGW(TAG, "/api/settings response missing expected numeric fields: '%.*s'", s_response_len, s_response_buf);
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG,
           "Applying settings: zone_cold_max_c=%.1f zone_optimal_max_c=%.1f fast_rise_c_per_min=%.1f "
           "rate_deadband_c_per_min=%.1f idle_pulse_period_ms=%.0f fast_pulse_period_ms=%.0f "
           "color_transition_exponent=%.1f",
           zone_cold_max_c->valuedouble, zone_optimal_max_c->valuedouble, fast_rise_c_per_min->valuedouble,
           rate_deadband_c_per_min->valuedouble, idle_pulse_period_ms->valuedouble, fast_pulse_period_ms->valuedouble,
           color_transition_exponent->valuedouble);
  led_display_set_tuning((float) zone_cold_max_c->valuedouble, (float) zone_optimal_max_c->valuedouble,
                         (float) fast_rise_c_per_min->valuedouble, (float) rate_deadband_c_per_min->valuedouble,
                         (uint32_t) idle_pulse_period_ms->valuedouble, (uint32_t) fast_pulse_period_ms->valuedouble,
                         (float) color_transition_exponent->valuedouble);

  cJSON_Delete(root);
}

// Applies the same sanity-clamp + suspicious-jump-confirmation gating the
// old REST-polling version used, then hands a validated reading straight to
// led_display_set_reading().
static void handle_reading(cJSON *reading) {
  cJSON *ok = cJSON_GetObjectItemCaseSensitive(reading, "thermocouple_ok");
  cJSON *temperature_c_item = cJSON_GetObjectItemCaseSensitive(reading, "thermocouple_c");
  cJSON *rate_item = cJSON_GetObjectItemCaseSensitive(reading, "thermocouple_rate_c_per_min");
  if (!cJSON_IsTrue(ok) || !cJSON_IsNumber(temperature_c_item) || !cJSON_IsNumber(rate_item)) {
    return;  // sidecar itself couldn't read the thermocouple this cycle -- nothing new to show
  }

  float temperature_c = (float) temperature_c_item->valuedouble;
  float rate_c_per_min = (float) rate_item->valuedouble;

  if (temperature_c < SANITY_MIN_C || temperature_c > SANITY_MAX_C) {
    ESP_LOGW(TAG, "Rejecting implausible reading: %.1f C", temperature_c);
    return;
  }

  if (s_have_prev) {
    float jump = fabsf(temperature_c - s_prev_temperature_c);
    if (jump > SUSPICIOUS_JUMP_C) {
      bool rising = temperature_c > s_prev_temperature_c;
      bool confirmed = s_have_pending && rising == s_pending_rising;
      if (!confirmed) {
        ESP_LOGW(TAG, "Holding suspicious jump for confirmation: %.1f C (last confirmed %.1f C)", temperature_c,
                  s_prev_temperature_c);
        s_pending_rising = rising;
        s_have_pending = true;
        return;
      }
      // A second consecutive jump beyond SUSPICIOUS_JUMP_C, continuing the
      // same direction as the first -- a real sustained trend, confirmed
      // and accepted immediately (not the held reading, this newer one).
    }
    s_have_pending = false;
  }

  ESP_LOGI(TAG, "Thermocouple: %.1f C (rate %.2f C/min)", temperature_c, rate_c_per_min);

  s_have_prev = true;
  s_prev_temperature_c = temperature_c;
  s_have_ever_valid = true;
  s_last_valid_us = esp_timer_get_time();

  led_display_set_reading(true, temperature_c, rate_c_per_min);
}

static void handle_ws_message(const char *json, int len) {
  cJSON *root = cJSON_ParseWithLength(json, len);
  if (root == NULL) {
    ESP_LOGW(TAG, "Failed to parse WS message: '%.*s'", len, json);
    return;
  }

  cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (cJSON_IsString(type) && strcmp(type->valuestring, "reading") == 0) {
    cJSON *reading = cJSON_GetObjectItemCaseSensitive(root, "reading");
    if (cJSON_IsObject(reading)) {
      handle_reading(reading);
    }
  } else if (cJSON_IsString(type) && strcmp(type->valuestring, "settings_changed") == 0) {
    // Deferred to settings_task -- a blocking HTTP GET has no business
    // running directly on the WS client's own event-callback context.
    xSemaphoreGive(s_refetch_settings_sem);
  }

  cJSON_Delete(root);
}

static void ws_event_handler(void *handler_arg, esp_event_base_t base, int32_t event_id, void *event_data) {
  (void) handler_arg;
  (void) base;
  esp_websocket_event_data_t *data = event_data;

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WebSocket connected");
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "WebSocket disconnected -- auto-reconnect will retry");
      break;
    case WEBSOCKET_EVENT_DATA:
      // op_code 1 = text frame; ignore ping/pong/close housekeeping frames,
      // which also surface here with op_code 0/9/10/8.
      if (data->op_code != 1 || data->data_len <= 0) {
        break;
      }
      // Reassemble a payload split across multiple events (won't normally
      // happen at this message size, but payload_len > buffer_size would
      // cause it) -- accumulate by payload_offset, dispatch once complete.
      if (data->payload_offset == 0) {
        s_ws_msg_len = 0;
      }
      int copy_len = data->data_len;
      if (s_ws_msg_len + copy_len >= WS_MSG_BUF_SIZE) {
        copy_len = WS_MSG_BUF_SIZE - 1 - s_ws_msg_len;
      }
      if (copy_len > 0) {
        memcpy(s_ws_msg_buf + s_ws_msg_len, data->data_ptr, copy_len);
        s_ws_msg_len += copy_len;
      }
      if (data->payload_offset + data->data_len >= data->payload_len) {
        handle_ws_message(s_ws_msg_buf, s_ws_msg_len);
      }
      break;
    default:
      break;
  }
}

// Resolves the sidecar (retrying indefinitely -- it may simply not have
// booted yet, a real race at power-on with no reason to give up), fetches
// its initial settings, connects the WebSocket client, then just waits for
// "settings changed" broadcasts to re-fetch. Runs for the lifetime of the
// device; everything after the first connect is event-driven.
static void settings_task(void *arg) {
  (void) arg;

  esp_ip4_addr_t ip;
  while (!resolve_sidecar(&ip)) {
    vTaskDelay(pdMS_TO_TICKS(MDNS_QUERY_TIMEOUT_MS));
  }
  ESP_LOGI(TAG, "Resolved %s to " IPSTR, FLU_MONITOR_MDNS_NAME, IP2STR(&ip));

  fetch_and_apply_settings(&ip);

  char ws_uri[URL_BUF_SIZE];
  snprintf(ws_uri, sizeof(ws_uri), "ws://" IPSTR "/ws", IP2STR(&ip));

  esp_websocket_client_config_t ws_config = {
      .uri = ws_uri,
  };
  esp_websocket_client_handle_t ws_client = esp_websocket_client_init(&ws_config);
  esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
  esp_websocket_client_start(ws_client);

  while (true) {
    xSemaphoreTake(s_refetch_settings_sem, portMAX_DELAY);
    ESP_LOGI(TAG, "Settings changed -- re-fetching");
    fetch_and_apply_settings(&ip);
  }
}

void flue_poll_init(void) {
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set("flu-display"));
  ESP_ERROR_CHECK(mdns_instance_name_set("flu-display"));

  s_refetch_settings_sem = xSemaphoreCreateBinary();
  xTaskCreate(settings_task, "flue_settings", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}

bool flue_poll_is_stale(void) {
  if (!s_have_ever_valid) {
    return true;
  }
  return (esp_timer_get_time() - s_last_valid_us) > (int64_t) STALE_READING_MS * 1000;
}
