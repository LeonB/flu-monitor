#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// Updates the zone/rate thresholds AND the pulse-rendering tuning (idle/fast
// pulse period, rate deadband, color-transition exponent, breathing shape) -- called by
// flue_poll.c once it fetches flu-monitor's current settings (at boot, and
// again on its "settings changed" WS event), so a value changed via the
// sidecar's own REST API actually updates the ring's rendering, not just
// its own classification. Defaults to config.h's ZONE_COLD_MAX_C/
// ZONE_OPTIMAL_MAX_C/FAST_RISE_C_PER_MIN/RATE_DEADBAND_C_PER_MIN/
// IDLE_PULSE_PERIOD_MS/FAST_PULSE_PERIOD_MS/COLOR_TRANSITION_EXPONENT until
// the first successful fetch. Thread-safe. Named "_tuning", not
// "_thresholds", since it now covers pulse-rendering constants too, not
// just the zone/rate classification thresholds it originally did.
void led_display_set_tuning(float zone_cold_max_c, float zone_optimal_max_c, float fast_rise_c_per_min,
                            float rate_deadband_c_per_min, uint32_t idle_pulse_period_ms,
                            uint32_t fast_pulse_period_ms, float color_transition_exponent, float breathing_exponent, uint32_t minimum_brightness, uint32_t maximum_brightness);

// Updates the three zone colours (packed 0xRRGGBB). Thread-safe.
void led_display_set_zone_colors(uint32_t cold, uint32_t optimal, uint32_t hot);

#ifdef __cplusplus
}
#endif
