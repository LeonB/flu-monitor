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

// --- Milestone 2: sensors (MCP9601 over I2C; BMP581 physically removed) ---

// Same bus/pins as the existing ESPHome sidecar's flu-monitor.yaml `i2c:`
// block -- keeps this rewrite's readings directly comparable to it.
#define I2C_SDA_GPIO      22
#define I2C_SCL_GPIO      20

// MCP960x errata: a clock-stretch bug corrupts data above ~85kHz, observed
// as stale/frozen register reads rather than an outright bus error (see
// rikeshkkpatel.co.uk/diy-reflow-oven/problems-with-the-mcp9600-thermocouple-amplifier).
// Requesting exactly 85000 still hit this in practice -- the ESP32's clock
// divider rounds the *requested* rate to the nearest achievable one, which
// can land slightly above it, so this leaves real margin below the errata
// threshold instead of sitting right on it.
// The MCP960x family is also never bus-scanned -- a full I2C scan locks it
// up (github.com/adafruit/Adafruit_Wippersnapper_Arduino/issues/299).
#define I2C_FREQ_HZ       50000

#define MCP9601_I2C_ADDR  0x67  // Adafruit breakout default (ADDR floating/high)

// The Feather V2's STEMMA QT connector's power is gated by a FET switch on
// this pin -- both sensors are wired through it, off by default. The
// ESPHome sidecar drives this permanently high via a `switch: platform:
// gpio` with `restore_mode: ALWAYS_ON`, set up before its i2c bus; without
// it the bus reads stuck low always, regardless of internal pull-ups
// (looks exactly like a wedged/shorted bus, but is really just "unpowered").
#define STEMMA_QT_POWER_GPIO  2

// --- Milestone 3: settings (NVS-backed; these are first-boot defaults only,
// before NVS has anything saved -- see settings.h) ---

// Same placeholder values as the ESPHome sidecar's own `substitutions:`
// block (flu-monitor.yaml) -- pending the real multi-week data-gathering
// run these thresholds exist to inform (see the repo root CLAUDE.md's
// "Project goal").
#define DEFAULT_ZONE_COLD_MAX_C          150.0f
#define DEFAULT_ZONE_OPTIMAL_MAX_C       280.0f
#define DEFAULT_FAST_RISE_C_PER_MIN      20.0f
#define DEFAULT_THERMOCOUPLE_DEADBAND_C  5.0f
#define DEFAULT_LOG_HEARTBEAT_MIN        15
