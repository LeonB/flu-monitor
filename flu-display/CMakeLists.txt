# flu-display: standalone ambient light display for the wood stove flue
# monitor project (see ../CLAUDE.md). Plain ESP-IDF, not ESPHome/Arduino --
# see git log / CLAUDE.md for why. Boilerplate below matches Espressif's own
# project template exactly.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flu-display)
