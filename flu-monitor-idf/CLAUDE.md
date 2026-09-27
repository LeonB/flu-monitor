# flu-monitor-idf (plain ESP-IDF rewrite of the ESPHome `flu-monitor` sidecar)

See the repo root `CLAUDE.md` for the bigger-picture project this is one half
of, and `README.md` (this folder) for day-to-day build/flash/OTA commands and
the REST API. This file covers implementation-specific gotchas, most of them
hard-won on real hardware.

## Why this rewrite exists

`../flu-monitor/` (ESPHome) works and stays running as the rollback path
throughout. The rewrite happened because the rate-of-change regression and
zone classification became real algorithmic C++ living inside a YAML
multiline lambda string — no separate compile step, awkward to iterate.
Plain ESP-IDF, matching `../flu-display/`'s own architecture, fixes that.
API-first, OTA, captive portal, mDNS were all part of the same ask; no MQTT
broker exists on this network, so WebSocket is the mechanism for pushing
live readings to `flu-display` instead.

## Status

**Milestones 1-4 done and verified on the real Feather board:**
- **Milestone 1**: WiFi (stored creds) + captive portal fallback (scanned
  network list, live test-connect-before-save, a dedicated failure screen) +
  mDNS + OTA.
- **Milestone 2**: BMP581 + MCP9601 both reading correctly, verified
  side-by-side against the ESPHome sidecar's own values on the same physical
  sensors.
- **Milestone 3**: NVS-backed settings (replacing ESPHome's YAML
  `substitutions:`), the rate/zone regression ported from the ESPHome
  lambda, and a REST API (`GET /api/reading`, `GET`/`POST /api/settings`).
- **Milestone 4**: a `/ws` broadcast endpoint (reading every ~30s,
  `settings_changed` on a successful settings POST), consumed by
  `../flu-display/`'s own Milestone 4 (WS subscription replacing its old 3s
  REST poll) -- verified end to end on both real devices.

Milestones 4-7 (WebSocket broadcast + `flu-display` update, Google Sheets
logging, web UI, event buttons) not yet built.

## STEMMA QT power (GPIO2) — the single biggest time sink so far

**GPIO2 gates power to the entire STEMMA QT connector (both sensors) and
must be driven HIGH before touching the I2C bus at all.** Without it, the
bus reads stuck low on every single line/every bias condition (floating,
internal pull-down, *and* internal pull-up all read 0) — indistinguishable
from a genuinely wedged/shorted bus by any software-level test. `sensors.c`'s
`sensors_init()` does this first, before anything else. The ESPHome sidecar
does the same thing via a `switch: platform: gpio` with `restore_mode:
ALWAYS_ON` and `setup_priority: 1200` (higher than i2c's own 1000) — see
`../flu-monitor/CLAUDE.md`'s own note on this exact pin.

## I2C bus recovery is needed on every boot, not just after a crash

`sensors.c`'s `i2c_bus_recover()` is a faithful port of ESPHome's own
`i2c_bus_esp_idf.cpp`'s `recover_()` (same NXP/Analog Devices bus-recovery
procedure: bit-bang 9 clock pulses, each waiting out clock-stretching, then a
START immediately followed by a STOP) — ESPHome's own boot log shows
"Performing bus recovery" on every single boot of this same hardware too,
not just after a bad state. An earlier, simpler version of this function
that didn't wait for clock-stretch release on each pulse (just toggled SCL
on a fixed delay) failed to unstick a genuinely stuck bus; matching the
proven ESPHome implementation exactly, rather than reinventing it, is what
actually worked.

## MCP960x errata: 85kHz nominal wasn't safe in practice

The MCP960x family has a documented clock-stretch errata (see
`../flu-monitor/CLAUDE.md`) that manifests as **stale/frozen register
reads**, not a bus error — a read looks completely valid, just wrong (or
just never updating). Two things learned the hard way, beyond what the
ESPHome sidecar's own writeup already covers:

- **Requesting exactly 85000 Hz still hit the errata.** The ESP32's clock
  divider rounds the *requested* rate to the nearest achievable one, which
  can land slightly above it. `config.h`'s `I2C_FREQ_HZ` is `50000`, leaving
  real margin instead of sitting right on the threshold.
