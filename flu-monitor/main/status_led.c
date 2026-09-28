#include "status_led.h"

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "status_led";

// Onboard NeoPixel, pin 0 on this board -- separate from the STEMMA QT I2C
// pins, but shares its power-enable line (STEMMA_QT_POWER_GPIO/GPIO2) with
// the STEMMA QT connector; sensors_init() already drives that high before
// status_led_init() is ever called, so no extra power sequencing is needed
// here.
#define STATUS_LED_GPIO  0

typedef struct {
  uint8_t r, g, b;
} rgb_t;

// Dim, not full 255 -- this is a glanceable status indicator sitting right
// next to the board, not something meant to be seen across a room like the
// flu-display LED ring.
static const rgb_t COLOR_CONNECTING = {40, 20, 0};  // amber
static const rgb_t COLOR_CONNECTED = {0, 30, 0};    // green
static const rgb_t COLOR_AP_MODE = {40, 0, 0};      // red

static led_strip_handle_t s_strip;

void status_led_init(void) {
  led_strip_config_t strip_config = {
      .strip_gpio_num = STATUS_LED_GPIO,
      .max_leds = 1,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
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
  ESP_LOGI(TAG, "Status NeoPixel initialized on GPIO%d", STATUS_LED_GPIO);
}

void status_led_set(status_led_state_t state) {
  rgb_t color;
  switch (state) {
    case STATUS_LED_CONNECTED:
      color = COLOR_CONNECTED;
      break;
    case STATUS_LED_AP_MODE:
      color = COLOR_AP_MODE;
      break;
    case STATUS_LED_CONNECTING:
    default:
      color = COLOR_CONNECTING;
      break;
  }
  led_strip_set_pixel(s_strip, 0, color.r, color.g, color.b);
  led_strip_refresh(s_strip);
}
