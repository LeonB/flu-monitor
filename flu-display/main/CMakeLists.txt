idf_component_register(SRCS main.c
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event esp_netif nvs_flash wifi_setup captive_portal)
