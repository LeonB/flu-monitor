#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts SoftAP mode (identified by ap_ssid/ap_password/max_connections),
// the DNS-hijack redirect server, and an HTTP server serving a WiFi-setup
// form at "/". Submitting the form saves the entered credentials to NVS
// (see wifi_setup.h) and reboots the device, which then retries the STA
// connection with them on the next boot.
//
// Adapted from Espressif's own example at
// $IDF_PATH/examples/protocols/http_server/captive_portal -- that example
// only serves a static page; this adds the actual credential-save handler
// it doesn't include.
void captive_portal_start(const char *ap_ssid, const char *ap_password, int max_connections);

#ifdef __cplusplus
}
#endif
