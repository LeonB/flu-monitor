"""Run actual firmware authentication code with host HTTP/NVS/clock shims.

macOS CommonCrypto supplies real PBKDF2-HMAC-SHA256 for the crypto shim.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = re.sub(r'^#include .*$','', (root/'main/dashboard_auth.c').read_text(), flags=re.M)
source = source.replace('getpeername(', 'fake_getpeername(')
preamble = r'''
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <CommonCrypto/CommonKeyDerivation.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_TIMEOUT 3
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define MBEDTLS_MD_SHA256 1
typedef int nvs_handle_t;
typedef struct {int fd;const char *cookie,*host,*origin;const char *set_cookie;} httpd_req_t;
bool dashboard_auth_is_remote(httpd_req_t *req);
static int64_t now;
static unsigned char blob[256];
static size_t blob_size;
static bool fail_hash, fail_nvs;
static int64_t esp_timer_get_time(void) {return now;}
static void esp_fill_random(void *out,size_t n) {arc4random_buf(out,n);}
static int mbedtls_pkcs5_pbkdf2_hmac_ext(int algorithm,const unsigned char *p,size_t len,const unsigned char *salt,size_t salt_len,unsigned rounds,size_t out_len,unsigned char *out) {
 (void)algorithm;if(fail_hash)return -1;return CCKeyDerivationPBKDF(kCCPBKDF2,(const char *)p,len,salt,salt_len,kCCPRFHmacAlgSHA256,rounds,out,out_len);
}
static int nvs_open(const char *a,int b,int *c) {(void)a;(void)b;*c=1;return fail_nvs?ESP_FAIL:ESP_OK;}
static int nvs_get_blob(int a,const char *b,void *out,size_t *n) {(void)a;(void)b;if(!blob_size||*n<blob_size)return ESP_FAIL;memcpy(out,blob,blob_size);*n=blob_size;return ESP_OK;}
static int nvs_set_blob(int a,const char *b,const void *out,size_t n) {(void)a;(void)b;assert(n<=sizeof blob);memcpy(blob,out,n);blob_size=n;return ESP_OK;}
static int nvs_commit(int a) {(void)a;return ESP_OK;}
static void nvs_close(int a) {(void)a;}
static int httpd_req_to_sockfd(httpd_req_t *r) {return r->fd;}
static int fake_getpeername(int fd,struct sockaddr *out,socklen_t *n) {
 if(!fd)return -1;
 if(fd==3 || fd==4){struct sockaddr_in6 *p=(struct sockaddr_in6 *)out;p->sin6_family=AF_INET6;inet_pton(AF_INET6,fd==3?"::ffff:192.168.1.34":"::ffff:127.0.0.1",&p->sin6_addr);*n=sizeof *p;return 0;}
 struct sockaddr_in *p=(struct sockaddr_in *)out;p->sin_family=AF_INET;p->sin_addr.s_addr=htonl(fd==1?0x7f000001:0xc0a80122);*n=sizeof *p;return 0;
}
static int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *key,char *out,size_t n) {
 const char *p=!strcmp(key,"Cookie")?r->cookie:!strcmp(key,"Host")?r->host:r->origin;
 if(!p||strlen(p)>=n)return ESP_FAIL;strcpy(out,p);return ESP_OK;
}
static int httpd_resp_set_hdr(httpd_req_t *r,const char *key,const char *value) {if(!strcmp(key,"Set-Cookie"))r->set_cookie=value;return ESP_OK;}
'''
tests = r'''
int main(void) {
 httpd_req_t remote={.fd=1,.host="demo.pagekite.me",.origin="https://demo.pagekite.me"};
 httpd_req_t local={.fd=2,.host="192.168.1.137",.origin="http://192.168.1.137"};
 assert(dashboard_auth_is_remote(&remote) && !dashboard_auth_is_remote(&local));
 remote.host="192.168.1.137";assert(dashboard_auth_is_remote(&remote));remote.host="demo.pagekite.me";
 httpd_req_t mapped_local={.fd=3},mapped_remote={.fd=4};assert(!dashboard_auth_is_remote(&mapped_local));assert(dashboard_auth_is_remote(&mapped_remote));
 dashboard_auth_init();assert(!dashboard_auth_is_configured() && !dashboard_auth_check(&remote));
 assert(dashboard_auth_set_password(NULL,"short")==ESP_ERR_INVALID_ARG);
 fail_hash=true;assert(dashboard_auth_set_password(NULL,"test-password-123")==ESP_FAIL);assert(!dashboard_auth_is_configured());fail_hash=false;
 fail_nvs=true;assert(dashboard_auth_set_password(NULL,"test-password-123")==ESP_FAIL);assert(!dashboard_auth_is_configured());fail_nvs=false;
 assert(dashboard_auth_set_password(NULL,"test-password-123")==ESP_OK);assert(dashboard_auth_is_configured());
 assert(blob_size==48 && memmem(blob,blob_size,"test-password-123",17)==NULL);
 s_configured=false;memset(&s_credential,0,sizeof s_credential);dashboard_auth_init();assert(dashboard_auth_is_configured());
 assert(dashboard_auth_login(&remote,"test-password-123",false)==ESP_OK);assert(strstr(remote.set_cookie,"HttpOnly") && strstr(remote.set_cookie,"SameSite=Strict") && strstr(remote.set_cookie,"Secure"));
 char valid[80];snprintf(valid,sizeof valid,"flu_session=%s",s_session);remote.cookie=valid;assert(dashboard_auth_check(&remote));
 char forged[128];snprintf(forged,sizeof forged,"x%s",valid);remote.cookie=forged;assert(!dashboard_auth_check(&remote));
 snprintf(forged,sizeof forged,"%sx",valid);remote.cookie=forged;assert(!dashboard_auth_check(&remote));
 snprintf(forged,sizeof forged,"other=abc; %s; extra=1",valid);remote.cookie=forged;assert(dashboard_auth_check(&remote));
 assert(dashboard_auth_check_csrf(&remote));remote.origin="https://demo.pagekite.me.evil";assert(!dashboard_auth_check_csrf(&remote));
 remote.origin="ftp://demo.pagekite.me";assert(!dashboard_auth_check_csrf(&remote));remote.origin="http://demo.pagekite.me";assert(!dashboard_auth_check_csrf(&remote));
 remote.origin=NULL;assert(!dashboard_auth_check_csrf(&remote));assert(dashboard_auth_check_csrf(&local));
 now=86400LL*1000000;assert(dashboard_auth_check(&remote));remote.cookie=NULL;assert(!dashboard_auth_check(&remote));remote.cookie=valid;now=0;
 for(int i=0;i<5;i++)assert(dashboard_auth_login(&remote,"incorrect-password",false)==ESP_ERR_NOT_FOUND);
 assert(dashboard_auth_login(&remote,"test-password-123",false)==ESP_ERR_TIMEOUT);now+=30000001;
 assert(dashboard_auth_login(&remote,"test-password-123",false)==ESP_OK);
 fail_hash=true;assert(dashboard_auth_login(&remote,"test-password-123",false)==ESP_ERR_NOT_FOUND);fail_hash=false;
 assert(dashboard_auth_set_password("incorrect-password","next-password-123")==ESP_ERR_NOT_FOUND);
 assert(dashboard_auth_set_password("test-password-123","next-password-123")==ESP_OK);assert(!*s_session);
 assert(dashboard_auth_login(&local,"test-password-123",false)==ESP_ERR_NOT_FOUND);
 assert(dashboard_auth_login(&local,"next-password-123",false)==ESP_OK);assert(!strstr(local.set_cookie,"Secure"));
 char remembered[80];
 assert(dashboard_auth_login(&local,"next-password-123",true)==ESP_OK);
 snprintf(remembered,sizeof remembered,"flu_session=%s",s_session);local.cookie=remembered;
 now=1000LL*86400*1000000;assert(dashboard_auth_check(&local));
 assert(strstr(local.set_cookie,"Max-Age=34560000"));
 dashboard_auth_init();assert(!dashboard_auth_check(&local));
 assert(dashboard_auth_login(&local,"next-password-123",true)==ESP_OK);snprintf(remembered,sizeof remembered,"flu_session=%s",s_session);
 assert(dashboard_auth_set_password("next-password-123","final-password-123")==ESP_OK);assert(!dashboard_auth_check(&local));
 assert(dashboard_auth_set_password("final-password-123","next-password-123")==ESP_OK);
 dashboard_auth_logout(&local);assert(!dashboard_auth_check(&local));now=0;
 assert(dashboard_auth_login(&local,"next-password-123",false)==ESP_OK);
 assert(!strstr(local.set_cookie,"Max-Age") && !strstr(local.set_cookie,"Expires"));
 now=86400LL*1000000;snprintf(remembered,sizeof remembered,"flu_session=%s",s_session);local.cookie=remembered;assert(dashboard_auth_check(&local));
 now+=86400LL*1000000;assert(dashboard_auth_check(&local));
 local.cookie=NULL;assert(!dashboard_auth_check(&local));
 dashboard_auth_init();local.cookie=remembered;assert(!dashboard_auth_check(&local));now=0;
 local.cookie=s_session;dashboard_auth_logout(&local);assert(!*s_session && strstr(local.set_cookie,"Max-Age=0"));
 puts("PASS password hashing/persistence, crypto/NVS failure, socket-based remote detection, cookie boundaries, browser-session versus persistent cookies, CSRF/HTTPS, lockout, password change and logout");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)/'auth.c';binary=Path(directory)/'auth'
    path.write_text(preamble+source+tests)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fsanitize-address-use-after-return=always',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
