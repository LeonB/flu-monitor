# Project review

Review date: 2026-09-29

Both firmwares build successfully. The UI has a coherent design and large,
practical event buttons, but several reliability and data-integrity issues
should be fixed before sustained data collection.

## Scope and verification

- Reviewed both firmwares, shared components, Google Sheets integration, and
  the dashboard in Chrome using the mock server.
- Successfully built `flu-monitor` and `flu-display` with ESP-IDF 5.5.5.
- Exercised dashboard navigation, the history graph, event entry, and the
  settings screen in the browser.
- Reproduced the rapid-rise filter rejection, settings-save race, and
  zero-deadband fallback with focused JavaScript checks.
- Confirmed Alpine's automatic `init()` invocation in the vendored library.
- Hardware behavior and the production webhook were not tested. Findings
  about those paths are based on source inspection.
- No source files were changed during the review.

**Independent verification pass (2026-09-29):** all 10 numbered findings
below were re-traced against the current source by a second reviewer
(Claude, in this session) and confirmed real -- see the "Independently
verified" note under each one for the specific code path checked. The
follow-up verification below corrects some of that pass's wording and
severity claims. The UI/usability section and the missing-features
list below were not re-verified in this pass.

**Follow-up verification (Codex, 2026-09-29):** checked the added notes
against the source and queried the actual Xtensa compiler used by the
monitor build. Three corrections are incorporated under findings #2, #4,
and #5: rejected readings lead to a neutral display after the stale timeout;
upload failures have serial-log diagnostics; and plain `char` defaults to
unsigned on this toolchain, so the claimed negative-length DNS copy does
not apply to this build. The other added notes are consistent with the code.

## Highest-priority findings

### 1. WiFi does not recover after a disconnect

The disconnect handler only signals the initial connection attempt; it never
reconnects afterward. A router restart or temporary signal loss can leave
either device offline until reboot, with its status LED still indicating
connected.

Add reconnect attempts with backoff and update the status LED.

Source: [wifi_setup.c](components/wifi_setup/wifi_setup.c), line 34.

**Independently verified (2026-09-29):** confirmed, and worse than described --
there is no reconnect code anywhere in either project's `main.c`, not just an
incomplete handler (`grep` for `WIFI_EVENT_STA_DISCONNECTED`/`esp_wifi_connect`/
`reconnect` in both `main.c` files returns nothing). `status_led_set(STATUS_LED_CONNECTED)`
(`main.c:117`) fires exactly once, right after the initial connect, and is
never called again -- there is no code path that reacts to a later
disconnect at all. Affects both `flu-monitor` and `flu-display` identically,
since they share this exact `wifi_setup.c`.

**Fixed (2026-09-29):** added `wifi_sta_enable_auto_reconnect()` to
`wifi_setup.c` -- a background task that retries the STA connection with
exponential backoff (2s, doubling to a 60s cap) whenever the link drops
after the initial connect, driven by a second event group so it never
interferes with `wifi_sta_try_connect()`/`wifi_sta_test_connect()`'s own.
Takes an optional status callback; both `main.c`s now call it right after
their initial successful connect, wired to `status_led_set()` so the LED
correctly reflects "reconnecting" instead of staying stuck on "connected."
Both firmwares build clean.

### 2. The display rejects sustained rapid temperature rises

Jump confirmation requires consecutive readings to agree within 10°C. At the
30-second sampling cadence, a sustained 40°C/min rise produces readings such
as 100 → 120 → 140 → 160°C, which the filter keeps rejecting. This sequence
was reproduced against the filter logic. The display eventually becomes
stale during precisely the event it should highlight.

Revise the confirmation logic to admit sustained trends while still rejecting
isolated glitches.

Source: [flue_poll.c](flu-display/main/flue_poll.c), line 196.

