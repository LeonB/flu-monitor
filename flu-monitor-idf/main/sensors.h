#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  THERMOCOUPLE_ZONE_UNKNOWN = 0,  // no plausible reading admitted yet
  THERMOCOUPLE_ZONE_COLD,
  THERMOCOUPLE_ZONE_GOOD,
  THERMOCOUPLE_ZONE_HOT,
} thermocouple_zone_t;

// "cold"/"good"/"hot"/"unknown" -- for REST responses and logging.
const char *thermocouple_zone_name(thermocouple_zone_t zone);

typedef struct {
  bool bmp581_ok;
  float bmp581_temperature_c;
  float bmp581_pressure_pa;

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

// Brings up the shared I2C bus and both sensors (BMP581 + MCP9601). Logs
// which of the two were found; a missing/failed one doesn't prevent the
// other from working (see sensors_read()'s per-sensor 'ok' fields) -- this
// is Milestone 2, verifying the hardware works at all, not yet wired into
// any alerting that would need both.
esp_err_t sensors_init(void);

// Takes one reading from each sensor (BMP581 in forced/single-shot mode,
// MCP9601 already free-running at its own internal cadence), and -- if the
// thermocouple reading is plausible -- admits it into the rate/zone
// regression window. Call this on a fixed, sensor-matching cadence only
// (currently every 30s, from main.c's sensor_log_task): the regression
// assumes each call is a genuinely new sample at that cadence, which is
// exactly the assumption an arbitrary-rate poller (e.g. a REST client) would
// break -- see flu-display/CLAUDE.md's own writeup of the rate-inflation bug
// this design avoids. Anything that just wants the latest values without
// triggering a new I2C read or feeding the window should call
// sensors_get_last_reading() instead.
void sensors_read(sensor_reading_t *out);

// Thread-safe copy of whatever sensors_read() most recently produced -- no
// I2C traffic, no window mutation. What GET /api/reading (Milestone 3) and
// any future WS broadcast (Milestone 4) should actually call.
void sensors_get_last_reading(sensor_reading_t *out);

#ifdef __cplusplus
}
#endif
