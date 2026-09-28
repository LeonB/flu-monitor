# flu-display (plain ESP-IDF, not ESPHome)

The ambient light display device -- see the repo root `CLAUDE.md` for the
bigger-picture project this is one half of, and `README.md` (in this folder)
for day-to-day build/flash commands and hardware pinout.

Status: **All of Milestones 1-5 verified working end-to-end on real
hardware.** Milestone 1: captive portal auto-popped on a real client
(confirmed via screenshot), form submission saved credentials to NVS, device
rebooted and joined the real network, stayed connected. Milestone 2
(superseded by Milestone 5, below): once connected, the device resolved
`flu-monitor`, polling its thermocouple sensor every `POLL_INTERVAL_MS`, and
logged the parsed, sanity-clamped reading plus rate-of-change to serial.
Milestone 3: the 24-LED SK6812 RGBW ring (wired to GPIO13, not GPIO25 -- see
below) renders the polled reading as a blue/amber/red gradient with a
breathing pulse, confirmed by camera to light up solid blue at room
temperature as expected. Since then, live in-person tuning (see below)
landed on: a less yellow-green, more amber optimal-zone color; the pulse's
bright peak also swapping toward the neighboring zone's color to hint at
heating/cooling direction; and slower pulse paces overall. Real-world field
testing (a skillet on induction, then an oven) also surfaced and fixed a
single-sample-trust bug in `flue_poll.c` (see below). Milestone 4: push-based
OTA updates (originally deferred). **Milestone 5** (new): switched from
polling `flu-monitor` (ESPHome) over REST every 3s to subscribing to
`flu-monitor`'s (the ESP-IDF rewrite's) WebSocket broadcast, and fetching
zone/rate thresholds from its REST API instead of this project's own
hardcoded `config.h` copies -- see "Switching to flu-monitor's WebSocket
broadcast" below.

## Switching to flu-monitor's WebSocket broadcast (Milestone 5)

- **This device is now event-driven, not a poller.** `flue_poll.c` owns a
  persistent `esp_websocket_client` connection to `flu-monitor`'s `/ws`
  endpoint instead of an `esp_http_client` GET every `POLL_INTERVAL_MS`.
  `main.c`'s own loop shrank to a lightweight staleness check (a timestamp
  comparison against `STALE_READING_MS`, no network I/O) -- every actual
  reading calls `led_display_set_reading()` directly from the WS client's
  own event-handler context as it arrives, roughly every 30s (the sidecar's
  own sensor cadence), not this device's old 3s poll interval.
- **The rate-of-change value comes straight from the broadcast payload**
  (`thermocouple_rate_c_per_min`), same as it already did from a second REST
  fetch before this change -- see the "Rate-of-change" bullet further down.
  Nothing new here, just a different transport for the same already-computed
  value.
- **The sanity clamp and suspicious-jump confirmation logic carried over
  unchanged**, just re-triggered by WS message arrival instead of a poll
  tick. Still needed: the sidecar's own raw `thermocouple_c` is reported
  as-read regardless of plausibility (only its *derived* rate/zone are
  protected by its own clamp -- see `../flu-monitor/CLAUDE.md`), so a
  glitched raw reading can still arrive over WS exactly as it could over
  REST.
- **Zone/rate thresholds (`ZONE_COLD_MAX_C`/`ZONE_OPTIMAL_MAX_C`/
  `FAST_RISE_C_PER_MIN`) became runtime-mutable** via
  `led_display_set_thresholds()` instead of being read as compile-time
  macros inside `led_display.c`'s zone/pulse-speed logic (which now takes
  them as parameters, refreshed once per render tick from a small
  critical-section-guarded state, same pattern as the reading itself).
  `flue_poll.c` fetches `GET /api/settings` once at boot and again whenever
  a `{"type":"settings_changed"}` broadcast arrives, so a threshold changed
  via the sidecar's own REST API actually moves the ring's gradient, not
  just its own classification. config.h's macros are now first-boot
  defaults only, used until the first successful fetch.
- **A blocking HTTP GET has no business running directly on the WS client's
  own event-callback context** -- `handle_ws_message()`'s `"settings_changed"`
  branch just gives a semaphore (`s_refetch_settings_sem`); a separate
  `settings_task` does the actual `fetch_and_apply_settings()` call. The
  reading path doesn't need this deferral, since
  `led_display_set_reading()` is just a quick critical-section write, safe
  to call directly from the event callback.
