#include "led_display.h"

#include <math.h>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led_display";

static led_strip_handle_t s_strip;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_valid = false;
static float s_temperature_c = 0.0f;
static float s_rate_c_per_min = 0.0f;

// Defaults to config.h's own placeholders until flue_poll.c's first
// settings fetch succeeds -- see led_display_set_tuning()'s doc comment.
static portMUX_TYPE s_threshold_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_zone_cold_max_c = ZONE_COLD_MAX_C;
static float s_zone_optimal_max_c = ZONE_OPTIMAL_MAX_C;
static float s_fast_rise_c_per_min = FAST_RISE_C_PER_MIN;
static float s_rate_deadband_c_per_min = RATE_DEADBAND_C_PER_MIN;
static uint32_t s_idle_pulse_period_ms = IDLE_PULSE_PERIOD_MS;
static uint32_t s_fast_pulse_period_ms = FAST_PULSE_PERIOD_MS;
static float s_color_transition_exponent = COLOR_TRANSITION_EXPONENT;

typedef struct {
  uint8_t r, g, b;
} rgb_t;

typedef enum {
  ZONE_COLD = 0,
  ZONE_OPTIMAL = 1,
  ZONE_HOT = 2,
} zone_t;

static rgb_t s_zone_colors[3] = {{0, 60, 255}, {255, 55, 0}, {255, 0, 0}};

static float lerpf(float a, float b, float t) {
  return a + (b - a) * t;
}

static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Preserve fractional colour and brightness until the final 8-bit output.
// Rounding once avoids the downward bias from three successive truncations.
static uint8_t scaled_channel(uint8_t trough, uint8_t peak, float color_t, float brightness) {
  float value = lerpf((float) trough, (float) peak, color_t) * brightness / 255.0f;
  return (uint8_t) lroundf(clampf(value, 0.0f, 255.0f));
}

// Matches the sidecar's own "Thermocouple Zone" classification (see
// ../flu-monitor/CLAUDE.md) exactly -- same two thresholds, same
// <=/> boundaries. Thresholds are passed in (not read from config.h's
// macros directly) since they're runtime-updatable -- see
// led_display_set_thresholds().
static zone_t temperature_to_zone(float temperature_c, float zone_cold_max_c, float zone_optimal_max_c) {
  if (temperature_c <= zone_cold_max_c) return ZONE_COLD;
  if (temperature_c <= zone_optimal_max_c) return ZONE_OPTIMAL;
  return ZONE_HOT;
}

// The pure color of the *next* zone in the trend direction -- used only for
// the pulse's bright-peak hue, so a heating/cooling reading pulses toward
// where it's headed. Already at the hottest/coldest zone and still trending
// that way has no next zone to swap to, so it just stays put (no pulse
// shift). A stable reading (rate 0) also gets no shift, same color as the
// trough.
static rgb_t trend_neighbor_color(float temperature_c, float rate_c_per_min, float zone_cold_max_c,
                                  float zone_optimal_max_c, const rgb_t colors[3]) {
  zone_t zone = temperature_to_zone(temperature_c, zone_cold_max_c, zone_optimal_max_c);
  if (rate_c_per_min > 0.0f) {
    return colors[zone < ZONE_HOT ? zone + 1 : zone];
  }
  if (rate_c_per_min < 0.0f) {
    return colors[zone > ZONE_COLD ? zone - 1 : zone];
  }
  return colors[zone];
}

// Maps a rate of rise to a breathing-pulse period: idle pace normally,
// speeding up toward fast_pulse_period_ms as the rate approaches
// fast_rise_c_per_min. A falling/stable reading (rate <= 0) is clamped to
// 0 here, so it always gets the slow idle pace.
static uint32_t rate_to_pulse_period_ms(float rate_c_per_min, float fast_rise_c_per_min, uint32_t idle_pulse_period_ms,
                                        uint32_t fast_pulse_period_ms) {
  float t = clampf(rate_c_per_min, 0.0f, fast_rise_c_per_min) / fast_rise_c_per_min;
  return (uint32_t) lerpf((float) idle_pulse_period_ms, (float) fast_pulse_period_ms, t);
}

