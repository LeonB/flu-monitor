#include "sheets_logger.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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

static float s_last_logged_thermocouple_c = NO_PRIOR_LOG_C;
static int64_t s_last_log_us = 0;

static QueueHandle_t s_event_queue;

// Fires the actual GET request. Always runs on this module's own task --
// Apps Script Web App latency has been observed ranging from ~1.5s to 40+s
// (see ../flu-monitor/CLAUDE.md), and nothing else on this device (sensor
// sampling, the REST/WS servers) should ever wait on that.
static void log_to_sheets(const sensor_reading_t *reading, const settings_t *settings, const char *event) {
  if (settings->google_sheets_webhook_url[0] == '\0') {
    ESP_LOGD(TAG, "No Google Sheets webhook configured yet -- skipping log");
    return;
  }

  char url[URL_BUF_SIZE];
  int len = snprintf(url, sizeof(url),
                     "%s?secret=%s&temperature=%.1f&pressure=%.0f&thermocouple=%.1f&cold_junction=%.1f&rate=%.2f&"
                     "zone=%s&event=%s",
                     settings->google_sheets_webhook_url, settings->google_sheets_secret,
                     reading->bmp581_temperature_c, reading->bmp581_pressure_pa, reading->thermocouple_c,
                     reading->cold_junction_c, reading->thermocouple_rate_c_per_min,
                     thermocouple_zone_name(reading->thermocouple_zone), event);
  if (len < 0 || (size_t) len >= sizeof(url)) {
    ESP_LOGW(TAG, "Webhook URL too long for the buffer -- skipping log (check google_sheets_webhook_url)");
    return;
  }

  // GET, not POST: Apps Script Web Apps always redirect to a
  // script.googleusercontent.com URL that only accepts GET, and
  // esp_http_client preserves the original method across that redirect
  // (unlike curl/browsers, which downgrade to GET on a redirect) -- a POST
  // here would fail on-device with HTTP 405 even though the same webhook
  // tests fine from a browser. Same reasoning as the ESPHome sidecar's own
  // http_request_async.get call -- see ../flu-monitor/CLAUDE.md.
  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 45000,  // Apps Script's own redirect chain has been observed taking 40+s
      .buffer_size_tx = 1024,  // the default 512B isn't enough for a long deployment-ID URL + query string
      .max_redirection_count = 6,
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
  if (err != ESP_OK) {
    // Cosmetic, not necessarily a real failure -- Apps Script writes the
    // row before it responds, so a logged client-side failure often still
    // means the row landed (see ../flu-monitor/CLAUDE.md).
    ESP_LOGW(TAG, "Google Sheets log failed (event=%s): %s -- row usually still lands regardless", event,
             esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Logged to Google Sheets (event=%s, status=%d)", event, esp_http_client_get_status_code(client));
  }
  esp_http_client_cleanup(client);
}

void sheets_logger_log_event(const char *event) {
  char label[EVENT_LABEL_MAX_LEN] = {0};
  strlcpy(label, event, sizeof(label));
  if (xQueueSend(s_event_queue, label, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Event queue full, dropping event log: %s", event);
  }
}

static void sheets_logger_task(void *arg) {
  (void) arg;

  while (true) {
    char event_label[EVENT_LABEL_MAX_LEN];
    if (xQueueReceive(s_event_queue, event_label, pdMS_TO_TICKS(30000)) == pdTRUE) {
      // An event-triggered log (Milestone 7's future POST /api/event) --
      // immediate, bypasses the deadband/heartbeat check entirely below,
      // and deliberately does not touch s_last_logged_thermocouple_c /
      // s_last_log_us, matching the ESPHome sidecar's own button-press
      // behavior: a deliberate annotation, not a routine sample, so it
      // doesn't delay or restart the next scheduled heartbeat/deadband
      // check either.
      sensor_reading_t reading;
      sensors_get_last_reading(&reading);
      settings_t settings;
      settings_get(&settings);
      log_to_sheets(&reading, &settings, event_label);
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
    // cascade into a burst of bogus logs (this happened once upstream in
    // the ESPHome build before its own clamp was added -- see
    // ../flu-monitor/CLAUDE.md).
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

    log_to_sheets(&reading, &settings, delta_due ? "temp_change" : "heartbeat");
    s_last_logged_thermocouple_c = reading.thermocouple_c;
    s_last_log_us = now_us;
  }
}

void sheets_logger_init(void) {
  s_event_queue = xQueueCreate(4, EVENT_LABEL_MAX_LEN);
  xTaskCreate(sheets_logger_task, "sheets_logger", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}
