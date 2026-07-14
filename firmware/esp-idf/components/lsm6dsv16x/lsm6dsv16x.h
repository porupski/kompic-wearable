/**
 * @file lsm6dsv16x.h
 * @brief ST LSM6DSV16X 6-axis IMU -- I2C bus 1, INT1 motion wake.
 *
 * Hardware:
 *   Chip       : ST LSM6DSV16X (accel + gyro + embedded functions)
 *   I2C bus    : 1  (GPIO1 SDA, GPIO2 SCL, 400 kHz)
 *   I2C addr   : 0x6B (fixed)
 *   INT1 pin   : GPIO8 (RTC-wake-capable; wake-on-motion / raise-to-wake)
 *   INT2 pin   : not routed on Mk I -- ignore
 *   WHO_AM_I   : 0x70 at REG_WHO_AM_I (0x0F)
 *   Power      : VDD 3V3 (analog), V_IO 1V8 (digital -- per v7.2 line 233)
 *
 * Replaces the old QMI8658 driver verbatim at the broker-payload + tile-API
 * level. The broker_imu_data_t shape, the complementary-filter logic, and
 * the haptic.c accel_z consumer all carry forward unchanged. Only the chip
 * layer (WHO_AM_I, register map, scaling) is new.
 *
 * Architecture: Blueprint 1 §1 (Core 0 owns I2C), Blueprint 4 §3 (read-before-write).
 *
 * Datasheet: ST DocID DS13176 / AN5824. Register addresses tagged [DSV]
 * (datasheet-verify) below are family-conventional for the LSM6DSV / DSV16X
 * line; bench bring-up confirms.
 */

#ifndef LSM6DSV16X_H
#define LSM6DSV16X_H

#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>

// -- Identity (used by imu_tile.c) --------------------------------------------
const char *lsm6dsv16x_get_chip_name(void);  // returns "LSM6DSV16X"
const char *lsm6dsv16x_get_chip_desc(void);  // returns "6-axis IMU + sensor fusion"

// -- Wire-level constants -----------------------------------------------------
#define LSM6DSV16X_I2C_ADDR       0x6B
#define LSM6DSV16X_WHO_AM_I_VAL   0x70
#define LSM6DSV16X_INT1_GPIO      GPIO_NUM_8

#define LSM6DSV16X_POLL_MS        20    // 50 Hz -- watch use case; gyro ODR set higher

// -- Register map (per ST DS13176; [DSV] -- bench confirms) -------------------
#define REG_WHO_AM_I              0x0F
#define REG_PIN_CTRL              0x02
#define REG_FIFO_CTRL1            0x07
#define REG_INT1_CTRL             0x0D
#define REG_INT2_CTRL             0x0E
#define REG_CTRL1                 0x10  // accel ODR + FS
#define REG_CTRL2                 0x11  // gyro  ODR + FS
#define REG_CTRL3                 0x12  // BDU, IF_INC, SW_RESET, BOOT
#define REG_CTRL4                 0x13
#define REG_CTRL5                 0x14
#define REG_CTRL6                 0x15
#define REG_CTRL7                 0x16
#define REG_CTRL8                 0x17
#define REG_CTRL9                 0x18
#define REG_CTRL10                0x19
#define REG_OUT_TEMP_L            0x20
#define REG_OUT_TEMP_H            0x21
#define REG_OUTX_L_G              0x22  // gyro X low byte; burst-read 12 bytes for XYZ_G + XYZ_XL
#define REG_FUNC_CFG_ACCESS       0x01
#define REG_WAKE_UP_THS           0x5B  // wake threshold [DSV]
#define REG_WAKE_UP_DUR           0x5C
#define REG_TAP_CFG0              0x56
#define REG_TAP_CFG1              0x57
#define REG_TAP_CFG2              0x58
#define REG_MD1_CFG               0x5E  // INT1 routing for embedded functions

// -- Control-register bit positions (subset; [DSV]) ---------------------------
#define CTRL3_SW_RESET            (1 << 0)
#define CTRL3_IF_INC              (1 << 2)
#define CTRL3_BDU                 (1 << 6)
#define CTRL3_BOOT                (1 << 7)

