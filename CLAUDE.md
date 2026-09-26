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
- **Display — `flu-display/`, a separate physical device (Lolin D32 Pro +
  24-LED SK6812 RGBW ring)**: in progress, see `flu-display/README.md` (or its
  own section below) for status. Screen-less ambient light box, placed
  wherever you'd actually glance at it, not necessarily next to the stove.
  Diffuses a color gradient (cool blue -> amber -> red) and pulses faster when
  temperature is rising quickly. Polls this sidecar's existing `web_server:`
  JSON API over WiFi (decided against ESP-NOW/MQTT/a from-scratch native-API
  client — see `flu-display`'s own notes below for why) — not MQTT, not
  ESP-NOW, that's settled now, not still open.
- **Alert**: push a phone notification if things cross into dangerous territory
  — not built yet.
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

## flu-display (plain ESP-IDF, not ESPHome -- see `flu-display/`)

Status: **All three milestones (WiFi + captive portal, poll `flu-monitor`'s
JSON API, drive the LED ring) verified working end-to-end on real hardware.**
Milestone 1: captive portal auto-popped on a real client (confirmed via
screenshot), form submission saved credentials to NVS, device rebooted and
joined the real network, stayed connected. Milestone 2: once connected, the
device resolves `flu-monitor`, polls its thermocouple sensor every
`POLL_INTERVAL_MS`, and logs the parsed, sanity-clamped reading plus
rate-of-change to serial. Milestone 3: the 24-LED SK6812 RGBW ring (wired to
GPIO13, not GPIO25 -- see below) renders the polled reading as a
blue/amber/red gradient with a breathing pulse, confirmed by camera to light
up solid blue at room temperature as expected. Since then, live in-person
tuning (see below) landed on: a less yellow-green, more amber good-zone
color; the pulse's bright peak also swapping toward the neighboring zone's
color to hint at heating/cooling direction; and slower pulse paces overall.

- **RGB color choices for the gradient need to be judged live on the real
  ring, not from a camera photo or from first-principles RGB values.** Two
  separate incidents: (1) `COLOR_GOOD` was originally `{255, 120, 0}` --
  looks like a reasonable amber in the abstract, but on the actual hardware
  it read as yellow-green, not amber; dropping green to `55` fixed it.
  (2) A camera photo of the ring lit solid blue came out with an obvious
  blue color-cast even on a supposedly-neutral scene, confirming the
  webcam's own auto white balance cannot be trusted to judge subtle color
  correctness either -- only the user's own eyes on the physical hardware
  can validate a color tuning here.
- **Blending directly in RGB between two hues that are far apart (e.g. the
  gradient's blue and amber) passes through a muddy, desaturated middle,
  not a perceptually "in-between" color.** Tried twice, both confirmed by
  eye on real hardware: first as a constant ~40% extrapolated-temperature
  blend, which looked like an unrelated purple tint rather than "blue
  leaning warm"; then as a pulse-peak blend capped around 65% of the way to
  blue, which looked washed-out white at high brightness rather than
  "bluer." The fix that actually worked: never hold a partial RGB blend --
  swap cleanly to the *other pure endpoint color* instead (see
  `trend_neighbor_color()` in `led_display.c`), and if a gradual-looking
  transition is wanted, vary *when* the swap happens (e.g. envelope-shaped
  via `COLOR_TRANSITION_EXPONENT`) rather than blending the color itself
  partway.
- **The LED ring's pulse/color design is now the honest-trough,
  trend-peak pattern**: the dim point of each breathing pulse always shows
  the true current-zone color (so a glance during the dim phase is never
  misleading), and only the bright peak swaps to the neighboring zone's
  pure color in the direction of the trend (cooling swaps toward the cooler
  zone's color, heating toward the warmer one, stable has no swap at all).
  `COLOR_TRANSITION_EXPONENT` (currently 11) shapes *when in the pulse* that
  swap happens, biased so it stays near the trough color for most of the
  cycle and only swings to the peak color in a quick ramp right at the top
  -- tuned up live from an initial guess of 3, through 6 and 9, based on
  the user wanting a longer amber hold and a quicker ramp.
- **Iterating on LED look-and-feel is fastest with a temporary
  `#if LED_TEST_PATTERN` block in `main.c`** that calls `led_display_init()`
  and holds `led_display_set_reading()` on one fixed (temperature, rate)
  pair indefinitely (skipping WiFi/polling entirely), rebuilding and
  reflashing after each tweak -- much faster to iterate than waiting for a
  real thermocouple reading to reach the state being tuned. Never commit
  this block; revert `main.c` back to the real polling loop (`git checkout
  -- flu-display/main/main.c`) once a tuning session is done, and rebuild
  the real firmware so the device goes back to actually polling
  `flu-monitor` before finishing up.
- **Correlate photo/log captures against the live serial log's own
  timestamps, not host-side `sleep` arithmetic.** A batch of test-pattern
  photos taken via pre-computed `sleep N` delays between camera captures
  drifted out of sync partway through (confirmed by two consecutive photos
  showing the same color when they were meant to be different scenarios) --
  `ffmpeg`'s own camera-open overhead isn't perfectly consistent call to
  call, and small per-capture delays compound over a multi-step sequence.
  The reliable fix: run `idf.py monitor` in the background (redirecting to
  a log file; attaching resets the board), and gate each capture on
  `grep -c` actually seeing that scenario's log line increment, rather than
  timing it from the outside.

- **Plain `.local` resolution turned out unreliable in practice, contrary to
  Milestone 2's first clean test run.** A later boot got consistent
  `ESP_ERR_HTTP_CONNECT` / `getaddrinfo() returns 202` failures on every
  single poll, *while the Mac resolved and pinged the same hostname fine at
  the same time* (confirmed with `dns-sd -B` and `ping`) -- so the sidecar's
  own mDNS responder was healthy; the failure was specific to the ESP32
  client's implicit resolution path. Switched to the fallback the original
  plan had already flagged as a contingency: an explicit `mdns_query_a()`
  call in `flue_poll.c`, building the request URL from the resolved IP
  (`IPSTR`/`IP2STR`) instead of handing `esp_http_client` a `.local`
  hostname directly.
- **Even the explicit `mdns_query_a()` call was itself flaky poll-to-poll**
  on this network -- observed RSSI swinging -38 to -62 across boots,
  plausibly lossy multicast rather than a code bug, since a single mDNS
  query has no built-in retry across that kind of loss. Fix: `flue_poll.c`
  now resolves once and caches the IP (`s_cached_ip`/`s_have_ip`), reusing
  it on every subsequent poll, and only triggers a fresh `mdns_query_a()`
  call when an actual HTTP request against the cached IP fails -- this
  turned "resolve via multicast every 3 seconds, hope it lands" into
  "resolve once, keep working off it," and a 60-second live test afterward
  showed zero failures. If `flu-monitor`'s IP ever changes via DHCP without
  its own connection dropping, this cache could go stale; not handled
  specially since a DHCP reassignment independent of a reconnect is rare on
  a home network and would just self-correct on the next HTTP failure.

- **`espressif/mdns` is a managed component fetched via the component
  manager** (`main/idf_component.yml`), not bundled in ESP-IDF core the way
  it was in older IDF versions -- confirmed by searching the cached
  v5.5.5 checkout and finding no `mdns` component there at all. `idf.py
  build` fetches it into `managed_components/` (git-ignored, like a package
  manager's install directory) and pins the resolved version in
  `dependencies.lock` (committed, for reproducible builds -- same idea as a
  lockfile in any other package manager).
- **Plain `.local` hostnames resolving automatically via `esp_http_client`
  once `mdns_init()` has run is NOT reliable enough to depend on** --
  Milestone 2's first test run made it look like it "just worked" with no
  extra code, but a later boot got consistent resolution failures under the
  exact same firmware. See the mDNS bullets above (Milestone 3 section) for
  what actually ended up robust: an explicit `mdns_query_a()` call, with its
  result cached and reused across polls rather than re-queried every time.
- **The sidecar's `web_server` JSON API is keyed by the sensor's exact
  entity name, URL-encoded, not its `object_id`** -- reuses the same URL
  format verified against the live sidecar earlier in this project:
  `GET http://flu-monitor.local/sensor/Thermocouple%20Temperature` ->
  `{"id":"sensor/Thermocouple Temperature","value":25,"state":"25.0 °C"}`.
  `main/flue_poll.c` parses the `"value"` field.
- **`flue_poll.c` reuses the sidecar's own defensive pattern**: a
  -40..600°C sanity clamp (same range and rationale as `flu-monitor.yaml`'s,
  see above) before a reading is accepted, and rate-of-change computed from
  the last *accepted* reading and its timestamp (via `esp_timer_get_time()`),
  not the raw poll cadence -- so one bad/rejected reading can't skew the rate
  calculation, and a poll/parse failure just returns `valid=false` rather
  than a stale or guessed value.
- **LED ring data line is on GPIO13, not the originally-planned GPIO25.**
  GPIO25 was chosen on the (wrong, for this board) assumption that GPIO12-15
  form the D32 Pro's onboard TF-card SPI bus, the ESP32's generic HSPI
  default pins -- checked afterward and the D32 Pro's SD card/TFT header
  actually shares *VSPI* (GPIO18/19/23, CS on 4/14) instead, so GPIO13 has no
  onboard conflict. GPIO25's only distinguishing feature turned out to be
  that it's one of the chip's two DAC-capable pins, irrelevant here since
  it's driven as a plain digital RMT output either way.
- **`espressif/led_strip`'s current API doesn't have a `led_pixel_format`
  field** (older tutorials/examples online do -- check the version actually
  fetched, not just search results). This version's `led_strip_config_t`
  instead has `color_component_format`, set via a helper macro like
  `LED_STRIP_COLOR_COMPONENT_FMT_GRBW` -- read
  `managed_components/espressif__led_strip/include/led_strip_types.h`
  directly rather than trust a remembered/guessed struct layout when the
  component gets bumped.
- **`led_display.c` runs its own ~30ms-tick FreeRTOS task** independent of
  the 3-second poll cadence, so the breathing pulse animates smoothly
  between polls. It reads the latest reading via a small
  `portENTER_CRITICAL`/`portEXIT_CRITICAL`-guarded shared state struct that
  `main.c` writes into after each poll -- not a queue/mutex, since it's a
  handful of plain scalars and the render task only ever needs the latest
  value, never a history.
- **Camera-based visual verification of the ring worked without any
  browser/artifact plumbing**: macOS exposes a plugged-in webcam (here, a
  Logitech C920) directly to `ffmpeg` via `-f avfoundation`, so
  `ffmpeg -f avfoundation -i "0" -frames:v 1 -update 1 out.jpg` grabs a
  single still frame straight from the command line. `-update 1` is
  required for the `image2` muxer to write one plain file instead of
  demanding a sequence pattern like `%03d.jpg`. The camera app (here iTerm,
  under System Settings -> Privacy & Security -> Camera) needs prior OS
  permission; an `Input/output error` opening the device despite the format
  probe succeeding is the symptom either of that permission being denied,
  or of another app already holding the camera open.

- **Toolchain: reuse ESPHome's cached ESP-IDF, don't reinstall it.** ESPHome
  already has a full ESP-IDF v5.5.5 checkout at
  `~/Library/Caches/esphome/idf/frameworks/5.5.5/` plus the Xtensa toolchain
  and Ninja under `~/Library/Caches/esphome/idf/tools/` -- multiple GB,
  already paid for. Only the IDF-native Python venv is missing (ESPHome
  manages its own venv separately, under a different naming convention than
  `idf.py`'s own `python_env/idf5.5_py3.14_env` expectation). One-time setup,
  scoped to just the `esp32` target to avoid pulling RISC-V toolchains for
  chip variants this project doesn't use:
  ```sh
  export IDF_TOOLS_PATH=~/Library/Caches/esphome/idf
  export IDF_PATH="$IDF_TOOLS_PATH/frameworks/5.5.5"
  PIP_INDEX_URL=https://pypi.org/simple "$IDF_PATH/install.sh" esp32
  ```
  (the `PIP_INDEX_URL` override is the same broken-corporate-pip-index
  workaround as the ESPHome setup above.) After that one-time step, every
  session just needs `flu-display/activate-idf.sh` sourced (not executed) in
  whatever command also needs `idf.py` -- Bash tool calls don't persist shell
  state between calls, so this has to happen in the *same* command as the
  `idf.py` invocation that follows it: `. ./activate-idf.sh && idf.py build`.

- **Captive portal is adapted from Espressif's own bundled example**
  (`$IDF_PATH/examples/protocols/http_server/captive_portal/`), not a
  third-party library, deliberately, per the "stay in the Espressif ecosystem"
  preference. That example only serves a static page with no way to actually
  submit/save credentials; `components/captive_portal/` adds the real
  `POST /save` handler (NVS-backed credential storage, then `esp_restart()` to
  retry STA with them) on top of it. `components/dns_server/` is copied
  near-verbatim from that same example.

- **`wifi_setup` had to become its own component**, not live inside `main/`
  as originally sketched -- `components/captive_portal/` also needs
  `wifi_creds_save()`, and a non-`main` component can't cleanly reach into
  `main/`'s private headers without `main` explicitly exposing them (and
  `main` itself depends on `captive_portal`, so that path risks circularity
  anyway). A small sibling component both can depend on is the clean fix.

- **`httpd_query_key_value()` does NOT URL-decode.** Its own doc comment in
  `esp_http_server.h` says so explicitly: keys/values from a
  `x-www-form-urlencoded` POST body come back with `+` and `%XX` still
  literal. `captive_portal.c`'s `url_decode()` handles this -- don't assume
  the extracted SSID/password are ready to use as-is.

- **One of the two D32 Pro boards on hand needed the manual BOOT+RST bootloader
  sequence** (`esptool`'s auto-reset via DTR/RTS failed with "No serial data
  received"); the other board's auto-reset worked fine over the same cable
  and command. Treat this as a per-board/cable hardware quirk to check for
  first, not a sign anything in the build or flash command is wrong.

- **Testing the captive portal from this Mac is subtle because it's
  multi-homed** (wired Ethernet as the default route, WiFi separate and not
  the default route). Plain command-line tools (`curl`, `dig`) resolve DNS via
  the *system* default resolver, which stayed on Ethernet even while joined to
  the ESP32's SoftAP -- so `dig @<ap-ip> some-hostname` timing out, or
  `curl http://captive.apple.com/...` returning Apple's real page instead of
  the redirect, does NOT mean the DNS hijack is broken. macOS's own Captive
  Network Assistant (the thing that actually pops the setup window) evidently
  probes through the specific just-joined WiFi interface directly, bypassing
  the system default resolver -- that popup actually appearing is the real
  signal to trust, not a `dig`/`curl` test run from the same dual-homed Mac.

## Other notes

- `secrets.yaml` and `.esphome/` are git-ignored.
- `web_server`'s own OTA endpoint is explicitly disabled (`ota: false` under
  `web_server:`) since the encrypted `ota:` platform already covers updates, and
  the web one would otherwise accept plaintext firmware uploads.
