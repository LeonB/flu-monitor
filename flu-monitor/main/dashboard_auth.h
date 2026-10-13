#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

void dashboard_auth_init(void);
bool dashboard_auth_is_configured(void);
bool dashboard_auth_is_remote(httpd_req_t *req);
bool dashboard_auth_check(httpd_req_t *req);
bool dashboard_auth_check_csrf(httpd_req_t *req);
esp_err_t dashboard_auth_login(httpd_req_t *req, const char *password, bool remember);
esp_err_t dashboard_auth_set_password(const char *current, const char *password);
void dashboard_auth_logout(httpd_req_t *req);
