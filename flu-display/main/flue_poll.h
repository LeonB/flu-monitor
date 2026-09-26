#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// One polled+validated reading of flu-monitor's thermocouple sensor.
typedef struct {
  bool valid;            // false if the poll, parse, or sanity check failed
  float temperature_c;   // only meaningful when valid
  float rate_c_per_min;  // change since the last *accepted* reading; 0.0 on
                          // the first accepted reading (no prior baseline)
} flue_reading_t;

// Starts mDNS so FLU_MONITOR_HOST (config.h) resolves. Call once at boot,
// after WiFi STA is connected.
void flue_poll_init(void);

// Blocking: performs one HTTP GET against flu-monitor's web_server JSON API,
// parses the response, and sanity-clamps it (same -40..600C range as
// flu-monitor.yaml's own clamp -- see CLAUDE.md). Safe to call repeatedly
// from a polling loop; on any failure returns a result with valid=false
// rather than a stale/guessed value.
flue_reading_t flue_poll_once(void);

#ifdef __cplusplus
}
#endif
