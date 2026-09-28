#include "ws_server.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ws_server";

static httpd_handle_t s_server = NULL;

// A handful of clients (flu-display, maybe a browser tab or two later) --
// no need for anything fancier than a small fixed array.
#define WS_MAX_CLIENTS 8
static int s_client_fds[WS_MAX_CLIENTS];
static int s_client_count = 0;
static SemaphoreHandle_t s_clients_mutex;

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
    s_client_fds[s_client_count++] = fd;
    ESP_LOGI(TAG, "Client connected (fd=%d), %d total", fd, s_client_count);
  }
  xSemaphoreGive(s_clients_mutex);
}

// Removing fd from s_client_fds[] only stops *this module* from trying to
// send to it again -- it does nothing to the underlying httpd session,
// which stays open (and still counted against max_open_sockets) until
// something explicitly closes it. This was a real, confirmed bug: a WS
// client going away uncleanly (WiFi drop, no close frame -- the normal
// case, not the exception) leaked one httpd socket slot every time,
// eventually pinning the server at max_open_sockets/max_open_sockets and
// hanging every new request indefinitely (see CLAUDE.md's full writeup).
// httpd_sess_trigger_close() is the documented way to close a session
// from a context other than the httpd worker task itself, which this is
// (send_work_cb runs on the httpd task via httpd_queue_work(), so it
// could probably close directly, but ws_handler()'s own close-frame path
// runs on the httpd task via the normal request-handling call chain where
// closing this way is the documented, safe pattern either way).
static void clients_remove(int fd) {
  xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
  for (int i = 0; i < s_client_count; i++) {
    if (s_client_fds[i] == fd) {
      s_client_fds[i] = s_client_fds[s_client_count - 1];
      s_client_count--;
      ESP_LOGI(TAG, "Client disconnected (fd=%d), %d remaining", fd, s_client_count);
      break;
    }
  }
  xSemaphoreGive(s_clients_mutex);

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
  char *payload;  // owned by this struct; freed by send_work_cb
} send_work_t;

static void send_work_cb(void *arg) {
  send_work_t *work = arg;
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
  }
  free(work->payload);
  free(work);
}

static void broadcast_raw(const char *json) {
  if (s_server == NULL) {
    return;
  }

  int fds[WS_MAX_CLIENTS];
  int count;
  xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
  count = s_client_count;
  memcpy(fds, s_client_fds, sizeof(int) * count);
  xSemaphoreGive(s_clients_mutex);

  for (int i = 0; i < count; i++) {
    send_work_t *work = malloc(sizeof(send_work_t));
    work->fd = fds[i];
    work->payload = strdup(json);
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

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    clients_remove(httpd_req_to_sockfd(req));
    return ESP_OK;
  }

  // Broadcast-only server -- no client->server frame is ever meaningful,
  // but the payload still has to be drained so the socket doesn't back up.
  if (frame.len > 0 && frame.len < 256) {
    uint8_t buf[256];
    frame.payload = buf;
    httpd_ws_recv_frame(req, &frame, frame.len);
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
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws_uri));
}
