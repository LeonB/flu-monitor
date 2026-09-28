#include "status_led.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Onboard LED on the D32 Pro, pin 5 -- unrelated to the LED ring's own
// GPIO13 data line. A plain GPIO blink is simpler than driving one more
// pixel through the led_strip/RMT driver just for connection status.
#define STATUS_LED_GPIO  5

// This board's onboard LED is wired active-low (sinks current to turn on)
// -- confirmed on real hardware: driving the pin HIGH left it completely
// dark instead of lit. Common on WeMos/Lolin boards; not documented in any
// pinout page found for this one.
#define STATUS_LED_ACTIVE_LOW  1

// Set from app_main()'s own connect-lifecycle transitions; read every tick
// by status_led_task. A single-word enum write/read is atomic enough on
// this platform without a lock -- worst case is one stale blink-phase read.
static volatile status_led_state_t s_state = STATUS_LED_CONNECTING;

static void status_led_task(void *arg) {
  (void) arg;
  bool on = false;
  while (true) {
    uint32_t delay_ms;
    switch (s_state) {
      case STATUS_LED_CONNECTED:
        on = true;
        delay_ms = 1000;  // stays solid; just re-checks for a state change periodically
        break;
      case STATUS_LED_AP_MODE:
        on = !on;
        delay_ms = 150;  // fast -- "needs attention, go set up WiFi"
        break;
      case STATUS_LED_CONNECTING:
      default:
        on = !on;
        delay_ms = 500;  // slow -- "in progress"
        break;
    }
#if STATUS_LED_ACTIVE_LOW
    gpio_set_level(STATUS_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(STATUS_LED_GPIO, on ? 1 : 0);
#endif
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
}

void status_led_init(void) {
  gpio_config_t io_conf = {
      .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&io_conf);
  xTaskCreate(status_led_task, "status_led", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
}

void status_led_set(status_led_state_t state) {
  s_state = state;
}
