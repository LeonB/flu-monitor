#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  THERMOCOUPLE_ZONE_UNKNOWN = 0,  // no plausible reading admitted yet
  THERMOCOUPLE_ZONE_COLD,
  THERMOCOUPLE_ZONE_OPTIMAL,
  THERMOCOUPLE_ZONE_HOT,
} thermocouple_zone_t;

// "cold"/"optimal"/"hot"/"unknown" -- for REST responses and logging.
const char *thermocouple_zone_name(thermocouple_zone_t zone);

// Whether a raw thermocouple reading is plausible for a stovepipe (same
// -40..600C clamp sensors.c uses to guard the rate-regression window) --
// exposed so other consumers of the raw reading (sheets_logger.c's own
// Sheets-logging gate) apply the identical rule.
bool thermocouple_reading_plausible(float c);

typedef struct {
  bool thermocouple_ok;
  float thermocouple_c;   // hot-junction, cold-junction compensated -- raw, always reported as read (see sensors.c)
  float cold_junction_c;  // MCP9601's own ambient/cold-junction sensor

  // Derived from thermocouple_c via a rolling-window regression, ported
  // from the ESPHome sidecar's own mcp9600 hot_junction on_value lambda.
  // Only updated when thermocouple_c passes the plausibility clamp -- on an
  // implausible reading these hold their last good values rather than
  // reset, exactly matching that lambda's own behavior (see sensors.c).
  float thermocouple_rate_c_per_min;
  thermocouple_zone_t thermocouple_zone;
} sensor_reading_t;

// Brings up the shared I2C bus and the MCP9601. Logs whether it was found
// -- a missing/failed sensor doesn't prevent boot (see sensors_read()'s
// thermocouple_ok field).
esp_err_t sensors_init(void);

// Takes one reading from the MCP9601 (already free-running at its own
// internal cadence), and -- if the thermocouple reading is plausible --
// admits it into the rate/zone regression window. Call this on a fixed,
// sampling cadence only (currently every 10s, from main.c's sensor_log_task).
// MCP9601's continuous conversions are faster than this sampling interval.
// The regression uses actual timestamps over a 150-second rolling window. Anything that just wants the latest values without
// triggering a new I2C read or feeding the window should call
// sensors_get_last_reading() instead.
void sensors_read(sensor_reading_t *out);

// Thread-safe copy of whatever sensors_read() most recently produced -- no
// I2C traffic, no window mutation. What GET /api/reading (Milestone 3) and
// any future WS broadcast (Milestone 4) should actually call.
void sensors_get_last_reading(sensor_reading_t *out);

// Ring buffer capacity for sensors_get_history() below -- ~4min resolution
// * 360 = 24h. Deliberately coarser than sensors_read()'s own 10s cadence
// (see sensors.c's HISTORY_PUSH_EVERY_N): a phone-width graph has no use
// for 8640 raw samples, and building a cJSON tree that large risks
// exhausting heap. Exposed (not just a private sensors.c define) so
// rest_api.c's GET /api/history handler can size its own output buffer to
// match.
#define SENSORS_HISTORY_CAPACITY 360

// One point in the 24h history ring buffer. uptime_s is
// esp_timer_get_time() at the time of this sample, seconds since boot (no
// RTC/NTP on this device, so not wall-clock time) -- GET /api/history's
// own handler converts these to "seconds ago" relative to now.
typedef struct {
  uint32_t uptime_s;
  float thermocouple_c;
  float thermocouple_rate_c_per_min;
  thermocouple_zone_t thermocouple_zone;
} history_sample_t;

// Copies up to max_out of the most recent history samples, oldest first,
// into out. Returns how many were actually copied (<= max_out, and <= the
// ring buffer's own current fill level). Thread-safe.
size_t sensors_get_history(history_sample_t *out, size_t max_out);

#ifdef __cplusplus
}
#endif
