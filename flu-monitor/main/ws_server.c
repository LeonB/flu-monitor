#include "ws_server.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ws_server";

static httpd_handle_t s_server = NULL;

// A handful of clients (flu-display, maybe a browser tab or two later) --
// no need for anything fancier than a small fixed array.
#define WS_MAX_CLIENTS 8
static int s_client_fds[WS_MAX_CLIENTS];
static uint32_t s_client_generations[WS_MAX_CLIENTS];
static uint32_t s_next_generation;
static int s_client_count = 0;
static SemaphoreHandle_t s_clients_mutex;
static uint32_t s_settings_revision;
static uint32_t s_applied_revision;
static int s_display_wifi_fd = -1;
static int s_display_wifi_rssi;
static int64_t s_display_wifi_us;

static void clients_add(int fd) {
  xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
  bool already = false;
  for (int i = 0; i < s_client_count; i++) {
    if (s_client_fds[i] == fd) {
      already = true;
      break;
    }
  }
  if (!already && s_client_count < WS_MAX_CLIENTS) {
    s_client_generations[s_client_count] = ++s_next_generation;
    s_client_fds[s_client_count++] = fd;
    ESP_LOGI(TAG, "Client connected (fd=%d), %d total", fd, s_client_count);
  }
  xSemaphoreGive(s_clients_mutex);
}

// Keep streaming clients newer than idle HTTP sessions when a new request arrives.
esp_err_t ws_server_session_open(httpd_handle_t server, int fd) {
  if (s_clients_mutex) {
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (int i = 0; i < s_client_count; i++) {
      if (httpd_ws_get_fd_info(server, s_client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
        httpd_sess_update_lru_counter(server, s_client_fds[i]);
    }
    xSemaphoreGive(s_clients_mutex);
  }
  httpd_sess_update_lru_counter(server, fd);
  return ESP_OK;
}

// Called for every httpd session teardown, including TCP disconnects and LRU eviction.
void ws_server_session_close(httpd_handle_t server, int fd) {
  (void) server;
  if (s_clients_mutex) {
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    if (s_display_wifi_fd == fd) s_display_wifi_fd = -1;
    for (int i = 0; i < s_client_count; i++) {
      if (s_client_fds[i] == fd) {
        s_client_fds[i] = s_client_fds[--s_client_count];
        s_client_generations[i] = s_client_generations[s_client_count];
        ESP_LOGI(TAG, "Session closed (fd=%d), %d WS clients remaining", fd, s_client_count);
        break;
      }
    }
    xSemaphoreGive(s_clients_mutex);
  }
  close(fd);  // A custom close callback owns the actual socket close.
}

uint32_t ws_server_settings_revision(bool advance) {
  xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
  if (advance && ++s_settings_revision == 0) ++s_settings_revision;
  uint32_t revision = s_settings_revision;
  xSemaphoreGive(s_clients_mutex);
  return revision;
}

bool ws_server_settings_applied(uint32_t revision) {
  xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
  bool applied = revision == s_applied_revision;
  xSemaphoreGive(s_clients_mutex);
  return applied;
}

bool ws_server_display_wifi(int *rssi) {
  if (!s_clients_mutex) return false; // HTTP starts just before WS initialization.
  xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
  bool fresh = s_display_wifi_fd >= 0 && esp_timer_get_time() - s_display_wifi_us < 35000000;
  if (fresh) *rssi = s_display_wifi_rssi;
  xSemaphoreGive(s_clients_mutex);
  return fresh;
}

// Request httpd teardown; its close callback removes bookkeeping and closes the socket.
static void clients_remove(int fd) {
  if (s_server != NULL) {
    esp_err_t err = httpd_sess_trigger_close(s_server, fd);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
      ESP_LOGW(TAG, "httpd_sess_trigger_close(fd=%d) failed: %s", fd, esp_err_to_name(err));
    }
  }
}

// httpd_ws_send_frame_async() is only safe to call from the httpd server's
// own worker context (or a callback it invokes) -- not directly from an
// arbitrary FreeRTOS task like main.c's sensor_log_task. httpd_queue_work()
// is the documented way to get a callback to run on that context from
// anywhere else.
typedef struct {
  int fd;
  uint32_t generation;
  char *payload;  // owned by this struct; freed by send_work_cb
} send_work_t;

static void send_work_cb(void *arg) {
  send_work_t *work = arg;
  bool current = false;
  xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
  for (int i = 0; i < s_client_count; i++) {
    if (s_client_fds[i] == work->fd && s_client_generations[i] == work->generation) current = true;
  }
  xSemaphoreGive(s_clients_mutex);
  if (!current || httpd_ws_get_fd_info(s_server, work->fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
    free(work->payload);
    free(work);
    return;
  }
  httpd_ws_frame_t frame = {
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = (uint8_t *) work->payload,
      .len = strlen(work->payload),
  };
  esp_err_t err = httpd_ws_send_frame_async(s_server, work->fd, &frame);
  if (err != ESP_OK) {
    // Most commonly the client went away without a clean WS close (a phone
    // locking its screen, WiFi drop, etc.) -- prune it so future broadcasts
    // don't keep paying for a dead socket.
    clients_remove(work->fd);
  } else {
    httpd_sess_update_lru_counter(s_server, work->fd);
  }
  free(work->payload);
  free(work);
}

static void broadcast_raw(const char *json) {
  if (s_server == NULL) {
    return;
  }

  int fds[WS_MAX_CLIENTS];
  uint32_t generations[WS_MAX_CLIENTS];
  int count;
  xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
  count = s_client_count;
  memcpy(fds, s_client_fds, sizeof(int) * count);
  memcpy(generations, s_client_generations, sizeof(uint32_t) * count);
  xSemaphoreGive(s_clients_mutex);

  for (int i = 0; i < count; i++) {
    send_work_t *work = malloc(sizeof(send_work_t));
    if (!work) continue;
    work->fd = fds[i];
    work->generation = generations[i];
    work->payload = strdup(json);
    if (!work->payload) { free(work); continue; }
    if (httpd_queue_work(s_server, send_work_cb, work) != ESP_OK) {
      ESP_LOGW(TAG, "httpd_queue_work failed for fd=%d, dropping this broadcast for it", fds[i]);
      free(work->payload);
      free(work);
    }
  }
}

void ws_server_broadcast_reading(const char *reading_json) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "reading");
  // Embeds the already-serialized reading as-is (no re-parse/re-escape) --
  // keeps this the single source of truth for the reading's own shape.
  cJSON_AddItemToObject(root, "reading", cJSON_CreateRaw(reading_json));

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  broadcast_raw(json);
  free(json);
}

