#pragma once

// Tunable constants for flu-monitor-idf. Plain #defines are the compile-time
// fallback layer -- once `settings.c` lands (Milestone 3), the values that
// used to be ESPHome's `substitutions:` block become NVS-backed and
// runtime-editable via the REST API; these #defines then only matter as
// first-boot defaults before NVS has anything saved.

// --- Milestone 1: WiFi + captive portal ---

// The SoftAP's own identity when no WiFi credentials are stored yet (or the
// stored ones fail to connect).
#define SETUP_AP_SSID       "Flu Monitor Setup"
#define SETUP_AP_PASSWORD   "flu-monitor-setup"
#define SETUP_AP_MAX_CONN   4

// How long to wait for a stored-credentials STA connection to succeed
// before giving up and falling back to the setup AP.
#define STA_CONNECT_TIMEOUT_MS   15000

// mDNS hostname. Deliberately NOT "flu-monitor" yet -- the existing ESPHome
// sidecar already advertises that name, and this project runs alongside it
// during development/testing. Rename to "flu-monitor" only at the final
// cutover (see the project plan's "Recommended structure" section).
#define MDNS_HOSTNAME       "flu-monitor-idf"
