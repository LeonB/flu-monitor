"""Exercise actual NVS migration, validation and REST settings code on the host.

Requires an existing ESP-IDF build (uses its cJSON dependency).
"""
import json
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
idf = Path(json.loads((root / 'build/project_description.json').read_text())['idf_path'])
cjson = idf / 'components/json/cJSON'
header = (root / 'main/settings.h').read_text().replace('#pragma once', '').replace('#include "esp_err.h"', '')
config = (root / 'main/config.h').read_text().replace('#pragma once', '')
settings = re.sub(r'^#include .*$','', (root / 'main/settings.c').read_text(), flags=re.M)
api = (root / 'main/rest_api.c').read_text()
serialization = api[api.index('static void settings_to_json('):api.index('static const httpd_uri_t settings_get_uri')]
post = api[api.index('static esp_err_t settings_post_handler('):api.index('static const httpd_uri_t settings_post_uri')]
preamble = r'''
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <ctype.h>
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portMAX_DELAY 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
typedef int SemaphoreHandle_t;
static unsigned char stored[4096];
static size_t stored_len;
static unsigned writes, wakes;
static int xSemaphoreCreateMutex(void) {return 1;}
static void xSemaphoreTake(int a,int b) {(void)a;(void)b;}
static void xSemaphoreGive(int a) {(void)a;}
static int nvs_open(const char *a,int b,int *c) {(void)a;(void)b;*c=1;return ESP_OK;}
static int nvs_get_blob(int a,const char *b,void *c,size_t *n) {(void)a;(void)b;if(!stored_len || *n<stored_len)return ESP_FAIL;memcpy(c,stored,stored_len);*n=stored_len;return ESP_OK;}
static int nvs_set_blob(int a,const char *b,const void *c,size_t n) {(void)a;(void)b;assert(n<=sizeof stored);memcpy(stored,c,n);stored_len=n;writes++;return ESP_OK;}
static int nvs_commit(int a) {(void)a;return ESP_OK;}
static void nvs_close(int a) {(void)a;}
typedef struct {int content_len;void *user_ctx;const char *input;int received,status;char response[4096];} httpd_req_t;
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_SOCK_ERR_TIMEOUT -2
static int httpd_req_recv(httpd_req_t *r,char *out,int n) {memcpy(out,r->input+r->received,n);r->received+=n;return n;}
static int httpd_resp_send_err(httpd_req_t *r,int status,const char *message) {r->status=status;snprintf(r->response,sizeof r->response,"%s",message);return ESP_OK;}
static int httpd_resp_set_type(httpd_req_t *r,const char *type) {(void)r;(void)type;return ESP_OK;}
static int httpd_resp_sendstr(httpd_req_t *r,const char *message) {r->status=200;snprintf(r->response,sizeof r->response,"%s",message);return ESP_OK;}
static bool configured=true, authenticated=true;
static bool dashboard_auth_is_configured(void) {return configured;}
static bool dashboard_auth_is_remote(httpd_req_t *r) {return r->user_ctx != NULL;}
static bool dashboard_auth_check(httpd_req_t *r) {(void)r;return authenticated;}
static bool auth_required(httpd_req_t *r) {if(authenticated)return false;r->status=401;return true;}
static bool dashboard_auth_check_csrf(httpd_req_t *r) {(void)r;return true;}
static double round_to(double a,double b) {return round(a/b)*b;}
static uint32_t ws_server_settings_revision(bool advance) {static uint32_t revision=1;if(advance)revision++;return revision;}
static void ws_server_broadcast_settings_changed(void) {}
static void pagekite_tunnel_start(void) {wakes++;}
static const char *esp_err_to_name(int error) {(void)error;return "error";}
'''
tests = r'''
static void write_api(cJSON *body,bool remote,int status) {
 char *text=cJSON_PrintUnformatted(body);assert(text);
 httpd_req_t request={.content_len=(int)strlen(text),.user_ctx=remote?(void *)1:NULL,.input=text};
 settings_post_handler(&request);assert(request.status==status);
 assert(!strstr(request.response,"test-private-secret"));free(text);
}
int main(void) {
 (void)TAG;(void)esp_err_to_name;
 settings_t old;set_defaults(&old);old.log_heartbeat_min=60;strcpy(old.zone_cold_color,"#fab100");
 memcpy(stored,&old,offsetof(settings_t,pagekite));stored_len=offsetof(settings_t,pagekite);
 settings_init();settings_t current;settings_get(&current);
 assert(current.log_heartbeat_min==60 && !strcmp(current.zone_cold_color,"#fab100"));
 assert(!current.pagekite.enabled && !*current.pagekite.secret && !*current.pagekite.relay);
 // Every historical prefix remains loadable, with defaults filling the tail.
 size_t sizes[]={offsetof(settings_t,zone_cold_color),offsetof(settings_t,breathing_exponent),offsetof(settings_t,maximum_brightness)};
 for(size_t i=0;i<sizeof sizes/sizeof sizes[0];i++){memcpy(stored,&old,sizes[i]);stored_len=sizes[i];settings_init();settings_get(&current);assert(current.log_heartbeat_min==60 && current.maximum_brightness==DEFAULT_MAXIMUM_BRIGHTNESS);}
 settings_t legacy;set_defaults(&legacy);legacy.pagekite.enabled=1;strcpy(legacy.pagekite.domain,"demo.pagekite.me");strcpy(legacy.pagekite.secret,"test-private-secret");
 stored_len=offsetof(settings_t,pagekite)+offsetof(pagekite_settings_t,custom_domain);memcpy(stored,&legacy,stored_len);
 settings_init();settings_get(&current);assert(current.pagekite.enabled && !strcmp(current.pagekite.domain,"demo.pagekite.me") && !strcmp(current.pagekite.secret,"test-private-secret") && !*current.pagekite.custom_domain);
 memset(&current.pagekite,0,sizeof current.pagekite);current.pagekite.enabled=1;assert(settings_set(&current)==ESP_ERR_INVALID_ARG);
 strcpy(current.pagekite.domain,"demo.pagekite.me");strcpy(current.pagekite.secret,"test-private-secret");strcpy(current.pagekite.custom_domain,"flumonitor.example.nl");assert(settings_set(&current)==ESP_OK);
 settings_init();settings_get(&current);assert(current.pagekite.enabled && !strcmp(current.pagekite.secret,"test-private-secret"));
 puts("PASS old NVS prefixes migrate; enabled configuration and secret survive reload");
 const char *bad[]={"https://demo.pagekite.me","demo.pagekite.me/path","bad\r\nInjected: yes","bad..host","-bad.host","bad-.host"};
 for(size_t i=0;i<sizeof bad/sizeof bad[0];i++){pagekite_settings_t p=current.pagekite;strcpy(p.domain,bad[i]);assert(!valid_pagekite(&p));}
 pagekite_settings_t same=current.pagekite;strcpy(same.custom_domain,same.domain);assert(!valid_pagekite(&same));
 pagekite_settings_t bad_alias=current.pagekite;strcpy(bad_alias.custom_domain,"https://bad.example");assert(!valid_pagekite(&bad_alias));
 settings_t invalid=current;memset(invalid.pagekite.secret,'x',sizeof invalid.pagekite.secret);assert(settings_set(&invalid)==ESP_ERR_INVALID_ARG);
 invalid=current;strcpy(invalid.pagekite.secret,"bad secret");assert(settings_set(&invalid)==ESP_ERR_INVALID_ARG);
 puts("PASS domain/header injection, invalid secret and unterminated field validation");
 for(int remote=0;remote<2;remote++){
  httpd_req_t request={.user_ctx=remote?(void *)1:NULL};settings_get_handler(&request);
  assert(request.status==200 && !strstr(request.response,"test-private-secret"));
  cJSON *json=cJSON_Parse(request.response);assert(json);assert(cJSON_IsTrue(cJSON_GetObjectItem(json,"pagekite_secret_set")));
  assert(!cJSON_GetObjectItem(json,"pagekite_secret"));if(remote)assert(!cJSON_GetObjectItem(json,"google_sheets_secret"));
  cJSON_AddStringToObject(json,"pagekite_secret","");write_api(json,remote,200);cJSON_DeleteItemFromObject(json,"pagekite_secret");write_api(json,remote,200);
  settings_get(&current);assert(!strcmp(current.pagekite.secret,"test-private-secret"));assert(wakes==0);
  cJSON_Delete(json);
 }
 cJSON *json=cJSON_CreateObject();settings_to_json(&current,json);
 cJSON_ReplaceItemInObject(json,"pagekite_enabled",cJSON_CreateString("true"));unsigned before=writes;write_api(json,false,400);assert(writes==before);
 cJSON_ReplaceItemInObject(json,"pagekite_enabled",cJSON_CreateBool(true));
 char long_secret[130];memset(long_secret,'x',129);long_secret[129]=0;cJSON_AddStringToObject(json,"pagekite_secret",long_secret);write_api(json,false,400);assert(writes==before);
 cJSON_ReplaceItemInObject(json,"pagekite_secret",cJSON_CreateString("replacement-secret"));write_api(json,false,200);settings_get(&current);assert(!strcmp(current.pagekite.secret,"replacement-secret") && wakes==1);
 cJSON_DeleteItemFromObject(json,"pagekite_secret");cJSON_ReplaceItemInObject(json,"pagekite_enabled",cJSON_CreateBool(false));write_api(json,true,200);settings_get(&current);assert(!current.pagekite.enabled && !strcmp(current.pagekite.secret,"replacement-secret") && wakes==2);
 cJSON_ReplaceItemInObject(json,"pagekite_enabled",cJSON_CreateBool(true));cJSON_ReplaceItemInObject(json,"pagekite_domain",cJSON_CreateString("https://invalid.host"));before=writes;write_api(json,false,400);assert(writes==before);
 cJSON_ReplaceItemInObject(json,"pagekite_domain",cJSON_CreateString("demo.pagekite.me"));cJSON_ReplaceItemInObject(json,"pagekite_enabled",cJSON_CreateBool(false));
 cJSON_DeleteItemFromObject(json,"pagekite_enabled");cJSON_DeleteItemFromObject(json,"pagekite_domain");write_api(json,false,200);settings_get(&current);assert(!current.pagekite.enabled && !strcmp(current.pagekite.domain,"demo.pagekite.me") && wakes==2);
 cJSON_Delete(json);
 authenticated=false;
 httpd_req_t guest={.user_ctx=(void *)1};settings_get_handler(&guest);assert(guest.status==401);
 httpd_req_t device={0};settings_get_handler(&device);assert(device.status==200 && !strstr(device.response,"google_sheets_secret") && !strstr(device.response,"google_sheets_webhook_url"));
 json=cJSON_CreateObject();settings_to_json(&current,json);write_api(json,false,401);write_api(json,true,401);
 configured=false;write_api(json,true,401);write_api(json,false,200);cJSON_Delete(json);
 puts("PASS authenticated settings enforcement, safe LAN display reads and local-only commissioning before enrollment");
 puts("PASS LAN/public credential redaction, blank/omitted secret retention, strict types/lengths, replacement, disabling and older clients");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'settings_test.c'
    binary = Path(directory) / 'settings_test'
    source.write_text(preamble + header + config + settings + serialization + post + tests)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I', str(cjson), str(source), str(cjson / 'cJSON.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
