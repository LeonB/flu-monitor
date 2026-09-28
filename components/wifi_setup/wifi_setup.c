#include "wifi_setup.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_setup";

#define NVS_NAMESPACE     "wifi_cfg"
#define NVS_KEY_SSID      "ssid"
#define NVS_KEY_PASS      "pass"
#define NVS_KEY_FAILED    "failed"      // u8: 1 if the last connect attempt failed
#define NVS_KEY_FAILED_SSID "failed_ssid"

// Event group bits for the STA connect wait below.
static EventGroupHandle_t s_sta_event_group;
#define STA_CONNECTED_BIT BIT0
#define STA_FAILED_BIT    BIT1

// Set by wifi_sta_try_connect(), read/cleared by wifi_sta_teardown().
static esp_netif_t *s_sta_netif = NULL;

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

  s_sta_netif = esp_netif_create_default_wifi_sta();

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

void wifi_sta_teardown(void) {
  ESP_ERROR_CHECK(esp_wifi_stop());
  ESP_ERROR_CHECK(esp_wifi_deinit());
  if (s_sta_netif != NULL) {
    // The documented counterpart to esp_netif_create_default_wifi_sta() --
    // unregisters its handlers and destroys the netif, so
    // captive_portal.c's wifi_init_softap() can create its own STA+AP
    // netifs from scratch without hitting a duplicate-netif assertion.
    esp_netif_destroy_default_wifi(s_sta_netif);
    s_sta_netif = NULL;
  }
}

bool wifi_sta_test_connect(const char *ssid, const char *pass, uint32_t timeout_ms) {
  s_sta_event_group = xEventGroupCreate();

  // Narrower registration than wifi_sta_try_connect()'s (that one also
  // handles WIFI_EVENT_STA_START, which fires on esp_wifi_start() -- here
  // STA is already started as part of the AP's own WIFI_MODE_APSTA, so
  // that event already fired once, long before any credentials existed).
  esp_event_handler_instance_t wifi_handler;
  esp_event_handler_instance_t ip_handler;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &sta_event_handler, NULL, &wifi_handler));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &ip_handler));

  wifi_config_t wifi_config = {0};
  strlcpy((char *) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  strlcpy((char *) wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  ESP_LOGI(TAG, "Testing '%s' while the setup AP stays up (timeout %" PRIu32 "ms)", ssid, timeout_ms);
  esp_wifi_connect();

  EventBits_t bits = xEventGroupWaitBits(s_sta_event_group, STA_CONNECTED_BIT | STA_FAILED_BIT, pdFALSE, pdFALSE,
                                         pdMS_TO_TICKS(timeout_ms));
  bool connected = (bits & STA_CONNECTED_BIT) != 0;

  if (!connected) {
    // Aborts the failed/hung attempt so a follow-up call with different
    // credentials starts clean instead of racing the previous one.
    esp_wifi_disconnect();
  }

  esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_handler);
  esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);

  vEventGroupDelete(s_sta_event_group);
  s_sta_event_group = NULL;
  return connected;
}

bool wifi_last_attempt_failed(char *failed_ssid_out, size_t failed_ssid_out_size) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }

  uint8_t failed = 0;
  esp_err_t err = nvs_get_u8(handle, NVS_KEY_FAILED, &failed);
  if (err == ESP_OK && failed && failed_ssid_out != NULL && failed_ssid_out_size > 0) {
    size_t len = failed_ssid_out_size;
    if (nvs_get_str(handle, NVS_KEY_FAILED_SSID, failed_ssid_out, &len) != ESP_OK) {
      failed_ssid_out[0] = '\0';
    }
  }
  nvs_close(handle);
  return err == ESP_OK && failed != 0;
}

esp_err_t wifi_mark_attempt_failed(const char *ssid) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_u8(handle, NVS_KEY_FAILED, 1);
  if (err == ESP_OK) {
    err = nvs_set_str(handle, NVS_KEY_FAILED_SSID, ssid);
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err;
}

int wifi_scan(wifi_scan_result_t *out_results, size_t max_results) {
  wifi_scan_config_t scan_config = {0};
  esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // blocking
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
    return -1;
  }

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) {
    return 0;
  }

  wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
  if (records == NULL) {
    ESP_LOGW(TAG, "Out of memory scanning for %u APs", ap_count);
    return -1;
  }
  uint16_t actual_count = ap_count;
  err = esp_wifi_scan_get_ap_records(&actual_count, records);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
    free(records);
    return -1;
  }

  int written = 0;
  for (uint16_t i = 0; i < actual_count && written < (int) max_results; i++) {
    const char *ssid = (const char *) records[i].ssid;
    if (ssid[0] == '\0') {
      continue;  // hidden network -- "type a name" in the UI covers this case
    }

    // esp_wifi_scan returns one entry per BSSID, not per network name --
    // collapse duplicates (an AP seen on multiple channels/radios) to
    // their strongest RSSI showing.
    bool duplicate = false;
    for (int j = 0; j < written; j++) {
      if (strncmp(out_results[j].ssid, ssid, WIFI_SSID_MAX_LEN) == 0) {
        if (records[i].rssi > out_results[j].rssi) {
          out_results[j].rssi = records[i].rssi;
          out_results[j].secured = records[i].authmode != WIFI_AUTH_OPEN;
        }
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    strlcpy(out_results[written].ssid, ssid, sizeof(out_results[written].ssid));
    out_results[written].rssi = records[i].rssi;
    out_results[written].secured = records[i].authmode != WIFI_AUTH_OPEN;
    written++;
  }
  free(records);

  // Strongest signal first -- small list, a plain insertion sort is fine.
  for (int i = 0; i < written - 1; i++) {
    for (int j = i + 1; j < written; j++) {
      if (out_results[j].rssi > out_results[i].rssi) {
        wifi_scan_result_t tmp = out_results[i];
        out_results[i] = out_results[j];
        out_results[j] = tmp;
      }
    }
  }

  return written;
}

esp_err_t wifi_clear_attempt_failed(void) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }
  // Absence of the key just means "no failure recorded" (wifi_last_attempt_failed
  // treats any read error as false), so a fresh device with nothing to erase
  // yet isn't a real error here.
  err = nvs_erase_key(handle, NVS_KEY_FAILED);
  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
    err = nvs_erase_key(handle, NVS_KEY_FAILED_SSID);
  }
  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err;
}
