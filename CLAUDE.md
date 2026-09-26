# flu-monitor

ESPHome firmware for an Adafruit ESP32 Feather V2 with a BMP581 (pressure/temperature)
and an MCP9601 (K-type thermocouple amp) over STEMMA QT/I2C. See `README.md` for
day-to-day flash/log commands and the hardware pinout table.

## Project goal (bigger than the code in this repo suggests)

This is the **sidecar module** of a DIY wood stove flue temperature monitor
(inspired by the "Oru" product). The end goal: an ambient light display you
can glance at to know if the stove is running too cold (creosote risk), in
the good zone, or too hot (overfire risk), including catching a fast
temperature spike, not just an absolute reading.

Full intended system:
- **Sense**: a K-type thermocouple (eventually a washer-style probe,
  magnetically/mechanically clamped to the stovepipe — currently still a bare-wire
  stand-in, see the I2C gotchas below) measures stovepipe surface temperature.
- **Read**: the MCP9601 in this repo's hardware converts that to a clean digital
  reading.
- **Process (this repo)**: this ESP32 sidecar reads it over I2C, and currently
  just logs it (see "Woodstove data-gathering logging" below) — it doesn't yet
  track rate-of-change or make any zone/alert decision.
- **Display — not built yet, a separate physical device**: a standalone,
  screen-less ambient light box (or several — "koppelen display boxes" was the
  original idea), placed wherever you'd actually glance at it, not necessarily
  next to the stove. Diffuses a color gradient (cool blue -> amber -> red) and
  pulses faster when temperature is rising quickly. Receives zone/color/pulse
  state wirelessly from the sidecar — ESP-NOW vs MQTT is still an open decision
  (ESP-NOW: lightweight, no router/broker; MQTT: more setup, plays nicer with
  Home Assistant).
- **Alert**: push a phone notification if things cross into dangerous territory
  — not built yet.
- Also still undecided: whether the sidecar's WiFi/logic layer stays ESPHome
  (as now) or becomes a custom build (webserver, REST API, WebSockets).

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
  overall shape of the analysis should carry over.

## Local ESPHome environment

ESPHome is installed via **pipx with Homebrew's Python 3.13**, not the system/pyenv
Python (3.14 at time of writing — ESPHome's dependencies aren't compatible with it yet):

```sh
pipx install --python /opt/homebrew/opt/python@3.13/bin/python3.13 esphome
```

The machine's global `pip.conf` points at a work CodeArtifact index
(`omniboost-pypi-...`) whose auth token can expire, which breaks ESPHome's own
first-run ESP-IDF toolchain install (it shells out to pip). If a build fails with
`401 Error, Credentials not correct` while "Installing ESP-IDF ... Python
dependencies", rerun with the public index forced for that one command:

```sh
PIP_INDEX_URL=https://pypi.org/simple esphome run flu-monitor.yaml ...
```

## Hard-won I2C gotchas (do not "fix" these back)

- **`i2c: scan: true` must stay off.** The MCP9601/MCP960x series locks up and stops
  responding to *all* further I2C traffic after receiving a full bus scan — this is
  confirmed by Adafruit's own firmware engineers, not a guess:
  https://github.com/adafruit/Adafruit_Wippersnapper_Arduino/issues/299. With
  `scan: true`, the sensor looked intermittently broken (worked right after flashing,
  then died) in a way that looked like a hardware/cable/power problem but wasn't.
  Diagnosing this burned a lot of time on the wrong track (cables, connectors,
  clock-stretch timeouts, a whole patched local copy of the `mcp9600` component to
  bypass a "device ID never responds" check) before the real cause turned up. The
  stock `mcp9600` component works fine and correctly identifies the chip
  (`Device ID: 0x41`) once `scan: false` is set — no local/patched component needed.

- **Bus frequency must stay ≤85kHz** while the MCP9601 is attached. This chip family
  has a real, documented Microchip silicon errata ("Intermittent I2C Read Command
  Clock Stretching Failure") that above ~85kHz can make a read silently return stale
  duplicate register data — it looks like a valid reading, not a bus error, so it's
  easy to mistake for a real (but wrong) temperature. Background/write-up:
  https://www.rikeshkkpatel.co.uk/diy-reflow-oven/problems-with-the-mcp9600-thermocouple-amplifier/
  85kHz was fine for the BMP581 too in testing, so this is a fleet-wide setting, not
  a per-device one.

- **A bare-wire thermocouple can produce wildly implausible readings if the
  leads get bridged by a conductive liquid** — observed 709.3°C and 891.2°C
  logged to the woodstove data-gathering sheet; confirmed cause was the probe
  briefly dunked in beer. That's an *analog* front-end disturbance (the tiny
  thermocouple EMF itself gets corrupted before the MCP9600 ever digitizes it),
  not digital I2C corruption — the two mechanisms produce the same symptom
  (implausible reading) for different reasons, and both are possible with this
  hardware, so don't assume one explains every future occurrence just because it
  explained this one. This specific failure mode should go away once the final
  insulated/sealed washer-style probe replaces the bare-wire stand-in on the real
  stovepipe. `flu-monitor.yaml`'s logging interval has a sanity clamp (reject
  readings outside roughly -40°C..600°C, a stovepipe has no business reading
  outside that) that filters this at the logging layer regardless of root cause.
  That clamp only catches *wildly* wrong values, though — a corrupted reading
  that happens to land inside the plausible range would sail through. Any future
  safety-critical alert logic (the actual overfire detection) should not trust a
  single sample; require at least two consecutive consistent readings before
  treating a spike as real.