**Independently verified (2026-09-29):** confirmed by tracing the exact logic
with the review's own example numbers (readings 100, 120, 140, 160 arriving
every 30s). `SUSPICIOUS_JUMP_C=15`, `CONFIRM_TOLERANCE_C=10`. Each new
reading is compared for confirmation against the *previous pending* value,
not the last *confirmed* one -- so on a steady climb, each newly-rejected
pending reading differs from the prior pending reading by the full per-tick
delta (20C here), which always exceeds the 10C tolerance. Confirmation can
therefore never succeed as long as the rate holds roughly steady. The last
confirmed reading remains cached, but the physical display switches to a
neutral pulse after more than 30 seconds without an accepted reading,
on the next staleness check (`STALE_READING_MS=30000`; `flu-display/main/main.c`,
lines 88-92). Temperature display resumes when a reading passes the filter,
for example when two consecutive pending readings land within 10C of each
other. The rapid-rise rejection remains a real bug; holding the last color
indefinitely was an inaccurate description.

**Fixed (2026-09-29):** replaced the magnitude-tolerance confirmation with a
direction-based one. A new suspicious jump is now confirmed once a
*second* consecutive jump beyond `SUSPICIOUS_JUMP_C` continues the *same
direction* (rising/falling) as the first, rather than needing to land
within `CONFIRM_TOLERANCE_C` of the held value -- a steady climb's
per-tick delta stays constant, so the old magnitude check could never
close the gap, while direction never has that problem. `CONFIRM_TOLERANCE_C`
is now unused and removed. Verified with a standalone host-side harness
against both the review's own example (100, 120, 140, 160, 180, 200 -- now
alternates HELD/ACCEPTED, advancing every other tick instead of freezing)
and the filter's original motivating case (a one-off glitch: 150, 250, 152
-- the glitch is still correctly held and discarded, since the
self-correcting 152 reading is a small delta from the last *confirmed*
150, not a same-direction continuation of the spike). `flu-display` builds
clean.

### 3. Event logging can silently lose or misalign annotations

The queue holds only four event names, and full-queue drops still result in
API success and a “Logged” toast. Temperature is captured when the worker
processes the event, while Sheets timestamps it on arrival. With documented
webhook delays of 40+ seconds, annotations can drift substantially from the
actual tap.

Queue the event's timestamp and sensor snapshot, and distinguish accepted,
delivered, and failed states. Event-triggered uploads also bypass the sensor
validity and plausibility checks used for periodic uploads.

Source: [sheets_logger.c](flu-monitor/main/sheets_logger.c), lines 87 and 101.

**Independently verified (2026-09-29):** confirmed on both counts.
`sheets_logger_log_event()`'s queue is `xQueueCreate(4, ...)` (line 155) with
a non-blocking `xQueueSend(..., 0)`; a full queue just logs an `ESP_LOGW`
nobody sees. `rest_api.c`'s `event_post_handler` (lines 440-444) calls it
without checking any result and unconditionally responds
`{"success":true}` -- a dropped event still shows "Logged" in the UI with no
way to tell. Separately, `sheets_logger_task` only calls
`sensors_get_last_reading()` when it actually dequeues an event, so a
backlog behind a slow webhook call (documented up to a 45s timeout) means
the temperature attached to a logged event can be from well after the
actual tap, not at it.

