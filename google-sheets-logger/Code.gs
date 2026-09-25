// Google Apps Script webhook that appends flu-monitor sensor readings to a
// Google Sheet. Bind this to a Sheet via Extensions -> Apps Script, paste it
// in place of the placeholder code, replace SHARED_SECRET below with your
// own random string, then Deploy -> New deployment -> Web app
// (Execute as: Me, Who has access: Anyone). Put the resulting /exec URL and
// the secret you chose into ESPHome's secrets.yaml (see README.md).
//
// The placeholder secret below is intentionally not a real one -- this file
// is a template checked into git, not the live deployed script. Set the
// real secret only inside the Apps Script editor for your own deployment,
// never here.
//
// This uses GET (query parameters), not POST with a JSON body. Apps Script
// Web Apps always respond with a redirect to a script.googleusercontent.com
// URL that only accepts GET; ESP-IDF's HTTP client (used by ESPHome's
// http_request component) preserves the original method across that
// redirect instead of downgrading to GET the way browsers and curl's
// default behavior do, so a POST here fails with HTTP 405 on the ESP32
// even though it can look fine when tested with a browser or a naive curl
// call. Using GET end-to-end sidesteps the whole issue since GET always
// redirects to GET regardless of the client's redirect-method policy.
const SHARED_SECRET = "REPLACE_ME_WITH_A_RANDOM_STRING";
const SHEET_NAME = "Sensor Log";
const HEADER_ROW = ["Timestamp", "Temperature (C)", "Pressure (Pa)", "Thermocouple (C)", "Cold Junction (C)", "Event"];

function doGet(e) {
  const p = e.parameter;

  if (p.secret !== SHARED_SECRET) {
    return ContentService.createTextOutput("forbidden").setMimeType(ContentService.MimeType.TEXT);
  }

  const sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME)
    || SpreadsheetApp.getActiveSpreadsheet().insertSheet(SHEET_NAME);

  if (sheet.getLastRow() === 0) {
    sheet.appendRow(HEADER_ROW);
  } else if (sheet.getRange(1, HEADER_ROW.length).getValue() !== HEADER_ROW[HEADER_ROW.length - 1]) {
    // Sheet predates the Event column (added later) -- backfill just the header cell.
    sheet.getRange(1, HEADER_ROW.length).setValue(HEADER_ROW[HEADER_ROW.length - 1]);
  }

  sheet.appendRow([
    new Date(),
    Number(p.temperature),
    Number(p.pressure),
    Number(p.thermocouple),
    Number(p.cold_junction),
    p.event || "",
  ]);

  return ContentService.createTextOutput("ok").setMimeType(ContentService.MimeType.TEXT);
}
