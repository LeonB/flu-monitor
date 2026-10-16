#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include "pagekite_tunnel.h"
#include "settings.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha1.h"
#include <setjmp.h>
#include <stdbool.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// Independently implemented PageKite CONNECT/chunk/SID wire protocol.
#define LIMIT 4096
#define STREAMS 8
#define BACKENDS 2
#define REQUEST_LIMIT 4096
#define LOCAL_PORT 80
static esp_tls_t *tls;
static jmp_buf failed;
static char response_buffer[LIMIT + 1];
static int transport = -1;
static unsigned reconnects;
static pagekite_settings_t config;
static TaskHandle_t tunnel_task;
static const char *domain, *custom_domain, *secret;
static bool config_changed(void) {
    pagekite_settings_t latest;
    settings_get_pagekite(&latest);
    return memcmp(&latest, &config, sizeof config) != 0;
}
static char client_token[37];
static void set_failure_status(const char *message);
typedef struct {
    char sid[64];
    int fd;
    char *request;
    size_t used, capacity;
    bool ready, write_eof;
    int64_t last_activity;
} stream_t;
static stream_t *streams;
static char pending_ping[32];
static int64_t ping_sent_at, last_ping_at;
static void die(const char *text) { fprintf(stderr, "%s\n", text); set_failure_status(text); longjmp(failed, 1); }

static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
static pagekite_status_state_t current_status = PAGEKITE_STATUS_DISABLED;
static int64_t retry_deadline_us;

static void set_status(pagekite_status_state_t state) {
    portENTER_CRITICAL(&status_mux);
    current_status = state;
    retry_deadline_us = 0;
    portEXIT_CRITICAL(&status_mux);
}

static void set_failure_status(const char *message) {
    bool auth_failure = strstr(message, "rejected kite") ||
                        strstr(message, "authentication") ||
                        strstr(message, "credentials");
    set_status(auth_failure ? PAGEKITE_STATUS_AUTH_ERROR : PAGEKITE_STATUS_NETWORK_ERROR);
}

static void set_retry_deadline(unsigned seconds) {
    portENTER_CRITICAL(&status_mux);
    retry_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    portEXIT_CRITICAL(&status_mux);
}

void pagekite_tunnel_get_status(pagekite_status_t *status) {
    if (!status) return;
    portENTER_CRITICAL(&status_mux);
    status->state = current_status;
    int64_t retry_at = retry_deadline_us;
    portEXIT_CRITICAL(&status_mux);
    int64_t remaining = retry_at - esp_timer_get_time();
    status->retry_in_s = remaining > 0 ? (uint32_t)((remaining + 999999) / 1000000) : 0;
}

