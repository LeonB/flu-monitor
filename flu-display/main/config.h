#pragma once

// Tunable constants for flu-display. Plain #defines are the ESP-IDF
// equivalent of the `substitutions:` block at the top of ../flu-monitor.yaml
// -- same idea (one place to tune things), different toolchain.

// --- Milestone 1: WiFi + captive portal ---

// The SoftAP's own identity when no WiFi credentials are stored yet (or the
// stored ones fail to connect). Analogous to flu-monitor.yaml's
// `wifi: ap: ssid/password:` fallback block.
#define SETUP_AP_SSID       "Flu Display Setup"
#define SETUP_AP_PASSWORD   "flu-display-setup"
#define SETUP_AP_MAX_CONN   4

// How long to wait for a stored-credentials STA connection to succeed
// before giving up and falling back to the setup AP.
#define STA_CONNECT_TIMEOUT_MS   15000

// --- Milestone 2: poll flu-monitor's JSON API ---

// mDNS hostname WITHOUT the ".local" suffix -- mdns_query_a() (see
// flue_poll.c) rejects/warns on a name that includes it. Relying on the
// implicit LWIP ".local" resolver hook (passing "flu-monitor.local" straight
// to esp_http_client) worked in initial testing but was NOT reliable across
// every boot/network condition observed since, so flue_poll.c resolves this
// explicitly instead. See CLAUDE.md's "flu-display" section.
#define FLU_MONITOR_MDNS_NAME   "flu-monitor"
#define MDNS_QUERY_TIMEOUT_MS   3000
#define POLL_INTERVAL_MS        3000

// Placeholders -- replace once flu-monitor's multi-week data-gathering run
// gives real cold/good/hot boundaries (see CLAUDE.md's "Project goal").
#define ZONE_COLD_MAX_C         150.0f
#define ZONE_GOOD_MAX_C         280.0f
#define FAST_RISE_C_PER_MIN     20.0f

// --- Milestone 3: LED ring ---

#define LED_GPIO                13
#define LED_COUNT               24

// Breathing-pulse timing: how long one dim->bright->dim cycle takes at the
// two ends of the range. Speeds up smoothly toward FAST_PULSE_PERIOD_MS as
// the *positive* rate of change (relative to FAST_RISE_C_PER_MIN, above)
// increases; a falling or stable reading always stays at the slow idle
// pace, since a fast drop isn't the failure mode this is watching for.
#define IDLE_PULSE_PERIOD_MS    4000
#define FAST_PULSE_PERIOD_MS    800

// Pulse brightness floor/ceiling, 0-255 per-pixel scale.
#define PULSE_BRIGHTNESS_MIN    60
#define PULSE_BRIGHTNESS_MAX    255

// How often the render task recomputes the pulse -- fast enough to look
// smooth, far below what would meaningfully load the RMT peripheral.
#define LED_RENDER_TICK_MS      30

// How long without a *valid* poll before the ring falls back to the
// neutral "no data yet" pulse instead of holding the last known color.
#define STALE_READING_MS        30000
