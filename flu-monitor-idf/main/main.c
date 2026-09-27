// flu-monitor-idf: Milestone 1 -- WiFi (with stored credentials) + captive
// portal fallback (with network scan and a "last attempt failed" state) +
// mDNS + OTA. Milestone 2 -- BMP581 + MCP9601 sensors. Milestone 3 --
// NVS-backed settings, rate/zone regression, and a REST API (GET
// /api/reading, GET+POST /api/settings) once connected to real WiFi. See
// the repo root CLAUDE.md and the approved project plan for the full
// context and architecture rationale.

#include <stdbool.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "captive_portal.h"
#include "config.h"
#include "ota_server.h"
#include "rest_api.h"
#include "sensors.h"
#include "settings.h"
#include "wifi_setup.h"

static const char *TAG = "main";

// Runs independent of WiFi state (both the connected idle loop and the
// captive-portal branch below keep this alive) -- verified by eyeballing
// this log against the existing ESPHome sidecar's readings, not by anything
// reachable over the network until WiFi connects and rest_api_start() runs.
static void sensor_log_task(void *arg) {
  (void) arg;
  while (true) {
    sensor_reading_t reading;
    sensors_read(&reading);

    if (reading.bmp581_ok) {
      ESP_LOGI(TAG, "BMP581: %.2f C, %.1f Pa", reading.bmp581_temperature_c, reading.bmp581_pressure_pa);
    } else {
      ESP_LOGW(TAG, "BMP581: read failed");
    }

    if (reading.thermocouple_ok) {
      ESP_LOGI(TAG, "MCP9601: thermocouple %.2f C, cold junction %.2f C, rate %.2f C/min, zone %s",
               reading.thermocouple_c, reading.cold_junction_c, reading.thermocouple_rate_c_per_min,
               thermocouple_zone_name(reading.thermocouple_zone));
    } else {
      ESP_LOGW(TAG, "MCP9601: read failed");
    }

    // Matches the ESPHome sidecar's own 30s update_interval for a fair
    // side-by-side comparison.
    vTaskDelay(pdMS_TO_TICKS(30000));
  }
}

static void init_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

static void start_mdns(void) {
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
  ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_HOSTNAME));
}

void app_main(void) {
  init_nvs();
  settings_init();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  if (sensors_init() != ESP_OK) {
    ESP_LOGW(TAG, "No sensors found at all -- continuing anyway (Milestone 2 verification build)");
  }
  xTaskCreate(sensor_log_task, "sensor_log", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);

  char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
  char pass[WIFI_PASS_MAX_LEN + 1] = {0};
  bool have_creds = wifi_creds_load(ssid, pass);

  bool connected = false;
  if (have_creds) {
    ESP_LOGI(TAG, "Found stored WiFi credentials for '%s'", ssid);
    connected = wifi_sta_try_connect(ssid, pass, STA_CONNECT_TIMEOUT_MS);
  } else {
    ESP_LOGI(TAG, "No stored WiFi credentials");
  }

  if (connected) {
    ESP_LOGI(TAG, "Connected. Starting mDNS + OTA.");

    // Confirms this image works well enough to join WiFi, canceling the
    // bootloader's rollback timer for it -- see sdkconfig.defaults for the
    // rollback config this depends on.
    esp_ota_mark_app_valid_cancel_rollback();

    // Clears any stale failure flag from an earlier, unrelated attempt --
    // the captive portal's own /save handler already clears this when a
    // *new* attempt starts, but a successful connect is also a clean
    // signal that whatever was recorded before no longer applies.
    wifi_clear_attempt_failed();

    start_mdns();
    httpd_handle_t server = rest_api_start();
    ota_server_register(server);

    while (true) {
      vTaskDelay(pdMS_TO_TICKS(10000));
      ESP_LOGI(TAG, "(still connected, idling -- sensor_log_task has the actual readings)");
    }
  } else {
    if (have_creds) {
      ESP_LOGW(TAG, "Failed to join '%s' -- recording for the setup portal's failure screen", ssid);
      wifi_mark_attempt_failed(ssid);
    }
    ESP_LOGW(TAG, "Not connected -- starting setup access point + captive portal");
    captive_portal_start(SETUP_AP_SSID, SETUP_AP_PASSWORD, SETUP_AP_MAX_CONN, STA_CONNECT_TIMEOUT_MS);
  }
}