// The "Apple sleep-LED" breathing curve: exp(sin(phase)), normalized from
// its natural range [1/e, e] back to [0, 1]. Unlike a plain sine (equal
// time at every brightness level), the exponential warp spends most of the
// cycle near the dark end and only flares briefly at the peak -- see
// config.h's comment above PULSE_BRIGHTNESS_MIN.
static float breath_envelope(double cycle_pos) {
  const float kExpSinMin = 0.36787944f;  // 1/e, exp(sin(x))'s minimum
  const float kExpSinMax = 2.71828183f;  // e, exp(sin(x))'s maximum
  float phase = (float) (2.0 * M_PI * cycle_pos) - (float) M_PI_2;
  float raw = expf(sinf(phase));
  return (raw - kExpSinMin) / (kExpSinMax - kExpSinMin);
}

static void render_task(void *arg) {
  (void) arg;
  double cycle_pos = 0.0;  // 0..1 fraction of the current pulse cycle
  TickType_t next_frame = xTaskGetTickCount();
  int64_t previous_frame_us = 0;
  int64_t stats_start_us = esp_timer_get_time();
  int64_t interval_total_us = 0, interval_min_us = INT64_MAX, interval_max_us = 0;
  uint32_t frames = 0, refresh_failures = 0;

  while (true) {
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = previous_frame_us ? now_us - previous_frame_us : 0;
    previous_frame_us = now_us;
    if (elapsed_us > 0) {
      interval_total_us += elapsed_us;
      if (elapsed_us < interval_min_us) interval_min_us = elapsed_us;
      if (elapsed_us > interval_max_us) interval_max_us = elapsed_us;
      frames++;
    }
    bool valid;
    float temperature_c, rate_c_per_min;

    portENTER_CRITICAL(&s_state_lock);
    valid = s_valid;
    temperature_c = s_temperature_c;
    rate_c_per_min = s_rate_c_per_min;
    portEXIT_CRITICAL(&s_state_lock);

    float zone_cold_max_c, zone_optimal_max_c, fast_rise_c_per_min, rate_deadband_c_per_min, color_transition_exponent;
    uint32_t idle_pulse_period_ms, fast_pulse_period_ms;
    rgb_t colors[3];
    portENTER_CRITICAL(&s_threshold_lock);
    zone_cold_max_c = s_zone_cold_max_c;
    zone_optimal_max_c = s_zone_optimal_max_c;
    fast_rise_c_per_min = s_fast_rise_c_per_min;
    rate_deadband_c_per_min = s_rate_deadband_c_per_min;
    idle_pulse_period_ms = s_idle_pulse_period_ms;
    fast_pulse_period_ms = s_fast_pulse_period_ms;
    color_transition_exponent = s_color_transition_exponent;
    for (int i = 0; i < 3; i++) colors[i] = s_zone_colors[i];
    portEXIT_CRITICAL(&s_threshold_lock);

    if (fabsf(rate_c_per_min) < rate_deadband_c_per_min) {
      rate_c_per_min = 0.0f;
    }

    uint32_t period_ms = valid ? rate_to_pulse_period_ms(rate_c_per_min, fast_rise_c_per_min, idle_pulse_period_ms,
                                                          fast_pulse_period_ms)
                                : idle_pulse_period_ms;
    // Real elapsed time keeps the pulse period correct through scheduling
    // jitter and LED transmission time, instead of assuming every frame is 30ms.
    cycle_pos = fmod(cycle_pos + (double) elapsed_us / ((double) period_ms * 1000.0), 1.0);

    float envelope = clampf(breath_envelope(cycle_pos), 0.0f, 1.0f);
    float brightness = lerpf((float) PULSE_BRIGHTNESS_MIN, (float) PULSE_BRIGHTNESS_MAX, envelope);

    if (valid) {
      // The dim trough always shows the true current-zone color, so the
      // reading stays honestly readable at a glance; the bright peak swaps
      // to the pure neighboring zone's color in the trend direction. A
      // stable reading (rate 0) has no shift, so it stays one solid color
      // through the whole pulse, same as before this was added.
      rgb_t trough_color = colors[temperature_to_zone(temperature_c, zone_cold_max_c, zone_optimal_max_c)];
      rgb_t peak_color = trend_neighbor_color(temperature_c, rate_c_per_min, zone_cold_max_c, zone_optimal_max_c, colors);
      float color_t = powf(envelope, color_transition_exponent);
      uint8_t r = scaled_channel(trough_color.r, peak_color.r, color_t, brightness);
      uint8_t g = scaled_channel(trough_color.g, peak_color.g, color_t, brightness);
      uint8_t b = scaled_channel(trough_color.b, peak_color.b, color_t, brightness);
      for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel_rgbw(s_strip, i, r, g, b, 0);
      }
    } else {
      // Neutral "no data yet" state: pulse the warm-white channel only, so
      // it reads as distinctly different from any real temperature color.
      for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel_rgbw(s_strip, i, 0, 0, 0, (uint8_t) lroundf(brightness));
      }
    }
    if (led_strip_refresh(s_strip) != ESP_OK) refresh_failures++;

    // A compact, infrequent diagnostic lets a serial log distinguish actual
    // dropped frames from the LEDs' remaining 8-bit brightness steps.
    if (now_us - stats_start_us >= 30000000 && frames > 0) {
      ESP_LOGI(TAG, "LED frames: %.1f fps, interval avg %.2f/min %.2f/max %.2f ms, refresh failures %lu",
               (double) frames * 1000000.0 / (double) interval_total_us,
               (double) interval_total_us / (double) frames / 1000.0,
               (double) interval_min_us / 1000.0, (double) interval_max_us / 1000.0,
               (unsigned long) refresh_failures);
      stats_start_us = now_us;
      interval_total_us = 0;
      interval_min_us = INT64_MAX;
      interval_max_us = 0;
      frames = 0;
      refresh_failures = 0;
    }

    // Absolute schedule includes render/transmit time rather than adding it
    // to the delay. If a frame overruns, resume from now instead of bursting.
    if (xTaskDelayUntil(&next_frame, pdMS_TO_TICKS(LED_RENDER_TICK_MS)) == pdFALSE) {
      next_frame = xTaskGetTickCount();
    }
  }
}

