# bmp5.c/bmp5.h/bmp5_defs.h are Bosch's own hardware-agnostic BMP5_SensorAPI
# (github.com/boschsensortec/BMP5_SensorAPI, BSD-3-Clause), vendored
# verbatim. bmp5_port.c is our own ESP-IDF/i2cdev glue for it.
idf_component_register(SRCS bmp5.c bmp5_port.c
                       INCLUDE_DIRS . include
                       REQUIRES i2cdev
                       PRIV_REQUIRES esp_rom)
