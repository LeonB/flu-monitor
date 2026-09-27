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
const HEADER_ROW = ["Timestamp", "Temperature (C)", "Pressure (Pa)", "Thermocouple (C)", "Cold Junction (C)", "Event", "Rate (C/min)", "Zone"];

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
    // Sheet predates one or more trailing columns added later (Event, then
    // Rate/Zone) -- backfill whichever header cells are still missing,
    // generalized to any number of newly-added trailing columns rather than
    // just the one most recently added.
    for (let col = sheet.getLastColumn() + 1; col <= HEADER_ROW.length; col++) {
      sheet.getRange(1, col).setValue(HEADER_ROW[col - 1]);
    }
  }

  // Numeric columns default to Sheets' "Automatic" format, which drops
  // insignificant trailing zeros (26.0 displays as 26) even though the
  // stored value is unchanged -- fix the display once per sheet rather than
  // relying on whoever's reading it to notice and reformat manually.
  if (sheet.getRange("B2").getNumberFormat() !== "0.0") {
    sheet.getRange("B2:B").setNumberFormat("0.0"); // Temperature (C)
    sheet.getRange("C2:C").setNumberFormat("0");   // Pressure (Pa)
    sheet.getRange("D2:D").setNumberFormat("0.0"); // Thermocouple (C)
    sheet.getRange("E2:E").setNumberFormat("0.0"); // Cold Junction (C)
    sheet.getRange("G2:G").setNumberFormat("0.00"); // Rate (C/min)
  }

  sheet.appendRow([
    new Date(),
    Number(p.temperature),
    Number(p.pressure),
    Number(p.thermocouple),
    Number(p.cold_junction),
    p.event || "",
    Number(p.rate),
    p.zone || "",
  ]);

  return ContentService.createTextOutput("ok").setMimeType(ContentService.MimeType.TEXT);
}