void led_display_init(void) {
  led_strip_config_t strip_config = {
      .strip_gpio_num = LED_GPIO,
      .max_leds = LED_COUNT,
      .led_model = LED_MODEL_SK6812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW,
      .flags = {
          .invert_out = false,
      },
  };
  led_strip_rmt_config_t rmt_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000,
      .flags = {
          .with_dma = false,
      },
  };
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
  ESP_ERROR_CHECK(led_strip_clear(s_strip));

  xTaskCreate(render_task, "led_render", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
  ESP_LOGI(TAG, "LED ring initialized on GPIO%d, %d pixels", LED_GPIO, LED_COUNT);
}

void led_display_set_reading(bool valid, float temperature_c, float rate_c_per_min) {
  portENTER_CRITICAL(&s_state_lock);
  s_valid = valid;
  s_temperature_c = temperature_c;
  s_rate_c_per_min = rate_c_per_min;
  portEXIT_CRITICAL(&s_state_lock);
}

void led_display_set_tuning(float zone_cold_max_c, float zone_optimal_max_c, float fast_rise_c_per_min,
                            float rate_deadband_c_per_min, uint32_t idle_pulse_period_ms,
                            uint32_t fast_pulse_period_ms, float color_transition_exponent) {
  portENTER_CRITICAL(&s_threshold_lock);
  s_zone_cold_max_c = zone_cold_max_c;
  s_zone_optimal_max_c = zone_optimal_max_c;
  s_fast_rise_c_per_min = fast_rise_c_per_min;
  s_rate_deadband_c_per_min = rate_deadband_c_per_min;
  s_idle_pulse_period_ms = idle_pulse_period_ms;
  s_fast_pulse_period_ms = fast_pulse_period_ms;
  s_color_transition_exponent = color_transition_exponent;
  portEXIT_CRITICAL(&s_threshold_lock);
}

// Packed 0xRRGGBB, copied under the same lock as the other rendering settings.
void led_display_set_zone_colors(uint32_t cold, uint32_t optimal, uint32_t hot) {
  const uint32_t packed[3] = {cold, optimal, hot};
  portENTER_CRITICAL(&s_threshold_lock);
  for (int i = 0; i < 3; i++) {
    s_zone_colors[i] = (rgb_t) {(packed[i] >> 16) & 255, (packed[i] >> 8) & 255, packed[i] & 255};
  }
  portEXIT_CRITICAL(&s_threshold_lock);
}
