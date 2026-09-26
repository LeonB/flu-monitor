idf_component_register(SRCS main.c flue_poll.c led_display.c
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event esp_netif nvs_flash wifi_setup captive_portal
                                     esp_http_client mdns json esp_timer led_strip)
