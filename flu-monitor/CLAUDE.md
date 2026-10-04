# flu-monitor (plain ESP-IDF, not ESPHome/Arduino)

See the repo root `CLAUDE.md` for the bigger-picture project this is one half
of, and `README.md` (this folder) for day-to-day build/flash/OTA commands and
the REST API. This file covers implementation-specific gotchas, most of them
hard-won on real hardware.

## Why this rewrite exists

This folder was originally `flu-monitor-idf/`, a ground-up rewrite of an
earlier ESPHome version of this same sidecar (then itself named
`flu-monitor/`) that ran alongside it as the rollback path throughout the
rewrite. The rewrite happened because the rate-of-change regression and zone
classification became real algorithmic C++ living inside a YAML multiline
lambda string — no separate compile step, awkward to iterate. Plain ESP-IDF,
matching `../flu-display/`'s own architecture, fixes that. API-first, OTA,
captive portal, mDNS were all part of the same ask; no MQTT broker exists on
this network, so WebSocket is the mechanism for pushing live readings to
`flu-display` instead. Once the rewrite caught up (see Status below) and a
BMP581 sensor originally alongside the MCP9601 was physically removed (see
"BMP581 removed" below), the ESPHome version was retired and removed
entirely, and this folder renamed from `flu-monitor-idf/` to take its place.

## Status

**Adopted as the production sidecar** (this folder's rename from
`flu-monitor-idf/` to `flu-monitor/`, and the ESPHome version's removal, was
that cutover). **Milestones 1-5 done and verified on the real Feather
board:**
- **Milestone 1**: WiFi (stored creds) + captive portal fallback (scanned
  network list, live test-connect-before-save, a dedicated failure screen) +
  mDNS + OTA.
- **Milestone 2**: BMP581 + MCP9601 both reading correctly, verified
  side-by-side against the ESPHome sidecar's own values on the same physical
  sensors. (The BMP581 was later physically removed from the board -- its
  ambient temperature reading was almost a duplicate of the MCP9601's own
  cold-junction reading -- and all BMP581 code has since been removed; see
  "BMP581 removed" below.)
- **Milestone 3**: NVS-backed settings (replacing ESPHome's YAML
  `substitutions:`), the rate/zone regression ported from the ESPHome
  lambda, and a REST API (`GET /api/reading`, `GET`/`POST /api/settings`).
- **Milestone 4**: a `/ws` broadcast endpoint (reading every ~30s,
  `settings_changed` on a successful settings POST), consumed by
  `../flu-display/`'s own Milestone 4 (WS subscription replacing its old 3s
  REST poll) -- verified end to end on both real devices.
- **Milestone 5**: periodic (deadband/heartbeat-gated) + event-triggered
  Google Sheets logging (`sheets_logger.c/.h`), ported from the ESPHome
  sidecar's own logic onto a dedicated FreeRTOS task so a slow Apps Script
  response never blocks sensor sampling or the REST/WS servers -- verified
  against the real production webhook (a real row logged, status 200) after
  fixing a missing TLS cert bundle attachment (see below).
- **Milestone 7**: `POST /api/event` + `GET /api/events` (`rest_api.c`'s
  `EVENTS[]`), the fixed woodstove-event taxonomy ported verbatim from the
  ESPHome sidecar's own template buttons (same slugs, e.g. `cold_start`,
  `added_wood` -- see "The woodstove-event taxonomy is a closed set" below)
  -- verified against the real production webhook (a real row logged,
  status 200, event=`added_wood`). No physical buttons or web UI to trigger
  these yet -- just the REST endpoint, per the plan's "at minimum" scope.
- **Milestone 6**: the embedded web UI (`main/web_ui/`) -- dashboard (glow
  circle, pulse math ported from `flu-display/main/led_display.c`), 24h
  graph (`GET /api/history`, a new downsampled ring buffer in `sensors.c`),
  settings, and the event-log bottom sheet, all one Alpine.js page with no
  build step, served from `/`. Visually verified live in a real browser
  (Chrome, via the Claude Code Chrome extension) -- dashboard glow,
  settings steppers/save-bar/undo, the 24h graph, and the event sheet
  (a real row logged: `added_wood`) all confirmed working end to end. Three
  real bugs only showed up once actually rendered (static checks alone
  missed all of them) -- see "Three real bugs only a real browser caught"
  below.

## STEMMA QT power (GPIO2) — the single biggest time sink so far

