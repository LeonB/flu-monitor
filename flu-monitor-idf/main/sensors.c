#include "sensors.h"

#include <string.h>

#include "bmp5_port.h"
#include "i2cdev.h"
#include "mcp960x.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "sensors";

// If a device was left holding the bus (e.g. SCL/SDA still low mid-transaction
// from an earlier reset/reflash), the new i2c_master driver's own bus/device
// setup never notices or recovers -- it just times out on the first real
// transaction forever after. Ported faithfully from ESPHome's own ESP-IDF
// i2c backend (i2c_bus_esp_idf.cpp's recover_()), which this same hardware
// needs on every boot (its log shows "Performing bus recovery" too) and
// which does succeed there -- an earlier, simpler version of this function
// that didn't wait for clock-stretch release on each pulse (relevant given
// the MCP9601's own clock-stretch errata, see config.h) failed to unstick a
// genuinely stuck bus, so this follows the proven implementation exactly
// rather than reinventing it. Standard NXP/Analog Devices bus-recovery
// procedure: bit-bang 9 clock pulses (waiting out clock-stretching on each),
// then a START immediately followed by a STOP.
static bool i2c_bus_recover(gpio_num_t scl_pin, gpio_num_t sda_pin) {
  const int half_period_us = 7;  // ~71kHz, matching ESPHome's own choice

  gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << scl_pin) | (1ULL << sda_pin),
      .mode = GPIO_MODE_INPUT_OUTPUT_OD,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);

  gpio_set_level(scl_pin, 1);
  gpio_set_level(sda_pin, 1);

  // Give the pull-up time to actually charge the bus high before judging it
  // stuck -- a single ~7us check (long enough for the *clock-stretch* wait
  // loop below, where a real slave is actively holding the line) isn't
  // necessarily long enough for a passive RC charge on a bus with two
  // devices' worth of capacitance.
  bool scl_high = false;
  for (int i = 0; i < 100 && !scl_high; i++) {
    esp_rom_delay_us(10);
    scl_high = gpio_get_level(scl_pin) != 0;
  }
  if (!scl_high) {
    ESP_LOGW(TAG, "I2C bus recovery: SCL is held LOW on the bus, cannot recover");
    return false;
  }

  ESP_LOGI(TAG, "I2C bus recovery: sending 9 clock pulses");
  for (int i = 0; i < 9; i++) {
    gpio_set_level(scl_pin, 0);
    esp_rom_delay_us(half_period_us);
    gpio_set_level(scl_pin, 1);
    esp_rom_delay_us(half_period_us);

    // A slave doing clock-stretching holds SCL low past our own release --
    // wait for it to actually let go, bounded so a genuinely dead bus
    // doesn't hang forever.
    int wait = 250;
    while (wait-- > 0 && gpio_get_level(scl_pin) == 0) {
      esp_rom_delay_us(half_period_us * 2);
    }
    if (gpio_get_level(scl_pin) == 0) {
      ESP_LOGW(TAG, "I2C bus recovery: SCL held LOW during clock pulse %d, giving up", i);
      return false;
    }
  }

  if (gpio_get_level(sda_pin) == 0) {
    ESP_LOGW(TAG, "I2C bus recovery: SDA still held LOW after 9 clock pulses");
    return false;
  }

  // START immediately followed by STOP (SDA high->low->high while SCL
  // stays high) snaps any still-mid-transaction device back to a clean
  // state, per the I2C spec's own recovery guidance.
  esp_rom_delay_us(half_period_us);
  gpio_set_level(sda_pin, 0);
  esp_rom_delay_us(half_period_us);
  gpio_set_level(sda_pin, 1);

  ESP_LOGI(TAG, "I2C bus recovery: done");
  return true;
}

static mcp960x_t s_mcp;
static bool s_mcp_ready = false;

static bmp5_port_t s_bmp;
static bool s_bmp_ready = false;
// Set once at init, reused for every forced-mode read -- bmp5_get_sensor_data()
// takes this as an input alongside the raw ADC data (Bosch's compensation
// math needs it), not just a one-time "set and forget" register write.
static struct bmp5_osr_odr_press_config s_bmp_osr_cfg;

