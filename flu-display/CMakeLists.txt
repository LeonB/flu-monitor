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
