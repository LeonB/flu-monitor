# flu-monitor

ESPHome firmware for an **Adafruit ESP32 Feather V2** with an **Adafruit BMP581**
pressure/temperature sensor attached over STEMMA QT.

## Files

- `flu-monitor.yaml` – the ESPHome device config
- `secrets.yaml` – Wi-Fi credentials and API key (git-ignored)

## Setup

ESPHome is installed with pipx (Python 3.13). Fill in `wifi_password` in `secrets.yaml`, then:

```sh
esphome run flu-monitor.yaml --device /dev/cu.usbserial-XXXX   # first flash over USB
esphome run flu-monitor.yaml                                   # later flashes over the air
esphome logs flu-monitor.yaml --device /dev/cu.usbserial-XXXX  # watch serial logs
```

If the device cannot join Wi-Fi it starts an access point called `Flu Monitor Fallback`
(password in `secrets.yaml`) with a captive portal where you can enter credentials.

## Hardware notes

| Function            | Pin    |
|---------------------|--------|
| STEMMA QT SDA       | GPIO22 |
| STEMMA QT SCL       | GPIO20 |
| STEMMA QT power     | GPIO2 (must be HIGH) |
| Red status LED      | GPIO13 |
| BMP581 I2C address  | 0x47 (0x46 with SDO jumper cut) |
