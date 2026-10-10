# Native C PageKite desktop prototype

An independently written C client for the PageKite CONNECT authentication
and multiplexed chunk/SID protocol. It uses OpenSSL on the desktop, not the
PageKite Python/MicroPython client. The Python runner provides a synthetic
HTTP backend and verifies public requests. No monitor APIs are exposed.

## Run

Requires a C compiler, make, pkg-config, OpenSSL and Python 3.

```sh
make -C tools/pagekite-prototype
python3 tools/pagekite-prototype/credentials.py --domain YOUR-KITE.pagekite.me
PAGEKITE_SECRET_FILE=/tmp/flu-pagekite-secret.json python3 tools/pagekite-prototype/run.py
rm /tmp/flu-pagekite-secret.json
```

The credential helper prompts for the account email/password and obtains the
shared tunnel secret over verified HTTPS. It stores only the domain and tunnel
secret, with owner-only permissions, outside the repository. It refuses to
overwrite an existing file. Alternatively, privately set PAGEKITE_DOMAIN and
PAGEKITE_SECRET. Never commit credentials or put them in command arguments.

The runner starts a loopback-only HTTP server on port 18776, connects the
native client to PageKite's IPv4 relay pool on port 443 and updates this kite's
DNS routing over verified HTTPS. Set PAGEKITE_RELAY to a known healthy relay
address to reuse it between runs instead of selecting another pool address.
That routing remains pointed at the selected
relay after the temporary tunnel closes. It does not create/delete kites,
modify other kite names, or change account settings.

Tests cover exact binary response bytes, 128 KiB binary/gzip downloads, a
32 KiB POST echo, eight requests across four concurrent clients and an idle
interval (PAGEKITE_IDLE_CHECKS controls the number of 35-second checks). Direct-relay HTTPS requests retain the public hostname for SNI and
Host, avoiding DNS propagation in those protocol checks. A separate final
request tests ordinary public DNS resolution. Failures stop the client and
backend; the native client also has a three-minute runtime limit.

## Verification: 2026-10-10

The C client authenticated with the hosted service using the existing trial
kite. Verified TLS uses the relay's documented identity `frontends.b5p.us`,
not the DNS pool label. Public HTTPS also verifies the kite hostname.
The initially assigned relay presented an expired certificate; another relay
in the pool verified correctly. No certificate verification was disabled.
The runner moved only the test kite's routing to the verified relay.

The final source passed all tests: exact binary response, 128 KiB binary and
gzip transfers, 32 KiB POST echo, eight requests across four concurrent
clients, four 35-second idle checks (140 seconds total), and ordinary public
hostname resolution over HTTPS. Earlier ordinary-hostname checks hit cached
relay addresses; keeping the final relay fixed allowed the cache to expire.
The temporary tunnel/backend were closed and the temporary credential file
was removed. Firmware was not changed or flashed for this experiment. This is protocol feasibility,
not an ESP32 integration or evidence of long-term SaaS reliability.

## Limits before an ESP-IDF port

- No automatic reconnect or relay failover; the current client tries one
  successfully connected pool address and fails if its TLS verification fails.
- Fixed eight-stream table and 64 KiB input-chunk limit; allocations and socket
  buffering need reduction/measurement on ESP32.
- Slow-peer backpressure, persistent HTTP connections, half-close behavior,
  arbitrary fragmentation and long-running stability are not comprehensively tested.
- Handles PageKite PING with PONG; an embedded port needs heartbeat timeout
  enforcement and recovery after network loss.
- No visitor authentication, custom-domain certificate test or OTA forwarding.
- An ESP-IDF implementation would replace OpenSSL with ESP-TLS/mbedTLS and
  run a bounded FreeRTOS task alongside the existing HTTP server.

## Wire/trust references

- https://github.com/pagekite/upagekite/blob/main/upagekite/proto.py
  (CONNECT, challenge signing, chunk framing, SID data/EOF and PING/PONG)
- https://github.com/pagekite/PyPagekite/blob/main/pagekite/common.py
  (documented relay identities and account service)
- https://github.com/pagekite/PyPagekite/blob/main/pagekite/pk.py
  (account shared-secret retrieval)

## ESP32 dashboard

`main/pagekite_tunnel.c` runs PageKite directly on ESP-IDF using ESP-TLS,
mbedTLS and the existing certificate bundle. It waits for SNTP before TLS
validation and destroys TLS/backend state before reconnecting with a 2–60
second retry delay. Relay identity is `frontends.b5p.us`; the current fixed
relay is `176.58.121.52`. Public URL:
https://flumonitor-vanutsteen.pagekite.me/

