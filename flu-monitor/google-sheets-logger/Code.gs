// Google Apps Script webhook that appends flu-monitor sensor readings to
// a Google Sheet. Bind this to a Sheet via Extensions -> Apps Script, paste
// it in place of the placeholder code, replace SHARED_SECRET below with your
// own random string, then Deploy -> New deployment -> Web app
// (Execute as: Me, Who has access: Anyone). Put the resulting /exec URL and
// the secret you chose into the sidecar's settings via
// POST /api/settings (see README.md) -- not a secrets file, since this
// project's settings are runtime/NVS-backed rather than compiled in.
//
// The placeholder secret below is intentionally not a real one -- this file
// is a template checked into git, not the live deployed script. Set the
// real secret only inside the Apps Script editor for your own deployment,
// never here.
//
// This uses GET (query parameters), not POST with a JSON body. Apps Script
// Web Apps always respond with a redirect to a script.googleusercontent.com
// URL that only accepts GET; ESP-IDF's own HTTP client (esp_http_client, see
// sheets_logger.c) preserves the original method across that redirect
// instead of downgrading to GET the way browsers and curl's default
// behavior do, so a POST here fails with HTTP 405 on the ESP32 even though
// it can look fine when tested with a browser or a naive curl call. Using
// GET end-to-end sidesteps the whole issue since GET always redirects to
// GET regardless of the client's redirect-method policy.
const SHARED_SECRET = "REPLACE_ME_WITH_A_RANDOM_STRING";
// A distinct tab from the old ESPHome sidecar's "Sensor Log" (which has
// Temperature/Pressure columns from the now-removed BMP581) -- this schema
// drops two *leading* columns rather than adding trailing ones, so the
// existing backfill-missing-trailing-columns logic below can't safely
// reconcile it against old rows without corrupting their meaning. Point at
// a fresh tab instead of silently shifting what every column means partway
// through the sheet's history.
const SHEET_NAME = "Sensor Log";
const HEADER_ROW = ["Timestamp", "Thermocouple (C)", "Cold Junction (C)", "Event", "Rate (C/min)", "Zone"];

function doGet(e) {
  const p = e.parameter;

  if (p.secret !== SHARED_SECRET) {
    return ContentService.createTextOutput("forbidden").setMimeType(ContentService.MimeType.TEXT);
  }

  const sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME)
    || SpreadsheetApp.getActiveSpreadsheet().insertSheet(SHEET_NAME);

  if (sheet.getLastRow() === 0) {
    sheet.appendRow(HEADER_ROW);
  } else {
    // Sheet predates one or more trailing columns added later (Rate/Zone
    // were the last such addition, on the old 8-column schema) -- backfill
    // whichever header cells are still missing, generalized to any number
    // of newly-added trailing columns rather than just the one most
    // recently added.
    for (let col = sheet.getLastColumn() + 1; col <= HEADER_ROW.length; col++) {
      sheet.getRange(1, col).setValue(HEADER_ROW[col - 1]);
    }
  }

  // Numeric columns default to Sheets' "Automatic" format, which drops
  // insignificant trailing zeros (26.0 displays as 26) even though the
  // stored value is unchanged -- fix the display once per sheet rather than
  // relying on whoever's reading it to notice and reformat manually.
  if (sheet.getRange("B2").getNumberFormat() !== "0.0") {
    sheet.getRange("B2:B").setNumberFormat("0.0"); // Thermocouple (C)
    sheet.getRange("C2:C").setNumberFormat("0.0"); // Cold Junction (C)
    sheet.getRange("E2:E").setNumberFormat("0.00"); // Rate (C/min)
  }

  // Two ways the device tells us when this actually happened -- prefer the
  // more accurate one (see REVIEW.md finding D4):
  //   - event_ts: an absolute Unix timestamp (seconds) from the device's own
  //     SNTP-synced wall clock, captured at the moment of the tap/reading.
  //     Immune to this request's own latency, however long it took to get
  //     here -- unlike age_ms below, which is necessarily computed before
  //     the request is even sent. 0 (or absent, on an older device build)
  //     means the device's clock hadn't synced yet.
  //   - age_ms: how long ago (in ms, relative to right now) the device says
  //     this reading/event actually happened -- only corrects for the
  //     device's own queue backlog, not this request's transit/processing
  //     time, so it's the fallback for when event_ts isn't available, not a
  //     full fix on its own. REVIEW.md finding #3 originally flagged this
  //     drift; missing/non-numeric age_ms (an older device build) falls back
  //     to 0, i.e. the original behavior of just using new Date().
  const eventTsS = Number(p.event_ts) || 0;
  const ageMs = Number(p.age_ms) || 0;
  const timestamp = eventTsS > 0 ? new Date(eventTsS * 1000) : new Date(Date.now() - ageMs);

  sheet.appendRow([
    timestamp,
    Number(p.thermocouple),
    Number(p.cold_junction),
    p.event || "",
    Number(p.rate),
    p.zone || "",
  ]);

  return ContentService.createTextOutput("ok").setMimeType(ContentService.MimeType.TEXT);
}
