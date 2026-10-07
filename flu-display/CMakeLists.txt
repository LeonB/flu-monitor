# flu-display: standalone ambient light display for the wood stove flue
# monitor project (see ../CLAUDE.md). Plain ESP-IDF, not ESPHome/Arduino --
# see git log / CLAUDE.md for why. Boilerplate below matches Espressif's own
# project template exactly.
cmake_minimum_required(VERSION 3.16)

# wifi_setup/captive_portal/dns_server live in ../components/, shared with
# ../flu-monitor/ -- see ../CLAUDE.md's "Shared components" section.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flu-display)

# Explicit hardware-test override; production builds leave this empty.
set(FLU_DISPLAY_TEST_WS_URI "" CACHE STRING "WebSocket URI for hardware fault injection")
if(FLU_DISPLAY_TEST_WS_URI)
    idf_component_get_property(display_main main COMPONENT_LIB)
    target_compile_definitions(${display_main} PRIVATE FLU_DISPLAY_TEST_WS_URI="${FLU_DISPLAY_TEST_WS_URI}")
endif()

# Keep the managed package untouched; build a checked, project-specific copy.
# This device must not follow a captive-portal redirect away from /ws.
idf_component_get_property(ws_component_dir espressif__esp_websocket_client COMPONENT_DIR)
idf_component_get_property(ws_component_lib espressif__esp_websocket_client COMPONENT_LIB)
set(ws_fixed_source "${CMAKE_BINARY_DIR}/esp_websocket_client_fixed.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ws_component_dir}/esp_websocket_client.c"
    "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_websocket_client.py")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_websocket_client.py"
    "${ws_component_dir}/esp_websocket_client.c" "${ws_fixed_source}"
    RESULT_VARIABLE ws_prepare_result)
if(NOT ws_prepare_result EQUAL 0)
    message(FATAL_ERROR "WebSocket redirect fix needs review")
endif()
get_target_property(ws_original_sources ${ws_component_lib} SOURCES)
list(LENGTH ws_original_sources ws_source_count)
if(NOT ws_source_count EQUAL 1)
    message(FATAL_ERROR "WebSocket component source layout changed: review redirect fix")
endif()
set_property(TARGET ${ws_component_lib} PROPERTY SOURCES "${ws_fixed_source}")
