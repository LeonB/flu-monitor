# Google Sheets logger

`Code.gs` is a Google Apps Script webhook that appends flu-monitor sensor
readings to a Google Sheet. It's a template: deploying it happens in your own
Google account, not from this repo.

## Deploy it

1. Create a Google Sheet (any name). A tab named `Sensor Log` is created
   automatically on first write if it doesn't already exist.
2. In the Sheet, go to **Extensions -> Apps Script**, delete the placeholder
   code, and paste in `Code.gs`.
3. Replace `SHARED_SECRET` in the script with your own random string, e.g.
   generate one with `openssl rand -hex 16`. This is the only thing standing
   between "anyone with the URL" and your sheet, since the deployment step
   below uses the simplest access model.
4. **Deploy -> New deployment**, type **Web app**. Execute as **Me**, access
   **Anyone**. Deploy, and authorize it when Google prompts you to.
5. Copy the resulting URL (ends in `/exec`).

## Wire it into ESPHome

Add both values to `secrets.yaml` (not committed):

```yaml
google_sheets_webhook_url: "https://script.google.com/macros/s/XXXXX/exec"
google_sheets_secret: "<the random string from step 3>"
```

See `flu-monitor.yaml`'s `http_request:` and `interval:` blocks for how these
get sent as a GET request with query parameters on a timer. It's GET rather
than POST deliberately: Apps Script Web Apps always respond with a redirect
to a `script.googleusercontent.com` URL that only accepts GET, and the
ESP32's HTTP client preserves the original method across that redirect
instead of downgrading to GET, so a POST body fails there with an HTTP 405
even though it can look fine from a browser or a plain `curl -L`.

## Updating the deployed script

Editing `Code.gs` in the Apps Script editor doesn't take effect until you
redeploy: **Deploy -> Manage deployments -> pencil icon on the active
deployment -> Version: New version -> Deploy**. The web app URL stays the
same across versions.
