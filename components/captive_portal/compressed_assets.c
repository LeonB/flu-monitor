#include "compressed_assets.h"
#include "miniz.h"
#include <stdlib.h>
#include <string.h>
#include "portal_assets_generated.h"

const compressed_asset_t *portal_styles_asset(void) { return &portal_styles; }
const compressed_asset_t *portal_alpine_asset(void) { return &portal_alpine; }
const compressed_asset_t *portal_root_asset(void) { return &portal_root; }

bool compressed_asset_accepts_gzip(httpd_req_t *req) {
  char encodings[160];
  if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", encodings, sizeof(encodings)) != ESP_OK) return false;
  char *save = NULL;
  for (char *token = strtok_r(encodings, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
    while (*token == ' ' || *token == '\t') token++;
    char *params = strchr(token, ';');
    if (params) *params++ = 0;
    char *end = token + strlen(token);
    while (end > token && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    if (strcasecmp(token, "gzip") != 0) continue;
    if (params) {
      while (*params == ' ' || *params == '\t') params++;
      if (strncasecmp(params, "q=", 2) == 0 && strtod(params + 2, NULL) <= 0) return false;
    }
    return true;
  }
  return false;
}

// Build-generated gzip has a fixed 10-byte header, raw DEFLATE, 8-byte trailer.
// Use the ESP32 ROM inflater with a bounded 32 KiB dictionary on the heap.
esp_err_t compressed_asset_send_identity(httpd_req_t *req, const unsigned char *gzip, size_t size, size_t raw_size) {
  if (size < 18 || gzip[0] != 0x1f || gzip[1] != 0x8b || gzip[2] != 8 || gzip[3] != 0)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid embedded asset");
  tinfl_decompressor *inflater = calloc(1, sizeof(*inflater));
  unsigned char *dictionary = malloc(TINFL_LZ_DICT_SIZE);
  if (!inflater || !dictionary) {
    free(inflater); free(dictionary);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "Not enough memory to decode asset", HTTPD_RESP_USE_STRLEN);
  }
  tinfl_init(inflater);
  size_t consumed = 0, produced = 0;
  esp_err_t result = ESP_OK;
  tinfl_status status;
  do {
    size_t input = size - 18 - consumed, output = TINFL_LZ_DICT_SIZE;
    status = tinfl_decompress(inflater, gzip + 10 + consumed, &input,
                             dictionary, dictionary, &output, 0);
    consumed += input; produced += output;
    if (status < 0 || status == TINFL_STATUS_NEEDS_MORE_INPUT || produced > raw_size) {
      result = ESP_FAIL;
      break;
    }
    if (output && (result = httpd_resp_send_chunk(req, (const char *)dictionary, output)) != ESP_OK) break;
  } while (status == TINFL_STATUS_HAS_MORE_OUTPUT);
  if (result == ESP_OK && status == TINFL_STATUS_DONE && produced == raw_size && consumed == size - 18)
    result = httpd_resp_send_chunk(req, NULL, 0);
  else if (result == ESP_OK) result = ESP_FAIL;
  free(dictionary); free(inflater);
  return result;
}
