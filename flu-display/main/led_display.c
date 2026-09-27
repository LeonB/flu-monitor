#include "led_display.h"

#include <math.h>

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led_display";

static led_strip_handle_t s_strip;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_valid = false;
static float s_temperature_c = 0.0f;
static float s_rate_c_per_min = 0.0f;

typedef struct {
  uint8_t r, g, b;
} rgb_t;

typedef enum {
  ZONE_COLD = 0,
  ZONE_OPTIMAL = 1,
  ZONE_HOT = 2,
} zone_t;

static const rgb_t COLOR_COLD = {0, 60, 255};
static const rgb_t COLOR_OPTIMAL = {255, 55, 0};
static const rgb_t COLOR_HOT = {255, 0, 0};

static float lerpf(float a, float b, float t) {
  return a + (b - a) * t;
}

static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static rgb_t lerp_rgb(rgb_t a, rgb_t b, float t) {
  rgb_t out = {
      (uint8_t) lerpf(a.r, b.r, t),
      (uint8_t) lerpf(a.g, b.g, t),
      (uint8_t) lerpf(a.b, b.b, t),
  };
  return out;
}

// Matches the sidecar's own "Thermocouple Zone" classification (see
// flu-monitor.yaml) exactly -- same two thresholds, same <=/> boundaries.
static zone_t temperature_to_zone(float temperature_c) {
  if (temperature_c <= ZONE_COLD_MAX_C) return ZONE_COLD;
  if (temperature_c <= ZONE_OPTIMAL_MAX_C) return ZONE_OPTIMAL;
  return ZONE_HOT;
}

static rgb_t zone_color(zone_t zone) {
  switch (zone) {
    case ZONE_COLD: return COLOR_COLD;
    case ZONE_OPTIMAL: return COLOR_OPTIMAL;
    default: return COLOR_HOT;
  }
}

// No blending between zones -- solid blue/amber/red only, picked straight
// off the same thresholds the sidecar uses to classify its own zone.
static rgb_t temperature_to_color(float temperature_c) {
  return zone_color(temperature_to_zone(temperature_c));
}

// The pure color of the *next* zone in the trend direction -- used only for
// the pulse's bright-peak hue, so a heating/cooling reading pulses toward
// where it's headed. Already at the hottest/coldest zone and still trending
// that way has no next zone to swap to, so it just stays put (no pulse
// shift). A stable reading (rate 0) also gets no shift, same color as the
// trough.
static rgb_t trend_neighbor_color(float temperature_c, float rate_c_per_min) {
  zone_t zone = temperature_to_zone(temperature_c);
  if (rate_c_per_min > 0.0f) {
    return zone_color(zone < ZONE_HOT ? zone + 1 : zone);
  }
  if (rate_c_per_min < 0.0f) {
    return zone_color(zone > ZONE_COLD ? zone - 1 : zone);
  }
  return zone_color(zone);
}

// Maps a rate of rise to a breathing-pulse period: idle pace normally,
// speeding up toward FAST_PULSE_PERIOD_MS as the rate approaches
// FAST_RISE_C_PER_MIN. A falling/stable reading (rate <= 0) is clamped to
// 0 here, so it always gets the slow idle pace.
static uint32_t rate_to_pulse_period_ms(float rate_c_per_min) {
  float t = clampf(rate_c_per_min, 0.0f, FAST_RISE_C_PER_MIN) / FAST_RISE_C_PER_MIN;
  return (uint32_t) lerpf((float) IDLE_PULSE_PERIOD_MS, (float) FAST_PULSE_PERIOD_MS, t);
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
  double cycle_pos = 0.0;  // 0..1 fraction of the way through the current pulse cycle

  while (true) {
    bool valid;
    float temperature_c, rate_c_per_min;

    portENTER_CRITICAL(&s_state_lock);
    valid = s_valid;
    temperature_c = s_temperature_c;
    rate_c_per_min = s_rate_c_per_min;
    portEXIT_CRITICAL(&s_state_lock);

    if (fabsf(rate_c_per_min) < RATE_DEADBAND_C_PER_MIN) {
      rate_c_per_min = 0.0f;
    }

    uint32_t period_ms = valid ? rate_to_pulse_period_ms(rate_c_per_min) : IDLE_PULSE_PERIOD_MS;
    cycle_pos += (double) LED_RENDER_TICK_MS / (double) period_ms;
    if (cycle_pos > 1.0) {
      cycle_pos -= 1.0;
    }

    float envelope = breath_envelope(cycle_pos);  // 0..1
    uint8_t brightness = (uint8_t) lerpf((float) PULSE_BRIGHTNESS_MIN, (float) PULSE_BRIGHTNESS_MAX, envelope);

    if (valid) {
      // The dim trough always shows the true current-zone color, so the
      // reading stays honestly readable at a glance; the bright peak swaps
      // to the pure neighboring zone's color in the trend direction. A
      // stable reading (rate 0) has no shift, so it stays one solid color
      // through the whole pulse, same as before this was added.
      rgb_t trough_color = temperature_to_color(temperature_c);
      rgb_t peak_color = trend_neighbor_color(temperature_c, rate_c_per_min);
      float color_t = powf(envelope, COLOR_TRANSITION_EXPONENT);
      rgb_t color = lerp_rgb(trough_color, peak_color, color_t);
      uint32_t r = (uint32_t) color.r * brightness / 255;
      uint32_t g = (uint32_t) color.g * brightness / 255;
      uint32_t b = (uint32_t) color.b * brightness / 255;
      for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel_rgbw(s_strip, i, r, g, b, 0);
      }
    } else {
      // Neutral "no data yet" state: pulse the warm-white channel only, so
      // it reads as distinctly different from any real temperature color.
      for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel_rgbw(s_strip, i, 0, 0, 0, brightness);
      }
    }
    led_strip_refresh(s_strip);

    vTaskDelay(pdMS_TO_TICKS(LED_RENDER_TICK_MS));
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
