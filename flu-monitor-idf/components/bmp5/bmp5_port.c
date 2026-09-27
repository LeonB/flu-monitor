#include "bmp5_port.h"

#include <string.h>

#include "esp_rom_sys.h"

static BMP5_INTF_RET_TYPE port_read(uint8_t reg_addr, uint8_t *read_data, uint32_t len, void *intf_ptr) {
  i2c_dev_t *dev = (i2c_dev_t *) intf_ptr;
  return i2c_dev_read_reg(dev, reg_addr, read_data, len) == ESP_OK ? BMP5_OK : BMP5_E_COM_FAIL;
}

static BMP5_INTF_RET_TYPE port_write(uint8_t reg_addr, const uint8_t *data, uint32_t len, void *intf_ptr) {
  i2c_dev_t *dev = (i2c_dev_t *) intf_ptr;
  return i2c_dev_write_reg(dev, reg_addr, data, len) == ESP_OK ? BMP5_OK : BMP5_E_COM_FAIL;
}

// Bosch's driver calls this for conversion-wait delays -- always short
// enough (microseconds to a few ms) that a busy-wait is the standard glue
// pattern for this API, not worth ceding the CPU for via vTaskDelay.
static void port_delay_us(uint32_t period_us, void *intf_ptr) {
  (void) intf_ptr;
  esp_rom_delay_us(period_us);
}

esp_err_t bmp5_port_init_desc(bmp5_port_t *port, uint8_t addr, i2c_port_t i2c_port, gpio_num_t sda_gpio,
                              gpio_num_t scl_gpio, uint32_t clk_speed_hz) {
  memset(port, 0, sizeof(*port));

  port->i2c_dev.port = i2c_port;
  port->i2c_dev.addr = addr;
  port->i2c_dev.cfg.sda_io_num = sda_gpio;
  port->i2c_dev.cfg.scl_io_num = scl_gpio;
  port->i2c_dev.cfg.master.clk_speed = clk_speed_hz;

  esp_err_t err = i2c_dev_create_mutex(&port->i2c_dev);
  if (err != ESP_OK) {
    return err;
  }

  port->bosch_dev.intf = BMP5_I2C_INTF;
  port->bosch_dev.intf_ptr = &port->i2c_dev;
  port->bosch_dev.read = port_read;
  port->bosch_dev.write = port_write;
  port->bosch_dev.delay_us = port_delay_us;

  return ESP_OK;
}
