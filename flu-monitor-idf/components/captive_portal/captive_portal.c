#include "captive_portal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "dns_server.h"
#include "wifi_setup.h"

static const char *TAG = "captive_portal";

// Set once by captive_portal_start(), read by save_post_handler().
static uint32_t s_sta_test_timeout_ms;

extern const char root_start[] asm("_binary_root_html_start");
extern const char root_end[] asm("_binary_root_html_end");
extern const char styles_css_start[] asm("_binary_styles_css_start");
extern const char styles_css_end[] asm("_binary_styles_css_end");
extern const char alpine_min_js_start[] asm("_binary_alpine_min_js_start");
extern const char alpine_min_js_end[] asm("_binary_alpine_min_js_end");

// Decodes application/x-www-form-urlencoded text in place: '+' -> space,
// '%XX' -> the byte XX. httpd_query_key_value() (used below) explicitly does
// NOT do this itself -- see its doc comment in esp_http_server.h -- so the
// caller (us) has to.
static void url_decode(const char *src, char *dst, size_t dst_size) {
  size_t out = 0;
  for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size; i++) {
    if (src[i] == '+') {
      dst[out++] = ' ';
    } else if (src[i] == '%' && src[i + 1] != '\0' && src[i + 2] != '\0') {
      char hex[3] = {src[i + 1], src[i + 2], '\0'};
      dst[out++] = (char) strtol(hex, NULL, 16);
      i += 2;
    } else {
      dst[out++] = src[i];
    }
  }
  dst[out] = '\0';
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
    ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
    ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
  }
}

static void wifi_init_softap(const char *ap_ssid, const char *ap_password, int max_connections) {
  esp_netif_create_default_wifi_ap();
  // APSTA, not plain AP: scanning for nearby networks (wifi_scan(), used by
  // the /scan endpoint below) needs an active STA interface even though
  // this device never actually associates as a station while the portal is
  // up -- the STA side just sits idle, ready to scan on demand.
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

  wifi_config_t wifi_config = {0};
  strlcpy((char *) wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
  wifi_config.ap.ssid_len = strlen(ap_ssid);
  strlcpy((char *) wifi_config.ap.password, ap_password, sizeof(wifi_config.ap.password));
  wifi_config.ap.max_connection = max_connections;
  wifi_config.ap.authmode = (strlen(ap_password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "SoftAP started. SSID:'%s' password:'%s'", ap_ssid, ap_password);
}

static esp_err_t root_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, root_start, root_end - root_start);
  return ESP_OK;
}

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
};

static esp_err_t styles_css_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/css");
  httpd_resp_send(req, styles_css_start, styles_css_end - styles_css_start);
  return ESP_OK;
}
static const httpd_uri_t styles_css_uri = {.uri = "/styles.css", .method = HTTP_GET, .handler = styles_css_get_handler};

static esp_err_t alpine_js_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_send(req, alpine_min_js_start, alpine_min_js_end - alpine_min_js_start);
  return ESP_OK;
}
static const httpd_uri_t alpine_js_uri = {.uri = "/alpine.min.js", .method = HTTP_GET, .handler = alpine_js_get_handler};

// Nearby networks, strongest signal first -- see wifi_scan()'s own doc
// comment for why this works while the SoftAP is up (APSTA mode).
static esp_err_t scan_get_handler(httpd_req_t *req) {
  wifi_scan_result_t results[24];
  int count = wifi_scan(results, sizeof(results) / sizeof(results[0]));
  if (count < 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
    return ESP_FAIL;
  }

  cJSON *root = cJSON_CreateArray();
  for (int i = 0; i < count; i++) {
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "ssid", results[i].ssid);
    cJSON_AddNumberToObject(entry, "rssi", results[i].rssi);
    cJSON_AddBoolToObject(entry, "secured", results[i].secured);
    cJSON_AddItemToArray(root, entry);
  }
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

static const httpd_uri_t scan_uri = {
    .uri = "/scan",
    .method = HTTP_GET,
    .handler = scan_get_handler,
};

// Whether the *previous* boot's connection attempt failed -- see
// wifi_last_attempt_failed()'s doc comment for why this can't just be
// observed live (the device reboots to attempt joining). The portal's own
// page polls this once on load to decide whether to show the failure
// state instead of the plain network list.
static esp_err_t status_get_handler(httpd_req_t *req) {
  char failed_ssid[WIFI_SSID_MAX_LEN + 1] = {0};
  bool failed = wifi_last_attempt_failed(failed_ssid, sizeof(failed_ssid));

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "failed", failed);
  cJSON_AddStringToObject(root, "failed_ssid", failed ? failed_ssid : "");
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

static const httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_get_handler,
};

static esp_err_t save_post_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len >= 256) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
    return ESP_FAIL;
  }

  char body[256] = {0};
  int received = 0;
  while (received < req->content_len) {
    int ret = httpd_req_recv(req, body + received, req->content_len - received);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request body");
      return ESP_FAIL;
    }
    received += ret;
  }
  body[received] = '\0';

  char ssid_raw[WIFI_SSID_MAX_LEN + 1] = {0};
  char pass_raw[WIFI_PASS_MAX_LEN + 1] = {0};
  httpd_query_key_value(body, "ssid", ssid_raw, sizeof(ssid_raw));
  httpd_query_key_value(body, "password", pass_raw, sizeof(pass_raw));

  char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
  char pass[WIFI_PASS_MAX_LEN + 1] = {0};
  url_decode(ssid_raw, ssid, sizeof(ssid));
  url_decode(pass_raw, pass, sizeof(pass));

  if (ssid[0] == '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Testing '%s' before saving anything", ssid);
  bool ok = wifi_sta_test_connect(ssid, pass, s_sta_test_timeout_ms);

  if (!ok) {
    ESP_LOGW(TAG, "Test connect to '%s' failed; not saving, not restarting", ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false}");
    return ESP_OK;
  }

  // The test connection above already proved these credentials work --
  // clear any stale failure from an unrelated earlier attempt so the next
  // boot's fresh success doesn't get shadowed by old state.
  wifi_clear_attempt_failed();

  if (wifi_creds_save(ssid, pass) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save credentials");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Saved credentials for '%s'; restarting to join for real", ssid);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");

  // Give the response time to actually reach the client before the reboot
  // tears down the network stack.
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
  return ESP_OK;  // unreachable
}

static const httpd_uri_t save_uri = {
    .uri = "/save",
    .method = HTTP_POST,
    .handler = save_post_handler,
};

// Redirects any unmatched request to "/", which is what makes iOS/Android/
// Windows auto-pop the captive-portal sign-in page.
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err) {
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static void start_webserver(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_open_sockets = 13;
  config.lru_purge_enable = true;

  ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
  ESP_ERROR_CHECK(httpd_start(&server, &config));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &styles_css_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &alpine_js_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scan_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save_uri));
  ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler));
}

void captive_portal_start(const char *ap_ssid, const char *ap_password, int max_connections,
                          uint32_t sta_test_timeout_ms) {
  s_sta_test_timeout_ms = sta_test_timeout_ms;

  // Redirected traffic generates a lot of harmless-but-noisy invalid
  // requests; quiet those down (same as Espressif's own example).
  esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
  esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

  wifi_init_softap(ap_ssid, ap_password, max_connections);
  start_webserver();

  dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
  start_dns_server(&dns_config);
}
