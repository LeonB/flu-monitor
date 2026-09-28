#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts the Google Sheets logging task: a periodic 30s check (matching the
// ESPHome sidecar's own `interval: 30s` tick) that only actually logs when
// the thermocouple has moved past `thermocouple_deadband_c` since the last
// point logged, or `log_heartbeat_min` has elapsed, whichever comes first --
// see ../flu-monitor/CLAUDE.md's "Woodstove data-gathering logging" for why
// (a couple of weeks' worth of burns need enough resolution to correlate
// against events, without flooding the sheet every 30s regardless of
// whether anything changed). Reads via sensors_get_last_reading()/
// settings_get() only -- never triggers its own I2C read or feeds the rate
// regression window. Does nothing (logs nothing, costs nothing) until
// google_sheets_webhook_url is actually set via POST /api/settings. Call
// once, after settings_init() and sensors_init().
void sheets_logger_init(void);

// Queues an immediate log tagged with the given event label (e.g.
// "cold_start", "added_wood") -- bypasses the periodic deadband/heartbeat
// gating entirely and does not reset either's clock, matching the ESPHome
// sidecar's own woodstove event-button behavior: these are deliberate
// annotations, not routine samples, so they shouldn't delay or restart the
// next scheduled heartbeat/deadband check. Safe to call from any task
// (e.g. a future POST /api/event handler, Milestone 7) -- the actual HTTP
// request always runs on this module's own task, never the caller's.
void sheets_logger_log_event(const char *event);

#ifdef __cplusplus
}
#endif
