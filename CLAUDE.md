# flu-monitor

ESPHome firmware for an Adafruit ESP32 Feather V2 with a BMP581 (pressure/temperature)
and an MCP9601 (K-type thermocouple amp) over STEMMA QT/I2C. See `README.md` for
day-to-day flash/log commands and the hardware pinout table.

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

## Other notes

- `secrets.yaml` and `.esphome/` are git-ignored. Nothing has been committed yet —
  worth doing once the config is in a state you're happy with.
- `web_server`'s own OTA endpoint is explicitly disabled (`ota: false` under
  `web_server:`) since the encrypted `ota:` platform already covers updates, and
  the web one would otherwise accept plaintext firmware uploads.
