// flu-display: Milestone 1 -- WiFi (with stored credentials) + captive
// portal fallback only. No polling or LED logic yet (Milestones 2-3).
// See CLAUDE.md's "Project goal" section for the full project context, and
// the plan this was built from for the architecture rationale (ESPHome vs.
// Arduino vs. plain ESP-IDF, why plain ESP-IDF was chosen).

#include <stdbool.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "captive_portal.h"
#include "config.h"
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
    ESP_LOGI(TAG, "Connected. Milestone 1 stops here -- polling/LED logic comes in Milestones 2-3.");
    // Nothing else to do yet; app_main returning is fine, FreeRTOS keeps
    // running (WiFi's own tasks, etc.) -- this idle loop just keeps a
    // visible heartbeat in the serial log for this milestone's testing.
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(10000));
      ESP_LOGI(TAG, "(still connected, idling -- Milestone 1)");
    }
  } else {
    ESP_LOGW(TAG, "Not connected -- starting setup access point + captive portal");
    captive_portal_start(SETUP_AP_SSID, SETUP_AP_PASSWORD, SETUP_AP_MAX_CONN);
  }
}
