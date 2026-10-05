# flu-display

Plain ESP-IDF firmware (not ESPHome/Arduino) for a **Lolin D32 Pro** driving a
64-LED Adafruit NeoPixel NeoMatrix 8x8 RGB (product 1487), showing the flue temperature reported by the
`flu-monitor` sidecar as a color/pulse gradient.

## Files

- `main/` — `main.c` (WiFi + LED wiring), `config.h` (all tunables),
  `flue_poll.c/.h` (subscribes to `flu-monitor`'s WebSocket broadcast and
  fetches its zone/rate settings over REST), `led_display.c/.h` (renders the
  matrix)
- `../components/wifi_setup/`, `../components/captive_portal/`,
  `../components/dns_server/` — WiFi credential storage (NVS) and the
  fallback setup access point, with a live test-connect-before-save flow;
  shared with `../flu-monitor/` (see the repo root `CLAUDE.md`'s "Shared
  components" section)
- `main/ota_server.c/.h` — authenticated `POST /ota` endpoint for pushing
  firmware updates over WiFi
- `activate-idf.sh` — sources the cached ESP-IDF toolchain (see `CLAUDE.md`
  for why it's reused rather than freshly installed)
- `ota_flash.sh` — pushes a build to a running device over WiFi
- `partitions_ota.csv` — two-OTA-slot partition table

## Setup

```sh
. ./activate-idf.sh && idf.py build                       # build
. ./activate-idf.sh && idf.py -p /dev/cu.usbserial-XXXX flash   # first flash (or after a fresh USB plug-in)
. ./activate-idf.sh && idf.py -p /dev/cu.usbserial-XXXX monitor # serial log
```

If the device has no stored WiFi credentials (or they fail to connect), it starts
a setup access point called **"Flu Display Setup"** (password `flu-display-setup`)
with a captive portal — connecting a phone/laptop to it should auto-pop a page to
enter your real WiFi credentials, which are then saved to NVS and it reboots to
join that network.

## Updating over WiFi (OTA)

Once a device is running (any build from after OTA support landed), further
updates don't need USB:

```sh
cp main/secrets.h.example main/secrets.h   # first time only -- fill in a real random string
. ./activate-idf.sh && idf.py build
./ota_flash.sh <device-ip-or-hostname>
```

`main/secrets.h` is git-ignored; the same secret has to be in it on every
machine that pushes updates. A device still running the old single-app
partition table needs one full USB reflash first (`idf.py flash`) to switch
partition layouts -- after that, OTA works going forward.

## Onboard LED: WiFi status at a glance

`status_led.c` blinks the board's onboard LED (separate from the 64-LED
matrix, which is dedicated to the temperature reading) to reflect the WiFi
connection lifecycle: slow blink while attempting to join stored
credentials, solid on once connected, fast blink while the setup access
point / captive portal is active. Since it joins almost instantly whenever
valid credentials are stored, the "connecting" blink is easy to miss —
it's most visible during AP mode or a slow/failing join. See `CLAUDE.md`
for the active-low wiring gotcha.

## Hardware notes

| Function              | Pin/value |
|-----------------------|-----------|
| LED matrix data (DIN)   | GPIO13 |
| LED matrix model      | Adafruit 1487, 64x RGB WS2812B/SK6812, GRB order |
| LED matrix power        | External 5V supply, **not** off the board's own 3.3V/USB rail |
| Common ground         | Matrix GND, ESP32 GND, and the 5V supply's GND all need to be tied together |
| Onboard status LED    | GPIO5, wired **active-low** (see `CLAUDE.md`) |

The matrix needs external 5V power sized for the intended brightness.
Using 60mA per pixel as a conservative full-white estimate gives 3.84A for
64 pixels; a regulated 5V 4A or larger supply covers this LED load. Keep a
common ground and connect GPIO13 to DIN (not DOUT). For reliable 5V operation,
use a 3.3V-to-5V logic buffer such as a 74AHCT125 on the data line. Follow
[Adafruit’s wiring guidance](https://learn.adafruit.com/adafruit-neopixel-uberguide/best-practices)
for the data resistor and supply capacitor.

All 64 pixels show the same breathing colour, so no row/column mapping is
needed. Zone colours, themes and pulse tuning remain unchanged. The neutral
no-data pulse uses equal RGB values instead of a separate warm-white channel.
This RGB firmware is incompatible with the previous RGBW ring.