**Fixed (2026-09-29):** `sheets_logger_log_event()` now captures
`sensors_get_last_reading()` synchronously, at the moment of the call
(the actual tap, via `rest_api.c`'s handler), and carries that reading
through the queue in a small struct alongside the label -- the worker task
no longer re-reads the sensor whenever it happens to dequeue the item, so
a backlog no longer changes which reading gets attached. Also now returns
`false` on a full queue instead of silently dropping; `event_post_handler`
checks this and responds with a real HTTP error (`event_log_push()` --
the local record feeding the dashboard's own event sheet/graph -- still
always succeeds independently, so a Sheets-queue-full failure doesn't
lose local visibility, only the permanent Sheets row). The dashboard's
existing "Couldn't log ... -- check the connection" toast already handles
any non-2xx response here, so no UI change was needed. Builds clean.

### 4. Failed periodic uploads advance the logging baseline anyway

`log_to_sheets()` returns no result, and its caller updates the heartbeat
clock and last logged temperature even after connection failures or HTTP
errors. Missing rows therefore suppress subsequent attempts.

Add delivery tracking and an idempotent retry mechanism to handle ambiguous
timeouts without duplicate rows.

Source: [sheets_logger.c](flu-monitor/main/sheets_logger.c), line 148.

**Independently verified (2026-09-29):** confirmed -- `log_to_sheets()` is
`static void` and its caller (lines 148-150) updates
`s_last_logged_thermocouple_c`/`s_last_log_us` unconditionally right after
calling it, with no result to check. Worth noting this was a deliberate bet,
not an oversight: the function's own comment (lines 76-81) argues Apps
Script commits the row before responding, so a client-side failure "usually"
still means the row landed. The real risk is when that assumption is wrong,
and it compounds with finding #1 -- if WiFi drops and never reconnects,
every subsequent periodic attempt advances the baseline while actually
failing, so no new rows reach Sheets for the rest of the outage. Failures
are reported in the serial log (`ESP_LOGW` for transport errors and status
logging for completed HTTP requests), but the dashboard has no delivery
status. The earlier claim that nothing anywhere signals the failure was
inaccurate.

**Fixed (2026-09-29):** `log_to_sheets()` now returns whether the request
actually completed (`esp_http_client_perform() == ESP_OK`); the periodic
caller only advances `s_last_logged_thermocouple_c`/`s_last_log_us` on
success, leaving both untouched on failure so the very next tick retries
rather than silently absorbing the miss. Noted in a comment that this is
a deliberate bet on the *other* side of the original tradeoff: since Apps
Script commits a row before responding, a transport failure that actually
did land server-side could now produce an occasional duplicate row
instead of a silent gap -- judged the safer failure mode for data
gathering (duplicates are easy to filter out later; gaps that look
identical to "nothing happened" are not). A full idempotent
accepted/delivered/failed tracking system, as the original finding
suggested, would need changes on the separate Google Apps Script side too
and was out of scope for this pass. Builds clean.

### 5. The captive DNS parser lacks packet bounds checks

The parser walks labels without checking the remaining packet length and
does not explicitly handle DNS compression pointers. It can interpret
bytes beyond the received packet as question data. The originally claimed
signed-`char` oversized-copy mechanism does not apply to the current build.

Implement bounded parsing and explicit handling or rejection of compressed
names before relying on the setup portal's robustness.

Source: [dns_server.c](components/dns_server/dns_server.c), line 78.

**Verification corrected (Codex, 2026-09-29):** queried the
`xtensa-esp32-elf-gcc` compiler identified in the build's
`compile_commands.json` with `-dM -E -x c -`. It defines
`__CHAR_UNSIGNED__`, and the build commands contain no signedness override.
Plain `char` therefore defaults to unsigned: a compression-pointer byte
does not become a negative length, and the previously claimed massive
`memcpy()` does not follow on this build.

The parser also operates on the zero-initialized **256-byte reply buffer**,
which contains a copy of the received packet, rather than directly on the
128-byte receive buffer. The assertion that an unterminated name necessarily
walks off the receive buffer was incorrect. Missing validation against the
actual packet length remains real: neither label parsing nor reading the
question's type/class verifies that the required bytes were received.
The original severity claim should not be treated as a demonstrated
memory-corruption exploit on this target. This is vendored Espressif example
code (per `flu-display/CLAUDE.md`), but it is the implementation running here.

**Fixed (2026-09-29), with a correction to my own earlier claim above:** I
initially described this as a negative-length-becomes-huge-`memcpy`
exploit, reasoning from `char`'s signedness being platform-defined without
actually checking this specific toolchain -- an overstated severity claim,
caught by the follow-up pass above and independently reconfirmed by me via
disassembly (`xtensa-esp32-elf-gcc -S`: reading `*label` compiles to
`l8ui`, an unsigned/zero-extending byte load, not a sign-extending one --
`0xC0` becomes `+192`, not `-64`, on this actual target). The negative-
length mechanism does not apply here.

What's still real and is what the fix actually addresses: `parse_dns_name()`
took a `packet_end` parameter it uses to bound every read against the
*actually received* length (the previous version had no such check at all,
against either buffer), reads the length byte as `uint8_t` regardless of
platform default signedness (defensive correctness, not fixing a live bug
on this specific toolchain), and explicitly rejects a compression-pointer
byte (0xC0+) rather than attempting to interpret it as a length --
compression is never needed for a captive portal's simple single-question
queries, so rejecting it costs nothing real. Also added a bounds check
before reading the question's type/class fields, which neither the
original code nor my first pass had covered. Verified with a standalone
host-side harness (compiled and run separately from the firmware): a
legitimate query name (`captive.apple.com`) still parses identically to
before, a label claiming to extend past the received packet is now
rejected (`NULL`) instead of read out-of-bounds, and a compression-pointer
byte is now rejected outright. Both `flu-monitor` and `flu-display` build
clean (this file is a shared component).

## Other functionality and code issues

### 6. Dashboard initialization runs twice

Alpine automatically invokes an `init()` method, and the page also invokes it
through `x-init`. The vendored Alpine code confirms this behavior. This
creates duplicate fetches, polling intervals, and animation loops.

Remove the explicit invocation.

Source: [dashboard.html](flu-monitor/main/web_ui/dashboard.html), line 89.

**Independently verified (2026-09-29):** confirmed -- `<body x-data="app()"
x-init="init()">`. Alpine auto-invokes an `init()` method found on an
`x-data` object on its own, so the explicit `x-init="init()"` runs it a
second time: two parallel 5s-polling `setInterval`s and two recursive
`requestAnimationFrame` loops, both mutating the same `cyclePos`/
`lastFrameMs` fields on the shared component instance. Likely source of any
glow-pulse jitter, plus doubled API traffic. One-line fix (drop
`x-init="init()"`).

### 7. Invalid readings can appear valid in the dashboard

An implausible reading such as 891°C retains `thermocouple_ok=true`, alongside
the previous zone and rate. History and periodic logging reject it, but the
UI displays it.

Expose validity consistently across consumers. Successful HTTP requests also
do not establish sensor freshness; the API needs a sample timestamp or age.

Source: [sensors.c](flu-monitor/main/sensors.c), line 327.

**Independently verified (2026-09-29):** confirmed -- `sensors_read()` sets
`out->thermocouple_ok = read_mcp9601(...)` (a successful I2C transaction)
independently of `thermocouple_reading_plausible()`, then stores the whole
struct into `s_last_reading` regardless of plausibility. The plausibility
gate only protects `thermocouple_update_regression()`'s admission into the
rate/zone history. So `GET /api/reading` can report
`{"thermocouple_ok":true,"thermocouple_c":891.0,...}` with the *previous*
zone/rate attached, and the dashboard has no separate client-side
plausibility check -- it shows exactly what the API sends.

### 8. The display can retain obsolete settings or an obsolete IP indefinitely

The display resolves the sidecar only once, and settings fetch failures have
no retry. Settings changed while disconnected are not fetched on
reconnection.

Re-resolve after repeated failures and fetch settings on every WebSocket
connection.

Source: [flue_poll.c](flu-display/main/flue_poll.c), line 289.

**Independently verified (2026-09-29):** confirmed -- `ws_event_handler`'s
`WEBSOCKET_EVENT_CONNECTED` case only logs; nothing refetches settings on
reconnection, so a change missed while offline (the `"settings_changed"`
broadcast that would have triggered it never arrives after the fact) stays
missed until the next actual settings change. The "resolves once, obsolete
IP" half is a documented, intentional tradeoff rather than an overlooked
bug -- `flu-display/CLAUDE.md`'s Milestone 5 notes explicitly call out that
a DHCP re-lease changing the sidecar's IP isn't specially handled, judged
rare enough on a home network not to be worth the complexity.

### 9. Saving settings has a reproducible race

If a user submits 155°C and changes the draft to 160°C while saving, the
request contains 155°C, but the UI records 160°C as saved. This was reproduced
using a delayed response.

Snapshot the submitted draft, disable duplicate saves, and reconcile with the
server response.

Source: [dashboard.js](flu-monitor/main/web_ui/dashboard.js), line 227.

**Independently verified (2026-09-29):** confirmed. `saveSettings()` does
correctly snapshot the request body (`JSON.stringify(this.draft)` runs
synchronously before the `await`), so the wire request itself isn't
affected. The bug is after the response arrives: `this.settings =
{...this.draft}` (line 236) reads `this.draft` again, live -- if the user
edited a stepper while the request was in flight, `this.settings` ends up
holding that newer, never-sent edit, while the device's NVS still has the
older value that was actually transmitted. The Save button also has no
`:disabled="saving"` guard against duplicate concurrent taps.

### 10. Zero-valued settings are incorrectly replaced by defaults

Setting the rate deadband to zero is allowed, but JavaScript's `||`
substitutes 3°C/min. A focused check verified that a 2°C/min rate then
displays “Holding steady.”

Use nullish fallback for valid zero values.

Source: [dashboard.js](flu-monitor/main/web_ui/dashboard.js), line 118.

**Independently verified (2026-09-29):** confirmed --
`this.settings.rate_deadband_c_per_min || DEFAULT_RATE_DEADBAND_C_PER_MIN`
treats an explicitly-set `0` as falsy and silently substitutes 3.0. The same
`||`-vs-legitimate-zero pattern recurs for `idle_pulse_period_ms`/
`fast_pulse_period_ms`/`color_transition_exponent` in `runGlowFrame()` and
for the zone thresholds in `graphSvg()` -- worth a sweep to nullish
coalescing (`??`) throughout, not just this one call site, though zero isn't
a meaningful value for most of those other fields in practice.

## UI and usability

### History freezes while the graph is open

Only the live endpoint keeps updating; historical ages and event positions
remain fixed, making the curve increasingly misleading. Refresh history
periodically and show gaps during outages.

Source: [dashboard.js](flu-monitor/main/web_ui/dashboard.js), line 196.

### Graph labels extend outside the chart

This was observed in Chrome. Dense events will also overlap. The chart needs
constrained labels or tappable markers, intermediate time ticks, and
temperature inspection.

Source: [dashboard.js](flu-monitor/main/web_ui/dashboard.js), `graphSvg`.

### The mock hides production differences

The mock accepts invalid settings without firmware validation and returns
friendly event labels, while firmware returns slugs such as `added_wood`.
This makes browser testing look better than the actual device.

Sources: [mock_server.py](flu-monitor/tools/mock_server.py) and
[rest_api.c](flu-monitor/main/rest_api.c), line 488.

### Settings and failed-load flows need clearer feedback

Settings lack field-level validation, bounds on steppers, and an
unsaved-changes guard. Failed initial settings/events requests have no retry,
leaving unusable screens.

Sources: [dashboard.js](flu-monitor/main/web_ui/dashboard.js) and
[dashboard.html](flu-monitor/main/web_ui/dashboard.html).

### Accessibility needs attention

The event sheet lacks dialog semantics, focus trapping, Escape dismissal,
and a close button. Stepper buttons are named only “+” or “−”. Animation has
no reduced-motion option.

Sources: [dashboard.html](flu-monitor/main/web_ui/dashboard.html) and
[dashboard.js](flu-monitor/main/web_ui/dashboard.js).

## Missing features worth prioritizing

For the current data-gathering phase:

1. Logging health: last successful upload, pending events, failures, and a
   test-webhook action.
2. Durable event storage, original timestamps, export, and correction of
   accidental annotations.
3. Device diagnostics: sensor fault, sample age, WiFi signal, uptime,
   firmware version, and display connection.
4. Recovery controls for changing WiFi and retrying sensor initialization.
5. Automated regression checks for filtering, settings validation, queue
   overflow, reconnection, and UI error states.

Phone notifications are explicitly planned but absent. Calibrated
classification is intentionally deferred. Fix recovery and logging integrity
first, because those directly affect the quality of the data used for that
later work.

## Review of the fix commits (Codex, 2026-09-29)

Reviewed the six commits `d99d9d4` through `bed0d93`, covering the five
highest-priority fixes and the review document. Both firmwares build
successfully with ESP-IDF 5.5.5, and `git diff --check 4748ca3..HEAD` passed.
The DNS bounds-check changes look reasonable. These commits were not tested
on hardware. No source files were changed during this review.

The following issues remain in the fixes; the earlier “Fixed” notes should
be read alongside these findings.

### C1. [P1] Reconnect can stop while still disconnected

Commit: `d99d9d4`.

If a delayed connection succeeds and then drops during backoff, both
`STA_CONNECTED_BIT` and `STA_FAILED_BIT` remain set. The task treats that
combination as success because it checks only the connected bit, clears
both bits, and returns to waiting indefinitely for another disconnect.
The link is already down, and its disconnect notification has been consumed.

Reproduced using the actual `reconnect_task()` body in a host-side harness
with simulated event-group and WiFi operations: the status callback reported
connected while the simulated link was disconnected, then the task returned
to its indefinite outer wait.

Track current connection state rather than treating accumulated event bits
as authoritative, and ensure that a subsequent disconnect cannot be erased
by handling an earlier successful connection.

Source: [wifi_setup.c](components/wifi_setup/wifi_setup.c), line 90.

**Fixed (2026-09-29):** replaced the bit-based check with a direct query of
`esp_wifi_sta_get_ap_info()` after the wait, which reports the driver's
actual current association state regardless of which bits fired or in what
order -- the coalescing this finding describes can no longer produce a
false "connected" report, since the fix no longer looks at the bits at all
for that decision. Builds clean.

### C2. [P1] HTTP failures still advance the logging baseline

Commit: `bd57cd0`.

`err == ESP_OK` means the HTTP exchange completed; responses such as 404 or
500 still count as success in `log_to_sheets()`. A wrong secret also returns
HTTP 200 with `"forbidden"` from the current Apps Script. The periodic caller
therefore still advances its baseline for requests that did not write a row.

Verified by source inspection of the success condition, the baseline update,
and the Apps Script authentication response. Check both the HTTP status and
the webhook acknowledgement before advancing the baseline.

Sources: [sheets_logger.c](flu-monitor/main/sheets_logger.c), line 91, and
[Code.gs](flu-monitor/google-sheets-logger/Code.gs), `doGet()`.

**Fixed (2026-09-29):** confirmed by reading `Code.gs` directly -- a bad
secret does return plain HTTP 200 with body `"forbidden"`. Added an
`HTTP_EVENT_ON_DATA` handler to capture the response body and now require
status 200 AND body exactly `"ok"` (Code.gs's own success contract) before
treating an upload as successful. Builds clean.

### C3. [P2] Event timestamps still drift

Commit: `6d94b1`.

The queue now captures temperature at enqueue time, but carries no event
timestamp. Apps Script still uses `new Date()` when processing the request,
so delayed annotations associate the captured temperature with the wrong
time. Original finding #3 is only partially fixed.

Verified by source inspection of `queued_event_t`, the request query fields,
and the Apps Script row timestamp. Capture the event time, send it with the
snapshot, and preserve it in Sheets independently of delivery time.

Sources: [sheets_logger.c](flu-monitor/main/sheets_logger.c), line 36, and
[Code.gs](flu-monitor/google-sheets-logger/Code.gs), line 69.

**Fixed (2026-09-29):** confirmed by reading `Code.gs` directly -- `new
Date()` with no timestamp param sent at all. `sheets_logger_log_event()`
now records when it was called; `log_to_sheets()` computes how old the
event was by the time its request actually fires and sends it as `age_ms`;
`Code.gs` backdates the row's timestamp by that amount instead of using
its own processing time. The periodic path sends `age_ms=0` (no meaningful
delay there). Note: `Code.gs` needs a manual redeploy against the live
webhook to take effect -- it's a template pasted into the Apps Script
editor, not part of the firmware build.

### C4. [P2] “Same direction” compares against the wrong reference

Commit: `3c50aac`.

For `150 → 250 → 170°C`, the new filter accepts 170 as confirmation of a
rising trend because both pending and current readings are above the last
confirmed 150. The temperature actually reversed sharply from the pending
250. This was reproduced using the actual filter block in a compiled
host-side harness.

The filter also accepts every other sample during a sustained rise with
per-tick jumps above 15°C. At the 30-second sensor cadence, accepted readings
are approximately 60 seconds apart, exceeding `STALE_READING_MS=30000`.
The display consequently switches to its neutral pulse between accepted
readings. This timing interaction was verified by source inspection.

Retain enough information about the pending reading to distinguish a
continued trend from a reversal, and coordinate confirmation delay with the
staleness policy so a healthy stream does not repeatedly appear unavailable.

Sources: [flue_poll.c](flu-display/main/flue_poll.c), line 205,
[config.h](flu-display/main/config.h), `STALE_READING_MS`, and
[main.c](flu-display/main/main.c), the staleness-check loop.

**Fixed (2026-09-29):** confirmed both parts by tracing the actual code.
The reversal false-positive: now stores the pending reading's actual value
(not just its direction) and confirms only when the new reading continues
*further* past the pending value in the same direction, relative to the
pending value itself -- not just "still displaced from the confirmed
baseline the same broad way." Verified with a standalone host-side
harness: the exact 150/250/170 counter-example now correctly holds
instead of confirming, a genuine continued rise after that same false
pending value still confirms correctly, and finding #2's original two
cases (sustained rise, one-off glitch) are unaffected.

The staleness interaction: `s_last_valid_us` now updates on any plausible
broadcast, including one held pending, not just on the accept path --
staleness reflects "is the sidecar still sending data," not "have we
accepted a new displayed value recently," decoupling it from the
confirmation filter's own admit rate. Both fixes build clean.

### C5. [P2] Retrying a queue-full event duplicates local annotations

Commit: `6d94b1`.

The API records the event locally before checking queue capacity, then
returns an error telling the user to retry. Each retry adds another local
marker, even when the Sheets queue remains full. The dashboard's generic
failure toast does not explain that the local annotation already succeeded.

Verified by source inspection of `event_post_handler()` and the dashboard's
error handling. Queue before recording locally, or return a partial-success
state that the UI handles explicitly without duplicating the local event.

Sources: [rest_api.c](flu-monitor/main/rest_api.c), line 443, and
[dashboard.js](flu-monitor/main/web_ui/dashboard.js), `logEvent()`.

**Fixed (2026-09-29):** always responds 200 now (the tap did succeed) with
a `sheets_queued` flag distinguishing whether the Sheets-delivery queue
had room, instead of an HTTP error implying a retry that would duplicate
the already-successful local record. `dashboard.js` shows a distinct,
non-alarming toast ("Sheets catching up") when the flag is false. Verified
live against the mock server (which sends no such field at all): falls
back to the normal "Logged: X" toast correctly. Builds clean.

## Review of the follow-up fix commits (Codex, 2026-09-29)

Reviewed `dc19007` through `da6a296`. Both firmware builds passed, and
`git diff --check bed0d93..HEAD` was clean. The Wi-Fi change addresses the
previously reported coalesced-event case. These findings qualify the
preceding “Fixed” notes. Verification used source inspection and host-side
harnesses containing the actual changed code; no hardware or deployed
webhook testing was performed.

### D1. [P1] Stable temperatures after a jump can freeze the display

Commits: `7d2d664`, `e4af4d9`.

Confirmation requires moving strictly beyond the pending reading. For
`150 → 170 → 170 → 170°C`, every 170°C sample is held pending because
equality never confirms the new plateau. The display remains at 150°C
indefinitely. Updating `s_last_valid_us` before confirmation also keeps
the old display marked fresh as long as plausible broadcasts continue.

Reproduced using the actual filter block in a compiled host-side harness:
only 150°C was accepted, with 170°C still pending after repeated samples.
Confirm consistent readings at a new level as well as continued trends,
while retaining reversal protection. Track stream freshness and accepted
reading age separately, or bound how long the old display can remain valid.

Source: [flue_poll.c](flu-display/main/flue_poll.c), lines 212 and 226–234.

**Fixed (2026-09-30):** confirmed by tracing the exact example (150, 170,
170, 170) through the actual code. Changed the strict `>`/`<` comparisons
to `>=`/`<=`, so a reading matching (not just exceeding) the pending value
also confirms. Verified with a standalone host-side harness: the plateau
case now confirms on the second sample, and C4's reversal counter-example,
the original sustained-rise example, the one-off-glitch self-correction,
and a genuine continued rise after a false pending value are all
unaffected. Builds clean.

### D2. [P1] Valid webhook acknowledgements can be rejected

Commit: `283bf39`.

The response handler ignores chunked bodies even though ESP-IDF delivers
their decoded body data through `HTTP_EVENT_ON_DATA`. A successful chunked
`200 "ok"` therefore captures zero bytes and is treated as failure.
The buffer is also reset only before the whole exchange, rather than
between redirect responses. Intermediate non-chunked redirect bodies can
be concatenated with the final `ok`, failing the exact two-byte check.
The Apps Script integration explicitly uses redirects.

Both cases were reproduced with the actual callback in a host-side harness:
a chunked `ok` captured zero bytes; a redirect body followed by `ok`
captured 15 bytes rather than two. The installed ESP-IDF implementation
dispatches body callbacks for these responses. False failure leaves the
periodic baseline unchanged, allowing repeated uploads and duplicate rows
despite the earlier successful write.

Capture decoded data regardless of transfer encoding, isolate the body
for each response, and validate only the final response's status and
acknowledgement. Verify chunked acknowledgements and redirects with bodies.

Source: [sheets_logger.c](flu-monitor/main/sheets_logger.c), lines 42–53,
and the final acknowledgement check in `log_to_sheets()`.

**Fixed (2026-09-30):** confirmed both mechanisms by reading
`esp_http_client.c` directly -- `http_on_body()` unconditionally dispatches
`HTTP_EVENT_ON_DATA` with already-de-chunked bytes regardless of
Transfer-Encoding (the chunked exclusion was simply wrong, not a
safety measure), and `HTTP_EVENT_ON_CONNECTED` does refire for the
redirect hop since Apps Script's target is cross-host. No longer excludes
chunked data, and resets the response buffer on `HTTP_EVENT_ON_CONNECTED`
(per hop) instead of once before the whole exchange. Verified with a
standalone host-side harness reproducing both exact scenarios (chunked
`"ok"`, and a redirect-hop body followed by the real `"ok"`) -- both now
correctly recognized, `"forbidden"` still correctly rejected. Also fixed
the identical mistake in `flue_poll.c`'s own copy of this pattern (used
for `/api/settings`), found while investigating this finding. Both
firmwares build clean.

### D3. [P2] Queue-full feedback implies delivery of a dropped event

Commit: `368cba8`.

The partial-success response avoids the earlier duplicate-local-event retry
prompt, but “Sheets catching up” is misleading when `sheets_queued:false`.
That event was discarded from Sheets delivery after `xQueueSend()` failed.
There is no later delivery mechanism; draining the queue delivers other
events, not this one. The toast and comments imply eventual delivery.

Verified by tracing the queue-full return through the API and dashboard.
Say “Saved locally; not sent to Sheets,” or retain the event for later
delivery before claiming that Sheets is catching up.

Sources: [dashboard.js](flu-monitor/main/web_ui/dashboard.js), lines
180–188; [rest_api.c](flu-monitor/main/rest_api.c), `event_post_handler()`;
and [sheets_logger.c](flu-monitor/main/sheets_logger.c),
`sheets_logger_log_event()`.

**Fixed (2026-09-30):** confirmed -- nothing anywhere retains a dropped
event for later delivery. Toast wording changed to "not sent to Sheets,"
accurate about what actually happened to that specific tap, while still
not implying a retry (which would duplicate the local record, per C5).

### D4. [P2] Event timestamps still include current-request latency

Commit: `10b78b5`.

The device calculates `age_ms` before sending the request, while Apps Script
subtracts it from processing time. Queue delay is corrected, but the
current request's connection, network, and processing delay before that
calculation remains in the recorded timestamp.

For an event at time 0, a 10-second queue wait followed by a 20-second
request delay sends age 10 seconds. Apps Script runs at time 30 and records
time 20 instead of time 0. The project documents webhook exchanges lasting
tens of seconds; the portion before timestamp calculation is uncorrected.
This follows from source inspection, not a deployed-webhook measurement.

Send an absolute event timestamp from a synchronized device clock. If
relative age remains a fallback, document its uncertainty. Periodic uploads
currently send age zero and retain processing-time timestamps as well.

Sources: [sheets_logger.c](flu-monitor/main/sheets_logger.c), lines 191–192,
and [Code.gs](flu-monitor/google-sheets-logger/Code.gs), lines 77–78.

**Fixed (2026-09-30), per explicit user decision to take the full fix over
the documented-limitation option:** `main.c` now starts SNTP (fire-and-
forget, nothing blocks on it) once WiFi connects. `sheets_logger.c` checks
whether the clock has actually synced (`time(NULL)` past a sanity cutoff,
since this chip has no battery-backed RTC) before trusting it, and when
synced, captures an absolute Unix timestamp at the moment of the tap/
reading and sends it as `event_ts` -- immune to the request's own latency,
however long it takes. Falls back to the existing `age_ms`-relative scheme
when the clock hasn't synced yet; `Code.gs` prefers `event_ts`, then
`age_ms`, then `new Date()` (three-tier fallback, verified with a
standalone test). Needs a manual `Code.gs` redeploy to take effect on the
live webhook, same as finding #3/C3. Both firmwares build clean.
