/**
 * @file max17048.h
 * @brief Maxim MAX17048 -- I2C fuel gauge (ModelGauge), Mk1b I2C bus 2.
 *
 * Hardware:
 *   Chip    : Maxim MAX17048 (single-cell Li-ion / LiPo fuel gauge)
 *   I2C bus : 2  (GPIO4 SDA, GPIO5 SCL, 400 kHz) -- shared with BQ25619
 *   I2C addr: 0x36 (fixed)
 *   Power   : self-powered from CELL (VBAT+). No cell = no chip power = no ACK.
 *
 * Register access is 16-bit big-endian. This driver is a stub at Stage 26
 * Batch 26.1: probe + accessors + logging only. No broker payload, no task,
 * no self-learning; those layers land once Ivan has a real cell attached
 * and current-vs-voltage curves to fit.
 *
 * Core 0 only. Callers acquire g_i2c2_mutex (shared with BQ25619 + DRV2605
 * on bus 2) before calling any read/write helper.
 */

#ifndef MAX17048_H
#define MAX17048_H

#define MAX17048_DRIVER_VERSION  "0.1.0"

#include "esp_err.h"
#include "driver/i2c.h"
#include <stdint.h>
#include <stdbool.h>

// -- Identity -----------------------------------------------------------------
const char *max17048_get_chip_name(void);  // returns "MAX17048"
const char *max17048_get_chip_desc(void);  // returns "1-cell Li-ion fuel gauge"

// -- I2C config ---------------------------------------------------------------
#define MAX17048_ADDR         0x36

// -- Register map (16-bit big-endian, per MAX17048 datasheet) -----------------
#define MAX17048_REG_VCELL    0x02  // 78.125 uV/LSB (16-bit; lower 4 bits = noise)
#define MAX17048_REG_SOC      0x04  // MSB = integer %, LSB = 1/256 %
#define MAX17048_REG_MODE     0x06
#define MAX17048_REG_VERSION  0x08  // upper nibble = 0x1 for MAX17048 family
#define MAX17048_REG_HIBRT    0x0A
#define MAX17048_REG_CONFIG   0x0C
#define MAX17048_REG_STATUS   0x1A
#define MAX17048_REG_CMD      0xFE  // write 0x5400 = POR

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Probe MAX17048 by reading VERSION. Logs the family match + raw
 *        VERSION value. Does NOT reset the IC or touch CONFIG. Called from
 *        boot_hw_init.c after I2C bus 2 scan confirms ACK at 0x36.
 *
 *        Caller must NOT hold g_i2c2_mutex (the init takes it internally).
 *
 * @return ESP_OK if VERSION reads back and upper nibble == 0x1.
 *         ESP_ERR_INVALID_RESPONSE if the family nibble is wrong (unlikely).
 *         Other esp_err_t on bus failure.
 */
esp_err_t max17048_init(i2c_port_t i2c_num);

// -- Read helpers (caller holds g_i2c2_mutex) ---------------------------------

/**
 * @brief Read a 16-bit register (big-endian on the wire).
 *        Returns the raw value in *out.
 */
esp_err_t max17048_read16(i2c_port_t i2c_num, uint8_t reg, uint16_t *out);

/**
 * @brief Read VCELL and convert to millivolts. Uses 78.125 uV/LSB scaling
 *        applied to the full 16-bit register value (lower 4 bits are the
 *        datasheet-defined noise floor, treated as part of the value here).
 */
esp_err_t max17048_read_vcell_mv(i2c_port_t i2c_num, uint16_t *vcell_mv_out);

/**
 * @brief Read SOC in hundredths of a percent (0..10000). Divide by 100 to
 *        get percent; the extra precision lets callers show a decimal without
 *        floating-point in the driver.
 */
esp_err_t max17048_read_soc_pct100(i2c_port_t i2c_num, uint16_t *soc_pct100_out);

#endif // MAX17048_H
