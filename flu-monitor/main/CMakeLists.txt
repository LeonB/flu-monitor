idf_component_register(SRCS main.c sensors.c settings.c rest_api.c ws_server.c sheets_logger.c status_led.c
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event esp_netif esp_wifi nvs_flash wifi_setup captive_portal ota_server
                                     esp_http_server esp_http_client app_update mdns i2cdev mcp960x
                                     driver esp_pm esp_rom esp_timer json mbedtls led_strip
                       EMBED_FILES web_ui/dashboard.html web_ui/dashboard.js web_ui/dashboard.css
                                   web_ui/fonts/caprasimo-400.woff2 web_ui/fonts/figtree-400.woff2
                                   web_ui/fonts/figtree-600.woff2 web_ui/fonts/figtree-700.woff2)

# Rebuild compressed payloads and cache tags whenever any served asset changes.
set(WEB_ASSET_HEADER "${CMAKE_CURRENT_BINARY_DIR}/web_assets_generated.h")
add_custom_command(OUTPUT "${WEB_ASSET_HEADER}"
                   COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_web_assets.py" "${WEB_ASSET_HEADER}"
                   DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_web_assets.py"
                           web_ui/dashboard.html web_ui/dashboard.js web_ui/dashboard.css
                           "${CMAKE_CURRENT_LIST_DIR}/../../components/captive_portal/alpine.min.js"
                           web_ui/fonts/caprasimo-400.woff2 web_ui/fonts/figtree-400.woff2
                           web_ui/fonts/figtree-600.woff2 web_ui/fonts/figtree-700.woff2
                   VERBATIM)
add_custom_target(web_asset_header DEPENDS "${WEB_ASSET_HEADER}")
add_dependencies(${COMPONENT_LIB} web_asset_header)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
