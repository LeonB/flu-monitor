# flu-monitor

Plain ESP-IDF (not ESPHome/Arduino) firmware for the wood stove flue
temperature sidecar, on an **Adafruit ESP32 Feather V2** with an **Adafruit
MCP9601** (K-type thermocouple amp) over STEMMA QT. This folder was a
ground-up rewrite (originally `flu-monitor-idf/`) of an earlier ESPHome
version of this same sidecar; that ESPHome project has since been retired
and removed, and this folder renamed to take its place. See the repo root
`CLAUDE.md` for the bigger-picture project this is one half of (alongside
`../flu-display/`), and this folder's own `CLAUDE.md` for
implementation-specific gotchas.

An onboard **Adafruit BMP581** (pressure/temperature) was originally
alongside it, but has since been physically removed — its ambient
temperature reading was almost a duplicate of the MCP9601's own cold-junction
reading, so it wasn't earning its board space. No BMP581 code remains.

**Status: Milestones 1-5 and 7 done and verified on real hardware** (WiFi +
captive portal + mDNS + OTA; MCP9601 sensor; NVS-backed settings + REST API +
rate/zone regression; WebSocket broadcast, now consumed by `../flu-display/`
instead of it polling REST; Google Sheets logging, verified against the real
production webhook; woodstove event logging via `POST /api/event`, verified
against the real production webhook too). Milestone 6 (web UI) not yet
built -- the event buttons currently need a raw `curl`/HTTP client, no UI
to tap yet.

## Files

- `main/main.c` — boot sequence: NVS, settings, sensors, WiFi (stored creds or
  captive portal fallback), then either the REST API + OTA (once connected)
  or the captive portal (if not)
- `main/config.h` — compile-time tunables and first-boot setting defaults
- `main/settings.c/.h` — NVS-backed runtime settings (zone thresholds,
  fast-rise rate, deadband, heartbeat, Google Sheets webhook/secret)