**GPIO2 gates power to the entire STEMMA QT connector (both sensors) and
must be driven HIGH before touching the I2C bus at all.** Without it, the
bus reads stuck low on every single line/every bias condition (floating,
internal pull-down, *and* internal pull-up all read 0) — indistinguishable
from a genuinely wedged/shorted bus by any software-level test. `sensors.c`'s
`sensors_init()` does this first, before anything else. The original
ESPHome sidecar did the same thing via a `switch: platform: gpio` with
`restore_mode: ALWAYS_ON` and `setup_priority: 1200` (higher than i2c's own
1000) -- without that ordering, the sensors were unpowered when the i2c bus
initialized, showing up as `SCL is held LOW on the bus` bus-recovery
failures at boot, the same failure mode this port's own explicit
before-anything-else ordering avoids.

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

The MCP960x family has a real, documented Microchip silicon errata
("Intermittent I2C Read Command Clock Stretching Failure") that above
~85kHz can make a read silently return stale duplicate register data --
it manifests as **stale/frozen register reads**, not a bus error, so a read
looks completely valid, just wrong (or just never updating). Background:
rikeshkkpatel.co.uk/diy-reflow-oven/problems-with-the-mcp9600-thermocouple-amplifier.
Two more things learned the hard way, on top of that:

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
  enough to trip the errata even at a "safe" nominal frequency.
  `init_mcp9601()` explicitly sets `sda_pullup_en`/`scl_pullup_en = true` on
  its `i2c_dev_t` before the first real transaction, matching ESPHome's
  default rather than relying on the driver's own default of `false` (the
  now-removed BMP581 init did the same, for the same reason, while it was
  still on the board).
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

