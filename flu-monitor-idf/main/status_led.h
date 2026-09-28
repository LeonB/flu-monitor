#pragma once

// Drives the board's onboard NeoPixel to show WiFi connection state as a
// solid color at a glance -- shares its power-enable line (GPIO2) with
// STEMMA QT, already driven high by sensors_init() before this is called.

typedef enum {
  STATUS_LED_CONNECTING,  // amber -- attempting to join stored WiFi creds
  STATUS_LED_CONNECTED,   // green -- joined WiFi, REST/WS/OTA all up
  STATUS_LED_AP_MODE,     // red -- setup access point / captive portal active
} status_led_state_t;

void status_led_init(void);
void status_led_set(status_led_state_t state);