static esp_err_t init_mcp9601(void) {
  esp_err_t err = mcp960x_init_desc(&s_mcp, MCP9601_I2C_ADDR, I2C_NUM_0, I2C_SDA_GPIO, I2C_SCL_GPIO);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "MCP9601: descriptor init failed: %s", esp_err_to_name(err));
    return err;
  }

  // mcp960x_init_desc() itself defaults to 100kHz -- override before the
  // first real transaction below to stay under the errata's ~85kHz ceiling.
  s_mcp.i2c_dev.cfg.master.clk_speed = I2C_FREQ_HZ;
  // ESPHome's own i2c component defaults these to true on ESP32 (its own
  // sidecar config never overrides that) -- the ESP32's internal weak
  // pull-up, added in parallel with the breakout boards' own pull-ups,
  // measurably speeds up the bus's rise time. Without it, the slower edges
  // distort SCL's effective duty cycle enough to trip the MCP960x's own
  // clock-stretch errata even at a "safe" nominal frequency.
  s_mcp.i2c_dev.cfg.sda_pullup_en = true;
  s_mcp.i2c_dev.cfg.scl_pullup_en = true;

  err = mcp960x_init(&s_mcp);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "MCP9601: not found at 0x%02x: %s", MCP9601_I2C_ADDR, esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "MCP9601: found, id=0x%02x rev=0x%02x", s_mcp.id, s_mcp.revision);

  // Type K, digital filtering off -- matches the ESPHome sidecar's own
  // mcp9600 config (no explicit filter set there, i.e. the power-on
  // default), so Milestone 2's side-by-side comparison is apples-to-apples.
  err = mcp960x_set_sensor_config(&s_mcp, MCP960X_TYPE_K, 0);
  if (err != ESP_OK) {
    return err;
  }

  // Explicitly force Normal (continuous) mode, 18-bit ADC, 1 burst sample,
  // 0.0625C cold-junction resolution -- ESPHome's own mcp9600 component
  // writes this same all-zero device-config register explicitly rather
  // than trusting the power-on-reset default. Worth doing the same here:
  // earlier debugging on this exact chip (before the STEMMA QT power fix)
  // involved I2C writes attempted while the bus was stuck low, and this
  // register is otherwise never touched, so its actual state shouldn't be
  // assumed -- this is what was making the first one or two readings after
  // boot come back frozen at 0.00 instead of a real conversion.
  return mcp960x_set_device_config(&s_mcp, MCP960X_MODE_NORMAL, MCP960X_SAMPLES_1, MCP960X_ADC_RES_18,
                                   MCP960X_TC_RES_0_0625);
}