static void write_tls(const void *data, size_t length) {
    const unsigned char *p = data;
    while (length) { int n = esp_tls_conn_write(tls, p, length); if (n <= 0) die("TLS write failed"); p += n; length -= n; }
}
static void read_tls(void *data, size_t length) {
    unsigned char *p = data;
    while (length) { int n = esp_tls_conn_read(tls, p, length); if (n <= 0) die("TLS read failed"); p += n; length -= n; }
}
static void line(char *data, size_t size) {
    size_t n = 0;
    while (n + 1 < size) { read_tls(data + n, 1); if (data[n++] == '\n') { data[n] = 0; return; } }
    die("Header too long");
}
static void hex(const unsigned char *data, size_t size, char *out) {
    for (size_t i=0; i<size; i++) sprintf(out+2*i, "%02x", data[i]);
}
static void signature(const char *payload, char out[37]) {
    unsigned char salt[4], digest[20]; char salt_hex[9], digest_hex[41];
    esp_fill_random(salt, sizeof salt);
    hex(salt, sizeof salt, salt_hex);
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    if (mbedtls_sha1_starts(&ctx) || mbedtls_sha1_update(&ctx, (const unsigned char *)secret, strlen(secret)) ||
        mbedtls_sha1_update(&ctx, (const unsigned char *)payload, strlen(payload)) ||
        mbedtls_sha1_update(&ctx, (const unsigned char *)salt_hex, 8) || mbedtls_sha1_finish(&ctx, digest)) die("Signature failed");
    mbedtls_sha1_free(&ctx); hex(digest, sizeof digest, digest_hex);
    snprintf(out, 37, "%s%.28s", salt_hex, digest_hex);
}
static void chunk(const void *data, size_t size) {
    char header[32]; int n = snprintf(header, sizeof header, "%zx\r\n", size);
    write_tls(header, n); write_tls(data, size);
}
static void auth_header_for(char *out, size_t capacity, const char *host, const char *challenge) {
    char payload[1024], sign[37];
    int n = snprintf(payload, sizeof payload, "http:%s:%s:%s", host, client_token, challenge);
    if (n < 0 || (size_t)n >= sizeof payload) die("Authentication too large");
    signature(payload, sign);
    n = snprintf(out, capacity, "X-PageKite: %s:%s\r\n", payload, sign);
    if (n < 0 || (size_t)n >= capacity) die("Authentication header too large");
}
static bool registered_host(const char *host) {
    return !strcmp(host, domain) || (custom_domain && *custom_domain && !strcmp(host, custom_domain));
}
static size_t read_chunk(char *out) {
    char length[32], *end; line(length, sizeof length);
    unsigned long n = strtoul(length, &end, 16);
    if (end == length || strcmp(end, "\r\n") || !n || n > LIMIT) die("Unsupported chunk length");
    read_tls(out, n); out[n] = 0; return n;
}
static void authenticate(void) {
    unsigned char nonce[18]; esp_fill_random(nonce, sizeof nonce);
    hex(nonce, sizeof nonce, client_token);
    char auth[512], alias_auth[512], request[1500];
    auth_header_for(auth, sizeof auth, domain, "");
    alias_auth[0] = 0;
    if (custom_domain && *custom_domain) auth_header_for(alias_auth, sizeof alias_auth, custom_domain, "");
    int n = snprintf(request, sizeof request,
        "CONNECT PageKite:1 HTTP/1.0\r\nX-PageKite-Features: AddKites\r\n"
        "X-PageKite-Version: flue-monitor-c-prototype-0.1\r\n%s%s\r\n", auth, alias_auth);
    if (n < 0 || (size_t)n >= sizeof request) die("Handshake request too large");
    write_tls(request, n);
    char *response = response_buffer;
    size_t used = 0;
    do {
        if (used >= LIMIT) die("Handshake too large");
        read_tls(response + used++, 1);
    } while (used < 4 || memcmp(response + used - 4, "\r\n\r\n", 4));
    response[used] = 0;
    for (int attempt=0; attempt<2; attempt++) {
        if (strstr(response, "X-PageKite-Reject") || strstr(response, "X-PageKite-Duplicate") ||
            strstr(response, "X-PageKite-Invalid")) die("Relay rejected kite (details suppressed)");
        char answers[1200] = {0}; size_t answers_used = 0; bool challenged = false;
        char *challenge = response;
        while ((challenge = strstr(challenge, "X-PageKite-SignThis:")) != NULL) {
            char *value = challenge + strlen("X-PageKite-SignThis:");
            char *end = strstr(value, "\r\n"); if (!end) die("Malformed challenge");
            *end = 0;
            while (*value == ' ' || *value == '\t') value++;
            char *first_colon = strchr(value, ':');
            char *second_colon = first_colon ? strchr(first_colon + 1, ':') : NULL;
            char *token = strrchr(value, ':');
            if (!first_colon || !second_colon || !token || token + 1 == value + strlen(value)) die("Malformed challenge");
            char host[SETTINGS_PAGEKITE_TEXT_MAX_LEN];
            size_t host_len = (size_t)(second_colon - first_colon - 1);
            if (!host_len || host_len >= sizeof host) die("Malformed challenge host");
            memcpy(host, first_colon + 1, host_len); host[host_len] = 0;
            if (!registered_host(host)) die("Relay challenged an unknown kite");
            auth_header_for(auth, sizeof auth, host, token + 1);
            size_t auth_len = strlen(auth);
            if (auth_len > sizeof answers - answers_used - 1) die("Too many relay challenges");
            memcpy(answers + answers_used, auth, auth_len); answers_used += auth_len;
            challenged = true;
            challenge = end + 2;
        }
        if (!challenged) {
            if (strstr(response, "X-PageKite-OK:")) { puts("AUTHENTICATED"); fflush(stdout); return; }
            die("Unexpected relay handshake");
        }
        n = snprintf(request, sizeof request, "NOOP: 1\r\n%s\r\n", answers);
        if (n < 0 || (size_t)n >= sizeof request) die("Authentication response too large");
        chunk(request, n); read_chunk(response);
    }
    die("Unresolved authentication challenge");
}
#define FRONTEND_POOL "frontends.b5p.us"
#define FRONTEND_CANDIDATES 8
#define FRONTEND_PROBE_TIMEOUT_MS 3000

