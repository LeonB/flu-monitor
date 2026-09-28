# flu-monitor: the wood stove flue temperature sidecar (see ../CLAUDE.md),
# rewritten from ESPHome as plain ESP-IDF (this folder was flu-monitor-idf/
# during development, alongside the original ESPHome flu-monitor/, which has
# since been retired and removed). Boilerplate below matches Espressif's own
# project template exactly.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flu-monitor)
