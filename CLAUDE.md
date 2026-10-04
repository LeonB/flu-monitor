# flu-monitor (project)

A DIY wood stove flue temperature monitor (inspired by the "Oru" product),
made of two physical devices/firmware folders:

- **`flu-monitor/`** — the **sidecar**: reads the stovepipe thermocouple
  (MCP9601) over I2C, logs it, and serves a REST/WebSocket API + Google
  Sheets logging. Plain ESP-IDF (not ESPHome/Arduino). See
  `flu-monitor/README.md` for day-to-day build/flash/OTA commands and its
  REST API, and `flu-monitor/CLAUDE.md` for implementation-specific gotchas
  (STEMMA QT power sequencing, MCP960x errata specifics, the live WiFi
  test-connect-before-save design). This folder was originally
  `flu-monitor-idf/`, built alongside an earlier ESPHome version of this
  same sidecar (also then named `flu-monitor/`) during a rewrite -- once the
  rewrite caught up (Milestones 1-5: WiFi/captive portal/mDNS/OTA, sensors,
  settings/REST API/rate-zone regression, WebSocket broadcast, Google Sheets
  logging) and a BMP581 sensor originally alongside the MCP9601 was
  physically removed (see `flu-monitor/CLAUDE.md`), the ESPHome version was
  retired and removed entirely, and this folder renamed to take its place.
  Milestone 7 (woodstove event logging via `POST /api/event`) is also done.
  Milestone 6 (embedded web UI at `/` -- dashboard, 24h graph, settings,
  event-log sheet) is also done, visually verified in a real browser (see
  `flu-monitor/CLAUDE.md`'s own Status section) -- all 7 planned milestones
  for this sidecar are now complete.
- **`flu-display/`** — the plain-ESP-IDF **display**: a screen-less ambient
  light box that subscribes to the sidecar's live broadcast and shows the
  reading as a color/pulse gradient. See `flu-display/README.md` for
  day-to-day build/flash commands and `flu-display/CLAUDE.md` for its
  implementation-specific gotchas (WiFi/captive portal, mDNS resolution, LED
  color/pulse tuning).

This file covers only what's shared context across all of them.

## Shared components

`components/` (this level, sibling to both firmware folders) holds
`wifi_setup/`, `captive_portal/`, `dns_server/`, and `ota_server/` --
ESP-IDF components consumed by both `flu-monitor/` and `flu-display/` via
each project's own `CMakeLists.txt` setting `EXTRA_COMPONENT_DIRS` to point
here. Until this was consolidated, each project carried its own copy that
had quietly diverged: `flu-monitor/`'s captive portal had grown a live
network scan, a test-connect-before-save flow (see `flu-monitor/CLAUDE.md`'s
"Live WiFi test-connect-before-save" section), and a proper Alpine.js UI,
while `flu-display/`'s stayed the original plain-HTML-form version from
Milestone 1. `flu-monitor/`'s richer version became the shared one (a
strict superset of what `flu-display/`'s had); `flu-display/`'s `main.c` was
updated to match `flu-monitor/`'s pattern of marking/clearing the
"last WiFi attempt failed" NVS flag so the shared portal's failure screen
works for it too. `dns_server/` and `ota_server.c` (113 of ~120 lines
byte-identical between the two projects' copies -- same auth check, same
streaming-OTA-write handler) needed no such reconciliation, just a
straight move; `ota_server_register()`/`ota_server_start()` now take the
secret as a parameter instead of `#include`-ing a project-private
`secrets.h` directly, so the shared component itself carries no secret of
its own.

Each device's captive-portal SoftAP SSID (`SETUP_AP_SSID` in each project's
own `config.h`, e.g. `"Flu Monitor Setup"` vs. `"Flu Display Setup"`) is
still what tells you which physical device you're configuring -- the
portal's own page (`components/captive_portal/root.html`) deliberately
doesn't brand itself by device name, since it's shared.

Not shared (deliberately): `status_led.c` (genuinely different hardware --
flu-display drives a plain GPIO status LED, flu-monitor drives a NeoPixel --
not just different code for the same job) and the LED-ring zone-color/pulse
math (ported into `flu-monitor/main/web_ui/dashboard.js` as a *JS
reimplementation* of `flu-display/main/led_display.c`'s logic, not a shared
library, since there's no lightweight way to share logic across C and JS on
this stack). The *tuning constants* that math depends on (pulse periods,
rate deadband, color-transition exponent) are a different story, though --
see `flu-monitor/CLAUDE.md`'s "LED/glow pulse-tuning constants now live in
settings" section: those used to be hardcoded independently in both
`flu-display/main/config.h` and `dashboard.js`, which had already drifted,
and now come from one live source the same way the zone/rate thresholds
already did.

## Telling the two physical devices apart (and the silent mis-flash failure mode)

