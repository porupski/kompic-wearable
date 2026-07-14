/**
 * @file veml6030.h
 * @brief Vishay VEML6030 Ambient Light Sensor -- I2C bus 1, auto-ranging.
 *
 * Hardware:
 *   Chip       : Vishay VEML6030 (16-bit ALS, ±I2C interrupt)
 *   I2C bus    : 1  (GPIO1 SDA, GPIO2 SCL, 400 kHz)
 *   I2C addr   : 0x10 (ADD = GND -- v7.2 JP10 default, line 327)
 *   Alt addr   : 0x48 (ADD = 3V3 -- not used; would collide with TMP117)
 *   INT pin    : not routed on Mk I (interrupt register polled if needed)
 *   Power      : VDD 3V3 only (v7.2 line 232; no V_IO split)
 *
 * Replaces the v5 BH1750 driver at the chip layer. The broker_light_data_t
 * shape carries forward verbatim; the auto_brightness percentage field is
 * preserved so light_tile.c builds unchanged (modulo identity-call swap).
 *
 * Wire shape difference: VEML6030 registers are 16-bit, LSB first.
 * BH1750 had no register address space -- you just wrote a command byte.
 *
 * Auto-ranging:
 *   The driver starts at gain=1/4, IT=100 ms (max ~15100 lux). If the raw
 *   ADC saturates (>= 0xFFF0), it drops to gain=1/8 (max ~30200 lux). If the
 *   raw ADC is small (< 100), it raises to gain=1 (better resolution).
 *   The current gain/IT pair drives the lux scaling factor.
 *
 * Architecture: Blueprint 1 §1, Blueprint 4 §3, Blueprint 10.
 */

#ifndef VEML6030_H
#define VEML6030_H

#include "esp_err.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>

// -- Identity (used by light_tile.c) ------------------------------------------
const char *veml6030_get_chip_name(void);  // returns "VEML6030"
const char *veml6030_get_chip_desc(void);  // returns "Ambient light (lux)"

// -- Wire-level constants -----------------------------------------------------
#define VEML6030_ADDR             0x10     // ADD = GND

// -- Register map (per Vishay 84366; all registers 16-bit, LSB first) ---------
#define VEML6030_REG_ALS_CONF     0x00
#define VEML6030_REG_ALS_WH       0x01
#define VEML6030_REG_ALS_WL       0x02
#define VEML6030_REG_POWER_SAVING 0x03
#define VEML6030_REG_ALS          0x04     // ambient light data (lux raw)
#define VEML6030_REG_WHITE        0x05     // white channel (broadband)
#define VEML6030_REG_ALS_INT      0x06     // interrupt status

// ALS_CONF bit layout (per Vishay AN84367):
//   bit  0   : ALS_SD          (1 = shutdown, 0 = power on)
//   bit  1   : ALS_INT_EN      (1 = enable INT)
//   bits 5:4 : ALS_PERS        (1, 2, 4, 8 readings before INT)
//   bits 9:6 : ALS_IT          (integration time)
//   bits 12:11: ALS_GAIN       (00=1x, 01=2x, 10=1/8x, 11=1/4x)
#define CONF_ALS_SD              (1 << 0)
#define CONF_ALS_INT_EN          (1 << 1)

#define CONF_GAIN_MASK           (0x03 << 11)
#define CONF_GAIN_1              (0x00 << 11)
#define CONF_GAIN_2              (0x01 << 11)
#define CONF_GAIN_1_8            (0x02 << 11)
#define CONF_GAIN_1_4            (0x03 << 11)

#define CONF_IT_MASK             (0x0F << 6)
#define CONF_IT_25MS             (0x0C << 6)
#define CONF_IT_50MS             (0x08 << 6)
#define CONF_IT_100MS            (0x00 << 6)
#define CONF_IT_200MS            (0x01 << 6)
#define CONF_IT_400MS            (0x02 << 6)
#define CONF_IT_800MS            (0x03 << 6)

// Gain / IT combinations (used by the auto-range state machine)
typedef enum {
    VEML6030_GAIN_1_8 = 0,
    VEML6030_GAIN_1_4 = 1,
    VEML6030_GAIN_1   = 2,
    VEML6030_GAIN_2   = 3,
} veml6030_gain_t;

typedef enum {
    VEML6030_IT_25MS  = 0,
    VEML6030_IT_50MS  = 1,
    VEML6030_IT_100MS = 2,
    VEML6030_IT_200MS = 3,
    VEML6030_IT_400MS = 4,
    VEML6030_IT_800MS = 5,
} veml6030_it_t;

// Resolution table (lx/count) from Vishay AN84367 §Table 5
// Indexed [gain][it]. Compute at init / on every range change.
extern const float veml6030_resolution_lx_per_count[4][6];

// -- Module config ------------------------------------------------------------
#define VEML6030_POLL_MS          500     // 2 Hz -- ambient light doesn't change fast

// -- Broker data struct (Blueprint 10 §3) -------------------------------------
// Field-for-field preserved from the old bh1750.h. light_tile.c reads .lux
// and .auto_brightness; no consumer needs to change.
typedef struct {
    float    lux;              // EMA-filtered illuminance (lux)
    uint8_t  auto_brightness;  // 1-100% derived from log(lux) curve

    // Mandatory bookkeeping
    uint32_t last_update_ms;
    bool     enabled;
} broker_light_data_t;

#define BROKER_LIGHT_TIMEOUT_MS  3000U

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Configure default gain (1/4x) + IT (100 ms), power on, wait one
 *        integration period for the first valid sample.
 *        Caller must NOT hold g_i2c_mutex.
 */
esp_err_t veml6030_init(i2c_port_t i2c_num);

/** @brief Set ALS_SD = 1 (shutdown). */
void veml6030_deinit(void);

// -- Measurement (caller holds g_i2c_mutex) -----------------------------------

esp_err_t veml6030_read_raw(i2c_port_t i2c_num, uint16_t *raw_out);

esp_err_t veml6030_set_range(i2c_port_t i2c_num,
                              veml6030_gain_t gain,
                              veml6030_it_t   it);

/** @brief Get the lux/count factor for the current configured range. */
float veml6030_current_resolution(void);

/** @brief Get the current configured gain / IT. */
void veml6030_get_range(veml6030_gain_t *gain_out, veml6030_it_t *it_out);

// -- Task ---------------------------------------------------------------------
void task_light_fn(void *arg);

// -- Lux-to-brightness curve (pure, exposed for testing) ----------------------
/**
 * @brief Map lux to backlight percentage via clamped log10 curve.
 *        Output range [5, 100]. Designed for a wide range (0..5000+ lux).
 */
uint8_t veml6030_lux_to_brightness(float lux);

#endif // VEML6030_H
