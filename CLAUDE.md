# flu-monitor (project)

A DIY wood stove flue temperature monitor (inspired by the "Oru" product),
made of two separate firmwares living in their own subfolders:

- **`flu-monitor/`** — the ESPHome **sidecar**: reads the stovepipe
  thermocouple over I2C and logs it. See `flu-monitor/README.md` for
  day-to-day flash/log commands and `flu-monitor/CLAUDE.md` for its
  implementation-specific gotchas (I2C errata, Google Sheets logging,
  woodstove data-gathering logic).
- **`flu-display/`** — the plain-ESP-IDF **display**: a screen-less ambient
  light box that polls the sidecar and shows the reading as a color/pulse
  gradient. See `flu-display/README.md` for day-to-day build/flash commands
  and `flu-display/CLAUDE.md` for its implementation-specific gotchas
  (WiFi/captive portal, mDNS resolution, LED color/pulse tuning).

This file covers only what's shared context across both.

## Project goal (bigger than either subfolder's code suggests)

The end goal: an ambient light display you can glance at to know if the
stove is running too cold (creosote risk), in the good zone, or too hot
(overfire risk), including catching a fast temperature spike, not just an
absolute reading.

Full intended system:
- **Sense**: a K-type thermocouple (eventually a washer-style probe,
  magnetically/mechanically clamped to the stovepipe — currently still a
  bare-wire stand-in, see `flu-monitor/CLAUDE.md`'s I2C gotchas) measures
  stovepipe surface temperature.
- **Read**: the MCP9601 in the sidecar's hardware converts that to a clean
  digital reading.
- **Process (`flu-monitor/`)**: the ESP32 sidecar reads it over I2C, and
  currently just logs it (see `flu-monitor/CLAUDE.md`'s "Woodstove
  data-gathering logging") — it doesn't yet track rate-of-change or make any
  zone/alert decision itself.
- **Display (`flu-display/`)**, a separate physical device (Lolin D32 Pro +
  24-LED SK6812 RGBW ring): all three build milestones done, see
  `flu-display/README.md`/`flu-display/CLAUDE.md` for status. Screen-less
  ambient light box, placed wherever you'd actually glance at it, not
  necessarily next to the stove. Diffuses a color gradient (cool blue ->
  amber -> red) and pulses faster when temperature is rising quickly. Polls
  the sidecar's existing `web_server:` JSON API over WiFi (decided against
  ESP-NOW/MQTT/a from-scratch native-API client — see `flu-display/CLAUDE.md`
  for why) — not MQTT, not ESP-NOW, that's settled now, not still open.
- **Alert**: push a phone notification if things cross into dangerous
  territory — not built yet.
- The sidecar's own WiFi/logic layer stays ESPHome (settled, not still open)
  — `flu-display` is plain ESP-IDF instead, a deliberate difference: it has no
  sensors to expose and no Home Assistant integration need, so ESPHome would
  have been overkill there specifically.

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
  cold/good boundaries can be.
- Thresholds derived from data collected with the current bare-wire stand-in
  probe are calibrated to *that* probe's response. Once the final washer-style
  clamp probe is mounted on the real stovepipe, expect to need a light
  recalibration pass (different thermal mass/contact/lag) even though the
  overall shape of the analysis should carry over. `flu-display`'s zone
  thresholds (`ZONE_COLD_MAX_C`/`ZONE_GOOD_MAX_C`/`FAST_RISE_C_PER_MIN` in
  `flu-display/main/config.h`) are still placeholders pending this analysis.