Both devices join the same WiFi network and look superficially similar once
connected (same `wifi_setup`/`captive_portal` stack, same OTA auth scheme),
and flashing one project's firmware onto the other project's physical board
does **not** fail loudly -- `app_main()` keeps booting either way, WiFi still
joins fine, the HTTP server still starts, so the symptom is a board that
looks alive and reachable but is quietly missing the one thing that
mattered. This actually happened: the `flu-display` board had
`flu-monitor`'s firmware flashed onto it at some point (most likely a wrong
`-p` port on an earlier `idf.py flash`), so it tried to init a MCP9601 that
physically doesn't exist on that board -- `init_mcp9601()` failed every
single boot, `thermocouple_ok` stayed `false`/all fields zeroed forever
(`GET /api/reading`/`/ws` both reflect whatever `sensors_read()` last wrote,
see `flu-monitor/CLAUDE.md`'s "The regression window..." section), and it
still answered mDNS as `flu-monitor` (`MDNS_HOSTNAME` is baked into
whichever firmware is actually running, not tied to the physical board),
colliding with the real sidecar's own `flu-monitor.local` and making
`flu-monitor.local` traffic land on the broken board unpredictably instead
of the real one. Nothing about that state looks like an obvious crash from
a distance -- it took directly comparing a serial boot log against a REST
response from a known-good IP to find it.

**Before flashing a port you haven't confirmed, read its boot log instead of
trusting which USB device node you *think* is which** -- macOS's
`/dev/cu.usbserial-XXXX` numbering carries no information about which
physical board is on which cable, and that assumption is exactly how the
mix-up above happened. A passive serial read (`idf.py monitor`, or just
opening the port) shows an unambiguous `app_init: Project name: flu-monitor`
or `flu-display` line within the first second of boot, before anything
project-specific runs -- that line can't lie. Runtime log tags are a second
confirmation: `sensors`/`MCP9601`/`rest_api` only ever appear in
`flu-monitor`'s own log output, since `flu-display` has no sensors.c, no
MCP9601, and no REST API of its own -- those tags showing up at all means
you're looking at `flu-monitor`'s firmware, regardless of which board you
expected to be talking to.

**On the network, once both are up**: each project's own `MDNS_HOSTNAME`
(`flu-monitor`/`flu-display` in each project's own `main/config.h`) should
resolve to exactly one IP each -- if a lookup (`dns-sd -G v4
flu-monitor.local` or equivalent) ever returns two different addresses, or
an address you weren't expecting, that's the signature of this exact
mis-flash, not a flaky network. `GET /api/wifi` (queried fresh every call,
never cached -- see `flu-monitor/CLAUDE.md`'s own note on this) is a quick
way to tell two candidate IPs apart by RSSI/BSSID when you're not sure which
one you're actually touching, since a board closer to the router will show
a visibly stronger signal than one further away.

## Project goal (bigger than either subfolder's code suggests)

The end goal: an ambient light display you can glance at to know if the
stove is running too cold (creosote risk), in the optimal zone, or too hot
(overfire risk), including catching a fast temperature spike, not just an
absolute reading.

Full intended system:
- **Sense**: a K-type thermocouple (eventually a washer-style probe,
  magnetically/mechanically clamped to the stovepipe — currently still a
  bare-wire stand-in, see `flu-monitor/CLAUDE.md`'s I2C gotchas) measures
  stovepipe surface temperature.
- **Read**: the MCP9601 in the sidecar's hardware converts that to a clean
  digital reading.
- **Process**: the ESP32 sidecar reads it over I2C, tracks rate-of-change/
  zone itself, and logs it (see `flu-monitor/CLAUDE.md`'s "The regression
  window..." section) via NVS-backed settings + a REST API + Google Sheets
  logging, including woodstove event annotations via `POST /api/event`
  (Milestones 1-5 and 7), plus an embedded web UI to actually tap those
  events from a phone at the stove (Milestone 6) -- all seven of this
  sidecar's planned milestones are done.
- **Display (`flu-display/`)**, a separate physical device (Lolin D32 Pro +
  24-LED SK6812 RGBW ring): all three build milestones done, see
  `flu-display/README.md`/`flu-display/CLAUDE.md` for status. Screen-less
  ambient light box, placed wherever you'd actually glance at it, not
  necessarily next to the stove. Diffuses a color gradient (cool blue ->
  amber -> red) and pulses faster when temperature is rising quickly.
  Subscribes to the sidecar's `/ws` WebSocket broadcast (decided against
  ESP-NOW/MQTT/a from-scratch native-API client — see `flu-display/CLAUDE.md`
  for why) — not MQTT, not ESP-NOW, that's settled now, not still open.
- **Alert**: push a phone notification if things cross into dangerous
  territory — not built yet.
- The sidecar's WiFi/logic layer **moved off ESPHome to plain ESP-IDF**
  (`flu-monitor/`, formerly `flu-monitor-idf/` during the rewrite -- see
  above): ESPHome's YAML+lambda model became awkward once the rate-of-change
  regression needed to be real algorithmic C++, not a string in a config
  file. `flu-display` was already plain ESP-IDF from the start, for an
  unrelated reason (no sensors to expose, no Home Assistant integration
  need, so ESPHome would have been overkill there specifically) — the two
  projects converging on the same stack is somewhat incidental, not evidence
  either one influenced the other's choice.

**Current phase is data-gathering, nothing else.** The event buttons (Cold
Start, Opened Stove, Added Wood, Damper Up/Down, Burning Optimally, Stove
Roaring, Dying Down, Fire Out, Stove Off) exist so a couple of weeks of real
burns can be correlated against flue temperature, to *derive* the zone
boundaries and a normal-operation rate-of-change baseline before writing any
actual classification/alert logic. Two things worth remembering when that
next phase starts:
- There won't be real overfire examples in the data (nobody should deliberately
  overfire a stove to collect a data point), so the dangerous-end threshold will
  have to come from a mix of the empirical normal-operation ceiling plus
  external stovepipe-safety reference values, not pure curve-fitting like the
  cold/optimal boundaries can be.
- Thresholds derived from data collected with the current bare-wire stand-in
  probe are calibrated to *that* probe's response. Once the final washer-style
  clamp probe is mounted on the real stovepipe, expect to need a light
  recalibration pass (different thermal mass/contact/lag) even though the
  overall shape of the analysis should carry over. `flu-display`'s zone
  thresholds (`ZONE_COLD_MAX_C`/`ZONE_OPTIMAL_MAX_C`/`FAST_RISE_C_PER_MIN` in
  `flu-display/main/config.h`) are still placeholders pending this analysis.
