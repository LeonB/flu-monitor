idf_component_register(SRCS captive_portal.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_server json dns_server wifi_setup
                       EMBED_FILES root.html styles.css alpine.min.js)
