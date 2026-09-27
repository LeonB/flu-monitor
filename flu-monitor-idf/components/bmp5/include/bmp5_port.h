#pragma once

// Thin ESP-IDF/i2cdev glue for Bosch's hardware-agnostic BMP5_SensorAPI
// (bmp5.c/bmp5.h/bmp5_defs.h, vendored verbatim in this component). Bosch's
// driver only needs read/write/delay function pointers; this is that ~20
// line implementation, built on esp-idf-lib's i2cdev so the BMP581 shares
// the same bus/mutex machinery as the MCP9601 (esp-idf-lib/mcp960x).

#include <i2cdev.h>
#include <esp_err.h>

#include "bmp5.h"

#ifdef __cplusplus
extern "C" {
#endif

// Owns both the low-level i2cdev bus descriptor and the Bosch bmp5_dev
// wrapper wired to it -- one struct is all sensors.c needs to carry around.
typedef struct {
  i2c_dev_t i2c_dev;
  struct bmp5_dev bosch_dev;
} bmp5_port_t;

// Sets up the i2cdev descriptor (address/port/pins/clock) and wires the
// Bosch driver's read/write/delay function pointers to it. Doesn't talk to
// the sensor yet -- call bmp5_init(&port->bosch_dev) next (see bmp5.h) to
// actually probe it.
esp_err_t bmp5_port_init_desc(bmp5_port_t *port, uint8_t addr, i2c_port_t i2c_port, gpio_num_t sda_gpio,
                              gpio_num_t scl_gpio, uint32_t clk_speed_hz);

#ifdef __cplusplus
}
#endif
