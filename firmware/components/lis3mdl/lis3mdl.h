/**
 * @file lis3mdl.h
 * @brief ST LIS3MDLTR 3-axis Magnetometer -- I2C bus 1, hard-iron + LRA cal.
 *
 * Hardware:
 *   Chip       : ST LIS3MDLTR (3-axis magnetometer, ±4/±8/±12/±16 gauss FS)
 *   I2C bus    : 1  (GPIO1 SDA, GPIO2 SCL, 400 kHz)
 *   I2C addr   : 0x1C (SA1 = GND -- JP6 default per v7.2 line 322)
 *   DRDY pin   : not routed on Mk I (polled via STATUS_REG instead)
 *   WHO_AM_I   : 0x3D at REG 0x0F
 *   Power      : VDD 3V3, VDD_IO 1V8 (per v7.2 lines 232-233)
 *
 * Replaces the v5 QMC5883P driver at the chip layer. The broker_mag_data_t
 * shape, the hard-iron min/max calibration sweep, the heading-from-atan2
 * computation, and the compass_tile API all carry forward. The chip layer
 * is wire-incompatible (different WHO_AM_I, different register map,
 * different sensitivity / range).
 *
 * LRA proximity (v7.2 line 282 / line 432):
 *   The haptic LRA sits ~6-7 mm below the LIS3MDLTR. Expect a static DC
 *   offset of ~1 gauss. Hard-iron cal (min/max sweep) subtracts it.
 *   Datasheet ±4 gauss FS leaves >2 G of headroom for Earth's field + offset.
 *
 * Architecture: Blueprint 1 §1, Blueprint 4 §3, Blueprint 9.
 */

#ifndef LIS3MDL_H
#define LIS3MDL_H

#include "esp_err.h"
#include "driver/i2c.h"
#include <stdint.h>
#include <stdbool.h>

// -- Identity (used by compass_tile.c) ----------------------------------------
const char *lis3mdl_get_chip_name(void);   // returns "LIS3MDLTR"
const char *lis3mdl_get_chip_desc(void);   // returns "3-axis magnetometer"

// -- Wire-level constants -----------------------------------------------------
#define LIS3MDL_ADDR              0x1C     // SA1 = GND
#define LIS3MDL_WHO_AM_I_VAL      0x3D

// -- Register map (per ST DM00075867; [DSV] bench-confirm) --------------------
#define LIS3MDL_REG_WHO_AM_I      0x0F
#define LIS3MDL_REG_CTRL1         0x20     // TEMP_EN, OM[1:0], DO[2:0], FAST_ODR, ST
#define LIS3MDL_REG_CTRL2         0x21     // FS[1:0], REBOOT, SOFT_RST
#define LIS3MDL_REG_CTRL3         0x22     // LP, SIM, MD[1:0]
#define LIS3MDL_REG_CTRL4         0x23     // OMZ[1:0], BLE
#define LIS3MDL_REG_CTRL5         0x24     // FAST_READ, BDU
#define LIS3MDL_REG_STATUS        0x27
#define LIS3MDL_REG_OUT_X_L       0x28     // XL, XH, YL, YH, ZL, ZH (use 0x80 | reg for auto-inc)
#define LIS3MDL_REG_OUT_TEMP_L    0x2E
#define LIS3MDL_REG_INT_CFG       0x30

#define LIS3MDL_AUTO_INC          0x80     // OR into reg address for multi-byte reads

// CTRL1 bits
#define CTRL1_OM_UHP_XY           (0x03 << 5)   // X/Y axes ultra-high-perf mode
#define CTRL1_DO_10HZ             (0x04 << 2)   // 10 Hz output rate (matches POLL_MS)
// CTRL2 bits: FS = 00 -> ±4 G (default). Leaves ~3 G headroom over Earth + LRA.
#define CTRL2_FS_4G               (0x00 << 5)
#define CTRL2_SOFT_RST            (1 << 2)
// CTRL3 MD: 00 = continuous, 11 = power-down
#define CTRL3_MD_CONTINUOUS       (0x00)
#define CTRL3_MD_POWER_DOWN       (0x03)
// CTRL4 OMZ: 11 = ultra-high (match CTRL1 X/Y mode)
#define CTRL4_OMZ_UHP             (0x03 << 2)
// CTRL5 BDU
#define CTRL5_BDU                 (1 << 6)

