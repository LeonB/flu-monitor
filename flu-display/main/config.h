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

// --- Milestones 2-3 (not implemented yet, kept here so the whole set of
// tunables lives in one place from the start) ---

#define FLU_MONITOR_HOST        "flu-monitor.local"
#define POLL_INTERVAL_MS        3000

// Placeholders -- replace once flu-monitor's multi-week data-gathering run
// gives real cold/good/hot boundaries (see CLAUDE.md's "Project goal").
#define ZONE_COLD_MAX_C         150.0f
#define ZONE_GOOD_MAX_C         280.0f
#define FAST_RISE_C_PER_MIN     20.0f

#define LED_GPIO                13
#define LED_COUNT               24