`wifi_setup.c`/`captive_portal.c`/`dns_server.c` now live in `../components/`,
shared with `../flu-display/` (see the repo root `CLAUDE.md`'s "Shared
components" section) -- this section's description of the test-connect
design is unchanged, just no longer specific to this folder alone.

`wifi_setup.c`'s `wifi_sta_test_connect()` attempts the real STA connection
*while the setup AP stays up* (both possible simultaneously in
`WIFI_MODE_APSTA`, already needed for the network-scan feature), and
`captive_portal.c`'s `/save` handler only persists credentials + reboots on
a *successful* test. A wrong password now shows an inline error on the same
screen instead of triggering a blind reboot that may or may not come back —
this was a deliberate design change mid-project (the original Milestone 1
design saved-then-rebooted-then-recorded-failure-for-next-boot, working but
worse UX for the common case of a typo).

## (Historical) Reflashing the old ESPHome sidecar changed the NVS layout

No longer a live concern -- the ESPHome sidecar has been retired and its
firmware/project files removed entirely, so there's nothing left to
reflash back and forth against. Kept for the underlying NVS-namespace fact,
in case it's ever relevant again: ESPHome's own partition table placed
`nvs` differently than this project's `partitions_ota.csv`, and even where
they'd coincidentally overlap, ESPHome's own WiFi-credential storage
format/keys differed from this project's own `wifi_cfg` namespace --
flashing ESPHome and then flashing back meant this project's stored WiFi
credentials were gone, requiring the captive portal setup flow to be redone.

## Only one HTTP server can bind port 80

`rest_api.c`'s `rest_api_start()` owns the single shared `httpd_handle_t` for
normal running state (once connected to WiFi); `ota_server_register()` and
`ws_server_register()` just add their own URI handlers onto that same server
instead of each starting their own. The captive portal's own server
(setup-time only, never running at the same time as the connected-state
server) is separate and unaffected.

## The REST API server is single-threaded, with an unresolved intermittent full hang

`rest_api_start()` explicitly sets `max_open_sockets = REST_API_MAX_OPEN_SOCKETS`
(7, matching `HTTPD_DEFAULT_CONFIG()`'s own default, but named so
`rest_api_log_socket_usage()` below can size its buffer correctly rather
than guessing), so up to 7 clients can have a socket open at once (plus
`ws_server.c`'s own separate `WS_MAX_CLIENTS = 8` cap on broadcast
fan-out). But `esp_http_server` here runs as a single FreeRTOS task --
`HTTPD_DEFAULT_CONFIG()` only has one `task_priority`/`stack_size`/
`core_id`, no thread pool -- so it services one HTTP request at a time,
serially, even with several sockets open simultaneously. `POST /ota`
writing firmware to flash in a loop on that same task is the one handler
long enough for this to matter in the ordinary case: during an OTA push,
the dashboard/REST API/WS are unresponsive to everyone else until it
finishes (Google Sheets logging is deliberately exempt -- `sheets_logger.c`
runs on its own task for exactly this reason).

Beyond that ordinary case, this server has hung completely at least twice,
under real usage, for reasons not yet fully root-caused:

- **First time**: `GET /api/reading` took 15-20s (past `curl`'s own
  timeout, `HTTP 000`), but a raw TCP connect succeeded and ping was
  normal. Looked exactly like a wedged httpd task. Wasn't (this time) --
  the boot log (captured by attaching serial *during* a fresh reset, not
  after) showed the device had roamed to a different AP within the same
  SSID at `rssi: -88`, a genuinely weak signal. A request that DID
  eventually respond, just after 5+ seconds, from a real client (not a
  synthetic one that gives up early) is what proved it was a slow/lossy
  link rather than a stuck task -- a truly deadlocked task would never
  respond no matter how long you wait. `GET /api/wifi` (queries
  `esp_wifi_sta_get_ap_info()` fresh on every call, not cached, unlike
  RSSI, which the WiFi driver only ever logs once at association) exists
  specifically so this can be checked on demand, without forcing a reboot
  to catch that one-shot boot-time log line.
- **Second time, shortly after (same boot)**: every request hung
  completely -- including using the bare IP (ruling out mDNS, see below)
  and a fresh `nc -zv` TCP-only probe (which eventually succeeded, after
  10+ seconds, meaning the TCP handshake itself was also slow). Crucially,
  **the device's own serial log showed nothing at all** for any of these
  attempts -- not even a warning, whereas a normal 404 (e.g. a browser's
  `/favicon.ico` request) does get logged. Multiple consecutive attempts
  over several minutes all failed identically, with the pattern getting
  *worse* over time rather than being randomly intermittent like the RSSI
  case -- consistent with some kind of accumulating resource exhaustion,
  not (just) signal quality. `POST /ota` was also affected at this point
  (`curl: (56) Recv failure: Connection reset by peer`), forcing a USB
  reflash instead to get diagnostics onto the device. Also confirmed mDNS
  was not the cause despite the symptom's obvious resemblance to this
  project's own documented mDNS flakiness (`../flu-display/CLAUDE.md`):
  `dns-sd -G v4 flu-monitor.local` resolved essentially instantly, and the
  hang persisted identically when hitting the bare IP directly (no
  hostname resolution involved at all).

`rest_api_log_socket_usage()` (called every ~30s from `main.c`'s own
`sensor_log_task`, piggybacking its cadence -- no dedicated timer) logs
the current open-socket count via `httpd_get_client_list()`, specifically
to catch this in the act next time: a count climbing toward 7 and staying
pinned there would confirm socket/resource exhaustion; a low, stable count
during a hang would point at something else (a genuinely wedged task, not
a resource leak). Deliberately synthetic load right after adding this
(sequential and concurrent `curl` bursts) did **not** reproduce either
hang, and the socket count stayed low (2-3) throughout -- whatever
triggers the second failure mode needed something closer to the original
real-world condition, not short scripted request bursts.

**Root cause found and fixed the next time it recurred.** The diagnostic
caught it directly: `Open sockets: 7/7`, pinned across multiple 30s ticks,
while `ws_server.c`'s own client count showed only 1-2 -- meaning most of
those 7 httpd sessions weren't legitimate, currently-tracked WS clients at
all. The bug: `ws_server.c`'s `clients_remove()` (called when a broadcast
send to a client fails, or a clean WS close frame arrives) only removed
the fd from *its own* bookkeeping array -- it never told httpd to actually
close the underlying session. A WS client going away uncleanly (a WiFi
drop with no close frame -- the normal case on this network, not the
exception) permanently leaked one httpd socket slot, every single time.
Fixed by also calling `httpd_sess_trigger_close(s_server, fd)` (the
documented API for closing a session from a context other than the httpd
worker task itself) in `clients_remove()`.