- **ESPHome's own `i2c:` component defaults `sda_pullup_enabled`/
  `scl_pullup_enabled` to `true` on ESP32** (`cv.SplitDefault(...,
  esp32=True)` in its own schema), and the sidecar's config never overrides
  that — so its bus has the ESP32's internal weak pull-up engaged *in
  parallel with* the breakout boards' own pull-ups. This rewrite wasn't
  doing that at first. The extra pull-up speeds up the bus's rise time,
  which matters here: slower edges distort SCL's effective duty cycle
  enough to trip the errata even at a "safe" nominal frequency. Both
  `init_mcp9601()` and `init_bmp581()` now explicitly set
  `sda_pullup_en`/`scl_pullup_en = true` on their `i2c_dev_t` before the
  first real transaction, matching ESPHome's default rather than relying on
  the driver's own default of `false`.
- **The MCP9601's device-config register (mode/ADC-resolution/burst count)
  is never touched by esp-idf-lib's `mcp960x` driver** — it's left at
  whatever the power-on-reset default is. ESPHome's own `mcp9600.cpp`
  explicitly writes this register to `0x00` (Normal mode, 18-bit ADC, 1
  burst sample) rather than trusting the default; `sensors.c` now does the
  same via `mcp960x_set_device_config()`, since earlier debugging on this
  exact chip (while the bus was still stuck low, before the GPIO2 fix)
  involved I2C writes that could have left this register in an unknown
  state.
- **The 18-bit ADC's first conversion takes ~320ms.** Without an explicit
  wait, the very first `sensors_read()` call (which can happen only tens of
  ms after `sensors_init()` returns) reads back the register's
  pre-conversion `0x0000` default — which passes the plausibility clamp
  fine (`0.00°C` is well inside `-40..600`) and was getting admitted into
  the rate-regression window as a real sample, producing a wildly wrong
  initial rate once a genuine second sample arrived 30s later.
  `init_mcp9601()` now waits 400ms after writing the device-config register,
  before returning, specifically to avoid this.

## The "GPIO is not usable, maybe conflict with others" warning is benign

Every boot logs `W (xxx) i2c.common: GPIO 22/20 is not usable, maybe
conflict with others` right after the I2C bus/device come up. Traced this
down to `esp_gpio_reserve()`'s bookkeeping: `esp-idf-lib/i2cdev`'s new
i2c_master-driver backend re-asserts its own bus's SDA/SCL pin reservation
every time a *new device* is added to an already-installed bus (not just
once at bus creation) — so the second sensor's device-add trips the "already
reserved" warning against the *bus's own* prior reservation of the same
pins, not a real external conflict. Confirmed empirically: dumping the
reservation mask (`esp_gpio_reserve(0)`) immediately before bus setup showed
bits 20/22 genuinely unset. Don't chase this warning as a real bug.

## Live WiFi test-connect-before-save (captive portal)

`wifi_setup.c`'s `wifi_sta_test_connect()` attempts the real STA connection
*while the setup AP stays up* (both possible simultaneously in
`WIFI_MODE_APSTA`, already needed for the network-scan feature), and
`captive_portal.c`'s `/save` handler only persists credentials + reboots on
a *successful* test. A wrong password now shows an inline error on the same
screen instead of triggering a blind reboot that may or may not come back —
this was a deliberate design change mid-project (the original Milestone 1
design saved-then-rebooted-then-recorded-failure-for-next-boot, working but
worse UX for the common case of a typo).

## Reflashing the ESPHome sidecar onto the same board changes the NVS layout

Useful for a side-by-side sensor-reading comparison (done for Milestone 2),
but be aware: ESPHome's own partition table places `nvs` differently than
this project's `partitions_ota.csv`, and even where they'd coincidentally
overlap, ESPHome's own WiFi-credential storage format/keys differ from this
project's `wifi_cfg` namespace. Flashing ESPHome and then flashing back
means this project's stored WiFi credentials are gone — expect to redo the
captive portal setup flow afterward. (Contrast with `flu-display`'s own
partition-table transition, where NVS happened to survive — see
`../flu-display/CLAUDE.md`. Not something to rely on in general.)

## Only one HTTP server can bind port 80

