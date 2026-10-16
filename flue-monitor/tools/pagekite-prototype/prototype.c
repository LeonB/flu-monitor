#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// Independently implemented PageKite CONNECT/chunk/SID wire protocol.
#define LIMIT 65536
#define STREAMS 8
static SSL *tls;
static int transport = -1;
static volatile sig_atomic_t stopping;
static const char *domain, *secret;
static char client_token[37];
static struct {char sid[64]; int fd;} streams[STREAMS];
static void die(const char *text) { fprintf(stderr, "%s\n", text); exit(1); }
static void stop(int signum) { (void)signum; stopping = 1; }
static void write_tls(const void *data, size_t length) {
    const unsigned char *p = data;
    while (length) { int n = SSL_write(tls, p, length); if (n <= 0) die("TLS write failed"); p += n; length -= n; }
}
static void read_tls(void *data, size_t length) {
    unsigned char *p = data;
    while (length) { int n = SSL_read(tls, p, length); if (n <= 0) die("TLS read failed"); p += n; length -= n; }
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
    if (RAND_bytes(salt, sizeof salt) != 1) die("Random generation failed");
    hex(salt, sizeof salt, salt_hex);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned length;
    if (!ctx || !EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) ||
        !EVP_DigestUpdate(ctx, secret, strlen(secret)) ||
        !EVP_DigestUpdate(ctx, payload, strlen(payload)) ||
        !EVP_DigestUpdate(ctx, salt_hex, 8) || !EVP_DigestFinal_ex(ctx, digest, &length)) die("Signature failed");
    EVP_MD_CTX_free(ctx); hex(digest, sizeof digest, digest_hex);
    snprintf(out, 37, "%s%.28s", salt_hex, digest_hex);
}
static void chunk(const void *data, size_t size) {
    char header[32]; int n = snprintf(header, sizeof header, "%zx\r\n", size);
    write_tls(header, n); write_tls(data, size);
}
static void auth_header(char *out, size_t capacity, const char *challenge) {
    char payload[1024], sign[37];
    int n = snprintf(payload, sizeof payload, "http:%s:%s:%s", domain, client_token, challenge);
    if (n < 0 || (size_t)n >= sizeof payload) die("Authentication too large");
    signature(payload, sign);
    n = snprintf(out, capacity, "X-PageKite: %s:%s\r\n", payload, sign);
    if (n < 0 || (size_t)n >= capacity) die("Authentication header too large");
}
static size_t read_chunk(char *out) {
    char length[32], *end; line(length, sizeof length);
    unsigned long n = strtoul(length, &end, 16);
    if (end == length || strcmp(end, "\r\n") || !n || n > LIMIT) die("Unsupported chunk length");
    read_tls(out, n); out[n] = 0; return n;
}
static void authenticate(void) {
    unsigned char nonce[18]; if (RAND_bytes(nonce, sizeof nonce) != 1) die("Random generation failed");
    hex(nonce, sizeof nonce, client_token);
    char auth[1200], request[1500]; auth_header(auth, sizeof auth, "");
    int n = snprintf(request, sizeof request,
        "CONNECT PageKite:1 HTTP/1.0\r\nX-PageKite-Features: AddKites\r\n"
        "X-PageKite-Version: flue-monitor-c-prototype-0.1\r\n%s\r\n", auth);
    write_tls(request, n);
    char *response = malloc(LIMIT + 1); if (!response) die("Allocation failed");
    size_t used = 0;
    do {
        if (used >= LIMIT) die("Handshake too large");
        read_tls(response + used++, 1);
    } while (used < 4 || memcmp(response + used - 4, "\r\n\r\n", 4));
    response[used] = 0;
    for (int attempt=0; attempt<2; attempt++) {
        if (strstr(response, "X-PageKite-Reject") || strstr(response, "X-PageKite-Duplicate")) die("Relay rejected kite (details suppressed)");
        if (strstr(response, "X-PageKite-OK:")) { free(response); puts("AUTHENTICATED"); fflush(stdout); return; }
        char *challenge = strstr(response, "X-PageKite-SignThis:");
        if (!challenge) die("Unexpected relay handshake");
        char *end = strstr(challenge, "\r\n"); if (!end) die("Malformed challenge"); *end = 0;
        char *token = strrchr(challenge, ':'); if (!token) die("Malformed challenge");
        auth_header(auth, sizeof auth, token + 1);
        n = snprintf(request, sizeof request, "NOOP: 1\r\n%s\r\n", auth);
        chunk(request, n); read_chunk(response);
    }
    die("Unresolved authentication challenge");
}
static void connect_tls(void) {
    struct addrinfo hints = {.ai_socktype=SOCK_STREAM}, *addresses;
    const char *relay = getenv("PAGEKITE_RELAY");
    if (!relay || !*relay) relay = "fe4_100.b5p.us";
    if (getaddrinfo(relay, "443", &hints, &addresses)) die("Relay DNS failed");
    char ip[INET6_ADDRSTRLEN] = "";
    for (struct addrinfo *p=addresses; p; p=p->ai_next) {
        int fd=socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(fd<0) continue;
        struct timeval timeout={10,0};
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
        if(!connect(fd,p->ai_addr,p->ai_addrlen)) {
            transport=fd;
            if (p->ai_family==AF_INET) inet_ntop(AF_INET,&((struct sockaddr_in *)p->ai_addr)->sin_addr,ip,sizeof ip);
            break;
        }
        close(fd);
    }
    freeaddrinfo(addresses); if(transport<0) die("Relay unavailable");
    SSL_CTX *ctx=SSL_CTX_new(TLS_client_method()); if(!ctx) die("TLS initialization failed");
    SSL_CTX_set_verify(ctx,SSL_VERIFY_PEER,NULL); SSL_CTX_set_min_proto_version(ctx,TLS1_2_VERSION);
    if(!SSL_CTX_set_default_verify_paths(ctx)) die("Cannot load CA certificates");
    tls=SSL_new(ctx); SSL_CTX_free(ctx); if(!tls) die("TLS initialization failed");
    const char *name="frontends.b5p.us"; // Published relay identity, distinct from the DNS pool name.
#if OPENSSL_VERSION_NUMBER >= 0x40000000L
    if(!SSL_set1_dnsname(tls,name)) die("Cannot set TLS identity");
#else
    if(!SSL_set1_host(tls,name)) die("Cannot set TLS identity");
#endif
    if(!SSL_set_tlsext_host_name(tls,name) || !SSL_set_fd(tls,transport) || SSL_connect(tls)!=1) die("Verified relay TLS handshake failed");
    printf("RELAY %s\n",ip); puts("TLS certificate and relay identity verified"); fflush(stdout);
}
static void eof(const char *sid) {
    char data[128]; int n=snprintf(data,sizeof data,"SID: %s\r\nEOF: 1WR\r\n\r\n",sid); chunk(data,n);
}
static void release(int i) { if(streams[i].fd>=0) close(streams[i].fd); streams[i].fd=-1; streams[i].sid[0]=0; }
static void receive(void) {
    char *data=malloc(LIMIT+1); if(!data) die("Allocation failed");
    size_t length=read_chunk(data); char *separator=strstr(data,"\r\n\r\n");
    if(!separator) die("Malformed frame");
    size_t header_size=separator-data+4; *separator=0;
    char sid[64]="", ping[128]="", end_flags[32]="";
    for(char *p=strtok(data,"\r\n");p;p=strtok(NULL,"\r\n")) {
        if(!strncmp(p,"SID: ",5)) snprintf(sid,sizeof sid,"%s",p+5);
        if(!strncmp(p,"PING: ",6)) snprintf(ping,sizeof ping,"%s",p+6);
        if(!strncmp(p,"EOF: ",5)) snprintf(end_flags,sizeof end_flags,"%s",p+5);
    }
    if(*ping) { char pong[160]; int n=snprintf(pong,sizeof pong,"NOOP: 1\r\nPONG: %s\r\n\r\n!",ping); chunk(pong,n); puts("Relay heartbeat answered"); fflush(stdout); }
    if(!*sid) { free(data); return; }
    int index=-1;
    for(int i=0;i<STREAMS;i++) if(!strcmp(streams[i].sid,sid)) {index=i;break;}
    if(index<0 && !*end_flags) for(int i=0;i<STREAMS;i++) if(!streams[i].sid[0]) {
        index=i; snprintf(streams[i].sid,sizeof streams[i].sid,"%s",sid);
        streams[i].fd=socket(AF_INET,SOCK_STREAM,0);
        struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(18776)};
        inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
        if(streams[i].fd<0 || connect(streams[i].fd,(struct sockaddr *)&address,sizeof address)) {eof(sid);release(i);index=-1;}
        else {struct timeval timeout={5,0};setsockopt(streams[i].fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);}
        break;
    }
    if(index>=0) {
        size_t left=length-header_size; const char *p=data+header_size;
        while(left) {ssize_t n=send(streams[index].fd,p,left,0);if(n<=0){eof(sid);release(index);index=-1;break;}p+=n;left-=n;}
        if(index>=0 && *end_flags) {eof(sid);release(index);}
    } else if(!*end_flags) eof(sid);
    free(data);
}
int main(void) {
    domain=getenv("PAGEKITE_DOMAIN"); secret=getenv("PAGEKITE_SECRET");
    if(!domain || !secret || !*domain || !*secret) die("Missing PageKite environment configuration");
    signal(SIGINT,stop); signal(SIGTERM,stop); signal(SIGPIPE,SIG_IGN);
    for(int i=0;i<STREAMS;i++) streams[i].fd=-1;
    connect_tls(); authenticate(); printf("URL https://%s\n",domain);fflush(stdout);
    time_t started=time(NULL);
    while(!stopping && time(NULL)-started<180) {
        fd_set ready;FD_ZERO(&ready);FD_SET(transport,&ready);int highest=transport;
        for(int i=0;i<STREAMS;i++) if(streams[i].fd>=0){FD_SET(streams[i].fd,&ready);if(streams[i].fd>highest)highest=streams[i].fd;}
        struct timeval timeout={1,0};int n=select(highest+1,&ready,NULL,NULL,&timeout);
        if(n<0){if(errno==EINTR)continue;die("select failed");}
        // Consume ready backend fds before relay dispatch can close/reuse them.
        for(int i=0;i<STREAMS;i++) if(streams[i].fd>=0 && FD_ISSET(streams[i].fd,&ready)) {
            char data[4200];int prefix=snprintf(data,sizeof data,"SID: %s\r\n\r\n",streams[i].sid);
            ssize_t got=recv(streams[i].fd,data+prefix,4096,0);
            if(got>0)chunk(data,prefix+got);else{eof(streams[i].sid);release(i);}
        }
        if(SSL_pending(tls) || FD_ISSET(transport,&ready)) receive();
    }
    for(int i=0;i<STREAMS;i++)release(i);
    SSL_free(tls);close(transport);return 0;
}
