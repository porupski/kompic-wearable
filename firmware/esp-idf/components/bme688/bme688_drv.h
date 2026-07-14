/**
 * @file bme688_drv.h
 * @brief Bosch BME688 environment + gas sensor driver -- Core 0 only.
 *
 * Replaces the bme280_drv at the chip layer. The BME688 is the next-generation
 * Bosch combo sensor: same I2C address (0x76), pin-compatible footprint, and
 * still does T/P/H -- but adds a hot-plate VOC gas sensor with a programmable
 * heater profile. The Bosch BME68x library exposes a stateful API with
 * callback-driven I2C; we wire those callbacks to ESP-IDF's i2c master driver.
 *
 * The broker_env_data_t shape carries forward verbatim from bme280_drv.h so
 * env_tile.c builds with only an include + identity-call rename.
 *
 * Wire shape:
 *   Bus     : I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
 *   Address : 0x76 (BME68X_I2C_ADDR_LOW per Bosch)
 *   INT pin : not routed on Mk I
 *
 * Heater behaviour:
 *   Forced-mode measurement triggers the on-chip heater for a programmable
 *   duration (default profile: 320 C for 150 ms). Gas resistance is sampled
 *   at the end of the heater pulse and read on the next forced-mode read.
 *   The whole forced-mode cycle blocks ~200 ms on I2C bus 1 (heater wait is
 *   on-chip, but we cannot release the bus during it -- see [DEFECT-002]).
 *
 * Architecture: Blueprint 1 §8, Blueprint 4, Blueprint 5, Blueprint 14a
 */

#ifndef BME688_DRV_H
#define BME688_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ── Broker data struct (driver owns shape, broker stores it) ──────────────────
// Field-for-field preserved from the old bme280_drv.h. env_tile.c builds
// with only the include swap + identity-call rename.
typedef struct {
    // Payload
    float    temperature_c;       // Degrees Celsius
    float    humidity_pct;        // Relative humidity %
    float    pressure_hpa;        // Atmospheric pressure in hPa
    float    altitude_m;          // Baro altitude (hypsometric, P0 = 1013.25 hPa)
    float    gas_resistance_ohm;  // BME688 gas sensor reading (kOhms typical)
    bool     gas_valid;           // true when the gas reading is stable

    // Zero Height reference -- written by Core 1 (UI button), preserved by Core 0 RBW
    float    home_ref_altitude_m;
    bool     home_ref_valid;

    // Bookkeeping
    uint32_t last_update_ms;
    bool     enabled;
} broker_env_data_t;

// Timeout: task runs at 0.5 Hz (2 s period). Allow 5 missed reads (10 s).
#define BROKER_ENV_TIMEOUT_MS   10000

// ── Identity ──────────────────────────────────────────────────────────────────
const char *bme688_get_chip_name(void);   // "BME688"
const char *bme688_get_chip_desc(void);   // "T/H/P/Gas"

// ── Driver API (called inside g_i2c_mutex from task_env_fn) ──────────────────

/**
 * @brief Initialise the BME688 using the Bosch BME68x library.
 *        Verifies WHO_AM_I (chip_id 0x61), runs soft reset, configures the
 *        T/P/H oversampling registers, programs the default heater profile.
 *        Must be called with g_i2c_mutex held.
 *
 * @param i2c_port I2C port (I2C_NUM_0)
 * @return ESP_OK on success
 */
esp_err_t bme688_drv_init(int i2c_port);

/**
 * @brief Trigger one forced-mode measurement (T/P/H + gas) and return results.
 *        Blocks ~200 ms inside this call (the heater pulse alone is 150 ms,
 *        plus T/P/H oversampling + I2C wire time).
 *        Must be called with g_i2c_mutex held for the full duration.
 *
 * @param temp_c    Output: temperature in C
 * @param hum_pct   Output: relative humidity %
 * @param press_hpa Output: pressure in hPa
 * @param gas_ohm   Output: gas resistance in Ohms (0.0 if heater is unstable)
 * @param gas_valid Output: true if the chip's gas_valid + heat_stable flags are set
 * @return ESP_OK on success
 */
esp_err_t bme688_read_forced(float *temp_c, float *hum_pct,
                              float *press_hpa, float *gas_ohm,
                              bool  *gas_valid);

/**
 * @brief Hypsometric altitude from pressure (unchanged from BME280 era).
 *        alt = 44330 x (1 - (P / P0)^0.190295)
 */
float bme688_pressure_to_altitude(float pressure_hpa, float sea_level_hpa);

// ── FreeRTOS task (pinned to Core 0 by boot_tasks.c) ─────────────────────────
void task_env_fn(void *arg);

#endif // BME688_DRV_H
