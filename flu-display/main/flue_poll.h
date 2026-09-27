#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts mDNS (so this device is itself discoverable, and so
// FLU_MONITOR_MDNS_NAME can be resolved), then in the background: resolves
// flu-monitor-idf, fetches its current zone/rate settings (applied via
// led_display_set_thresholds()), and connects a persistent WebSocket to its
// /ws broadcast endpoint. Call once at boot, after WiFi STA is connected.
//
// Non-blocking and event-driven from here on -- there's no poll loop to run
// anymore. Each broadcast reading is validated (same sanity clamp and
// suspicious-jump confirmation as the old REST-polling version) and, if
// accepted, passed straight to led_display_set_reading() as it arrives. A
// "settings changed" broadcast triggers a fresh settings re-fetch. The
// WebSocket client's own built-in auto-reconnect handles a transient
// WiFi/sidecar drop.
void flue_poll_init(void);

// True if no valid reading has arrived recently enough to trust (either
// none yet, or it's been more than STALE_READING_MS since the last one) --
// call this from a lightweight timer/task to fall back to the neutral
// pulse. No network I/O happens here.
bool flue_poll_is_stale(void);

#ifdef __cplusplus
}
#endif
