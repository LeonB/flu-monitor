idf_component_register(SRCS main.c ota_server.c sensors.c settings.c rest_api.c ws_server.c sheets_logger.c
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event esp_netif nvs_flash wifi_setup captive_portal
                                     esp_http_server esp_http_client app_update mdns i2cdev mcp960x bmp5
                                     driver esp_rom esp_timer json mbedtls)
