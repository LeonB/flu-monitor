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

// Blue at/below the cold zone's ceiling, red at/above a "hot" ceiling placed
// as far past the good zone as the good zone is wide, amber exactly in the
// middle of the good zone -- entirely derived from the two zone thresholds
// in config.h, so there's nothing extra to keep in sync once those get real
// values from the sidecar's data-gathering run (see CLAUDE.md).
static float cold_anchor_c(void) { return ZONE_COLD_MAX_C; }
static float good_anchor_c(void) { return (ZONE_COLD_MAX_C + ZONE_GOOD_MAX_C) / 2.0f; }
static float hot_anchor_c(void) { return ZONE_GOOD_MAX_C + (ZONE_GOOD_MAX_C - ZONE_COLD_MAX_C) / 2.0f; }

static const rgb_t COLOR_COLD = {0, 60, 255};
static const rgb_t COLOR_GOOD = {255, 55, 0};
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

// Three-stop piecewise-linear gradient: solid blue at/below the cold
// anchor, solid red at/above the hot anchor, interpolated through amber in
// between.
static rgb_t temperature_to_color(float temperature_c) {
  float t_cold = cold_anchor_c();
  float t_good = good_anchor_c();
  float t_hot = hot_anchor_c();

  if (temperature_c <= t_cold) {
    return COLOR_COLD;
  }
  if (temperature_c >= t_hot) {
    return COLOR_HOT;
  }
  if (temperature_c <= t_good) {
    return lerp_rgb(COLOR_COLD, COLOR_GOOD, (temperature_c - t_cold) / (t_good - t_cold));
  }
  return lerp_rgb(COLOR_GOOD, COLOR_HOT, (temperature_c - t_good) / (t_hot - t_good));
}

// The pure zone color one step toward the trend direction -- used only for
// the pulse's bright-peak hue. A partial RGB blend between two colors this
// far apart (e.g. 65% of the way from amber to blue) comes out as a muddy,
// desaturated mix that reads as washed-out white at high brightness rather
// than "leaning toward the next color" -- a clean swap between the two pure
// endpoints reads far better than any partial blend does. A stable reading
// (rate 0) gets no shift at all, same color as the trough.
static rgb_t trend_neighbor_color(float temperature_c, float rate_c_per_min) {
  float t_good = good_anchor_c();
  if (rate_c_per_min > 0.0f) {
    return (temperature_c <= t_good) ? COLOR_GOOD : COLOR_HOT;
  }
  if (rate_c_per_min < 0.0f) {
    return (temperature_c <= t_good) ? COLOR_COLD : COLOR_GOOD;
  }
  return temperature_to_color(temperature_c);
}

// Maps a rate of rise to a breathing-pulse period: idle pace normally,
// speeding up toward FAST_PULSE_PERIOD_MS as the rate approaches
// FAST_RISE_C_PER_MIN. A falling/stable reading (rate <= 0) is clamped to
// 0 here, so it always gets the slow idle pace.
static uint32_t rate_to_pulse_period_ms(float rate_c_per_min) {
  float t = clampf(rate_c_per_min, 0.0f, FAST_RISE_C_PER_MIN) / FAST_RISE_C_PER_MIN;
  return (uint32_t) lerpf((float) IDLE_PULSE_PERIOD_MS, (float) FAST_PULSE_PERIOD_MS, t);
}

static void render_task(void *arg) {
  double phase = 0.0;

  while (true) {
    bool valid;
    float temperature_c, rate_c_per_min;

    portENTER_CRITICAL(&s_state_lock);
    valid = s_valid;
    temperature_c = s_temperature_c;
    rate_c_per_min = s_rate_c_per_min;
    portEXIT_CRITICAL(&s_state_lock);

    uint32_t period_ms = valid ? rate_to_pulse_period_ms(rate_c_per_min) : IDLE_PULSE_PERIOD_MS;
    phase += 2.0 * M_PI * ((double) LED_RENDER_TICK_MS / (double) period_ms);
    if (phase > 2.0 * M_PI) {
      phase -= 2.0 * M_PI;
    }

    float envelope = 0.5f + 0.5f * (float) sin(phase);  // 0..1
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
