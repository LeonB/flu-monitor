idf_component_register(SRCS main.c sensors.c settings.c rest_api.c ws_server.c sheets_logger.c status_led.c
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event esp_netif esp_wifi nvs_flash wifi_setup captive_portal ota_server
                                     esp_http_server esp_http_client app_update mdns i2cdev mcp960x
                                     driver esp_rom esp_timer json mbedtls led_strip
                       EMBED_FILES web_ui/dashboard.html web_ui/dashboard.js web_ui/dashboard.css web_ui/alpinejs.min.js
                                   web_ui/fonts/caprasimo-400.woff2 web_ui/fonts/figtree-400.woff2
                                   web_ui/fonts/figtree-600.woff2 web_ui/fonts/figtree-700.woff2)
