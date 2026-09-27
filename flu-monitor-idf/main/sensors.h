#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool bmp581_ok;
  float bmp581_temperature_c;
  float bmp581_pressure_pa;

  bool thermocouple_ok;
  float thermocouple_c;   // hot-junction, cold-junction compensated
  float cold_junction_c;  // MCP9601's own ambient/cold-junction sensor
} sensor_reading_t;

// Brings up the shared I2C bus and both sensors (BMP581 + MCP9601). Logs
// which of the two were found; a missing/failed one doesn't prevent the
// other from working (see sensors_read()'s per-sensor 'ok' fields) -- this
// is Milestone 2, verifying the hardware works at all, not yet wired into
// any alerting that would need both.
esp_err_t sensors_init(void);

// Takes one reading from each sensor (BMP581 in forced/single-shot mode,
// MCP9601 already free-running at its own internal cadence). Each sensor's
// 'ok' field reflects whether *this* reading succeeded, independent of the
// other.
void sensors_read(sensor_reading_t *out);

#ifdef __cplusplus
}
#endif
