idf_component_register(SRCS ota_server.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_http_server app_update)
