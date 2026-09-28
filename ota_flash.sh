#!/usr/bin/env bash
# Shared implementation behind flu-monitor/ota_flash.sh and
# flu-display/ota_flash.sh (both thin wrappers calling this with their own
# project directory) -- pushes build/<project-name>.bin to a running device
# over WiFi via its OTA endpoint. Requires <project-dir>/main/secrets.h
# (copy main/secrets.h.example and fill in a real OTA_SECRET first -- see
# that project's README.md). Only works once the device is already running
# an OTA-capable build; the very first flash onto a device still has to go
# over USB (idf.py flash).
set -euo pipefail

if [ $# -ne 2 ]; then
  echo "Usage: $0 <project-dir> <device-ip-or-hostname>" >&2
  exit 1
fi
PROJECT_DIR="$1"
HOST="$2"
# idf.py names the .elf/.bin after the project()'s own name (project.cmake),
# which for both flu-monitor/ and flu-display/ matches their directory name.
PROJECT_NAME="$(basename "$(cd "$PROJECT_DIR" && pwd)")"

SECRET_FILE="$PROJECT_DIR/main/secrets.h"
if [ ! -f "$SECRET_FILE" ]; then
  echo "Error: $SECRET_FILE not found -- copy main/secrets.h.example to main/secrets.h and fill in a real secret." >&2
  exit 1
fi
SECRET=$(sed -n 's/^#define OTA_SECRET *"\(.*\)"/\1/p' "$SECRET_FILE")
if [ -z "$SECRET" ]; then
  echo "Error: could not find OTA_SECRET in $SECRET_FILE" >&2
  exit 1
fi

BIN="$PROJECT_DIR/build/${PROJECT_NAME}.bin"
if [ ! -f "$BIN" ]; then
  echo "Error: $BIN not found -- run 'idf.py build' first." >&2
  exit 1
fi

echo "Pushing $BIN to http://${HOST}/ota ..."
curl -f --data-binary "@${BIN}" "http://${HOST}/ota?secret=${SECRET}"
echo