void ws_server_broadcast_settings_changed(void) {
  broadcast_raw("{\"type\":\"settings_changed\"}");
}

// The httpd core handles the WS handshake entirely internally and
// deliberately does NOT call our own URI handler for it (see httpd_uri.c:
// "If the request is websocket handshake, then do not call the
// uri->handler") -- this callback is the one hook that actually fires once
// a connection is open, which is why client tracking lives here and not in
// ws_handler() below (which only ever runs for a later client->server
// frame -- something this broadcast-only server's own clients, like
// flu-display, never send).
static esp_err_t ws_post_handshake_cb(httpd_req_t *req) {
  clients_add(httpd_req_to_sockfd(req));
  return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req) {
  httpd_ws_frame_t frame = {0};
  frame.type = HTTPD_WS_TYPE_TEXT;
  // First call with no payload buffer just gets the frame length.
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "httpd_ws_recv_frame (length probe) failed: %s", esp_err_to_name(err));
    return err;
  }
  if (frame.len >= 256) return ESP_FAIL;

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    clients_remove(httpd_req_to_sockfd(req));
    return ESP_OK;
  }

  // Display acknowledgements are small text frames. Drain other small frames too.
  if (frame.len > 0 && frame.len < 256) {
    uint8_t buf[256];
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) return err;
    buf[frame.len] = 0;
    if (frame.type == HTTPD_WS_TYPE_TEXT) {
      cJSON *root = cJSON_Parse((char *) buf);
      cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
      cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
      cJSON *rssi = cJSON_GetObjectItemCaseSensitive(root, "rssi");
      if (cJSON_IsString(type) && strcmp(type->valuestring, "wifi_status") == 0 &&
          cJSON_IsNumber(rssi) && rssi->valuedouble >= -127 && rssi->valuedouble <= 0 &&
          rssi->valuedouble == (int)rssi->valuedouble) {
        xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
        s_display_wifi_fd = httpd_req_to_sockfd(req);
        s_display_wifi_rssi = (int)rssi->valuedouble;
        s_display_wifi_us = esp_timer_get_time();
        xSemaphoreGive(s_clients_mutex);
      }
      if (cJSON_IsString(type) && strcmp(type->valuestring, "settings_applied") == 0 &&
          cJSON_IsNumber(revision) && revision->valuedouble >= 1 && revision->valuedouble <= UINT32_MAX) {
        xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
        if (revision->valuedouble == s_settings_revision) s_applied_revision = s_settings_revision;
        xSemaphoreGive(s_clients_mutex);
        ESP_LOGI(TAG, "Display applied settings revision %lu", (unsigned long) s_applied_revision);
      }
      cJSON_Delete(root);
    }
  }
  return ESP_OK;
}

static const httpd_uri_t ws_uri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .is_websocket = true,
    .ws_post_handshake_cb = ws_post_handshake_cb,
};

void ws_server_register(httpd_handle_t server) {
  s_server = server;
  s_clients_mutex = xSemaphoreCreateMutex();
  s_settings_revision = esp_random() | 1;
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws_uri));
}
