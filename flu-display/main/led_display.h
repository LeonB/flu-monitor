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

#ifdef __cplusplus
}
#endif