Verified live: before the fix, a pinned 7/7 never recovered on its own,
not even after many minutes with zero new client activity. After the fix,
a post-reboot burst of reconnect activity (`flu-display` re-establishing
its WS connection after `flu-monitor`'s own restart) still spiked the
count to 7/7 briefly, but it now **recovers** -- settling back down and
holding steady (observed at 4/7, stable for 2+ minutes) instead of staying
pinned. That settled count is still higher than the "just 1 legitimate
WS client" baseline would suggest, which points at some remaining
churn/slow-to-detect staleness (each stale fd is only pruned reactively,
on the *next* failed broadcast attempt against it specifically, up to a
30s wait) -- worth continued attention via this same logging, but the
severe failure mode (permanent, total lockup with zero recovery) is
confirmed fixed.

## `EMBED_FILES` collides on basename, not full path

`main/web_ui/`'s first attempt named its copies of the shared design-system
assets `styles.css` and `alpine.min.js` -- the same basenames
`../components/captive_portal/` already uses for its own, *different*, copies
of those files. Build failed with `ninja: error: ... multiple rules
generate styles.css.S`. ESP-IDF's `EMBED_FILES` generates an intermediate
`<basename>.S`/`.o` pair per embedded file, and that intermediate filename
is derived from the basename only, dropping the directory -- so two
different components each embedding their own `styles.css` collide in the
same build tree even though the source files live in entirely different
directories and neither component references the other's copy. Fixed by
renaming `main/web_ui/`'s copies to `dashboard.css`/`alpinejs.min.js`
(also updates the extern symbol names, e.g. `_binary_dashboard_css_start`
-- the symbol name is derived from this same intermediate basename).
Anything else embedding files across multiple components needs basenames
unique across the *whole* build, not just within its own directory.

**Confirmed by direct experiment: this collision happens even when both
components' `EMBED_FILES` point at the exact same file on disk** (tried
pointing `main/`'s own `EMBED_FILES` at `../../components/captive_portal/
styles.css` directly, to physically de-duplicate `dashboard.css` from it --
same `ninja: error: ... multiple rules generate styles.css.S`). Each
component's own `idf_component_register(EMBED_FILES ...)` call independently
generates a build rule for `<basename>.S` in the shared build tree,
regardless of whether the underlying source content is identical -- so
sharing one physical file across two components' embeds isn't possible
without a bigger restructure (e.g. one component embedding it and exporting
an accessor the other calls), not just picking the same path. `dashboard.css`
and `components/captive_portal/styles.css` are instead kept
*byte-identical in content* (the Google Fonts `@import` that used to be
dashboard.css-only was moved out into a `<link>` in `dashboard.html`'s own
`<head>`, since only the connected-mode page has real internet to load it)
but stay two separate embedded files under different names, kept in sync by
hand -- there's no way to make ESP-IDF treat them as one without an
`EMBED_FILES`-level rename/alias facility it doesn't have.

## Three real bugs only a real browser caught

`main/web_ui/` built cleanly, and every backing REST endpoint tested fine
via `curl`, but the page itself rendered completely blank when actually
opened in Chrome (visual verification was flagged as pending in an earlier
commit -- this is what turned up once it happened). None of these three
would have been caught by static checks (`node --check`, balanced-tag
counting) -- only a live browser, console errors, and screenshots surfaced
them:

- **`dashboard.js` must load (and define `app()` on `window`) *before*
  `alpinejs.min.js` runs**, not after, even though both are
  `<script defer>` and `defer` preserves *document* order. Alpine
  auto-starts synchronously at the end of its own script -- scanning the
  DOM and evaluating `x-data="app()"` immediately -- which happens before a
  *later* deferred script gets a chance to execute. With
  `<script src="/alpinejs.min.js" defer>` listed first, Alpine scanned the
  DOM and threw `ReferenceError: app is not defined` on every directive,
  leaving the whole page unrendered. Fixed by swapping the two `<script>`
  tags' order. `../components/captive_portal/root.html` never hit this because
  its own data function is inline (no `src`, no `defer`), which executes
  synchronously at its position in the parse -- before any deferred script
  runs at all.
- **A CSS `transition` on an element whose inline style is rewritten every
  animation frame fights the animation instead of smoothing it.**
  `.glow-outer`/`.glow-inner` had `transition: box-shadow .3s ease` /
  `transition: background-color .15s linear, color .15s linear` -- looked
  like reasonable general-purpose polish, copied without thinking through
  the interaction. `dashboard.js`'s `runGlowFrame()` already writes a new
  inline `background-color`/`box-shadow` value ~30 times a second via
  `requestAnimationFrame`; each write restarted a new CSS transition before
  the previous one finished, so the rendered color perpetually chased a
  moving target and never reached it -- rendered as a barely-visible,
  washed-out near-transparent blend instead of the real zone color. The
  glow circle was there, just practically invisible. Fixed by removing
  both `transition` declarations -- the JS's own envelope-curve
  interpolation already provides all the smoothness needed.
- **`:viewBox="..."` (Alpine's dynamic attribute binding) silently produces
  a lowercase `viewbox` attribute, which SVG does not recognize** (SVG
  attribute names are case-sensitive). A directive attribute like
  `:viewBox` isn't on the browser's fixed allowlist of camelCase SVG
  attributes preserved during HTML parsing -- only a bare, statically-
  written `viewBox="..."` is -- so the HTML parser lowercases the
  *directive's own attribute name* to `:viewbox` before Alpine ever reads
  it, and Alpine's `setAttribute` call inherits that already-lowercased
  name. Confirmed via `element.getAttribute('viewbox')` (lowercase)
  returning the value while `getAttribute('viewBox')` returned `null`. The
  graph's `<svg>` rendered at a tiny fixed intrinsic size instead of
  stretching to its container, with the drawn content confined to a small
  corner. Fixed by hardcoding `viewBox="0 0 342 230"` as a static literal
  instead of binding it (`graphHeight` was always a fixed 230 anyway, so
  there was nothing that actually needed to be dynamic). General lesson: 
  never bind SVG's camelCase presentation attributes (`viewBox`,
  `preserveAspectRatio`, etc.) dynamically via `:attr` in Alpine (or likely
  any framework using plain `setAttribute` off an HTML-parsed directive
  name) -- keep them static, or set them via the SVG DOM's own typed
  properties (e.g. `svgEl.viewBox.baseVal`) instead of `setAttribute`.

## The woodstove-event taxonomy is a closed set, not free text

`rest_api.c`'s `POST /api/event` validates the request's `event` field
against a fixed `EVENTS[]` list (`cold_start`, `opened_stove`, `added_wood`,
`damper_up`, `damper_down`, `burning_optimally`, `roaring`, `dying_down`,
`fire_out`, `stove_off`) and rejects anything else with a `400`, rather than
logging whatever string a caller sends. Deliberate: per the repo root
`CLAUDE.md`'s "Current phase is data-gathering" section, the whole point of
these annotations is correlating burns against each other later, which
needs a consistent, known vocabulary -- free-text labels from a rushed tap
at the stove wouldn't reliably give that. The slugs are ported verbatim
from the original ESPHome sidecar's own template buttons (`GET
/api/events` exposes both the slug and a human-readable label, e.g.
`{"slug":"roaring","label":"Stove Roaring"}` -- note the slug/label mismatch
on that one specifically, carried over as-is from the ESPHome config rather
than "fixed" to match, since any existing Sheets data already uses `roaring`).

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

## The onboard NeoPixel's power line is shared with STEMMA QT

`status_led.c` drives the onboard NeoPixel (GPIO0) as a solid WiFi-status
color, but doesn't do any power-enable sequencing of its own -- Adafruit's
own pinout docs confirm the NeoPixel and STEMMA QT connector share the same
power-enable pin (`STEMMA_QT_POWER_GPIO`/GPIO2), and `sensors_init()`
already drives it high, unconditionally, before `status_led_init()` is ever
called in `main.c`. If `status_led_init()` is ever reordered to run before
`sensors_init()`, the NeoPixel would stay dark regardless of what color it's
told to show.

## `esp_http_client` needs its TLS cert bundle attached explicitly

`sheets_logger.c`'s first version against the real Google Sheets webhook
failed every single attempt with `ESP_ERR_HTTP_CONNECT`, even though
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` (and `..._DEFAULT_FULL=y`) were already
on in `sdkconfig` -- that config only makes the bundle *available*, esp-tls
still has no way to verify `script.google.com`'s certificate unless
`esp_http_client_config_t.crt_bundle_attach` is explicitly set to
`esp_crt_bundle_attach` (from `esp_crt_bundle.h`, `mbedtls` component).
ESPHome's own `http_request` component does this wiring automatically, which
is why the equivalent ESPHome config never needed to think about it. Fixed
by setting `.crt_bundle_attach = esp_crt_bundle_attach` on the config and
adding `mbedtls` to `main/CMakeLists.txt`'s `PRIV_REQUIRES`.

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

MCP9601 uses `esp-idf-lib/mcp960x` via the component registry. Deliberately
*not* copying ESPHome's own `mcp9600.cpp` directly, even though its source
was extremely useful for finding the gotchas above: ESPHome is GPL-3.0 (a
real copyleft concern for otherwise permissively-licensed vendored code),
and that file is written against ESPHome's own `Component`/`i2c::I2CDevice`
base classes and HAL — not standalone, and pulling it in would drag along a
meaningful slice of ESPHome's own core, working against the entire reason
this rewrite exists. Reading ESPHome's source for the *procedural knowledge*
(which register, which pin, which order) while keeping the actual driver
code as the manufacturer's/registry's own reference implementation got the
same debugging value without either problem. (The BMP581 previously used the
same approach -- Bosch's own official `BMP5_SensorAPI`, vendored verbatim --
before the sensor and all its code were removed; see "BMP581 removed"
below.)

## BMP581 removed

The BMP581 (pressure/ambient-temperature sensor) was physically removed from
the board -- its ambient temperature reading was almost a duplicate of the
MCP9601's own cold-junction reading, so it wasn't earning its board space.
All BMP581 code was removed to match: `sensors.c/.h`'s `bmp581_*` fields and
`init_bmp581()`/`read_bmp581()`, `rest_api.c`'s `bmp581_*` JSON fields, the
vendored `components/bmp5/` directory, and its `bmp5`/`BMP581_I2C_ADDR`
references in `CMakeLists.txt`/`config.h`. `sheets_logger.c`'s webhook URL
also dropped its `temperature`/`pressure` query params, which were sourced
from the BMP581 -- **the Google Apps Script webhook itself (external to this
repo) may still expect those params**; not updated here since its source
isn't tracked in this repo.

## `ota_server.c` moved to `../components/`

Was a near-duplicate of `../flu-display/main/ota_server.c` (113 of ~120
lines byte-identical -- the auth check and the entire streaming-OTA-write
handler). Now the shared `components/ota_server/`. The one real difference
between the two projects' old copies -- flu-monitor already has a shared
httpd server to register onto (`rest_api_start()`'s handle), flu-display has
none of its own -- became two entry points on the same component:
`ota_server_register(server, secret)` for a caller with a server already
running, `ota_server_start(secret)` (starts its own dedicated 8192-stack
httpd first) for a caller with none. Each project still keeps its own
`secrets.h`/`OTA_SECRET` (the two devices' secrets are intentionally
different values) -- the shared component takes `secret` as a parameter
rather than including a project-private header itself, so it carries no
secret of its own and doesn't need visibility into either project's
`main/secrets.h`.

## LED/glow pulse-tuning constants now live in settings

`settings_t` (see `settings.h`) gained four fields flu-monitor itself never
reads -- `idle_pulse_period_ms`, `fast_pulse_period_ms`,
`rate_deadband_c_per_min`, `color_transition_exponent` -- purely to be the
one live source both `../flu-display/main/led_display.c`'s physical LED
ring and this device's own `main/web_ui/dashboard.js` glow now fetch via
`GET /api/settings`, the same mechanism `zone_cold_max_c`/
`zone_optimal_max_c`/`fast_rise_c_per_min` already used. Before this, all
four were hardcoded independently in `flu-display/main/config.h` *and*
`dashboard.js` -- two copies with no mechanism keeping them in sync.

That duplication had already drifted: `flu-display/CLAUDE.md`'s own tuning
history describes `COLOR_TRANSITION_EXPONENT` being live-tuned on real
hardware up through **11** ("a longer amber hold, quicker ramp"), but both
hardcoded copies had `3.0` -- the original, untuned guess. **This refactor
deliberately did NOT change the value** -- `DEFAULT_COLOR_TRANSITION_EXPONENT`
in `config.h` is `3.0f`, a behavior-preserving default matching what was
already running, not a silent retune. Whether `3.0` or `11` (or something
re-tuned fresh) is actually correct on the real hardware is still an open
question, now answerable by changing exactly one NVS-backed value instead
of two hardcoded ones.

## `max_uri_handlers` must cover every endpoint on this server

Adding the 4 embedded-font routes (see the web UI section above) pushed
this server's total registered handler count from 13 to 17 -- `rest_api.c`
registers 15 of its own (10 REST endpoints + 5 static assets), plus
`ota_server_register()` (1) and `ws_server_register()` (1), all onto the
same shared `httpd_handle_t`. `rest_api_start()`'s `config.max_uri_handlers`
was already explicitly bumped from `HTTPD_DEFAULT_CONFIG()`'s default of 8
to 16 once before, for exactly this reason (see the comment history) --
but 16 wasn't enough headroom, and 17 > 16 silently wasn't caught by the
build. **The actual failure mode on real hardware: an immediate,
permanent boot crash-loop.** `httpd_register_uri_handler()` fails with
`ESP_ERR_HTTPD_HANDLERS_FULL` once the ceiling is hit; every registration
call is wrapped in `ESP_ERROR_CHECK`, so the *last* one registered --
`ws_server_register()`, called after `rest_api_start()` and
`ota_server_register()` in `main.c` -- is the one that aborts. The device
reboots roughly every 3 seconds forever, re-hitting the identical assert
each time, `esp_ota_mark_app_valid_cancel_rollback()` never being reached
(it's later in `main.c`, after this line) meaning the bootloader's OTA
rollback safety net would eventually have kicked in on its own too, given
enough failed-boot cycles -- caught first via a serial boot log instead of
waiting for that. `POST /ota` is also a registered handler on this exact
same server, so **OTA cannot recover from this failure mode** -- the crash
happens within ~10-20ms of the server starting, before there's any
practical window to push a whole new firmware image through; a USB
reflash (`idf.py -p <port> flash`) was required. Fixed by raising
`max_uri_handlers` to 24, with real headroom above the current count
rather than the exact number, specifically so the next endpoint added
doesn't silently repeat this. **Any future change adding a new
`httpd_register_uri_handler()` call anywhere on this shared server should
double check the total against this ceiling.**

Also discovered while chasing this, but unrelated to `max_uri_handlers`
itself -- see the next section.

## A failed WiFi connect attempt left stale WiFi/netif state for the captive portal fallback

Found on the very first boot after the `max_uri_handlers` fix above's OTA
push: weak WiFi (rssi -77 on this network, see "The REST API server is
single-threaded..." above for prior weak-signal history) caused the
*initial* STA connect attempt to genuinely fail, and `main.c` fell back to
`captive_portal_start()` per its designed failure path -- which then
itself crashed, on `wifi_init_softap()`'s call to
`esp_netif_create_default_wifi_sta()` (`assert failed: ... config or
if_key is NULL or duplicate key`). Root cause: `wifi_setup.c`'s
`wifi_sta_try_connect()` (which just failed) already called
`esp_netif_create_default_wifi_sta()` earlier in the very same boot --
`wifi_init_softap()` unconditionally calls it again, unaware a STA netif
already exists from the failed connect attempt, and the duplicate
registration aborts.

**Fixed**: `wifi_sta_try_connect()`'s own doc comment already said it
"leaves WiFi running in STA mode either way (caller decides what to do
next on failure)" -- the missing half was that no caller ever actually did
anything with that. Added `wifi_sta_teardown()` (`components/wifi_setup/`):
`esp_wifi_stop()` + `esp_wifi_deinit()` + `esp_netif_destroy_default_wifi()`
on the STA netif captured from `wifi_sta_try_connect()`'s own return value
(previously discarded) -- the documented, symmetric counterpart to
`esp_netif_create_default_wifi_sta()`, not a guess at undocumented
behavior. Both `main.c`s now call it in the `have_creds && !connected`
branch, right where `wifi_mark_attempt_failed()` already was, before
falling through to `captive_portal_start()`.

**Verified on real hardware, not just by reasoning about the fix**: forced
a deterministic connect failure (`STA_CONNECT_TIMEOUT_MS` temporarily set
to `1` for one test build/flash, reverted immediately after) and confirmed
via serial log a clean teardown (`wifi:stop sw txq`, `wifi:Deinit lldesc
rx mblock:10`, no assertion) followed by `captive_portal: SoftAP started`
-- no crash. Separately, `flu-display` then hit this exact failure path
*for real* (genuine rssi -89 on a later boot, not the artificial test) and
recovered the same clean way, entering its own setup AP instead of
crashing -- a second, independent, non-artificial confirmation.

## A stale, gitignored `sdkconfig` turned a new Kconfig default into a permanent boot crash-loop

`main.c`'s `configure_power_management()` (added by the "Enable automatic
light sleep on both devices" commit) originally wrapped `esp_pm_configure()`
in `ESP_ERROR_CHECK` -- fine as long as that call could never fail, which
turned out not to be true. That same commit added `CONFIG_PM_ENABLE=y` to
`sdkconfig.defaults`, but the actual `sdkconfig` (gitignored, locally
generated, last regenerated before that commit existed) still had
`# CONFIG_PM_ENABLE is not set` baked in from an earlier build --
`sdkconfig.defaults` only seeds keys *missing* from `sdkconfig`, it never
overrides a key that's already present, so the stale value won silently.
With `CONFIG_PM_ENABLE` actually off, `esp_pm_configure()` returned
`ESP_ERR_NOT_SUPPORTED` (confirmed directly against the installed ESP-IDF
5.5.5 source -- `esp_pm/pm_impl.c`'s own `#ifndef CONFIG_PM_ENABLE`
early-return) on every single boot, right after WiFi connected --
`ESP_ERROR_CHECK` turned that into a hard `abort()`, rebooting forever
before the REST/WS servers ever registered. The symptom that actually
surfaced from this was confusing by itself: `GET /api/reading`/`/ws`
intermittently showed `thermocouple_ok:false` with every field zeroed and
`thermocouple_zone:"unknown"`, which looked like a sensor/I2C problem (the
zone classifier can only read "unknown" if no read has ever succeeded since
the last reset) but was actually just whichever request happened to land
during the reboot window.

Fixed two ways: deleting the stale `sdkconfig` and letting `idf.py build`
regenerate it fresh from `sdkconfig.defaults` resolved this specific
occurrence (verified via a clean serial boot log and 5+ minutes of
uninterrupted sensor reads afterward), and `configure_power_management()`
itself no longer uses `ESP_ERROR_CHECK` -- it logs a warning and continues
without automatic light sleep on any future `esp_pm_configure()` failure,
since that's a far better fallback than bricking the device over a
recoverable power-management config problem. Any future addition to
`sdkconfig.defaults` carries the same latent staleness risk on a machine
with an already-generated `sdkconfig` sitting around -- seeding is one-way,
not a sync, so this exact failure mode can recur for a *different* Kconfig
option later even with this specific fix in place.

## The 24h graph had three real gaps vs. the original design, only one of them a genuine logic bug

Caught by comparing a live screenshot against the original design mockup
(`design-import/Flue Monitor.dc.html`) side by side:

- **Missing status pill.** The graph screen's topbar only ever had a
  settings-gear button on the right; the design shows a colored-dot +
  temp + rate pill there too (same info as the dashboard's own glow
  circle, just compact). Added `.status-pill` next to the gear, reusing
  `zoneColor()`/`rateLabel` that already existed for the dashboard.
- **Missing in-band zone labels.** The bottom-of-screen Cold/Optimal/Hot
  dot legend (`.graph-legend`) was the wrong shape of information -- the
  design puts the actual threshold numbers (e.g. "Hot above 280°C")
  directly on each colored band. Replaced the external legend with
  in-band `<text>` labels drawn inside `graphSvg`'s own band-rendering
  loop (skipped when a band's too thin to hold text legibly).
- **A logged event could visually appear to be "in the future."** This
  one **is not a timestamp bug** -- an event's `age_s` is never negative,
  confirmed by fetching `/api/history` immediately after `POST
  /api/event` on the real device (`events: [[0, "stove_off"]]`, never
  negative). The real cause: temperature *samples* only land every ~4min
  (`HISTORY_PUSH_EVERY_N`), but *events* are logged with second-level
  precision -- so a just-logged event (age 0) legitimately has a smaller
  age than the most recent temperature sample (age up to ~4min), landing
  its marker to the *right* of the curve's last plotted point on the
  x-axis. Visually indistinguishable from "in the future" even though the
  underlying number is correct. Fixed by extending the plotted curve
  itself with a synthetic age-0 point sourced from the already-live
  `GET /api/reading` value (`dashboard.js`'s `graphSvg` getter builds
  `plotSamples = [...samples, [0, reading.thermocouple_c]]` when the
  reading is valid) -- the curve now visibly reaches "now" instead of
  stopping ~4min short of it, and any recent event naturally lands on or
  right at that endpoint instead of past it. Also clamped `xOf()`'s input
  to `[0, maxAgeS]` as a defensive belt-and-suspenders measure. The
  current-reading dot at the curve's end is now colored by the live zone
  too (previously a fixed accent color), matching the design.

## Local mock server for iterating on the web UI without real hardware

`tools/mock_server.py` (stdlib-only Python) serves the real, unmodified
`main/web_ui/` files with synthetic REST responses standing in for the
device -- no ESP32, no flashing, no reboot needed for a CSS/layout/JS-only
change. This exists because the three graph-screen bugs above were hard to
iterate on against the real device: each fix attempt meant a full
build/flash/reboot/reconnect cycle just to see a CSS tweak, and the
"event in the future" bug specifically needed a *freshly logged* event
(age near 0) to reproduce, which the real device's RAM-only event log
loses on every reflash. The mock server's `POST /api/event` handler
reproduces that exact edge case on demand (log an event through the real
UI, it lands at true age 0 immediately) without touching hardware at all.
Verified working end-to-end via a headless Playwright run (`npx playwright
install chromium` -- no project dependency added, this is a one-off dev
tool, not shipped code) that loaded the mock server, clicked into the
graph view, logged a fresh event through the real "Log what you did" flow,
and screenshotted before/after to confirm the marker landed on the curve's
endpoint rather than past it.

## Misc

- **`idf.py monitor` exits with Ctrl+], not Ctrl+C.** Ctrl+C is intercepted
  by the monitor for other purposes. Ctrl+T then Ctrl+H shows its full
  shortcut menu.
- **A background `idf.py monitor`/`esphome logs` session holds the serial
  port open exclusively** — a concurrent `idf.py flash`/`ota_flash.sh` from
  another shell will fail with "Resource temporarily unavailable" until it's
  stopped.
