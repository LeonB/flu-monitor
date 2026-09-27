# flu-monitor-idf: the wood stove flue temperature sidecar (see ../CLAUDE.md),
# rewritten from ESPHome as plain ESP-IDF. Boilerplate below matches
# Espressif's own project template exactly.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flu-monitor-idf)
