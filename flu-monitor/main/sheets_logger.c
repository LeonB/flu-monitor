#include "sheets_logger.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "sensors.h"
#include "settings.h"

static const char *TAG = "sheets_logger";

// Far outside any real reading, so the first tick after boot always logs
// (delta_due wins the reason tag) -- same sentinel value/reasoning as the
// ESPHome sidecar's own last_logged_thermocouple_c global.
#define NO_PRIOR_LOG_C  -1000.0f

#define EVENT_LABEL_MAX_LEN  32
#define URL_BUF_SIZE         512

// A wall clock that hasn't synced via SNTP yet (main.c's start_sntp()) reads
// an epoch near 0, since this chip has no battery-backed RTC to carry real
// time across a reboot -- anything before this cutoff (2024-01-01, comfortably
// before this project existed) means "not synced yet," not "it's actually
// 1970." See REVIEW.md finding D4: age_ms alone only corrects for queue
// backlog, not the webhook request's own latency (documented up to 40+s);
// an absolute timestamp sidesteps that entirely once the clock is synced.
#define MIN_VALID_EPOCH_S  1704067200

static bool wall_clock_synced(void) {
  return time(NULL) >= MIN_VALID_EPOCH_S;
}

static float s_last_logged_thermocouple_c = NO_PRIOR_LOG_C;
static int64_t s_last_log_us = 0;

// Captures the webhook's response body (Code.gs always replies with the
// bare text "ok" or "forbidden", never JSON) -- esp_http_client_perform()
// returning ESP_OK only means the HTTP exchange itself completed; Code.gs
// responds with a plain HTTP 200 even on a rejected secret (see its own
// doGet(), `return ContentService.createTextOutput("forbidden")...` sets no
// error status), so neither perform()'s result nor the status code alone
// can tell a genuinely-written row apart from a silently-rejected request.
#define WEBHOOK_RESPONSE_BUF_SIZE 16
static char s_webhook_response_buf[WEBHOOK_RESPONSE_BUF_SIZE];
static int s_webhook_response_len;

static esp_err_t webhook_http_event_handler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
    // A fresh transport connection means a fresh response is starting --
    // reset here, per hop, rather than once before the whole (possibly
    // multi-hop) esp_http_client_perform() call. Apps Script Web Apps
    // always redirect to a different host (script.googleusercontent.com),
    // which esp_http_client cannot serve over a reused connection, so this
    // fires again for that hop too -- without this, an earlier hop's own
    // response body (if the redirect response happens to carry one) could
    // get prepended to the final "ok"/"forbidden" body, failing the exact
    // match below even on a genuine success.
    s_webhook_response_len = 0;
    return ESP_OK;
  }
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    // Body data reaches this callback already de-chunked by the underlying
    // http_parser regardless of Transfer-Encoding -- esp_http_client.c's
    // http_on_body() unconditionally dispatches this event with clean,
    // already-decoded bytes. An earlier version of this handler excluded
    // chunked responses (copying an existing pattern elsewhere in this
    // codebase), which just silently discarded a real "ok" acknowledgement
    // whenever Google happened to serve it chunked -- a false failure, not
    // a safety measure; there was never anything to decode here.
    int copy_len = evt->data_len;
    if (s_webhook_response_len + copy_len >= WEBHOOK_RESPONSE_BUF_SIZE) {
      copy_len = WEBHOOK_RESPONSE_BUF_SIZE - 1 - s_webhook_response_len;
    }
    if (copy_len > 0) {
      memcpy(s_webhook_response_buf + s_webhook_response_len, evt->data, copy_len);
      s_webhook_response_len += copy_len;
    }
  }
  return ESP_OK;
}

// Carries the sensor reading captured at the moment of the actual event tap
// (see sheets_logger_log_event() below) through the queue, rather than the
// worker task re-reading sensors_get_last_reading() whenever it happens to
// dequeue this -- a backlog behind a slow webhook call previously meant the
// temperature/rate attached to an event could be from well after the tap.
// enqueued_us (monotonic) lets log_to_sheets() fall back to telling Code.gs
// how old this event already was by the time the request went out, for
// backdating the row's timestamp when the wall clock isn't synced yet.
// enqueued_epoch_s (wall clock, 0 if not synced at enqueue time) is the
// preferred, more accurate mechanism when available -- see REVIEW.md
// finding D4: the enqueued_us/age_ms scheme only corrects for queue
// backlog, not the webhook request's own latency (documented up to 40+s),
// since age_ms is necessarily computed before that request goes out.
typedef struct {
  char label[EVENT_LABEL_MAX_LEN];
  sensor_reading_t reading;
  int64_t enqueued_us;
  time_t enqueued_epoch_s;
} queued_event_t;

