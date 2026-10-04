// flu-display: Milestone 1 (WiFi with stored credentials + captive portal
// fallback), Milestone 2 (poll flu-monitor's JSON API), Milestone 3 (drive
// the LED ring off the polled reading), Milestone 4 (subscribe to
// flu-monitor's WebSocket broadcast instead of polling REST, and fetch
// zone/rate thresholds from its REST API instead of this project's own
// config.h copies).
// See CLAUDE.md's "Project goal" section for the full project context, and
// the plan this was built from for the architecture rationale (ESPHome vs.
// Arduino vs. plain ESP-IDF, why plain ESP-IDF was chosen).

#include <stdbool.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_ota_ops.h"

#include "captive_portal.h"
#include "config.h"
#include "flue_poll.h"
#include "led_display.h"
#include "ota_server.h"
#include "secrets.h"
#include "status_led.h"
#include "wifi_setup.h"

static const char *TAG = "main";

// Reflects wifi_sta_enable_auto_reconnect()'s own connect/disconnect
// notifications onto the status LED -- see main() below. STATUS_LED_CONNECTING
// doubles as "reconnecting," same meaning as the initial join attempt.
static void wifi_status_changed(bool connected) {
  status_led_set(connected ? STATUS_LED_CONNECTED : STATUS_LED_CONNECTING);
}

static void init_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

static void configure_power_management(void) {
  // WiFi already uses modem power save by default. Automatic light sleep
  // lets the CPU and radio sleep between LED frames and network work.
  const esp_pm_config_t config = {
      .max_freq_mhz = 240,
      .min_freq_mhz = 80,
      .light_sleep_enable = true,
  };
  // Not ESP_ERROR_CHECK: this previously aborted (and crash-looped) every
  // single boot on flu-monitor when CONFIG_PM_ENABLE was missing from a
  // stale sdkconfig -- see ../flu-monitor/CLAUDE.md, "A stale, gitignored
  // sdkconfig turned a new Kconfig default into a permanent boot
  // crash-loop" (this project's own sdkconfig happened to already be
  // correct, but the same latent risk applies here too). Running without
  // automatic light sleep is a fine fallback; bricking the device on any
  // future esp_pm_configure() failure is not.
  esp_err_t err = esp_pm_configure(&config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_pm_configure failed: %s -- continuing without automatic light sleep", esp_err_to_name(err));
  }
}

void app_main(void) {
  init_nvs();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  status_led_init();

  char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
  char pass[WIFI_PASS_MAX_LEN + 1] = {0};
  bool have_creds = wifi_creds_load(ssid, pass);

  bool connected = false;
  if (have_creds) {
    ESP_LOGI(TAG, "Found stored WiFi credentials for '%s'", ssid);
    status_led_set(STATUS_LED_CONNECTING);
    connected = wifi_sta_try_connect(ssid, pass, STA_CONNECT_TIMEOUT_MS);
  } else {
    ESP_LOGI(TAG, "No stored WiFi credentials");
  }

  if (connected) {
    ESP_LOGI(TAG, "Connected. Subscribing to flu-monitor and driving the LED ring.");
    configure_power_management();
    status_led_set(STATUS_LED_CONNECTED);
    wifi_sta_enable_auto_reconnect(wifi_status_changed);

    // Confirms this image works well enough to join WiFi, canceling the
    // bootloader's rollback timer for it. A build broken badly enough to
    // never reach here leaves the last known-good image as the boot target
    // on the next reset instead -- see sdkconfig.defaults for the rollback
    // config this depends on.
    esp_ota_mark_app_valid_cancel_rollback();

    // Clears any stale failure flag from an earlier, unrelated attempt --
    // the shared captive portal's own /save handler already clears this
    // when a *new* attempt starts, but a successful connect is also a
    // clean signal that whatever was recorded before no longer applies.
    wifi_clear_attempt_failed();

    ota_server_start(OTA_SECRET);
    led_display_init();
    flue_poll_init();

    // Everything from here on is event-driven: flue_poll.c's own WS event
    // handler calls led_display_set_reading() directly as broadcasts
    // arrive. This tick only watches for staleness (no network I/O) --
    // falls back to the neutral pulse if nothing's arrived in a while,
    // rather than holding a stale color forever.
    while (true) {
      if (flue_poll_is_stale()) {
        led_display_set_reading(false, 0.0f, 0.0f);
      }
      vTaskDelay(pdMS_TO_TICKS(STALENESS_CHECK_INTERVAL_MS));
    }
  } else {
    if (have_creds) {
      ESP_LOGW(TAG, "Failed to join '%s' -- recording for the setup portal's failure screen", ssid);
      wifi_mark_attempt_failed(ssid);
      // wifi_sta_try_connect() leaves WiFi initialized and running in STA
      // mode even on failure -- undo that before captive_portal_start()
      // does its own from-scratch WiFi/netif setup, or it crashes on a
      // duplicate-netif assertion (see wifi_sta_teardown()'s doc comment).
      wifi_sta_teardown();
    }
    ESP_LOGW(TAG, "Not connected -- starting setup access point + captive portal");
    status_led_set(STATUS_LED_AP_MODE);
    captive_portal_start(SETUP_AP_SSID, SETUP_AP_PASSWORD, SETUP_AP_MAX_CONN, STA_CONNECT_TIMEOUT_MS);
  }
}
