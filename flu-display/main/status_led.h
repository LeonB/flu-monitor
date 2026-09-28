#pragma once

// Drives the D32 Pro's onboard plain (non-RGB) LED to show WiFi connection
// state at a glance -- separate from the 24-LED SK6812 ring (led_display.c),
// which is dedicated to the flue temperature reading, not connection state.

typedef enum {
  STATUS_LED_CONNECTING,  // slow blink -- attempting to join stored WiFi creds
  STATUS_LED_CONNECTED,   // solid on -- joined WiFi, subscribed to flu-monitor
  STATUS_LED_AP_MODE,     // fast blink -- setup access point / captive portal active
} status_led_state_t;

void status_led_init(void);
void status_led_set(status_led_state_t state);
