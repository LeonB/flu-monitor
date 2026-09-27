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
#include "mdns.h"

static const char *TAG = "flue_poll";

// flu-monitor's web_server exposes each sensor at /sensor/<exact entity
// NAME, URL-encoded>, not its object_id -- verified against the live
// device: GET .../sensor/Thermocouple%20Temperature returns
// {"id":"sensor/Thermocouple Temperature","value":25,"state":"25.0 °C"}.
#define THERMOCOUPLE_PATH "/sensor/Thermocouple%20Temperature"

// Same reasoning as flu-monitor.yaml's own sanity clamp (see CLAUDE.md): a
// stovepipe has no business reading outside this range, so anything outside
// it is a corrupted/failed reading, not real data -- and must not become the
// new rate-of-change baseline.
#define SANITY_MIN_C  -40.0f
#define SANITY_MAX_C  600.0f

// The sanity clamp above only catches *wildly* wrong values -- it has to
// stay wide, since a real overfire could genuinely reach a few hundred
// degrees. A single corrupted/glitched reading landing inside that wide
// range sails straight through otherwise, briefly flashing the display to
// a misleading color (observed: an oven at ~150C spiking to a bogus 250C+
// reading, self-correcting on the very next poll a few seconds later).
// A jump bigger than this in a single poll interval isn't trusted
// immediately -- it's held pending, and only accepted once the *next* poll
// agrees with it too (within CONFIRM_TOLERANCE_C), so a one-off glitch
// (which typically self-corrects by the next poll) never reaches the
// display, while a real fast change still shows up within one extra poll.
#define SUSPICIOUS_JUMP_C     15.0f
#define CONFIRM_TOLERANCE_C   10.0f

#define HTTP_RESPONSE_BUF_SIZE  256
#define HTTP_TIMEOUT_MS         5000
#define URL_BUF_SIZE            64

static char s_response_buf[HTTP_RESPONSE_BUF_SIZE];
static int s_response_len;

static bool s_have_prev = false;
static float s_prev_temperature_c;
static int64_t s_prev_time_us;

static bool s_have_pending = false;
static float s_pending_temperature_c;

static bool s_have_ip = false;
static esp_ip4_addr_t s_cached_ip;

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

// Resolves FLU_MONITOR_MDNS_NAME via an explicit mDNS query rather than
// relying on the implicit ".local" resolution hook that esp_http_client
// would otherwise trigger on its own by parsing the hostname out of the
// URL. That implicit path worked in initial testing but proved unreliable
// on a later boot (consistent ESP_ERR_HTTP_CONNECT / getaddrinfo failures
// even though the Mac and other clients resolved the same hostname fine at
// the same time) -- see CLAUDE.md's "flu-display" section.
//
// Even mdns_query_a() itself turned out flaky poll-to-poll on this network
// (weak/variable RSSI observed -38 to -62 -- plausibly lossy multicast, not
// a code bug), so this is only called to populate/refresh a cached IP, not
// on every single poll: flue_poll_once() reuses the last resolved address
// until an actual HTTP request against it fails, rather than gambling a
// fresh multicast round trip every 3 seconds.
static bool resolve_flu_monitor(esp_ip4_addr_t *out_addr) {
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

flue_reading_t flue_poll_once(void) {
  flue_reading_t result = {0};

  if (!s_have_ip) {
    if (!resolve_flu_monitor(&s_cached_ip)) {
      return result;
    }
    s_have_ip = true;
  }

  char url[URL_BUF_SIZE];
  snprintf(url, sizeof(url), "http://" IPSTR "%s", IP2STR(&s_cached_ip), THERMOCOUPLE_PATH);

  s_response_len = 0;

  esp_http_client_config_t config = {
      .url = url,
      .event_handler = http_event_handler,
      .timeout_ms = HTTP_TIMEOUT_MS,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "HTTP GET to " IPSTR " failed: %s -- will re-resolve next poll", IP2STR(&s_cached_ip),
              esp_err_to_name(err));
    esp_http_client_cleanup(client);
    s_have_ip = false;
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

  if (s_have_prev) {
    float jump = fabsf(temperature_c - s_prev_temperature_c);
    if (jump > SUSPICIOUS_JUMP_C) {
      bool confirmed = s_have_pending && fabsf(temperature_c - s_pending_temperature_c) <= CONFIRM_TOLERANCE_C;
      if (!confirmed) {
        ESP_LOGW(TAG, "Holding suspicious jump for confirmation: %.1f C (last confirmed %.1f C)", temperature_c,
                  s_prev_temperature_c);
        s_pending_temperature_c = temperature_c;
        s_have_pending = true;
        return result;
      }
    }
    s_have_pending = false;
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