- **`GPIO2` (STEMMA QT power) needs `setup_priority: 1200`** on its switch, higher
  than the i2c bus's own priority (`BUS = 1000`). Without this, the sensors are
  unpowered when the i2c bus initializes and you'll see `SCL is held LOW on the bus`
  bus-recovery failures at boot.

- When chasing a fresh I2C issue, don't trust an isolated one-off reading (spike or
  drop) as proof of a real physical event or of corruption — check several
  consecutive poll cycles. A real physical event (e.g. touching the thermocouple tip)
  shows as a smooth multi-sample ramp and decay; corrupted/stale data shows as a
  single isolated outlier surrounded by otherwise-stable values.

## Flashing/logging while iterating

- First flash (or whenever USB is plugged in) must go over serial:
  `esphome run flu-monitor.yaml --device /dev/cu.usbserial-XXXX` — find the exact
  port with `ls /dev/cu.usbserial*`.
- Once on Wi-Fi, OTA works from anywhere on the LAN:
  `esphome run flu-monitor.yaml --device flu-monitor.local`.
- To capture a fresh **boot-time** log (setup/dump_config only fires once per boot,
  right at the start), you have to reset *right before* attaching the log stream —
  reconnecting to an already-running device misses it entirely. Over USB:
  ```sh
  python3 -m esptool --port /dev/cu.usbserial-XXXX --after hard_reset chip_id
  esphome logs flu-monitor.yaml --device /dev/cu.usbserial-XXXX
  ```
  Over Wi-Fi only (no USB), use the native API to press the `Restart` button and
  reconnect in a retry loop instead (see chat history for the aioesphomeapi script
  used to do this — it's the same idea as `esphome logs`, just needs to survive the
  reboot's connection drop).
- `logger: level: INFO` is the production setting. Bumping to `DEBUG` shows
  register-level writes/warnings; `CONFIG`-and-above lines (like `Found device at
  address` from a scan, or a component's `dump_config()` output) need at least
  `DEBUG` — they don't show at `INFO` even though `INFO` is a "lower" verbosity
  in casual terms. This tripped up early debugging more than once.

## Google Sheets logging gotchas

See `google-sheets-logger/README.md` for the actual setup steps. Two things that
cost real debugging time and are easy to accidentally reintroduce:

- **Use `http_request.get`, never `.post`, against a Google Apps Script Web App.**
  These always respond with a redirect to a `script.googleusercontent.com` URL
  that only accepts GET. Browsers and curl's default behavior downgrade the
  method to GET when following that redirect, so a POST can look like it works
  fine when hand-tested — but the ESP32's HTTP client (ESP-IDF's
  `esp_http_client`) preserves the original method across the redirect instead,
  so a POST fails on-device with HTTP 405 even though the exact same webhook
  tests fine from a browser or `curl -L`. Sending a GET with the data as query
  parameters (which is what's in `flu-monitor.yaml` now) sidesteps this
  entirely, since GET always redirects to GET regardless of the client's
  redirect-method policy.
- **`!secret` can't be used inside a lambda's raw C++ text** — it only resolves
  when it's the *entire* value of a YAML node, not textually inside a bigger
  string. To get a secret into a lambda (e.g. to build a URL with dynamic
  sensor data appended), pull it in via `substitutions:` instead —
  substitutions do raw `${...}` text replacement across the whole parsed
  config, lambda bodies included, and a substitution's value can itself come
  from `!secret`.
- `http_request:`'s default 512B `buffer_size_tx` isn't enough for a long
  Apps Script URL (deployment ID + query string); undersized shows up as an
  intermittent `HTTP_CLIENT: Out of buffer` / `esp_http_client_open ESP_FAIL`,
  not a clean error pointing at the buffer. Bumped to 1024B here.
- When an Apps Script Web App looks broken after editing the code, check
  whether the deployment was actually redeployed as a **New version** — the
  "Deploy" button in the edit-deployment dialog silently no-ops if the Version
  dropdown is still left on the old version. "Deployment successfully updated"
  does not mean your new code is live.
- A redirect loop between the `/exec` URL and its `script.googleusercontent.com`
  echo URL happened once and resolved itself on retry a few seconds later —
  treat it as transient Google-side flakiness, not a config problem, unless it
  repeats.
- **Apps Script Web App latency is genuinely bad and not fixable from our
  side.** Direct `curl` timing against the deployed webhook (not the ESP32,
  a fast machine on a fast connection) showed round trips from 1.5s to 40+
  seconds across 8 back-to-back calls, 2 of those 8 essentially timed out.
  This is a documented characteristic of "Anyone"-access Web Apps, not
  something our config controls, and "Anyone" access is required here since
  the ESP32 has no way to do a Google login. Because Apps Script executes
  the handler and writes to the sheet *before* sending the client a
  response, a logged client-side failure usually still means the row landed
  — confirmed twice by checking the sheet directly after a logged failure.
- **We use a third-party `http_request_async` fork instead of ESPHome's
  stock `http_request`**, specifically so one of these slow/failed Google
  calls doesn't block the whole device's main loop (sensors, dashboard,
  everything) for the duration. It's pinned to a tag
  (`ref: esphome-2026.8`), not `main`. It's low-adoption and AI-written —
  see google-sheets-logger/README.md for the trust trade-off and how to
  revert to the stock component if that ever matters more than the
  blocking behavior it fixes.
- **Numeric cells show 26 instead of 26.0 by default.** `Number(p.temperature)`
  turns the string `"26.0"` the ESP32 sends into the plain JS number `26` --
  there's no such thing as a trailing zero on a number type -- and Sheets'
  default "Automatic" cell format then drops it on display. The stored value
  is correct either way; it's purely cosmetic. `Code.gs` now sets an explicit
  number format (`0.0` for the three temperature columns, `0` for pressure) the
  first time it touches a sheet that doesn't have it yet, self-healing an
  existing sheet on its very next write after redeploy -- but it won't
  retroactively reformat rows already written before that fix landed.
- **Sheet row timestamps come from Apps Script's own `new Date()` at
  write/processing time, not anything the ESP32 sends.** Combined with the
  latency variability above, this means row order/spacing in the sheet
  reflects when Google got around to processing each request, not necessarily
  the order the device actually triggered them in. Observed once: a
  `heartbeat` row landing only 21s after the previous one, which looked like a
  scheduling bug -- but the row *after* that was exactly 15:00 after the one
  *before* the odd one, confirming the device's internal schedule was never
  wrong, just delayed/out-of-order arrival at Google's end. Don't chase this
  as a device-side bug without checking whether the surrounding rows still
  add up to the expected interval once the odd one is skipped.

## Woodstove data-gathering logging

The periodic (non-button) log to Sheets is gated, not unconditional every tick:

- `thermocouple_deadband_c` and `log_heartbeat_min` (both `substitutions:`) control
  it -- only actually posts when the thermocouple has moved past the deadband since
  the last point logged, or the heartbeat interval has elapsed, whichever first.
  Currently 5°C / 15min; deliberately conservative during data-gathering so real
  transitions aren't blurred out. Tune up once the real noise floor and what counts
  as signal is clearer.
- Every periodic log's Event column gets tagged `temp_change` or `heartbeat` (via
  the `log_reason` global, set as a side effect of the interval's condition lambda)
  so you can tell a real-temperature-driven row from a routine keep-alive one at a
  glance. Button-press events aren't gated by any of this -- they always log
  immediately with their own label, since those are deliberate annotations. This
  also means **button presses don't reset the heartbeat clock** -- they call
  `log_to_sheets` directly and never touch `last_log_millis`, so pressing a
  button doesn't delay or restart the next scheduled heartbeat/deadband check.
- The deadband compares against `last_logged_thermocouple_c` (last value actually
  *sent*, not last raw reading), which is exactly why the corrupted-reading sanity
  clamp above has to run first and skip the tick entirely on failure -- if a garbage
  value ever got logged, it would become the new baseline and make every subsequent
  *real* reading look like a huge jump, cascading into a burst of bogus logs. This
  happened once before the clamp was added (four `temp_change` rows in under two
  minutes off of one bad reading) -- if that pattern reappears, suspect the clamp's
  range needs adjusting, not the deadband logic itself.

## Other notes

- `secrets.yaml` and `.esphome/` are git-ignored.
- `web_server`'s own OTA endpoint is explicitly disabled (`ota: false` under
  `web_server:`) since the encrypted `ota:` platform already covers updates, and
  the web one would otherwise accept plaintext firmware uploads.
