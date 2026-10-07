idf_component_register(SRCS main.c flue_poll.c led_display.c status_led.c
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event esp_netif esp_wifi nvs_flash wifi_setup captive_portal ota_server
                                     esp_http_client esp_http_server app_update mdns json
                                     esp_pm esp_timer led_strip esp_websocket_client driver)