// Resolve PageKite's published frontend pool, try every returned IPv4
// address with certificate verification, and choose the quickest TLS route.
// esp-tls selects only one getaddrinfo result, so probe addresses explicitly.
static bool connect_verified(const char *host, int timeout_ms) {
    tls = esp_tls_init();
    if (!tls) return false;
    esp_tls_cfg_t cfg = {.crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = FRONTEND_POOL, .timeout_ms = timeout_ms};
    if (esp_tls_conn_new_sync(host, strlen(host), 443, &cfg, tls) != 1 ||
        esp_tls_get_conn_sockfd(tls, &transport) != ESP_OK) {
        esp_tls_conn_destroy(tls); tls = NULL; transport = -1;
        return false;
    }
    return true;
}

static bool select_frontend(char selected[INET_ADDRSTRLEN]) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *results = NULL;
    if (getaddrinfo(FRONTEND_POOL, "443", &hints, &results) != 0 || !results) return false;
    char candidates[FRONTEND_CANDIDATES][INET_ADDRSTRLEN];
    size_t count = 0;
    for (struct addrinfo *it = results; it && count < FRONTEND_CANDIDATES; it = it->ai_next) {
        struct sockaddr_in *address = (struct sockaddr_in *)it->ai_addr;
        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &address->sin_addr, ip, sizeof ip)) continue;
        bool duplicate = false;
        for (size_t i = 0; i < count; i++) if (!strcmp(ip, candidates[i])) duplicate = true;
        if (!duplicate) strcpy(candidates[count++], ip);
    }
    freeaddrinfo(results);
    bool found = false;
    int64_t fastest = INT64_MAX;
    for (size_t i = 0; i < count; i++) {
        int64_t started = esp_timer_get_time();
        if (!connect_verified(candidates[i], FRONTEND_PROBE_TIMEOUT_MS)) continue;
        int64_t elapsed = esp_timer_get_time() - started;
        esp_tls_conn_destroy(tls); tls = NULL; transport = -1;
        if (!found || elapsed < fastest) {
            strcpy(selected, candidates[i]); fastest = elapsed; found = true;
        }
    }
    return found;
}

static void ddns_signature(const char *ip, char out[49]) {
    unsigned char salt[4], digest[20];
    char salt_hex[9], digest_hex[41], payload[SETTINGS_PAGEKITE_TEXT_MAX_LEN * 2 + 64];
    esp_fill_random(salt, sizeof salt); hex(salt, sizeof salt, salt_hex);
    int n = snprintf(payload, sizeof payload, "%s%s:%s%s", secret, domain, ip, salt_hex);
    if (n < 0 || (size_t)n >= sizeof payload) die("DNS update signature input too large");
    mbedtls_sha1_context ctx; mbedtls_sha1_init(&ctx);
    if (mbedtls_sha1_starts(&ctx) ||
        mbedtls_sha1_update(&ctx, (const unsigned char *)payload, (size_t)n) ||
        mbedtls_sha1_finish(&ctx, digest)) die("DNS update signature failed");
    mbedtls_sha1_free(&ctx); hex(digest, sizeof digest, digest_hex);
    snprintf(out, 49, "%s%s", salt_hex, digest_hex);
    memset(payload, 0, sizeof payload);
}

typedef struct { char body[96]; size_t used; } ddns_reply_t;
static esp_err_t ddns_event_handler(esp_http_client_event_t *event) {
    ddns_reply_t *reply = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0 && reply) {
        size_t amount = (size_t)event->data_len;
        if (amount > sizeof reply->body - 1 - reply->used) amount = sizeof reply->body - 1 - reply->used;
        memcpy(reply->body + reply->used, event->data, amount);
        reply->used += amount; reply->body[reply->used] = 0;
    }
    return ESP_OK;
}

