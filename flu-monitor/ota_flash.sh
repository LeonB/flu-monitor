#!/usr/bin/env bash
# Pushes build/flu-monitor.bin to a running device over WiFi via its OTA
# endpoint -- see ../ota_flash.sh for the shared implementation (identical
# logic to flu-display's own wrapper, just a different project directory).
exec "$(dirname "$0")/../ota_flash.sh" "$(dirname "$0")" "$@"
