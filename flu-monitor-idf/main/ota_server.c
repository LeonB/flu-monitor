#include "ota_server.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "secrets.h"

static const char *TAG = "ota_server";

// Reads the "secret" query param off the request URL and compares it
// against OTA_SECRET. This is the only auth here -- a shared secret over
// plain HTTP on the home LAN, the same trust level flu-monitor's own
// Google Sheets webhook secret uses, not an encrypted channel like
// ESPHome's own OTA. Acceptable for a single-home network; wouldn't be if
// this were ever exposed beyond it.
static bool secret_ok(httpd_req_t *req) {
  char query[128] = {0};
  if (httpd_req_get_url_query_len(req) == 0 || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return false;
  }
  char secret[64] = {0};
  if (httpd_query_key_value(query, "secret", secret, sizeof(secret)) != ESP_OK) {
    return false;
  }
  return strcmp(secret, OTA_SECRET) == 0;
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
  if (!secret_ok(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Bad or missing secret");
    return ESP_FAIL;
  }

  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  if (update_partition == NULL) {
    ESP_LOGE(TAG, "No free OTA partition to update into");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Starting OTA: running='%s' -> target='%s', %d bytes incoming", running->label,
            update_partition->label, (int) req->content_len);

  esp_ota_handle_t ota_handle;
  esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
    return ESP_FAIL;
  }

  char buf[1024];
  size_t remaining = req->content_len;
  while (remaining > 0) {
    size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
    int recv_len = httpd_req_recv(req, buf, chunk);
    if (recv_len <= 0) {
      if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      ESP_LOGE(TAG, "httpd_req_recv failed: %d", recv_len);
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request body");
      return ESP_FAIL;
    }

    err = esp_ota_write(ota_handle, buf, recv_len);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
      return ESP_FAIL;
    }
    remaining -= (size_t) recv_len;
  }

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    // Most commonly ESP_ERR_OTA_VALIDATE_FAILED -- the received image
    // failed its own header/checksum validation. Whatever's currently
    // running is untouched either way.
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "New image failed validation");
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_set_boot_partition failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OTA write complete, rebooting into '%s'", update_partition->label);
  httpd_resp_sendstr(req, "OK, rebooting\n");

  // Give the response time to actually reach the client before the reboot
  // tears down the network stack -- same reasoning as captive_portal.c's
  // own pre-restart delay.
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;  // unreachable
}

static const httpd_uri_t ota_uri = {
    .uri = "/ota",
    .method = HTTP_POST,
    .handler = ota_post_handler,
};

void ota_server_start(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;  // esp_ota_* calls + logging need more than the 4096 default

  ESP_LOGI(TAG, "Starting OTA server on port %d", config.server_port);
  ESP_ERROR_CHECK(httpd_start(&server, &config));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_uri));
}