- `main/sensors.c/.h` — MCP9601 init/read, I2C bus recovery, and the
  rate/zone regression (ported from the ESPHome sidecar's own lambda)
- `main/rest_api.c/.h` — `GET /api/reading`, `GET`/`POST /api/settings`
- `main/ws_server.c/.h` — `/ws` broadcast endpoint (reading + settings-changed
  events), registered onto the same shared HTTP server
- `main/sheets_logger.c/.h` — periodic (deadband/heartbeat-gated) + event-
  triggered Google Sheets logging, on its own FreeRTOS task so a slow Apps
  Script response never blocks sensor sampling or the REST/WS servers
- `main/ota_server.c/.h` — authenticated `POST /ota` endpoint, registered onto
  `rest_api`'s shared HTTP server (only one server can bind port 80)
- `components/wifi_setup/`, `components/captive_portal/`, `components/dns_server/`
  — WiFi credential storage (NVS) and the fallback setup access point, with a
  live test-connect-before-save flow (see `CLAUDE.md`)
- `activate-idf.sh` — sources the cached ESP-IDF toolchain (same one
  `flu-display` uses; see `../flu-display/CLAUDE.md` for the one-time setup)
- `ota_flash.sh` — pushes a build to a running device over WiFi
- `partitions_ota.csv` — two-OTA-slot partition table
- `google-sheets-logger/` — the Google Apps Script webhook `sheets_logger.c`
  posts to, plus its own setup `README.md`

## Setup

```sh
. ./activate-idf.sh && idf.py build                             # build
. ./activate-idf.sh && idf.py -p /dev/cu.usbserial-XXXX flash   # first flash (or after a fresh USB plug-in)
. ./activate-idf.sh && idf.py -p /dev/cu.usbserial-XXXX monitor # serial log (exit with Ctrl+], not Ctrl+C)
```

If the device has no stored WiFi credentials (or they fail to connect), it
starts a setup access point called **"Flu Monitor Setup"** (password
`flu-monitor-setup`) with a captive portal: pick your network from the
scanned list, enter its password, and the device tests the connection live
(while the setup AP stays up) before ever saving anything or rebooting — a
wrong password shows an inline error on the same screen instead of a blind
reboot-and-hope.

## Updating over WiFi (OTA)

```sh
cp main/secrets.h.example main/secrets.h   # first time only -- fill in a real random string
. ./activate-idf.sh && idf.py build
./ota_flash.sh <device-ip-or-hostname>     # e.g. flu-monitor.local
```

`main/secrets.h` is git-ignored; the same secret has to be in it on every
machine that pushes updates.

## REST API (once connected to WiFi)

```sh
curl http://flu-monitor.local/api/reading
curl http://flu-monitor.local/api/settings
curl -X POST http://flu-monitor.local/api/settings \
  -H "Content-Type: application/json" \
  -d '{"log_heartbeat_min":15,"zone_cold_max_c":150,"zone_optimal_max_c":280,"fast_rise_c_per_min":20,"thermocouple_deadband_c":5,"google_sheets_webhook_url":"","google_sheets_secret":""}'
```

`POST /api/settings` replaces the whole settings object (no partial/PATCH
semantics) and validates before persisting — an invalid payload (e.g.
`zone_cold_max_c >= zone_optimal_max_c`) gets a `400` and leaves the stored
settings untouched. On success, it also broadcasts `{"type":"settings_changed"}`
over `/ws` (see below).

## Woodstove event logging (once connected to WiFi)

```sh
curl http://flu-monitor.local/api/events   # the fixed taxonomy: [{"slug":"cold_start","label":"Cold Start"}, ...]
curl -X POST http://flu-monitor.local/api/event \
  -H "Content-Type: application/json" \
  -d '{"event":"cold_start"}'
```

`POST /api/event` logs an immediate, un-gated Sheets row (see "Google Sheets
logging" below) for one of the fixed event slugs `GET /api/events` lists --
an unknown slug gets a `400`. No web UI exists yet to tap these from a
phone at the stove (Milestone 6); for now this needs a raw HTTP client.

## WebSocket broadcast (once connected to WiFi)

```sh
# any WS client, e.g. `websocat ws://flu-monitor.local/ws`
```

Broadcasts `{"type":"reading","reading":<same shape as GET /api/reading>}`
every ~30s (matching the sensor's own update cadence), and
`{"type":"settings_changed"}` immediately after a successful
`POST /api/settings` — `flu-display` uses this instead of polling REST, and
re-fetches `/api/settings` on the latter event.

## Google Sheets logging

Configure `google_sheets_webhook_url`/`google_sheets_secret` via
`POST /api/settings` (see above) to enable. Once set, `sheets_logger.c` logs
a row on its own ~30s-tick task whenever either is true:

- **`temp_change`**: the thermocouple reading has moved by at least
  `thermocouple_deadband_c` since the last logged row.
  - **`heartbeat`**: at least `log_heartbeat_min` minutes have passed since
    the last logged row, regardless of delta.

The dedup baseline (`s_last_logged_thermocouple_c`) is in-RAM only, not
NVS-persisted, so a reboot always logs one row immediately on the first tick
— matching the ESPHome sidecar's own `restore_value: false` behavior.
`POST /api/event` (see "Woodstove event logging" above) logs a fourth kind
of row, tagged with one of the fixed event slugs instead of `temp_change`/
`heartbeat` -- immediate and un-gated, bypassing the deadband/heartbeat
check entirely and not resetting either's clock.

## Onboard NeoPixel: WiFi status at a glance

`status_led.c` drives the board's onboard NeoPixel as a solid color
reflecting the WiFi connection lifecycle: **amber** while attempting to join
stored credentials, **green** once connected (REST/WS/OTA all up), **red**
while the setup access point / captive portal is active. Shares its
power-enable line with STEMMA QT (GPIO2, already driven high by
`sensors_init()` before this ever runs) — see `CLAUDE.md`.

## Hardware notes

| Function              | Pin/value |
|------------------------|-----------|
| STEMMA QT SDA          | GPIO22 |
| STEMMA QT SCL          | GPIO20 |
| STEMMA QT power        | GPIO2 (must be driven HIGH — see `CLAUDE.md`) |
| Onboard NeoPixel data  | GPIO0 (WiFi status color, see above) |
| MCP9601 I2C address    | 0x67 |
| I2C bus speed          | 50kHz (see `CLAUDE.md`'s MCP960x errata notes) |
