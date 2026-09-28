.PHONY: gsheets-code

# Device to fetch the live google_sheets_secret from -- override for a
# device still running the old "flu-monitor-idf" mDNS hostname, e.g.:
#   make gsheets-code HOST=flu-monitor-idf.local
HOST ?= flu-monitor.local

# Prints google-sheets-logger/Code.gs with the real secret substituted in
# for the REPLACE_ME_WITH_A_RANDOM_STRING placeholder, and copies the same
# text to the clipboard (macOS pbcopy) -- paste it straight into the Apps
# Script editor after making a change to Code.gs. The committed Code.gs
# keeps the placeholder; this never writes the real secret to any file,
# only stdout/clipboard.
#
# Unlike the old ESPHome version of this target (which read secrets.yaml),
# this project's settings -- including google_sheets_secret -- are
# runtime/NVS-backed, not compiled in, so this fetches the secret live from
# the device's own GET /api/settings instead.
gsheets-code:
	@secret=$$(curl -sf "http://$(HOST)/api/settings" | sed -n 's/.*"google_sheets_secret":"\([^"]*\)".*/\1/p'); \
	if [ -z "$$secret" ]; then \
		echo "Error: could not fetch google_sheets_secret from http://$(HOST)/api/settings" >&2; \
		exit 1; \
	fi; \
	sed "s/REPLACE_ME_WITH_A_RANDOM_STRING/$$secret/" google-sheets-logger/Code.gs; \
	sed "s/REPLACE_ME_WITH_A_RANDOM_STRING/$$secret/" google-sheets-logger/Code.gs | pbcopy 2>/dev/null && \
		echo "--- also copied to clipboard ---" >&2
