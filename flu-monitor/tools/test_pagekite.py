"""Compile the firmware's actual routing/frame/queue functions with socket shims."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'main/pagekite_tunnel.c').read_text()
state = source[source.index('typedef struct {'):source.index('static void die(')]
core = source[source.index('static void eof('):source.index('static void task(')]
preamble = r'''
#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define LIMIT 4096
#define STREAMS 8
#define BACKENDS 2
#define REQUEST_LIMIT 4096
#define LOCAL_PORT 18776
static char response_buffer[LIMIT+1];
static int transport, peer;
static const char *domain="demo.pagekite.me", *custom_domain="demo.example.nl";
static int64_t esp_timer_get_time(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static unsigned esp_get_free_heap_size(void) {return 100000;}
static int esp_reset_reason(void) {return 3;}
static unsigned esp_get_minimum_free_heap_size(void) {return 90000;}
static void die(const char *message) {fprintf(stderr,"%s\n",message);abort();}
static void read_tls(void *data,size_t n) {size_t got=0;while(got<n){ssize_t r=recv(transport,(char *)data+got,n-got,0);assert(r>0);got+=r;}}
static void write_tls(const void *data,size_t n) {assert(send(transport,data,n,0)==(ssize_t)n);}
static void line(char *data,size_t n) {size_t i=0;while(i+1<n){read_tls(data+i,1);if(data[i++]=='\n'){data[i]=0;return;}}abort();}
static void chunk(const void *data,size_t n) {char h[32];int z=snprintf(h,sizeof h,"%zx\r\n",n);write_tls(h,z);write_tls(data,n);}
static int esp_tls_get_bytes_avail(void *unused) {(void)unused;return 0;}
static void *tls;
static bool changed;
static bool config_changed(void) {return changed;}
'''
tests = r'''
static void reset(void) {
 for(int i=0;i<STREAMS;i++)release(i);
 char b[8192];while(recv(peer,b,sizeof b,MSG_DONTWAIT)>0){}
}
static void request(const char *data) {
 reset();streams[0].request=malloc(REQUEST_LIMIT+1);assert(streams[0].request);strcpy(streams[0].sid,"test");
 const char *split=strstr(data,"\r\n");assert(split);
 if(!strstr(data,"Host:")){size_t n=split-data+2;memcpy(streams[0].request,data,n);snprintf(streams[0].request+n,REQUEST_LIMIT-n,"Host: demo.pagekite.me\r\n%s",data+n);}
 else strcpy(streams[0].request,data);
 streams[0].used=strlen(streams[0].request);prepare(0);
}
static void expect_reject(const char *data,int code) {
 request(data);assert(!streams[0].sid[0]);char b[2048];int n=recv(peer,b,sizeof b-1,0);assert(n>0);b[n]=0;char expected[32];snprintf(expected,sizeof expected,"HTTP/1.1 %d",code);assert(strstr(b,expected));
}
static void feed(const char *sid,const char *flags,const void *payload,size_t size) {
 char h[256];int n=snprintf(h,sizeof h,"SID: %s\r\n%s%s%s\r\n",sid,*flags?"EOF: ":"",flags,*flags?"\r\n":"");
 char length[32];int z=snprintf(length,sizeof length,"%zx\r\n",n+size);
 assert(send(peer,length,z,0)==z);assert(send(peer,h,n,0)==n);
 if(size)assert(send(peer,payload,size,0)==(ssize_t)size);
 receive();
}
int main(void) {
 streams=calloc(STREAMS,sizeof *streams);assert(streams);
 for(int i=0;i<STREAMS;i++)streams[i].fd=-1;
 int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));transport=pair[0];peer=pair[1];int buffer_size=262144;setsockopt(peer,SOL_SOCKET,SO_SNDBUF,&buffer_size,sizeof buffer_size);
 request("GET /api/settings?cache=1 HTTP/1.1\r\nHost: demo.pagekite.me\r\nAccept-Encoding: gzip\r\nIf-None-Match: \"tag\"\r\nX-Forwarded-For: bad\r\n\r\n");
 assert(streams[0].ready);assert(strstr(streams[0].request,"GET /api/tunnel/settings HTTP/1.1"));assert(strstr(streams[0].request,"Accept-Encoding: gzip"));assert(strstr(streams[0].request,"If-None-Match: \"tag\""));assert(strstr(streams[0].request,"Host: demo.pagekite.me"));assert(!strstr(streams[0].request,"Forwarded"));
 request("POST /api/auth/login HTTP/1.1\r\nHost: demo.pagekite.me\r\nCookie: flu_session=test\r\nOrigin: https://demo.pagekite.me\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}");assert(streams[0].ready);assert(strstr(streams[0].request,"Cookie: flu_session=test"));assert(strstr(streams[0].request,"Origin: https://demo.pagekite.me"));
 expect_reject("GET /api/auth/status HTTP/1.1\r\nHost: 192.168.1.137\r\n\r\n",400);
 const char *blocked[]={"/probe","/ota?secret=x","/api/tunnel/settings","/ws","/api/wifi","/%2e%2e/ota","//api/settings"};
 for(size_t i=0;i<sizeof blocked/sizeof blocked[0];i++){char b[256];snprintf(b,sizeof b,"GET %s HTTP/1.1\r\n\r\n",blocked[i]);expect_reject(b,404);}
 expect_reject("DELETE /api/settings HTTP/1.1\r\n\r\n",405);
 expect_reject("GET / HTTP/1.1\r\nContent-Length: 1\r\n\r\n",400);
 expect_reject("GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",400);
 expect_reject("GET / HTTP/1.1\r\nUpgrade: websocket\r\n\r\n",400);
 expect_reject("GET / HTTP/1.1\r\n\r\nGET /ota HTTP/1.1\r\n\r\n",400);
 expect_reject("GET / HTTP/1.1\r\nAccept-Encoding: gzip\nInjected: 1\r\n\r\n",400);
 puts("PASS exact routing, credential-free settings mapping, header filtering, upgrade/pipeline rejection");
 reset();const char *first="GET /api/rea",*second="ding HTTP/1.1\r\nHost: demo.pagekite.me\r\n\r\n";
 feed("frag","",first,strlen(first));assert(!streams[0].ready);
 feed("frag","",second,strlen(second));assert(streams[0].ready);
 feed("frag","W",NULL,0);assert(streams[0].sid[0]&&streams[0].write_eof);
 feed("frag","R",NULL,0);assert(!streams[0].sid[0]);
 char *big=malloc(60000);memset(big,'x',60000);feed("big","",big,60000);assert(!streams[0].sid[0]);free(big);
 feed("next","", "GET / HTTP/1.1\r\nHost: demo.pagekite.me\r\n\r\n",strlen("GET / HTTP/1.1\r\nHost: demo.pagekite.me\r\n\r\n"));assert(streams[0].ready);
 puts("PASS fragmented HTTP headers, half-close, oversized frame draining and subsequent frame alignment");

 request("POST /api/event HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 22\r\n\r\n{\"event\":\"added_wood\"}");
 assert(streams[0].ready);assert(strstr(streams[0].request,"POST /api/event HTTP/1.1"));assert(strstr(streams[0].request,"Content-Length: 22"));assert(strstr(streams[0].request,"{\"event\":\"added_wood\"}"));
 request("GET /api/settings/status HTTP/1.1\r\n\r\n");assert(streams[0].ready);
 request("GET /api/pagekite/status HTTP/1.1\r\n\r\n");assert(streams[0].ready);assert(strstr(streams[0].request,"GET /api/pagekite/status HTTP/1.1"));
 reset();const char *post="POST /api/settings HTTP/1.1\r\nHost: demo.pagekite.me\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n";
 feed("post","",post,strlen(post));assert(!streams[0].ready);
 feed("post","","{\"a\":",5);assert(!streams[0].ready);
 feed("post","","1}",2);assert(streams[0].ready);assert(strstr(streams[0].request,"POST /api/tunnel/settings HTTP/1.1"));assert(strstr(streams[0].request,"{\"a\":1}"));
 reset();char payload[1501];memset(payload,'x',1500);payload[1500]=0;
 const char *large_post="POST /api/settings HTTP/1.1\r\nHost: demo.pagekite.me\r\nContent-Type: application/json\r\nContent-Length: 1500\r\n\r\n";
 feed("large-post","",large_post,strlen(large_post));feed("large-post","",payload,800);assert(!streams[0].ready);
 feed("large-post","",payload+800,700);assert(streams[0].ready);assert(streams[0].capacity<=REQUEST_LIMIT);
 assert(strstr(streams[0].request,"Content-Length: 1500"));assert(!strcmp(strstr(streams[0].request,"\r\n\r\n")+4,payload));
 expect_reject("POST /api/event HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}",400);
 expect_reject("POST /api/event HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2048\r\n\r\n",413);
 expect_reject("POST /api/event HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}",415);
 expect_reject("POST /api/event HTTP/1.1\r\nContent-Type: application/json\r\n\r\n{}",411);
 expect_reject("POST /api/event HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}GET / HTTP/1.1\r\n\r\n",400);
 reset();feed("short","",post,strlen(post));feed("short","W",NULL,0);assert(!streams[0].sid[0]);
 puts("PASS JSON POST bodies across frames, exact settings/event routing, acknowledgements, duplicate lengths and truncated/pipelined body rejection");
 reset();int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);int one=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
 struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(LOCAL_PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(!bind(listener,(struct sockaddr *)&address,sizeof address));assert(!listen(listener,8));
 for(int i=0;i<STREAMS;i++){streams[i].request=malloc(REQUEST_LIMIT+1);assert(streams[i].request);snprintf(streams[i].sid,sizeof streams[i].sid,"q%d",i);strcpy(streams[i].request,"GET / HTTP/1.1\r\nHost: demo.pagekite.me\r\n\r\n");streams[i].used=strlen(streams[i].request);prepare(i);}
 start_backends();int active=0;for(int i=0;i<STREAMS;i++)active+=streams[i].fd>=0;assert(active==BACKENDS);assert(streams[0].request==NULL && streams[1].request==NULL && streams[2].request!=NULL);
 int backend[3];for(int i=0;i<2;i++){backend[i]=accept(listener,NULL,NULL);assert(backend[i]>=0);char b[512];assert(recv(backend[i],b,sizeof b,0)>0);assert(recv(backend[i],b,sizeof b,0)==0);}
 release(0);close(backend[0]);start_backends();assert(streams[2].fd>=0);assert(streams[3].fd<0);backend[2]=accept(listener,NULL,NULL);assert(backend[2]>=0);
 reset();close(backend[1]);close(backend[2]);close(listener);
 changed=true;int64_t started=esp_timer_get_time();session_loop();assert(esp_timer_get_time()-started>=900000);
 puts("PASS configuration change exits the active session after the save-response grace period");
 close(transport);close(peer);free(streams);
 puts("PASS eight queued streams, at most two local backends, queued request starts after close");return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)/'test.c';binary=Path(directory)/'test'
    path.write_text(preamble+state+core+tests)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