// PageKite requires the public kite DNS to follow the selected frontend.
// Update it over separately verified HTTPS after freeing the probe TLS context.
static void update_kite_dns(const char *ip) {
    char sign[49], url[320]; ddns_signature(ip, sign);
    int n = snprintf(url, sizeof url,
        "https://up.pagekite.net/?hostname=%s&myip=%s&sign=%s", domain, ip, sign);
    memset(sign, 0, sizeof sign);
    if (n < 0 || (size_t)n >= sizeof url) die("DNS update URL too large");
    ddns_reply_t reply = {0};
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 15000,
        .buffer_size = 512, .buffer_size_tx = 512,
        .event_handler = ddns_event_handler, .user_data = &reply,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = "up.pagekite.net",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) die("DNS update client unavailable");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    memset(url, 0, sizeof url);
    // Some PageKite DDNS responses end with an unclean close before ESP-IDF's
    // chunk parser sees the terminal chunk. Trust the update only when the
    // complete application-level success token and HTTP success status arrived.
    bool accepted = status == 200 &&
        ((!strncmp(reply.body, "good", 4) && reply.used >= 4) ||
         (!strncmp(reply.body, "nochg", 5) && reply.used >= 5));
    if (!accepted) {
        bool explicit_rejection = status == 200 && reply.used > 0 &&
            strncmp(reply.body, "good", 4) && strncmp(reply.body, "nochg", 5);
        memset(&reply, 0, sizeof reply);
        die(explicit_rejection ? "PageKite DNS credentials or kite domain rejected" :
                                 "Verified PageKite DNS update failed");
    }
    if (err != ESP_OK) ESP_LOGW("pagekite", "DDNS success confirmed despite incomplete chunked terminator");
    memset(&reply, 0, sizeof reply);
}

