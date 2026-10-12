#pragma once
#include <stdint.h>

typedef enum {
    PAGEKITE_STATUS_DISABLED,
    PAGEKITE_STATUS_CONNECTING,
    PAGEKITE_STATUS_CONNECTED,
    PAGEKITE_STATUS_AUTH_ERROR,
    PAGEKITE_STATUS_NETWORK_ERROR,
} pagekite_status_state_t;

typedef struct {
    pagekite_status_state_t state;
    uint32_t retry_in_s;
} pagekite_status_t;

// Start or wake the NVS-configured verified-TLS dashboard tunnel.
// Settings changes close old sockets after forwarding the save response.
void pagekite_tunnel_start(void);
void pagekite_tunnel_get_status(pagekite_status_t *status);
