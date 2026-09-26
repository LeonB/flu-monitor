.PHONY: gsheets-code

# Prints google-sheets-logger/Code.gs with the real secret from secrets.yaml
# substituted in for the REPLACE_ME_WITH_A_RANDOM_STRING placeholder, and
# copies the same text to the clipboard (macOS pbcopy) -- paste it straight
# into the Apps Script editor after making a change to Code.gs. The
# committed Code.gs keeps the placeholder; this never writes the real
# secret to any file, only stdout/clipboard.
gsheets-code:
	@secret=$$(sed -n 's/^google_sheets_secret: *"\(.*\)"/\1/p' secrets.yaml); \
	if [ -z "$$secret" ]; then \
		echo "Error: google_sheets_secret not found in secrets.yaml" >&2; \
		exit 1; \
	fi; \
	sed "s/REPLACE_ME_WITH_A_RANDOM_STRING/$$secret/" google-sheets-logger/Code.gs; \
	sed "s/REPLACE_ME_WITH_A_RANDOM_STRING/$$secret/" google-sheets-logger/Code.gs | pbcopy 2>/dev/null && \
		echo "--- also copied to clipboard ---" >&2
