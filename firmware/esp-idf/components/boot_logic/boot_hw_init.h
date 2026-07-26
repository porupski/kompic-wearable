/**
 * @file boot_hw_init.h
 * @brief Hardware bringup -- I2C buses, WHO_AM_I probe, driver init,
 *        broker hw_alive latching, cross-driver wiring.
 *
 * Two mutex handles are exposed for driver code that serialises I2C
 * traffic per bus:
 *   g_i2c_mutex  -- I2C bus 0 (SDA=1 / SCL=2), sensors + RTC
 *   g_i2c2_mutex -- I2C bus 1 (SDA=4 / SCL=5), DRV2605 + BQ25619
 *
 * Call boot_hw_init() ONCE from main.c, after broker_init() /
 * cross_driver_init() / app_nvs_init() and BEFORE boot_tasks_start().
 */

#ifndef BOOT_HW_INIT_H
#define BOOT_HW_INIT_H


// Driver version: MAJOR.MINOR.PATCH -- bump PATCH on any change here,
// MINOR on feature adds, MAJOR on release quality (beta / RC / GA).
#define BOOT_LOGIC_DRIVER_VERSION  "0.3.0"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

// -- I2C pin map (v7.2 §GPIO ASSIGNMENT) --------------------------------------
#define BOOT_I2C0_SDA_GPIO   1
#define BOOT_I2C0_SCL_GPIO   2
#define BOOT_I2C1_SDA_GPIO   4
#define BOOT_I2C1_SCL_GPIO   5
#define BOOT_I2C_FREQ_HZ     400000

// -- I2C mutex handles (defined in boot_hw_init.c) ----------------------------
extern SemaphoreHandle_t g_i2c_mutex;
extern SemaphoreHandle_t g_i2c2_mutex;

/**
 * @brief Full hardware bringup.
 *        Installs both I2C buses, creates the mutexes, WHO_AM_I probes every
 *        chip, calls each driver's _init() and latches
 *        broker_xxx_set_hw_status(true) on success, initialises the
 *        non-I2C peripherals (encoder / WS2812 / flashlight / SD / mic PDM /
 *        haptic queue), and registers cross-driver callbacks.
 *
 *        Best-effort per driver: a WHO_AM_I miss on one chip does not abort
 *        the whole bringup; broker_xxx_set_hw_status(false) is left as-is and
 *        that chip's task will idle at runtime.
 *
 * @param cal  NVS-loaded calibration blob (mag hard-iron etc.). Passed to
 *             drivers that need seeded coefficients.
 */
void boot_hw_init(const app_calibration_t *cal);

#ifdef __cplusplus
}
#endif

#endif // BOOT_HW_INIT_H