The tunnel forwards an explicit allowlist of dashboard assets and GETs for
readings, history, event labels and device signal strength. `/api/settings`
maps to `/api/tunnel/settings`, which omits the Sheets secret and webhook URL
and marks the response `credentials_redacted`. Settings and event logging
are enabled remotely: JSON POST /api/settings maps to POST
/api/tunnel/settings, which retains stored Sheets credentials when omitted
and accepts explicit replacements. POST /api/event records a normal event;
GET /api/settings/status supports display acknowledgement after saves.
OTA, WiFi configuration, WebSocket upgrades and unrelated paths remain
unavailable through the dashboard tunnel. Visitor authentication is deferred.

Memory/socket bounds: eight streams with up to 4 KiB combined request headers/body,
two active loopback HTTP connections, a 4 KiB frame buffer, a 12 KiB task and
one outbound TLS socket. Frames up to 64 KiB are consumed in bounded pieces;
responses stream in 2 KiB pieces. Request buffers are allocated only while collecting/queueing requests
and freed immediately after forwarding, saving about 16 KiB at idle. Visitor
headers are parsed and rebuilt, preserving Accept-Encoding and If-None-Match
and using explicit lengths and application/json for permitted POSTs. Bodies
must be shorter than 2048 bytes, matching the settings handler. Request buffers
grow in 512-byte increments instead of reserving the maximum for every request.
Each local connection carries one complete GET or JSON POST and is half-closed for writing
after transmission: ESP-IDF HTTPD ignores Connection: close alone, so relying
on that header caused forwarding slots to remain occupied. Inactive streams
expire after 30 seconds. Slow peers can still stall the tunnel task within
its socket timeout; long-term soak, relay failover and forced WiFi loss remain
untested.

The monitor's Settings → PageKite form now configures the enabled flag,
kite domain, relay and shared secret in NVS. No private header or build-time
credential is needed. The shared secret is write-only: GET returns
`pagekite_secret_set`, and blank/omitted secret values preserve it on POST.
Changing tunnel settings applies after the save response, without a reboot;
disabling closes the sockets and retains the configuration. Previous NVS
settings migrate without losing values. The default relay's routing was
established during the desktop test; automatic DDNS updates and relay
selection are not implemented by the firmware.

For live configuration tests, use the credential helper above to create an
owner-only temporary JSON, then run
`python3 tools/test_pagekite_configuration.py 192.168.1.137 /tmp/flu-pagekite-secret.json`.
It configures the existing kite, tests omitted/blank secret retention, remote
disable/LAN re-enable and temporary secret replacement, and restores the
valid configured tunnel in finally. Remove the temporary credential file.
`python3 tools/test_pagekite_settings.py` verifies migration, validation and
REST behavior without device changes (requires an existing ESP-IDF build).

Hardware verification, 2026-10-10: OTA-flashed the identified monitor at
192.168.1.137. Chrome loaded the public live dashboard and burn-history view.
All eleven assets passed exact gzip/binary content, identity fallback, ETag
304 and stale-tag checks through HTTPS. Live readings/history/events/WiFi,
18 asset requests across six concurrent clients, redacted settings and dashboard POST validation passed. The display link remained connected.
One public TCP connection timed out; the full asset check passed on rerun.
The editable version also passed a temporary monitor-only save and a
display-setting save/restoration through HTTPS, with matching display
acknowledgements and stored credentials unchanged. Event transport was tested
with an invalid slug so no fake event was added to the burn log. Firmware size
is 0x1395b0, with 0x3da50 bytes (16%) free in the application slot. Request
buffers were then made temporary after a mixed-traffic heap minimum of 9260
bytes was observed; hardware checks were repeated on that final version.

Offline verification: `python3 tools/test_pagekite.py` compiles the actual
firmware frame/routing/queue functions under AddressSanitizer/UBSan and checks
fragmented JSON bodies, half-close, oversized-frame draining, duplicate
lengths/truncated bodies, header injection/pipelining, settings/event routing,
buffer release and the two-backend limit.
`node tools/test_settings_sync.js` covers settings save/acknowledgement behavior.
Live checks:

```
python3 tools/test_pagekite_dashboard.py https://flumonitor-vanutsteen.pagekite.me
python3 tools/test_web_assets.py https://flumonitor-vanutsteen.pagekite.me
```

Final editable-firmware check: free heap 106548 bytes; minimum 28232 bytes; reconnects 0. Temporary settings changes were restored and both device links remained healthy.