static QueueHandle_t s_event_queue;

// Fires the actual GET request. Always runs on this module's own task --
// Apps Script Web App latency has been observed ranging from ~1.5s to 40+s
// (see ../google-sheets-logger/README.md), and nothing else on this device (sensor
// sampling, the REST/WS servers) should ever wait on that.
//
// Two, independent ways to tell Code.gs when this actually happened, sent
// together so it can prefer the more accurate one (see REVIEW.md finding D4):
//   - event_epoch_s: an absolute Unix timestamp from this device's own
//     SNTP-synced wall clock (main.c's start_sntp()), captured once at the
//     moment of the tap/reading -- immune to any later delay, queue backlog
//     or webhook latency alike, however long this specific request takes.
//     0 if the clock hadn't synced yet when this was captured.
//   - age_ms: how long ago (relative to right now, monotonic) the
//     reading/event actually happened -- 0 for the periodic path, which logs
//     essentially immediately, but potentially tens of seconds for a queued
//     event that sat behind a slow prior webhook call. Only corrects for
//     queue backlog, not this request's own latency, since it's necessarily
//     computed before the request goes out -- the fallback for when the
//     clock isn't synced, not a full fix on its own.
// Returns whether the row was actually written -- the periodic caller uses
// this to decide whether to advance its deadband/heartbeat baseline (see
// REVIEW.md finding #4: it previously advanced unconditionally, so a failed
// upload silently suppressed every subsequent attempt until the next real
// change cleared an ever-growing threshold).
static bool log_to_sheets(const sensor_reading_t *reading, const settings_t *settings, const char *event,
                          int64_t age_ms, time_t event_epoch_s) {
  if (settings->google_sheets_webhook_url[0] == '\0') {
    ESP_LOGD(TAG, "No Google Sheets webhook configured yet -- skipping log");
    return false;
  }

  char url[URL_BUF_SIZE];
  int len = snprintf(url, sizeof(url),
                     "%s?secret=%s&thermocouple=%.1f&cold_junction=%.1f&rate=%.2f&zone=%s&event=%s&age_ms=%lld&event_ts=%lld",
                     settings->google_sheets_webhook_url, settings->google_sheets_secret, reading->thermocouple_c,
                     reading->cold_junction_c, reading->thermocouple_rate_c_per_min,
                     thermocouple_zone_name(reading->thermocouple_zone), event, (long long) age_ms,
                     (long long) event_epoch_s);
  if (len < 0 || (size_t) len >= sizeof(url)) {
    ESP_LOGW(TAG, "Webhook URL too long for the buffer -- skipping log (check google_sheets_webhook_url)");
    return false;
  }

  // webhook_http_event_handler() resets s_webhook_response_len itself, on
  // HTTP_EVENT_ON_CONNECTED -- per hop, not just once here -- so no manual
  // reset is needed before starting the request.

  // GET, not POST: Apps Script Web Apps always redirect to a
  // script.googleusercontent.com URL that only accepts GET, and
  // esp_http_client preserves the original method across that redirect
  // (unlike curl/browsers, which downgrade to GET on a redirect) -- a POST
  // here would fail on-device with HTTP 405 even though the same webhook
  // tests fine from a browser. Same reasoning as the ESPHome sidecar's own
  // http_request_async.get call -- see ../google-sheets-logger/README.md.
  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 45000,  // Apps Script's own redirect chain has been observed taking 40+s
      .buffer_size_tx = 1024,  // the default 512B isn't enough for a long deployment-ID URL + query string
      .max_redirection_count = 6,
      .event_handler = webhook_http_event_handler,
      // Without this, esp-tls has no way to verify script.google.com's
      // certificate and the connection fails outright (ESP_ERR_HTTP_CONNECT)
      // -- ESPHome's own http_request component attaches its bundled CA
      // store automatically; esp_http_client requires doing this explicitly.
      // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE (sdkconfig) provides the bundle
      // this attaches.
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);
  // ESP_OK only means the HTTP exchange completed -- Code.gs answers with a
  // plain HTTP 200 even when it rejects the request (a wrong secret returns
  // 200 "forbidden", not an error status; see its own doGet()), so the
  // status code alone can't distinguish a written row from a rejected one
  // either. Checking the actual response body against Code.gs's own "ok"
  // contract is the only way to know the row was really written.
  bool ok = err == ESP_OK && esp_http_client_get_status_code(client) == 200 && s_webhook_response_len == 2 &&
            strncmp(s_webhook_response_buf, "ok", 2) == 0;
  if (!ok) {
    // Treated as a real failure for baseline-advancement purposes (see this
    // function's own doc comment), even though it's genuinely ambiguous --
    // Apps Script writes the row before it responds, so a client-side
    // failure often still means the row landed (see
    // ../google-sheets-logger/README.md). Between the two possible wrong
    // outcomes -- an occasional duplicate row if it actually landed, or a
    // permanently silent gap if it didn't and this weren't retried -- a
    // duplicate is far easier to spot and discard in analysis later than a
    // gap that looks identical to "nothing happened."
    ESP_LOGW(TAG, "Google Sheets log failed (event=%s): %s -- row may still have landed regardless", event,
             esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Logged to Google Sheets (event=%s, status=%d)", event, esp_http_client_get_status_code(client));
  }
  esp_http_client_cleanup(client);
  return ok;
}

