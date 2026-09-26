# flu-display

Plain ESP-IDF firmware (not ESPHome/Arduino) for a **Lolin D32 Pro** driving a
24-LED SK6812 RGBW NeoPixel ring, showing the flue temperature reported by the
`flu-monitor` sidecar as a color/pulse gradient.

## Files

- `main/` — `main.c` (WiFi + polling + LED wiring), `config.h` (all tunables),
  `flue_poll.c/.h` (polls `flu-monitor`'s JSON API), `led_display.c/.h`
  (renders the ring)
- `components/wifi_setup/`, `components/captive_portal/`, `components/dns_server/`
  — WiFi credential storage (NVS) and the fallback setup access point
- `activate-idf.sh` — sources the cached ESP-IDF toolchain (see `CLAUDE.md`
  for why it's reused rather than freshly installed)

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

## Hardware notes

| Function              | Pin/value |
|-----------------------|-----------|
| LED ring data (DIN)   | GPIO13 |
| LED ring model        | 24x SK6812 RGBW |
| LED ring power        | External 5V supply, **not** off the board's own 3.3V/USB rail |
| Common ground         | Ring GND, ESP32 GND, and the 5V supply's GND all need to be tied together |