- **The sidecar resolves once at boot, not on every reconnect.** Unlike the
  old poller (which re-resolved via a fresh `mdns_query_a()` after any
  failed HTTP request), `flue_poll.c` now resolves `FLU_MONITOR_MDNS_NAME`
  once (retrying indefinitely if the sidecar simply hasn't booted yet -- a
  real race at power-on with no reason to give up) and hands
  `esp_websocket_client` a fixed `ws://<ip>/ws` URI; its own built-in
  auto-reconnect handles a transient WiFi/sidecar drop against that same
  address. A DHCP re-lease changing the sidecar's IP while this device
  stays connected to the same AP isn't specially handled -- rare enough on
  a home network that it wasn't worth the complexity of tearing down and
  recreating the WS client with a freshly-resolved URI, at least for now.
- **`FLU_MONITOR_MDNS_NAME` was repointed from the original ESPHome
  sidecar's `"flu-monitor"` to the ESP-IDF rewrite's `"flu-monitor-idf"`**
  for this Milestone -- the coordinated cutover the root `CLAUDE.md` had
  flagged as pending. That ESPHome sidecar has since been retired and
  removed entirely (no rollback path remains), and the ESP-IDF rewrite's own
  folder/mDNS hostname were later renamed from `flu-monitor-idf` back to
  the now-vacant `flu-monitor` -- `FLU_MONITOR_MDNS_NAME` was updated to
  `"flu-monitor"` again to match (see `config.h`), a source-only change not
  yet reflashed as of that rename.

## OTA updates (`ota_server.c`, `ota_flash.sh`, `partitions_ota.csv`)

- **Switching from a single-app to a two-OTA-slot partition table requires
  a one-time full USB reflash**, not something OTA itself can bootstrap --
  the partition table is a fixed on-flash layout, and the running device
  has no way to repartition itself out from under its own running image.
  After that one reflash, further updates go over WiFi via `ota_flash.sh`.
- **WiFi credentials (in NVS) survived the partition-table change
  unexpectedly** -- both the old single-app table and the new
  `partitions_ota.csv` happen to place `nvs` at the same offset (`0x9000`,
  since both list it first with the same declared size and `idf.py flash`
  only writes the regions it's told to), so the already-flashed NVS
  contents were left untouched. Not something to rely on in general
  (a differently-ordered or differently-sized partition table would not
  preserve this), but worth knowing this specific transition didn't require
  re-entering WiFi credentials via the captive portal.
- **The bootloader's rollback safety net is real and was verified by
  accident, not just in theory.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
  leaves a freshly-OTA'd image in a "pending verify" state until
  `esp_ota_mark_app_valid_cancel_rollback()` runs (here, once WiFi connects
  -- see `main.c`); forcing an extra reset immediately after a push, before
  that call had a chance to run, made the device correctly boot back into
  the *previous* slot instead of the new one, rather than getting stuck.
  Confirmed by reading the actual `otadata` sequence numbers with
  `$IDF_PATH/components/app_update/otatool.py ... read_otadata` and cross-
  checking against the boot log's "Loaded app from partition at offset"
  line -- don't trust which slot *should* be active from otadata sequence
  numbers alone without also checking what the bootloader actually printed,
  since an unconfirmed image's fate depends on timing (whether it reset
  again before confirming itself), not just what was last written.
- **The OTA endpoint's only auth is a shared secret in the URL query
  string** (`main/secrets.h`, git-ignored, mirroring `flu-monitor`'s
  `secrets.yaml` pattern -- copy `main/secrets.h.example` to start), sent
  over plain HTTP. Same trust level as `flu-monitor`'s own Google Sheets
  webhook secret: fine for a device that never leaves the home LAN, not
  something to expose beyond it.

- **A single glitched reading can slip through the wide sanity clamp and
  briefly flash a misleading color.** Observed live: an oven ramping
  smoothly around 150C, but the ring flashed red a couple of times before
  settling back -- one poll briefly reported a reading above the hot-zone
  threshold, self-correcting on the very next poll a few seconds later.
  The `-40..600C` sanity clamp can't be tightened to catch this: it has to
  stay wide since a real overfire could genuinely reach into that range, so
  a one-off glitch landing inside it sails straight through. Fixed by
  requiring confirmation: a reading that jumps more than `SUSPICIOUS_JUMP_C`
  in a single poll is held as "pending" rather than displayed immediately,
  and only accepted once the *next* poll agrees with it (within
  `CONFIRM_TOLERANCE_C`) -- a one-off glitch typically self-corrects by the
  next poll and never gets confirmed, while a real fast change still shows
  up within one extra ~3s poll interval. This is the same "don't trust a
  single sample" principle already written down for the sidecar's own
  future alert logic (see `../flu-monitor/CLAUDE.md`), just not previously
  applied to the display.