bool sheets_logger_log_event(const char *event) {
  queued_event_t item = {0};
  strlcpy(item.label, event, sizeof(item.label));
  // Captured now, synchronously, at the moment of the actual tap -- not
  // whenever sheets_logger_task gets around to dequeuing this (see this
  // function's own doc comment in sheets_logger.h).
  sensors_get_last_reading(&item.reading);
  item.enqueued_us = esp_timer_get_time();
  item.enqueued_epoch_s = wall_clock_synced() ? time(NULL) : 0;
  if (xQueueSend(s_event_queue, &item, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Event queue full, dropping event log: %s", event);
    return false;
  }
  return true;
}

static void sheets_logger_task(void *arg) {
  (void) arg;

  while (true) {
    queued_event_t item;
    if (xQueueReceive(s_event_queue, &item, pdMS_TO_TICKS(30000)) == pdTRUE) {
      // An event-triggered log (Milestone 7's future POST /api/event) --
      // immediate, bypasses the deadband/heartbeat check entirely below,
      // and deliberately does not touch s_last_logged_thermocouple_c /
      // s_last_log_us, matching the ESPHome sidecar's own button-press
      // behavior: a deliberate annotation, not a routine sample, so it
      // doesn't delay or restart the next scheduled heartbeat/deadband
      // check either.
      settings_t settings;
      settings_get(&settings);
      int64_t age_ms = (esp_timer_get_time() - item.enqueued_us) / 1000;
      log_to_sheets(&item.reading, &settings, item.label, age_ms, item.enqueued_epoch_s);
      continue;
    }

    // Timed out waiting for an event -- this is the periodic check.
    sensor_reading_t reading;
    sensors_get_last_reading(&reading);
    if (!reading.thermocouple_ok) {
      continue;  // sidecar's own sensor read failed this cycle; nothing to log
    }

    // Same sanity clamp as the regression window's own gate (see
    // sensors.c) -- must run before touching s_last_logged_thermocouple_c,
    // since a garbage value admitted as the new deadband baseline would
    // make every subsequent *real* reading look like a huge jump and
    // cascade into a burst of bogus logs. This happened once on the
    // original ESPHome sidecar before its own equivalent clamp was added
    // (four temp_change rows logged in under two minutes off of one bad
    // reading).
    if (!thermocouple_reading_plausible(reading.thermocouple_c)) {
      ESP_LOGW(TAG, "Ignoring implausible thermocouple reading for Sheets logging: %.1f C", reading.thermocouple_c);
      continue;
    }

    settings_t settings;
    settings_get(&settings);

    int64_t now_us = esp_timer_get_time();
    float delta = fabsf(reading.thermocouple_c - s_last_logged_thermocouple_c);
    bool delta_due = delta >= settings.thermocouple_deadband_c;
    bool heartbeat_due = (now_us - s_last_log_us) >= (int64_t) settings.log_heartbeat_min * 60 * 1000000;
    if (!delta_due && !heartbeat_due) {
      continue;
    }

    time_t event_epoch_s = wall_clock_synced() ? time(NULL) : 0;
    if (log_to_sheets(&reading, &settings, delta_due ? "temp_change" : "heartbeat", 0, event_epoch_s)) {
      s_last_logged_thermocouple_c = reading.thermocouple_c;
      s_last_log_us = now_us;
    }
    // On failure, deliberately leave both untouched -- delta_due/heartbeat_due
    // will keep re-triggering on the very next tick until a log actually
    // succeeds, rather than silently absorbing the miss (see REVIEW.md
    // finding #4).
  }
}

void sheets_logger_init(void) {
  s_event_queue = xQueueCreate(4, sizeof(queued_event_t));
  xTaskCreate(sheets_logger_task, "sheets_logger", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}
