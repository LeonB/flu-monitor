#include "wifi_setup.h"

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_setup";

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

// Event group bits for the STA connect wait below.
static EventGroupHandle_t s_sta_event_group;
#define STA_CONNECTED_BIT BIT0
#define STA_FAILED_BIT    BIT1

static void sta_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "STA disconnected");
    if (s_sta_event_group != NULL) {
      xEventGroupSetBits(s_sta_event_group, STA_FAILED_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    if (s_sta_event_group != NULL) {
      xEventGroupSetBits(s_sta_event_group, STA_CONNECTED_BIT);
    }
  }
}

bool wifi_creds_load(char *ssid_out, char *pass_out) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }

  size_t ssid_len = WIFI_SSID_MAX_LEN;
  size_t pass_len = WIFI_PASS_MAX_LEN;
  esp_err_t ssid_err = nvs_get_str(handle, NVS_KEY_SSID, ssid_out, &ssid_len);
  esp_err_t pass_err = nvs_get_str(handle, NVS_KEY_PASS, pass_out, &pass_len);
  nvs_close(handle);

  if (ssid_err != ESP_OK || pass_err != ESP_OK || ssid_out[0] == '\0') {
    return false;
  }
  return true;
}

esp_err_t wifi_creds_save(const char *ssid, const char *pass) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(handle, NVS_KEY_PASS, pass);
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err;
}

bool wifi_sta_try_connect(const char *ssid, const char *pass, uint32_t timeout_ms) {
  s_sta_event_group = xEventGroupCreate();

  esp_netif_create_default_wifi_sta();

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

  esp_event_handler_instance_t wifi_handler;
  esp_event_handler_instance_t ip_handler;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, &wifi_handler));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &ip_handler));

  wifi_config_t wifi_config = {0};
  strlcpy((char *) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  strlcpy((char *) wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Attempting to join '%s' (timeout %" PRIu32 "ms)", ssid, timeout_ms);
  EventBits_t bits = xEventGroupWaitBits(s_sta_event_group, STA_CONNECTED_BIT | STA_FAILED_BIT, pdFALSE, pdFALSE,
                                         pdMS_TO_TICKS(timeout_ms));

  // These handlers did their job (signalling the event group above); the
  // sta_event_handler's auto-reconnect-on-disconnect behavior isn't wanted
  // for the rest of this function's lifetime, so leave them registered --
  // main.c doesn't currently unregister them, this is a Milestone 1 known
  // simplification, not a bug: WIFI_EVENT_STA_START only fires once per
  // esp_wifi_start(), so no repeated connect attempts occur without one.
  (void) wifi_handler;
  (void) ip_handler;

  bool connected = (bits & STA_CONNECTED_BIT) != 0;
  vEventGroupDelete(s_sta_event_group);
  s_sta_event_group = NULL;
  return connected;
}