static esp_err_t init_bmp581(void) {
  esp_err_t err = bmp5_port_init_desc(&s_bmp, BMP581_I2C_ADDR, I2C_NUM_0, I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_FREQ_HZ);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "BMP581: i2cdev descriptor init failed: %s", esp_err_to_name(err));
    return err;
  }
  // See the matching comment in init_mcp9601() -- same bus, same reasoning.
  s_bmp.i2c_dev.cfg.sda_pullup_en = true;
  s_bmp.i2c_dev.cfg.scl_pullup_en = true;

  int8_t rslt = bmp5_soft_reset(&s_bmp.bosch_dev);
  if (rslt != BMP5_OK) {
    ESP_LOGW(TAG, "BMP581: soft reset failed (%d) -- not present or wrong address?", rslt);
    return ESP_FAIL;
  }

  rslt = bmp5_init(&s_bmp.bosch_dev);
  if (rslt != BMP5_OK) {
    ESP_LOGW(TAG, "BMP581: init failed (%d)", rslt);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "BMP581: found, chip_id=0x%02x", s_bmp.bosch_dev.chip_id);

  // oversampling 8x/16x, iir_filter 4x -- matches the ESPHome sidecar's
  // bmp581_i2c config (COEFF_3 is that driver's own "4x" step; see the
  // macro table in bmp5_defs.h) for the same apples-to-apples comparison.
  s_bmp_osr_cfg.osr_t = BMP5_OVERSAMPLING_8X;
  s_bmp_osr_cfg.osr_p = BMP5_OVERSAMPLING_16X;
  s_bmp_osr_cfg.press_en = BMP5_ENABLE;
  s_bmp_osr_cfg.odr = BMP5_ODR_240_HZ;  // irrelevant in forced mode -- we trigger every reading ourselves
  rslt = bmp5_set_osr_odr_press_config(&s_bmp_osr_cfg, &s_bmp.bosch_dev);
  if (rslt != BMP5_OK) {
    ESP_LOGW(TAG, "BMP581: osr/odr config failed (%d)", rslt);
    return ESP_FAIL;
  }

  struct bmp5_iir_config iir_cfg = {
      .set_iir_t = BMP5_IIR_FILTER_COEFF_3,
      .set_iir_p = BMP5_IIR_FILTER_COEFF_3,
  };
  rslt = bmp5_set_iir_config(&iir_cfg, &s_bmp.bosch_dev);
  if (rslt != BMP5_OK) {
    ESP_LOGW(TAG, "BMP581: iir config failed (%d)", rslt);
    return ESP_FAIL;
  }

  // The INT_STATUS register's DRDY bit (what read_bmp581() polls after
  // triggering a forced read) only ever latches once this source is
  // enabled -- purely a register gate, independent of whether an actual
  // interrupt pin is wired up.
  struct bmp5_int_source_select int_source = {.drdy_en = BMP5_ENABLE};
  rslt = bmp5_int_source_select(&int_source, &s_bmp.bosch_dev);
  if (rslt != BMP5_OK) {
    ESP_LOGW(TAG, "BMP581: drdy interrupt source select failed (%d)", rslt);
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t sensors_init(void) {
  // The Feather V2's STEMMA QT connector (both sensors) is unpowered until
  // this is driven high -- see config.h's STEMMA_QT_POWER_GPIO comment. Must
  // happen before anything else here touches the bus.
  gpio_config_t power_cfg = {
      .pin_bit_mask = 1ULL << STEMMA_QT_POWER_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&power_cfg);
  gpio_set_level(STEMMA_QT_POWER_GPIO, 1);
  vTaskDelay(pdMS_TO_TICKS(50));  // let the sensors' own power-on settle before touching the bus

  i2c_bus_recover(I2C_SCL_GPIO, I2C_SDA_GPIO);

  ESP_ERROR_CHECK(i2cdev_init());

  s_mcp_ready = init_mcp9601() == ESP_OK;
  s_bmp_ready = init_bmp581() == ESP_OK;

  return (s_mcp_ready || s_bmp_ready) ? ESP_OK : ESP_FAIL;
}

static bool read_bmp581(float *temperature_c, float *pressure_pa) {
  if (!s_bmp_ready) {
    return false;
  }

  int8_t rslt = bmp5_set_power_mode(BMP5_POWERMODE_FORCED, &s_bmp.bosch_dev);
  if (rslt != BMP5_OK) {
    ESP_LOGW(TAG, "BMP581: trigger forced read failed (%d)", rslt);
    return false;
  }

  // Poll for data-ready rather than a blind delay -- conversion time
  // depends on the OSR settings above; this comfortably covers them
  // without hardcoding a worst-case guess.
  bool ready = false;
  for (int i = 0; i < 50; i++) {
    uint8_t int_status = 0;
    rslt = bmp5_get_interrupt_status(&int_status, &s_bmp.bosch_dev);
    if (rslt == BMP5_OK && (int_status & BMP5_INT_ASSERTED_DRDY)) {
      ready = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!ready) {
    ESP_LOGW(TAG, "BMP581: data-ready timeout");
    return false;
  }

  struct bmp5_sensor_data data;
  rslt = bmp5_get_sensor_data(&data, &s_bmp_osr_cfg, &s_bmp.bosch_dev);
  if (rslt != BMP5_OK) {
    ESP_LOGW(TAG, "BMP581: read failed (%d)", rslt);
    return false;
  }

  *temperature_c = data.temperature;
  *pressure_pa = data.pressure;
  return true;
}

static bool read_mcp9601(float *thermocouple_c, float *cold_junction_c) {
  if (!s_mcp_ready) {
    return false;
  }

  esp_err_t tc_err = mcp960x_get_thermocouple_temp(&s_mcp, thermocouple_c);
  esp_err_t cj_err = mcp960x_get_ambient_temp(&s_mcp, cold_junction_c);
  if (tc_err != ESP_OK || cj_err != ESP_OK) {
    ESP_LOGW(TAG, "MCP9601: read failed (thermocouple: %s, cold junction: %s)", esp_err_to_name(tc_err),
             esp_err_to_name(cj_err));
    return false;
  }
  return true;
}

void sensors_read(sensor_reading_t *out) {
  memset(out, 0, sizeof(*out));
  out->bmp581_ok = read_bmp581(&out->bmp581_temperature_c, &out->bmp581_pressure_pa);
  out->thermocouple_ok = read_mcp9601(&out->thermocouple_c, &out->cold_junction_c);
}