`rest_api.c`'s `rest_api_start()` owns the single shared `httpd_handle_t` for
normal running state (once connected to WiFi); `ota_server_register()` and
`ws_server_register()` just add their own URI handlers onto that same server
instead of each starting their own. The captive portal's own server
(setup-time only, never running at the same time as the connected-state
server) is separate and unaffected.

## A WebSocket URI handler is never called for its own handshake

`ws_server.c`'s first version tracked new clients with `if (req->method ==
HTTP_GET) { ...; return ESP_OK; }` inside the registered handler, reasoning
(wrongly) that the handshake request itself would arrive as a `GET` and could
be detected that way. It compiled fine and looked reasonable, but silently
never worked: a client could connect and the server would broadcast forever
without ever reaching it. Confirmed straight from `esp_http_server`'s own
source (`httpd_uri.c`): once `is_websocket` is set, the framework completes
the entire WS handshake internally and explicitly does **not** call the
registered handler for it (its own comment: *"If the request is websocket
handshake, then do not call the uri->handler"*) — the handler is only ever
invoked later, for an actual client→server data frame. `req->method` on such
a later invocation is just the URI's own statically-registered method
(`HTTP_GET`), not a live signal distinguishing "this is the handshake" —
there's no invocation where that branch's premise is even true. Since a
broadcast-only client like `flu-display` never sends anything, the handler
was never entered at all, and the client list stayed empty forever.

The actual hook that fires once a connection is open is
`ws_post_handshake_cb` (a separate callback field on `httpd_uri_t`, gated by
`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT` — off by default, on in this
project's `sdkconfig.defaults`). Client tracking lives there now;
`ws_handler()` itself only ever runs for a real subsequent frame (a client
CLOSE, or draining an unexpected data frame).

## The regression window assumes a fixed, sensor-matching read cadence

`sensors_read()` admits the thermocouple reading into the rate-regression
window on *every call* — it must only be called on the sensor's own true
update cadence (currently every 30s, from `main.c`'s `sensor_log_task`), not
from an arbitrary poller. This is exactly the rate-inflation bug
`flu-display/CLAUDE.md` documents for its own polling-based rate calc:
timestamping a value that only actually changes once per 30s as if every
poll were a fresh sample makes any real delta look many times faster than
it is. `GET /api/reading` (and any future WebSocket broadcast) must call
`sensors_get_last_reading()` instead — a thread-safe copy of whatever the
periodic task last computed, no I2C traffic, no window mutation.

An implausible reading (outside `-40..600°C`) leaves the rate/zone at their
last-known-good values rather than resetting them, exactly matching the
ESPHome lambda's own `return;`-on-glitch behavior — the raw
`thermocouple_c`/`cold_junction_c` fields are still reported as read either
way; only the *derived* rate/zone are protected from a single glitch
poisoning the ~3-minute window.

## Sensor drivers: vendored official/registry drivers, not copied ESPHome components

BMP581 uses Bosch's own official `BMP5_SensorAPI` (`components/bmp5/`,
vendored verbatim, BSD-3-Clause) plus a ~40-line ESP-IDF/i2cdev glue layer
(`bmp5_port.c/.h`). MCP9601 uses `esp-idf-lib/mcp960x` via the component
registry. Deliberately *not* copying ESPHome's own `bmp581_base.cpp`/
`mcp9600.cpp` directly, even though their source was extremely useful for
finding the gotchas above: ESPHome is GPL-3.0 (a real copyleft concern for
otherwise BSD-3 vendored code), and those files are written against
ESPHome's own `Component`/`i2c::I2CDevice` base classes and HAL — not
standalone, and pulling them in would drag along a meaningful slice of
ESPHome's own core, working against the entire reason this rewrite exists.
Reading ESPHome's source for the *procedural knowledge* (which register,
which pin, which order) while keeping the actual driver code as the
manufacturer's/registry's own reference implementation got the same
debugging value without either problem.

## Misc

- **`idf.py monitor` exits with Ctrl+], not Ctrl+C.** Ctrl+C is intercepted
  by the monitor for other purposes. Ctrl+T then Ctrl+H shows its full
  shortcut menu.
- **A background `idf.py monitor`/`esphome logs` session holds the serial
  port open exclusively** — a concurrent `idf.py flash`/`ota_flash.sh` from
  another shell will fail with "Resource temporarily unavailable" until it's
  stopped.