- **Rate-of-change is now polled from the sidecar's own "Thermocouple
  Rate" sensor, not derived locally from repeated polls.** The original
  local calc (a raw two-point delta between `flue_poll_once()` calls, real
  elapsed time via `esp_timer_get_time()`) had a real bug: the mcp9600's own
  `update_interval` is 30s, far slower than this device's 3s poll cadence,
  but every poll still updates the "previous reading" timestamp regardless
  of whether the underlying value actually changed -- so whenever it
  finally did change, the full 30s-worth of delta got divided by an
  assumed ~3s, making any real change look ~10x faster than it was.
  Observed live as a few spurious amber/blue trend-color flashes while
  sitting at an otherwise-stable reading (a `RATE_DEADBAND_C_PER_MIN`
  deadband was added first as a quick mitigation -- it's still in
  `led_display.c` and still useful as a perceptual floor, but the real fix
  was computing the rate off the sensor's own real update cadence instead,
  which only the sidecar can do). At the time this was fixed, `flue_poll.c`
  fetched `/sensor/Thermocouple%20Rate` (the ESPHome sidecar's REST
  endpoint) the same way it fetched the temperature; since Milestone 5
  (above), both values arrive together in the same WS broadcast payload
  (`thermocouple_c`/`thermocouple_rate_c_per_min`) -- the underlying fix
  (computing the rate off the sidecar's own real update cadence, not this
  device's poll cadence) is unchanged, only the transport is different now.
  Either way, this device has never kept its own previous-timestamp state
  for the rate calculation.
- **Induction cooktops are a uniquely bad environment for testing a bare,
  unshielded thermocouple** -- observed wild, fast swings (e.g. 73C to 164C
  within a couple of minutes) that don't look like real thermal behavior
  (a pan has real thermal mass; it can't swing 90C in under a minute) and
  don't match a smooth ramp when the same probe was moved to an oven
  instead. The likely cause: induction hobs drive a strong, rapidly-switching
  magnetic field to induce current in the pan, and a bare unshielded
  thermocouple wire near the coil picks up that field as noise on its own
  millivolt-level signal. This is a harsher EMI environment than the actual
  target (a wood stove has no oscillating magnetic field), so don't read
  too much into bad results from an induction-hob bench test specifically --
  an oven, hot water, or a heat gun are all more representative test
  sources.

- **RGB color choices for the gradient need to be judged live on the real
  ring, not from a camera photo or from first-principles RGB values.** Two
  separate incidents: (1) `COLOR_OPTIMAL` (named `COLOR_GOOD` at the time)
  was originally `{255, 120, 0}` --
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
  -- main.c`, run from this folder) once a tuning session is done, and
  rebuild the real firmware so the device goes back to actually polling
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
- **(Historical -- describes the ESPHome-era REST mechanism Milestone 5
  above replaced; kept for the URL-format detail, not as current
  behavior.)** The sidecar's `web_server` JSON API was keyed by the
  sensor's exact entity name, URL-encoded, not its `object_id`:
  `GET http://flu-monitor.local/sensor/Thermocouple%20Temperature` ->
  `{"id":"sensor/Thermocouple Temperature","value":25,"state":"25.0 °C"}`.
  `flue_poll.c` no longer calls this endpoint at all -- see "Switching to
  flu-monitor's WebSocket broadcast" above.
- **`flue_poll.c` reuses the sidecar's own defensive pattern**: a
  -40..600°C sanity clamp (same range and rationale as `flu-monitor.yaml`'s
  own clamp, see `../flu-monitor/CLAUDE.md`) before a reading is accepted --
  see "Switching to flu-monitor's WebSocket broadcast" above for how
  rate-of-change and this clamp both carried over into the WS-based
  version unchanged. A poll/parse failure (or, now, an implausible/held
  broadcast) just leaves the display state as-is rather than showing a
  stale or guessed value.
- **The onboard status LED (GPIO5) is wired active-low.** `status_led.c`
  drives it to reflect WiFi connection state (see `README.md`). The first
  version drove GPIO5 HIGH for "on," matching every other LED in both
  projects (the NeoPixel ring, `flu-monitor`'s onboard NeoPixel) --
  confirmed dark on real hardware instead of lit. No pinout reference found
  for this board documents the LED's polarity either way; the fix was
  simply inverting the output (`STATUS_LED_ACTIVE_LOW` in `status_led.c`)
  after observing it live, the same "judge on real hardware, don't assume"
  lesson as the RGB color-tuning notes below.
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
  session just needs `activate-idf.sh` sourced (not executed) in whatever
  command also needs `idf.py` -- Bash tool calls don't persist shell state
  between calls, so this has to happen in the *same* command as the
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
