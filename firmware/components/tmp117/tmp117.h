/**
 * @file tmp117.h
 * @brief Texas Instruments TMP117 skin-temperature sensor -- Core 0 only.
 *
 * New chip in Mk I -- no legacy driver to port. The TMP117 is a 16-bit
 * digital temperature sensor with 0.0078 C/LSB resolution and a published
 * +/- 0.1 C accuracy across the human-body range (-20 .. +50 C). On Mk I
 * it sits on the skin-side of the case, reading skin contact temperature.
 *
 * Hardware (v7.2 §I2C Bus Assignment, §SKIN TEMP):
 *   Bus     : I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
 *   Address : 0x48 (ADD0 = GND -- factory default)
 *   ALERT pin : not routed on Mk I (we poll on a 5 s task)
 *
 * Operating mode: continuous conversion at the default ODR (~1 Hz). At
 * each poll tick we read the result register; no mode reconfiguration is
 * needed at runtime.
 *
 * The TMP117 has no tile of its own -- the health tile picks up the skin
 * temperature reading via broker_skin_read() and displays it as a subtile
 * row. That broker channel is consumed by the health_tile alongside the
 * MAX30101's HR/SpO2 data.
 *
 * Architecture: Blueprint 1 §8, Blueprint 4, Blueprint 5, Blueprint 14b
 */

#ifndef TMP117_H
#define TMP117_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c.h"

// ── I2C address + port ────────────────────────────────────────────────────────
#define TMP117_ADDR              0x48
#define TMP117_I2C_PORT          I2C_NUM_0

// ── Register map (TI SBOS719C, 2021 rev) ─────────────────────────────────────
// All registers are 16-bit, MSB first on the wire.
#define TMP117_REG_TEMP_RESULT   0x00
#define TMP117_REG_CONFIG        0x01
#define TMP117_REG_THIGH_LIMIT   0x02
#define TMP117_REG_TLOW_LIMIT    0x03
#define TMP117_REG_EEPROM_UL     0x04
#define TMP117_REG_EEPROM1       0x05
#define TMP117_REG_EEPROM2       0x06
#define TMP117_REG_TEMP_OFFSET   0x07
#define TMP117_REG_EEPROM3       0x08
#define TMP117_REG_DEVICE_ID     0x0F   // Expected: 0x0117 (low 12 bits = device-id)

// CONFIG register bits (datasheet Table 7-7)
#define TMP117_CONFIG_RESET      (1u << 1)      // soft reset (auto-clears)
#define TMP117_CONFIG_DRDY_MASK  (1u << 13)     // data-ready flag (read-only)
#define TMP117_LSB_PER_C         128            // 1 LSB = 1/128 C = 0.0078125 C

// ── Module config ────────────────────────────────────────────────────────────
#define TMP117_POLL_MS           5000   // Skin temperature changes slowly

// ── Broker data struct ───────────────────────────────────────────────────────
// New broker channel. Driver owns the shape; the broker proxy + macro entry
// must be added in data_broker.{c,h} as a follow-up (same pattern that the
// other Phase 2/3 drivers leave behind -- see DEFECT-002 in this module).
typedef struct {
    float    skin_temp_c;       // Skin contact temperature in C
    bool     valid;             // true after the first successful conversion

    // Mandatory bookkeeping
    uint32_t last_update_ms;
    bool     enabled;
} broker_skin_data_t;

#define BROKER_SKIN_TIMEOUT_MS   15000   // 3x the 5 s poll period

// ── Identity ──────────────────────────────────────────────────────────────────
const char *tmp117_get_chip_name(void);   // "TMP117"
const char *tmp117_get_chip_desc(void);   // "Skin temperature"

// ── Driver API (called inside g_i2c_mutex) ──────────────────────────────────

/**
 * @brief Probe DEVICE_ID, soft-reset, leave the chip in default continuous
 *        conversion mode. Caller must NOT hold g_i2c_mutex.
 */
esp_err_t tmp117_init(i2c_port_t i2c_num);

/**
 * @brief Read TEMP_RESULT register and convert to degrees Celsius.
 *        Caller holds g_i2c_mutex.
 *
 * @param i2c_num   I2C port
 * @param temp_c    Output: temperature in C
 * @return ESP_OK on success
 */
esp_err_t tmp117_read_temp_c(i2c_port_t i2c_num, float *temp_c);

/**
 * @brief Last successful temperature reading. Returns NaN if no valid read
 *        has occurred yet. Pure CPU; safe to call from any context.
 */
float tmp117_get_last_temp_c(void);

// ── FreeRTOS task (pinned to Core 0 by boot_tasks.c) ─────────────────────────
void task_skin_fn(void *arg);

#endif // TMP117_H