static void connect_tls(const char *ip) {
    if (!connect_verified(ip, 10000)) die("Verified relay TLS handshake failed");
}
static void eof(const char *sid) {
    char data[128]; int n=snprintf(data,sizeof data,"SID: %s\r\nEOF: 1WR\r\n\r\n",sid); chunk(data,n);
}
static void release(int i) {
    if (streams[i].fd >= 0) close(streams[i].fd);
    free(streams[i].request);
    memset(&streams[i], 0, sizeof streams[i]);
    streams[i].fd = -1;
}
// Probe the relay even when no visitor traffic arrives. A live TCP socket
// alone cannot detect a silently dropped NAT mapping or a stalled relay.
static void heartbeat(int64_t now) {
    if (*pending_ping) {
        if (now - ping_sent_at >= 15000000) die("Relay heartbeat timed out");
        return;
    }
    if (now - last_ping_at < 30000000) return;
    snprintf(pending_ping, sizeof pending_ping, "%llx", (unsigned long long)now);
    char data[96];
    int n = snprintf(data, sizeof data, "NOOP: 1\r\nPING: %s\r\n\r\n!", pending_ping);
    chunk(data, n);
    ping_sent_at = last_ping_at = now;
}
static bool reserve_request(stream_t *s, size_t needed) {
    if (needed > REQUEST_LIMIT) return false;
    if (needed <= s->capacity) return true;
    size_t capacity = (needed + 511) / 512 * 512;
    if (capacity > REQUEST_LIMIT) capacity = REQUEST_LIMIT;
    char *buffer = realloc(s->request, capacity + 1);
    if (!buffer) return false;
    s->request = buffer; s->capacity = capacity;
    return true;
}
// Only known dashboard requests can reach the local server. Rebuild headers
// and forward one length-delimited JSON body; never upgrades/pipelines.
static const char *route(const char *method, const char *path) {
    if (!strcmp(method, "POST")) {
        if (!strcmp(path, "/api/settings")) return "/api/tunnel/settings";
        if (!strcmp(path, "/api/event")) return "/api/event";
        if (!strcmp(path, "/api/auth/login") || !strcmp(path, "/api/auth/setup") ||
            !strcmp(path, "/api/auth/password") || !strcmp(path, "/api/auth/logout")) return path;
        return NULL;
    }
    static const char *allowed[] = {
        "/", "/dashboard.js", "/dashboard.css", "/burn-graph.js",
        "/alpinejs.min.js", "/vendor/uplot.min.js", "/vendor/uplot.min.css",
        "/fonts/caprasimo-400.woff2", "/fonts/figtree-400.woff2",
        "/fonts/figtree-600.woff2", "/fonts/figtree-700.woff2",
        "/api/reading", "/api/history", "/api/events", "/api/devices/wifi", "/api/settings/status",
        "/api/auth/status",
        "/api/pagekite/status"
    };
    if (!strcmp(path, "/api/settings")) return "/api/tunnel/settings";
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
        if (!strcmp(path, allowed[i])) return allowed[i];
    return NULL;
}
static void reply(const char *sid, int code, const char *reason, const char *body) {
    char data[768];
    int n = snprintf(data, sizeof data,
        "SID: %s\r\n\r\nHTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n%s",
        sid, code, reason, (unsigned)strlen(body), body);
    if (n < 0 || (size_t)n >= sizeof data) die("Response too large");
    chunk(data, n); eof(sid);
}
static void reject(int i, int code, const char *reason) {
    reply(streams[i].sid, code, reason, "{\"error\":\"Request unavailable through the dashboard tunnel\"}\n");
    release(i);
}
static void prepare(int i) {
    stream_t *s = &streams[i];
    char *end = strstr(s->request, "\r\n\r\n");
    if (!end) return;
    size_t header_size = end - s->request + 4;
    // Work on a copy: a POST body may arrive in later PageKite frames.
    // The shared receive buffer is free after receive() copies the frame data.
    char *headers = response_buffer;
    memcpy(headers, s->request, header_size); headers[header_size] = 0;
    for (size_t n = 0; n < header_size; n++) {
        unsigned char c = headers[n];
        if (c == '\r' && n + 1 < header_size && headers[n + 1] == '\n') { n++; continue; }
        if ((c < 32 && c != '\t') || c == 127) { reject(i, 400, "Bad Request"); return; }
    }
    char *first = strstr(headers, "\r\n");
    if (!first) { reject(i, 400, "Bad Request"); return; }
    *first = 0;
    char method[8], path[512], version[16], extra;
    if (sscanf(headers, "%7s %511s %15s %c", method, path, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) {
        reject(i, 400, "Bad Request"); return;
    }
    bool post = !strcmp(method, "POST");
    if (!post && strcmp(method, "GET")) { reject(i, 405, "Method Not Allowed"); return; }
    char *query = strchr(path, '?');
    if (query) *query = 0;
    char encoding[256] = "identity", etag[128] = "";
    unsigned long body_size = 0;
    bool has_length = false, json_type = false;
    char request_host[160] = "localhost", cookie[384] = "", origin[256] = "";
    bool have_host = false, have_cookie = false, have_origin = false;
    char *header = first + 2;
    while (*header && header < headers + header_size - 4) {
        char *next = strstr(header, "\r\n");
        if (!next) { reject(i, 400, "Bad Request"); return; }
        *next = 0;
        char *colon = strchr(header, ':');
        if (!colon || colon == header || header[0] == ' ' || header[0] == '\t') {
            reject(i, 400, "Bad Request"); return;
        }
        *colon++ = 0;
        while (*colon == ' ' || *colon == '\t') colon++;
        if (!strcasecmp(header, "Transfer-Encoding") || !strcasecmp(header, "Upgrade") ||
            !strcasecmp(header, "Content-Encoding")) {
            reject(i, 400, "Bad Request"); return;
        }
        if (!strcasecmp(header, "Expect")) { reject(i, 417, "Expectation Failed"); return; }
        if (!strcasecmp(header, "Content-Length")) {
            char *tail;
            if (has_length || *colon < '0' || *colon > '9') { reject(i, 400, "Bad Request"); return; }
            body_size = strtoul(colon, &tail, 10);
            if (*tail || body_size >= 2048) { reject(i, 413, "Content Too Large"); return; }
            has_length = true;
        }
        if (!strcasecmp(header, "Content-Type"))
            json_type = !strncasecmp(colon, "application/json", 16) && (!colon[16] || colon[16] == ';');
        if (!strcasecmp(header, "Host")) {
            if (have_host || strlen(colon) >= sizeof request_host) { reject(i, 400, "Bad Request"); return; }
            strcpy(request_host, colon); have_host = true;
        }
        if (!strcasecmp(header, "Cookie")) {
            if (have_cookie || strlen(colon) >= sizeof cookie) { reject(i, 431, "Request Header Fields Too Large"); return; }
            strcpy(cookie, colon); have_cookie = true;
        }
        if (!strcasecmp(header, "Origin")) {
            if (have_origin || strlen(colon) >= sizeof origin) { reject(i, 431, "Request Header Fields Too Large"); return; }
            strcpy(origin, colon); have_origin = true;
        }
        if (!strcasecmp(header, "Accept-Encoding")) {
            if (strlen(colon) >= sizeof encoding) { reject(i, 431, "Request Header Fields Too Large"); return; }
            strcpy(encoding, colon);
        }
        if (!strcasecmp(header, "If-None-Match")) {
            if (strlen(colon) >= sizeof etag) { reject(i, 431, "Request Header Fields Too Large"); return; }
            strcpy(etag, colon);
        }
        header = next + 2;
    }
    if (post && (!has_length || !body_size)) { reject(i, 411, "Length Required"); return; }
    if (post && !json_type) { reject(i, 415, "Unsupported Media Type"); return; }
    if (!post && body_size) { reject(i, 400, "Bad Request"); return; }
    if (header_size + body_size > REQUEST_LIMIT) { reject(i, 413, "Content Too Large"); return; }
    if (s->used > header_size + body_size) { reject(i, 400, "Bad Request"); return; }
    if (s->used < header_size + body_size) return;
    const char *target = route(method, path);
    if (!target) { reject(i, 404, "Not Found"); return; }
    char body_header[96] = "";
    if (post) snprintf(body_header, sizeof body_header,
        "Content-Type: application/json\r\nContent-Length: %lu\r\n", body_size);
    if (!have_host || (strcasecmp(request_host, domain) &&
        (!custom_domain || !*custom_domain || strcasecmp(request_host, custom_domain)))) {
        reject(i, 400, "Bad Request"); return;
    }
    char forwarded_auth[704] = "";
    if (have_cookie) snprintf(forwarded_auth + strlen(forwarded_auth), sizeof forwarded_auth - strlen(forwarded_auth), "Cookie: %s\r\n", cookie);
    if (have_origin) snprintf(forwarded_auth + strlen(forwarded_auth), sizeof forwarded_auth - strlen(forwarded_auth), "Origin: %s\r\n", origin);
    int n = snprintf(response_buffer, sizeof response_buffer,
        "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: %s\r\n%s%s%s%s%s\r\n",
        method, target, request_host, encoding, *etag ? "If-None-Match: " : "", etag,
        *etag ? "\r\n" : "", forwarded_auth, body_header);
    if (n < 0 || (size_t)n + body_size > REQUEST_LIMIT) { reject(i, 431, "Request Header Fields Too Large"); return; }
    if (!reserve_request(s, n + body_size)) { reject(i, 503, "Service Unavailable"); return; }
    memmove(s->request + n, s->request + header_size, body_size);
    memcpy(s->request, response_buffer, n);
    s->used = n + body_size; s->request[s->used] = 0; s->ready = true;
}
static void start_backends(void) {
    int active = 0;
    for (int i = 0; i < STREAMS; i++) if (streams[i].fd >= 0) active++;
    for (int i = 0; i < STREAMS && active < BACKENDS; i++) {
        stream_t *s = &streams[i];
        if (!*s->sid || !s->ready || s->fd >= 0) continue;
        s->fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(LOCAL_PORT)};
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (s->fd < 0) { reject(i, 503, "Service Unavailable"); continue; }
        struct timeval timeout = {3, 0};
        setsockopt(s->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        if (connect(s->fd, (struct sockaddr *)&address, sizeof address)) {
            reject(i, 503, "Service Unavailable"); continue;
        }
        size_t offset = 0;
        while (offset < s->used) {
            int n = send(s->fd, s->request + offset, s->used - offset, 0);
            if (n <= 0) break;
            offset += n;
        }
        if (offset != s->used) { reject(i, 502, "Bad Gateway"); continue; }
        // ESP-IDF HTTPD does not close sessions for Connection: close alone.
        // Each proxy socket carries one complete GET or JSON POST: half-close
        // now so HTTPD sees EOF after sending its response and releases it.
        shutdown(s->fd, SHUT_WR);
        free(s->request); s->request = NULL; s->used = 0; s->capacity = 0;
        s->last_activity = esp_timer_get_time(); active++;
    }
}
static void receive(void) {
    char length_line[32], *end;
    line(length_line, sizeof length_line);
    unsigned long left = strtoul(length_line, &end, 16);
    if (end == length_line || strcmp(end, "\r\n") || !left || left > 65536)
        die("Unsupported frame length");
    char headers[1024]; size_t used = 0;
    while (left) {
        if (used >= sizeof headers - 1) die("Frame headers too large");
        read_tls(headers + used++, 1); left--;
        if (used >= 4 && !memcmp(headers + used - 4, "\r\n\r\n", 4)) break;
    }
    if (used < 4 || memcmp(headers + used - 4, "\r\n\r\n", 4)) die("Malformed frame");
    headers[used] = 0;
    char sid[64] = "", ping[128] = "", pong[128] = "", flags[32] = "";
    char *save;
    for (char *p = strtok_r(headers, "\r\n", &save); p; p = strtok_r(NULL, "\r\n", &save)) {
        if (!strncmp(p, "SID: ", 5)) {
            if (strlen(p + 5) >= sizeof sid) die("Invalid stream id");
            strcpy(sid, p + 5);
        }
        if (!strncmp(p, "PING: ", 6)) snprintf(ping, sizeof ping, "%s", p + 6);
        if (!strncmp(p, "PONG: ", 6)) snprintf(pong, sizeof pong, "%s", p + 6);
        if (!strncmp(p, "EOF: ", 5)) snprintf(flags, sizeof flags, "%s", p + 5);
    }
    if (*ping) {
        char pong[160]; int n = snprintf(pong, sizeof pong, "NOOP: 1\r\nPONG: %s\r\n\r\n!", ping);
        chunk(pong, n);
    }
    int index = -1;
    if (*sid) for (int i = 0; i < STREAMS; i++)
        if (!strcmp(streams[i].sid, sid)) { index = i; break; }
    if (index < 0 && *sid && !*flags && left) {
        for (int i = 0; i < STREAMS; i++) if (!*streams[i].sid) {
            streams[i].request = malloc(513);
            streams[i].capacity = 512;
            if (!streams[i].request) break;
            streams[i].request[0] = 0;
            index = i; strcpy(streams[i].sid, sid);
            streams[i].last_activity = esp_timer_get_time(); break;
        }
        if (index < 0) reply(sid, 503, "Service Unavailable", "{\"error\":\"Tunnel busy\"}\n");
    }
    // Drain oversized/denied payloads in bounded pieces to preserve tunnel framing.
    while (left) {
        size_t amount = left > LIMIT ? LIMIT : left;
        read_tls(response_buffer, amount); left -= amount;
        if (index >= 0) {
            stream_t *s = &streams[index];
            if (s->ready || s->write_eof || s->used + amount > REQUEST_LIMIT ||
                memchr(response_buffer, 0, amount)) {
                reject(index, 400, "Bad Request"); index = -1; continue;
            }
            if (!reserve_request(s, s->used + amount)) {
                reject(index, 503, "Service Unavailable"); index = -1; continue;
            }
            memcpy(s->request + s->used, response_buffer, amount);
            s->used += amount; s->request[s->used] = 0;
            s->last_activity = esp_timer_get_time();
            prepare(index);
            if (!*s->sid) index = -1;
        }
    }
    if (*pending_ping && !strcmp(pong, pending_ping)) pending_ping[0] = 0;
    if (index >= 0 && *flags) {
        if (strchr(flags, 'R')) { release(index); }
        else if (strchr(flags, 'W')) {
            if (!streams[index].ready) { reject(index, 400, "Bad Request"); return; }
            streams[index].write_eof = true;
            if (streams[index].fd >= 0) shutdown(streams[index].fd, SHUT_WR);
        }
    }
}
static void session_loop(void) {
    int64_t reconfigure_at = 0;
    pending_ping[0] = 0;
    last_ping_at = esp_timer_get_time();
    for (;;) {
        // Allow the save response to reach the browser before replacing its tunnel.
        if (config_changed() && !reconfigure_at) reconfigure_at = esp_timer_get_time() + 1000000;
        if (reconfigure_at && esp_timer_get_time() >= reconfigure_at) return;
        start_backends();
        fd_set ready; FD_ZERO(&ready); FD_SET(transport, &ready); int highest = transport;
        for (int i = 0; i < STREAMS; i++) if (streams[i].fd >= 0) {
            FD_SET(streams[i].fd, &ready);
            if (streams[i].fd > highest) highest = streams[i].fd;
        }
        struct timeval timeout = {0, 100000};
        int n = select(highest + 1, &ready, NULL, NULL, &timeout);
        if (n < 0) { if (errno == EINTR) continue; die("Tunnel select failed"); }
        // Process backend descriptors before relay dispatch can release/reuse them.
        for (int i = 0; i < STREAMS; i++) if (streams[i].fd >= 0 && FD_ISSET(streams[i].fd, &ready)) {
            char data[2200];
            int prefix = snprintf(data, sizeof data, "SID: %s\r\n\r\n", streams[i].sid);
            int got = recv(streams[i].fd, data + prefix, 2048, 0);
            if (got > 0) {
                chunk(data, prefix + got); streams[i].last_activity = esp_timer_get_time();
            } else { eof(streams[i].sid); release(i); }
        }
        if (esp_tls_get_bytes_avail(tls) > 0 || FD_ISSET(transport, &ready)) receive();
        int64_t now = esp_timer_get_time();
        heartbeat(now);
        for (int i = 0; i < STREAMS; i++) if (*streams[i].sid && now - streams[i].last_activity > 30000000) {
            eof(streams[i].sid); release(i);
        }
    }
}
static void task(void *unused) {
    (void)unused;
    domain = config.domain; custom_domain = config.custom_domain; secret = config.secret;
    streams = calloc(STREAMS, sizeof *streams);
    if (!streams) { ESP_LOGE("pagekite", "Stream allocation failed"); tunnel_task = NULL; vTaskDelete(NULL); return; }
    for (int i = 0; i < STREAMS; i++) streams[i].fd = -1;
    static unsigned backoff = 2;
    for (;;) {
        settings_get_pagekite(&config);
        if (!config.enabled) {
            set_status(PAGEKITE_STATUS_DISABLED);
            // Wake on settings save; disabling closes all relay/backend sockets.
            memset(config.secret, 0, sizeof config.secret);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            backoff = 2;
            continue;
        }
        set_status(PAGEKITE_STATUS_CONNECTING);
        // SNTP must have set wall-clock time before certificate validation.
        if (time(NULL) < 1700000000) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); continue; }
        transport = -1;
        if (!setjmp(failed)) {
            char selected[INET_ADDRSTRLEN];
            if (!select_frontend(selected)) die("No verified PageKite frontend available");
            if (config_changed()) continue;
            update_kite_dns(selected);
            if (config_changed()) continue;
            connect_tls(selected); authenticate(); backoff = 2;
            set_status(PAGEKITE_STATUS_CONNECTED);
            ESP_LOGI("pagekite", "Verified automatic PageKite frontend ready");
            session_loop();
        }
        for (int i = 0; i < STREAMS; i++) release(i);
        if (tls) esp_tls_conn_destroy(tls);
        tls = NULL; transport = -1;
        if (config_changed()) { backoff = 2; continue; }
        reconnects++;
        ESP_LOGW("pagekite", "Tunnel disconnected; retry in %u seconds", backoff);
        set_retry_deadline(backoff);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff * 1000));
        if (backoff < 60) backoff = backoff > 30 ? 60 : backoff * 2;
    }
}
void pagekite_tunnel_start(void) {
    pagekite_settings_t initial;
    settings_get_pagekite(&initial);
    if (!initial.enabled) { set_status(PAGEKITE_STATUS_DISABLED); return; }
    set_status(PAGEKITE_STATUS_CONNECTING);
    if (tunnel_task) { xTaskNotifyGive(tunnel_task); return; }
    if (xTaskCreate(task, "pagekite", 12288, NULL, 1, &tunnel_task) != pdPASS)
        { set_status(PAGEKITE_STATUS_NETWORK_ERROR); ESP_LOGE("pagekite", "Unable to allocate tunnel task"); }
}
