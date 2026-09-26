// flu-display: Milestone 1 (WiFi with stored credentials + captive portal
// fallback), Milestone 2 (poll flu-monitor's JSON API), and Milestone 3
// (drive the LED ring off the polled reading).
// See CLAUDE.md's "Project goal" section for the full project context, and
// the plan this was built from for the architecture rationale (ESPHome vs.
// Arduino vs. plain ESP-IDF, why plain ESP-IDF was chosen).

#include <stdbool.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "captive_portal.h"
#include "config.h"
#include "flue_poll.h"
#include "led_display.h"
#include "wifi_setup.h"

static const char *TAG = "main";

static void init_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

void app_main(void) {
  init_nvs();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

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
    ESP_LOGI(TAG, "Connected. Polling flu-monitor and driving the LED ring.");
    flue_poll_init();
    led_display_init();

    bool have_ever_valid = false;
    int64_t last_valid_us = 0;

    while (true) {
      flue_reading_t reading = flue_poll_once();
      if (reading.valid) {
        ESP_LOGI(TAG, "Thermocouple: %.1f C (rate %.2f C/min)", reading.temperature_c, reading.rate_c_per_min);
        have_ever_valid = true;
        last_valid_us = esp_timer_get_time();
        led_display_set_reading(true, reading.temperature_c, reading.rate_c_per_min);
      } else {
        ESP_LOGW(TAG, "Poll failed or reading rejected -- see flue_poll warnings above");
        bool stale = !have_ever_valid ||
                     (esp_timer_get_time() - last_valid_us) > (int64_t) STALE_READING_MS * 1000;
        if (stale) {
          // No reading yet, or the last good one is too old to trust --
          // fall back to the neutral pulse rather than holding a stale
          // color. A single missed poll doesn't trigger this by itself.
          led_display_set_reading(false, 0.0f, 0.0f);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
  } else {
    ESP_LOGW(TAG, "Not connected -- starting setup access point + captive portal");
    captive_portal_start(SETUP_AP_SSID, SETUP_AP_PASSWORD, SETUP_AP_MAX_CONN);
  }
}
