#include "dashboard_auth.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "nvs.h"

#define AUTH_NAMESPACE "dashboard_auth"
#define AUTH_KEY "credential"
#define AUTH_PASSWORD_MIN 8
#define AUTH_PASSWORD_MAX 128
#define AUTH_PBKDF2_ROUNDS 12000
// Browsers cap persistent cookies; refresh this lifetime during remembered use.
#define AUTH_COOKIE_SECONDS (400 * 24 * 60 * 60)

typedef struct {
  uint8_t salt[16];
  uint8_t hash[32];
} credential_t;

static credential_t s_credential;
static bool s_configured;
static char s_session[65];
static bool s_remember;

static void session_cookie(httpd_req_t *req) {
  // HTTPD retains the header pointer until the response is sent.
  static char cookie[160];
  char persistence[40] = "";
  if (s_remember) snprintf(persistence, sizeof persistence, "; Max-Age=%d", AUTH_COOKIE_SECONDS);
  snprintf(cookie, sizeof cookie,
           "flu_session=%s; Path=/%s; HttpOnly; SameSite=Strict%s",
           s_session, persistence, dashboard_auth_is_remote(req) ? "; Secure" : "");
  httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}
static unsigned s_failed_logins;
static int64_t s_login_lockout_until_us;

static bool derive(const char *password, const uint8_t salt[16], uint8_t out[32]) {
  if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
      (const unsigned char *)password, strlen(password), salt, 16,
      AUTH_PBKDF2_ROUNDS, 32, out) != 0) {
    memset(out, 0, 32);
    return false;
  }
  return true;
}

static bool equal_secret(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

static bool password_matches(const char *password) {
  uint8_t candidate[32];
  bool derived = derive(password, s_credential.salt, candidate);
  bool matches = derived && equal_secret(candidate, s_credential.hash, sizeof candidate);
  memset(candidate, 0, sizeof candidate);
  return matches;
}

static void make_token(char out[65]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t bytes[32];
  esp_fill_random(bytes, sizeof bytes);
  for (size_t i = 0; i < sizeof bytes; ++i) {
    out[i * 2] = hex[bytes[i] >> 4];
    out[i * 2 + 1] = hex[bytes[i] & 15];
  }
  out[64] = 0;
  memset(bytes, 0, sizeof bytes);
}

void dashboard_auth_init(void) {
  memset(s_session, 0, sizeof s_session);
  s_remember = false;
  nvs_handle_t nvs;
  size_t size = sizeof s_credential;
  if (nvs_open(AUTH_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
    if (nvs_get_blob(nvs, AUTH_KEY, &s_credential, &size) == ESP_OK && size == sizeof s_credential)
      s_configured = true;
    nvs_close(nvs);
  }
}

bool dashboard_auth_is_configured(void) { return s_configured; }

// The tunnel connects only through IPv4 loopback. Never trust a visitor's
// Host/forwarded headers to decide whether first-use enrollment is local.
bool dashboard_auth_is_remote(httpd_req_t *req) {
  struct sockaddr_storage peer;
  socklen_t len = sizeof peer;
  if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &len) != 0) return true;
  uint32_t ip;
  if (peer.ss_family == AF_INET) {
    ip = ntohl(((struct sockaddr_in *)&peer)->sin_addr.s_addr);
  } else if (peer.ss_family == AF_INET6) {
    const struct in6_addr *v6 = &((struct sockaddr_in6 *)&peer)->sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(v6)) {
      memcpy(&ip, &v6->s6_addr[12], sizeof ip);
      ip = ntohl(ip);
    } else {
      // Only ULA/link-local peers count as local enrollment connections.
      return !((v6->s6_addr[0] & 0xfe) == 0xfc ||
               (v6->s6_addr[0] == 0xfe && (v6->s6_addr[1] & 0xc0) == 0x80));
    }
  } else return true;
  return !((ip >> 24) == 10 || (ip >> 20) == 0xac1 ||
           (ip >> 16) == 0xc0a8 || (ip >> 16) == 0xa9fe);

}

bool dashboard_auth_check(httpd_req_t *req) {
  if (!s_configured) return false;
  char cookie[256];
  if (!s_session[0] ||
      httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) != ESP_OK) return false;
  const char *p = cookie;
  while (*p) {
    while (*p == ' ' || *p == ';') ++p;
    const char *end = strchr(p, ';');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n == 76 && !strncmp(p, "flu_session=", 12)) {
      bool valid = equal_secret((const uint8_t *)(p + 12), (const uint8_t *)s_session, 64);
      if (valid && s_remember) session_cookie(req);
      return valid;
    }
    if (!end) break;
    p = end + 1;
  }
  return false;
}

bool dashboard_auth_check_csrf(httpd_req_t *req) {
  char host[96], origin[160];
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK ||
      httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) != ESP_OK) return false;
  bool https = !strncmp(origin, "https://", 8);
  if (!https && strncmp(origin, "http://", 7)) return false;
  // Credentials and authenticated writes through the tunnel require HTTPS.
  if (dashboard_auth_is_remote(req) && !https) return false;
  return strcmp(origin + (https ? 8 : 7), host) == 0;
}

esp_err_t dashboard_auth_login(httpd_req_t *req, const char *password, bool remember) {
  if (esp_timer_get_time() < s_login_lockout_until_us) return ESP_ERR_TIMEOUT;
  if (!s_configured || !password || strlen(password) < AUTH_PASSWORD_MIN ||
      strlen(password) > AUTH_PASSWORD_MAX || !password_matches(password)) {
    if (++s_failed_logins >= 5) {
      s_failed_logins = 0;
      s_login_lockout_until_us = esp_timer_get_time() + 30LL * 1000000;
    }
    return ESP_ERR_NOT_FOUND;
  }
  s_failed_logins = 0;
  s_login_lockout_until_us = 0;
  make_token(s_session);
  s_remember = remember;
  session_cookie(req);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return ESP_OK;
}

esp_err_t dashboard_auth_set_password(const char *current, const char *password) {
  if (!password || strlen(password) < AUTH_PASSWORD_MIN || strlen(password) > AUTH_PASSWORD_MAX)
    return ESP_ERR_INVALID_ARG;
  if (esp_timer_get_time() < s_login_lockout_until_us) return ESP_ERR_TIMEOUT;
  if (s_configured && (!current || strlen(current) > AUTH_PASSWORD_MAX || !password_matches(current))) {
    if (++s_failed_logins >= 5) { s_failed_logins = 0; s_login_lockout_until_us = esp_timer_get_time() + 30LL * 1000000; }
    return ESP_ERR_NOT_FOUND;
  }

  credential_t next;
  esp_fill_random(next.salt, sizeof next.salt);
  if (!derive(password, next.salt, next.hash)) return ESP_FAIL;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(nvs, AUTH_KEY, &next, sizeof next);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err == ESP_OK) {
    s_credential = next;
    s_configured = true;
    s_failed_logins = 0;
    s_login_lockout_until_us = 0;
    memset(s_session, 0, sizeof s_session);
  }
  memset(&next, 0, sizeof next);
  return err;
}

void dashboard_auth_logout(httpd_req_t *req) {
  memset(s_session, 0, sizeof s_session);
  httpd_resp_set_hdr(req, "Set-Cookie", "flu_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}
