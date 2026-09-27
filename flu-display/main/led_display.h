#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the LED ring driver and its background render task. Call once at
// boot. Until the first led_display_set_reading() call, the ring shows the
// neutral "no data yet" pulse.
void led_display_init(void);

// Updates what the ring should show, based on the latest polled reading.
// Pass valid=false to fall back to the neutral "no data" pulse (no reading
// has arrived yet, or the last one is too stale to trust) --
// temperature_c/rate_c_per_min are ignored in that case.
void led_display_set_reading(bool valid, float temperature_c, float rate_c_per_min);

// Updates the zone/rate thresholds used for color classification and pulse
// speed -- called by flue_poll.c once it fetches flu-monitor-idf's current
// settings (at boot, and again on its "settings changed" WS event), so a
// threshold changed via the sidecar's own REST API actually updates the
// ring's gradient, not just its own classification. Defaults to config.h's
// ZONE_COLD_MAX_C/ZONE_OPTIMAL_MAX_C/FAST_RISE_C_PER_MIN until the first
// successful fetch. Thread-safe.
void led_display_set_thresholds(float zone_cold_max_c, float zone_optimal_max_c, float fast_rise_c_per_min);

#ifdef __cplusplus
}
#endif