// CTRL1: ODR_XL[7:4], FS_XL[3:2]
#define CTRL1_ODR_120HZ           (0x06 << 4)    // [DSV]
#define CTRL1_FS_XL_4G            (0x02 << 0)    // [DSV] -- ST family: 00=2g, 10=4g, 11=8g, 01=16g
// CTRL2: ODR_G[7:4],  FS_G[3:1]
#define CTRL2_ODR_240HZ           (0x07 << 4)    // [DSV]
#define CTRL2_FS_G_2000DPS        (0x07 << 1)    // [DSV] -- max range; conservative for watch

// INT1_CTRL: bit 0 DRDY_XL, bit 1 DRDY_G; INT1_CTRL bit 7 enables embedded INT routing
#define INT1_DRDY_XL              (1 << 0)
#define INT1_DRDY_G               (1 << 1)

// MD1_CFG: bit 5 = INT1_WU (wake-up routed to INT1)
#define MD1_INT1_WU               (1 << 5)

// TAP_CFG0: bit 0 LIR (latched interrupt -- recommended for wake-up use)
#define TAP_CFG0_LIR              (1 << 0)
// TAP_CFG2: bit 7 = INTERRUPTS_ENABLE
#define TAP_CFG2_INT_EN           (1 << 7)

// -- Scaling (ST datasheet typical; ±4g accel + ±2000 dps gyro) ---------------
// Accel: at ±4g full-scale, sensitivity is 0.122 mg/LSB = 8192 LSB/g.
#define ACCEL_LSB_PER_G           8192.0f
// Gyro: at ±2000 dps full-scale, sensitivity is 70 mdps/LSB ~= 14.29 LSB/dps.
#define GYRO_LSB_PER_DPS          14.286f
// Temperature: 256 LSB/°C with 25 °C offset.
#define TEMP_LSB_PER_C            256.0f
#define TEMP_OFFSET_C             25.0f

#define GRAVITY_MS2               9.80665f

// -- Broker data struct (Blueprint 4 §3) --------------------------------------
// Field names and order preserved EXACTLY from the old qmi8658.h so that
// imu_tile.c, haptic.c, and any other consumer build without changes.
// haptic.c specifically reads broker_imu_data_t.accel_z during sweep
// calibration (it peaks at the LRA resonant frequency).
typedef struct {
    // Accelerometer (m/s²) -- gravity-corrected, ±4g range
    float accel_x;
    float accel_y;
    float accel_z;

    // Gyroscope (°/s) -- ±2000 dps range
    float gyro_x;
    float gyro_y;
    float gyro_z;

    // Complementary-filtered orientation (computed in task, not in tile)
    float roll_deg;
    float pitch_deg;

    // On-chip temperature (°C)
    float temperature;

    // Mandatory bookkeeping (Blueprint 4 §3)
    uint32_t last_update_ms;
    bool     enabled;
} broker_imu_data_t;

#define BROKER_IMU_TIMEOUT_MS  500U

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Verify WHO_AM_I (0x70), soft-reset, configure accel + gyro + INT1.
 *        Caller must NOT hold g_i2c_mutex (takes it internally).
 *        Called from boot_hw_init.c after I2C scan confirms 0x6B alive.
 * @return ESP_OK on success.
 */
esp_err_t lsm6dsv16x_init(i2c_port_t i2c_num);

/**
 * @brief FreeRTOS task. Pinned to Core 0 via boot_tasks.c.
 *        Polls at LSM6DSV16X_POLL_MS interval, applies complementary filter,
 *        writes to broker. Read-before-write so UI-owned `enabled` is preserved.
 *        Stack: 4096 bytes.
 */
void task_imu_fn(void *arg);

/**
 * @brief Burst-read TEMP + GYRO + ACCEL (12 bytes total, plus 2 temp bytes
 *        from 0x20). Scales raw values into out's float fields. Caller holds
 *        g_i2c_mutex. Does NOT touch out->roll_deg/pitch_deg (filter math
 *        lives in the task).
 */
esp_err_t lsm6dsv16x_read(i2c_port_t i2c_num, broker_imu_data_t *out);

/**
 * @brief Configure GPIO8 as a falling-edge interrupt and install an ISR that
 *        notifies the given FreeRTOS task. Used for wake-on-motion. Idempotent.
 *        Pass NULL to uninstall. NOT thread-safe -- call from boot_hw_init.c
 *        before any task is created.
 */
esp_err_t lsm6dsv16x_install_int1_isr(TaskHandle_t notify_task);

#endif // LSM6DSV16X_H
