#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts SoftAP mode (identified by ap_ssid/ap_password/max_connections),
// the DNS-hijack redirect server, and an HTTP server serving a WiFi-setup
// form at "/". Submitting the form first test-connects to the given
// network (see wifi_setup.h's wifi_sta_test_connect(), waiting up to
// sta_test_timeout_ms) while the AP/portal stay up; only a successful test
// gets saved to NVS and triggers a reboot to join for real. A failed test
// leaves the AP/portal running so the form can report the error and let
// the user try again without a reboot round-trip.
//
// Adapted from Espressif's own example at
// $IDF_PATH/examples/protocols/http_server/captive_portal -- that example
// only serves a static page; this adds the actual credential-save handler
// it doesn't include.
void captive_portal_start(const char *ap_ssid, const char *ap_password, int max_connections,
                          uint32_t sta_test_timeout_ms);

#ifdef __cplusplus
}
#endif