// Sensitivity at ±4 gauss FS per datasheet: 6842 LSB/gauss = 68.42 LSB/uT.
#define LIS3MDL_LSB_PER_UT        68.42f

// -- Module config ------------------------------------------------------------
#define LIS3MDL_POLL_MS           100      // 10 Hz -- matches CTRL1 DO setting

// -- Broker data struct (Blueprint 9 §2) --------------------------------------
// Field names + order preserved verbatim from the old qmc5883p.h so
// compass_tile.c, fusion.c, and any other broker consumer build unchanged.
typedef struct {
    float    x_ut;           // X axis in microtesla
    float    y_ut;           // Y axis in microtesla
    float    z_ut;           // Z axis in microtesla
    float    heading_deg;    // Compass heading 0-360
    uint8_t  cardinal;       // 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
    bool     calibrated;     // True after successful hard-iron sweep
    bool     calibrating;    // True during active calibration run
    uint8_t  cal_countdown;  // Seconds remaining in calibration (0 idle)

    // Mandatory bookkeeping
    uint32_t last_update_ms;
    bool     enabled;
} broker_mag_data_t;

#define BROKER_MAG_TIMEOUT_MS  2000U

// -- Calibration struct -------------------------------------------------------
typedef struct {
    float offset_x;
    float offset_y;
    float scale_x;
    float scale_y;
    float scale_avg;
    bool  calibrated;
} lis3mdl_calibration_t;

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Verify WHO_AM_I (0x3D), soft-reset, configure for continuous mode
 *        (±4 G, 10 Hz, UHP X/Y/Z, BDU). Caller must NOT hold g_i2c_mutex.
 *        Called from boot_hw_init.c after I2C scan confirms 0x1C alive.
 */
esp_err_t lis3mdl_init(i2c_port_t i2c_num);

/** @brief Set MD = power-down (~1 uA). */
void lis3mdl_deinit(void);

/** @brief Sensor task -- 10 Hz; reads XYZ, applies calibration, writes broker. */
void task_mag_fn(void *arg);

/** @brief Calibration task -- background figure-8 rotation capture. */
void task_mag_cal_fn(void *arg);

// -- Measurement --------------------------------------------------------------

/** @brief Burst-read 6 bytes from OUT_X_L (auto-incremented), scale to uT.
 *         Caller MUST hold g_i2c_mutex. */
esp_err_t lis3mdl_read_raw(i2c_port_t i2c_num, float *x_ut, float *y_ut, float *z_ut);

/** @brief Heading from horizontal X/Y components + magnetic declination (deg).
 *         Returns 0-360. Pure function. */
float lis3mdl_calculate_heading(float x_ut, float y_ut, float declination_deg);

// -- Power management ---------------------------------------------------------

esp_err_t lis3mdl_set_standby(i2c_port_t i2c_num);
esp_err_t lis3mdl_wake(i2c_port_t i2c_num);

// -- Calibration --------------------------------------------------------------

/** @brief Seed calibration from NVS at boot. */
void lis3mdl_seed_calibration(float offset_x, float offset_y,
                               float scale_x,  float scale_y,
                               float scale_avg);

void lis3mdl_start_calibration(lis3mdl_calibration_t *cal);
void lis3mdl_update_calibration(lis3mdl_calibration_t *cal, float x_ut, float y_ut);
esp_err_t lis3mdl_finish_calibration(lis3mdl_calibration_t *cal);

/**
 * @brief Measure the static DC offset from the on-board LRA. Place the device
 *        flat in a magnetically clean location; the function takes N samples,
 *        averages them, and returns the average XY field as the LRA offset.
 *        Caller must NOT hold g_i2c_mutex. ~1 second total.
 */
esp_err_t lis3mdl_measure_lra_offset(i2c_port_t i2c_num,
                                      float *out_offset_x_ut,
                                      float *out_offset_y_ut,
                                      float *out_offset_z_ut);

#endif // LIS3MDL_H
