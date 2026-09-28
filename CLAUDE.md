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
  Milestone 6 (embedded web UI at `/` -- dashboard, 24h graph, settings)
  is built and flashed, all its REST endpoints confirmed live, but its
  actual rendering/interaction hasn't been checked in a real browser yet
  (see `flu-monitor/CLAUDE.md`'s own Status section).
- **`flu-display/`** — the plain-ESP-IDF **display**: a screen-less ambient
  light box that subscribes to the sidecar's live broadcast and shows the
  reading as a color/pulse gradient. See `flu-display/README.md` for
  day-to-day build/flash commands and `flu-display/CLAUDE.md` for its
  implementation-specific gotchas (WiFi/captive portal, mDNS resolution, LED
  color/pulse tuning).

This file covers only what's shared context across all of them.

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
  events from a phone at the stove (Milestone 6 -- built, not yet visually
  verified in a browser).
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
