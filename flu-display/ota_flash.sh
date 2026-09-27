#!/usr/bin/env bash
# Pushes build/flu-display.bin to a running device over WiFi via its OTA
# endpoint. Requires main/secrets.h (copy main/secrets.h.example and fill in
# a real OTA_SECRET first -- see README.md). Only works once the device is
# already running an OTA-capable build; the very first flash onto a device
# still has to go over USB (idf.py flash).
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: $0 <device-ip-or-hostname>" >&2
  exit 1
fi
HOST="$1"

SECRET_FILE="$(dirname "$0")/main/secrets.h"
if [ ! -f "$SECRET_FILE" ]; then
  echo "Error: $SECRET_FILE not found -- copy main/secrets.h.example to main/secrets.h and fill in a real secret." >&2
  exit 1
fi
SECRET=$(sed -n 's/^#define OTA_SECRET *"\(.*\)"/\1/p' "$SECRET_FILE")
if [ -z "$SECRET" ]; then
  echo "Error: could not find OTA_SECRET in $SECRET_FILE" >&2
  exit 1
fi

BIN="$(dirname "$0")/build/flu-display.bin"
if [ ! -f "$BIN" ]; then
  echo "Error: $BIN not found -- run 'idf.py build' first." >&2
  exit 1
fi

echo "Pushing $BIN to http://${HOST}/ota ..."
curl -f --data-binary "@${BIN}" "http://${HOST}/ota?secret=${SECRET}"
echo
