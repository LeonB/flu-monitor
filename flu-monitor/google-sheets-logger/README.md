# Google Sheets logger

`Code.gs` is a Google Apps Script webhook that appends flu-monitor sensor
readings to a Google Sheet. It's a template: deploying it happens in your own
Google account, not from this repo.

This is a fork of the original ESPHome sidecar's own version of this same
script (that sidecar has since been retired and removed), adjusted for the
BMP581 sensor's physical removal -- no more `Temperature (C)`/`Pressure (Pa)`
columns, and pointed at a distinct sheet tab (`Sensor Log`)
rather than reusing the old 8-column tab, since dropping two *leading*
columns isn't something the header-backfill logic (designed for
trailing-column *additions*) can reconcile safely against old rows.

## Deploy it

1. Create a Google Sheet (any name), or reuse the existing one from the old
   ESPHome sidecar's deployment -- a tab named `Sensor Log` is
   created automatically on first write if it doesn't already exist.
2. In the Sheet, go to **Extensions -> Apps Script**, delete the placeholder
   code, and paste in `Code.gs`.
3. Replace `SHARED_SECRET` in the script with your own random string, e.g.
   generate one with `openssl rand -hex 16`. This is the only thing standing
   between "anyone with the URL" and your sheet, since the deployment step
   below uses the simplest access model.
4. **Deploy -> New deployment**, type **Web app**. Execute as **Me**, access
   **Anyone**. Deploy, and authorize it when Google prompts you to.
5. Copy the resulting URL (ends in `/exec`).

## Wire it into the sidecar

Unlike the old ESPHome sidecar (compiled-in via `secrets.yaml`), this
project's settings are runtime/NVS-backed -- set both values via
`POST /api/settings` (see the repo root's own `README.md` for the full
request shape):

```sh
curl -X POST http://flu-monitor.local/api/settings \
  -H "Content-Type: application/json" \
  -d '{..., "google_sheets_webhook_url":"https://script.google.com/macros/s/XXXXX/exec","google_sheets_secret":"<the random string from step 3>"}'
```

See `main/sheets_logger.c` for how these get sent as a GET request with
query parameters, on its own FreeRTOS task. It's GET rather than POST
deliberately: Apps Script Web Apps always respond with a redirect to a
`script.googleusercontent.com` URL that only accepts GET, and the ESP32's
HTTP client (`esp_http_client`) preserves the original method across that
redirect instead of downgrading to GET, so a POST body fails there with an
HTTP 405 even though it can look fine from a browser or a plain `curl -L`.

## Expect occasional failures -- this is normal

Google Apps Script Web Apps have real, unpredictable latency. Direct `curl`
testing against the deployed webhook showed round trips anywhere from 1.5s to
40+ seconds (timing out) across a handful of back-to-back calls, from a
regular machine on a fast connection, nothing device-specific about it. Two
consequences:

- **A logged timeout/connection failure usually isn't a lost row.** Apps
  Script executes the handler function and writes to the sheet *before* it
  sends the client a response/redirect. If the ESP32 (or curl) fails to
  follow that response chain in time, the write has typically already
  happened. This does *not* apply to a `404` specifically, though --
  observed once live (a scheduled heartbeat log), and a direct `curl -L`
  retry against the same URL immediately afterward succeeded cleanly
  (`302 -> 200 "ok"`), confirming the deployment itself was fine and this
  was a one-off, transient Google-side routing hiccup, not a dropped
  request the row still landed from. Check the sheet before assuming either
  way.
- **`sheets_logger.c` runs its own dedicated FreeRTOS task** for these
  requests, plain ESP-IDF `esp_http_client` -- not ESPHome's `http_request`
  component or any fork of it (this project isn't ESPHome at all). The
  point is the same one the old ESPHome sidecar needed a third-party fork
  for: an occasional 20-40s Google stall blocks only that task, never
  sensor sampling or the REST/WS servers.

## Updating the deployed script

Editing `Code.gs` in the Apps Script editor doesn't take effect until you
redeploy: **Deploy -> Manage deployments -> pencil icon on the active
deployment -> Version: New version -> Deploy**. The web app URL stays the
same across versions.
