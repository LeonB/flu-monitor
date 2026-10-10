#pragma once
#include "esp_http_server.h"
#include <stdbool.h>
typedef struct {
  const unsigned char *data;
  size_t size;
  size_t raw_size;
  const char *etag;
  const char *gzip_etag;
} compressed_asset_t;
const compressed_asset_t *portal_root_asset(void);
const compressed_asset_t *portal_styles_asset(void);
const compressed_asset_t *portal_alpine_asset(void);
bool compressed_asset_accepts_gzip(httpd_req_t *req);
esp_err_t compressed_asset_send_identity(httpd_req_t *req, const unsigned char *gzip, size_t size, size_t raw_size);
