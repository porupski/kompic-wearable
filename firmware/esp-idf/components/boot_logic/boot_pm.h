/**
 * @file boot_pm.h
 * @brief Stage 11 Item D Block A -- ESP32-S3 dynamic frequency scaling +
 *        light-sleep enable at boot.
 *
 * This is a THIN wrapper around esp_pm_configure. Kept here (not in driver
 * components) so PM policy lives with the rest of the boot orchestration.
 *
 * Prerequisites (must be clicked in menuconfig):
 *   CONFIG_PM_ENABLE=y
 *   CONFIG_PM_DFS_INIT_AUTO=y
 *   CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
 *   CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
 *   CONFIG_PM_SLP_IRAM_OPT=y   (recommended, tiny code cost)
 *
 * Without CONFIG_PM_ENABLE, esp_pm_configure() is a no-op that returns OK.
 * Safe to leave this call in the boot path even before the sdkconfig is set.
 *
 * Concurrency:
 *   Any driver that must hold APB at MAX_FREQ during a critical section
 *   (RMT wire timing, SDMMC, PDM I2S) creates a `esp_pm_lock_handle_t` and
 *   acquires ESP_PM_APB_FREQ_MAX around the critical work. See ws2812.c +
 *   sdcard.c for the reference pattern.
 *
 * Blueprint 1 §1 -- boot orchestration lives in boot_logic/.
 */

#ifndef BOOT_PM_H
#define BOOT_PM_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure DFS 240/40 MHz + auto light-sleep. Idempotent.
 *
 * Must be called AFTER boot_power_init (GPIO0 driven HIGH) and BEFORE any
 * task that owns a PM lock is created -- so the safe slot in main.c is
 * between boot_power_init() and boot_hw_init(). Ordering matters only
 * because the RTOS scheduler must not have started; the PM policy is
 * observed by every subsequent vTaskDelay() >= FREERTOS_IDLE_TIME_BEFORE_SLEEP
 * ticks.
 *
 * @return ESP_OK if PM is enabled (or disabled at compile-time -- no-op OK).
 *         ESP-IDF error otherwise; we log but continue booting.
 */
esp_err_t boot_pm_init(void);

/**
 * @brief Dump active PM locks to stdout. Requires CONFIG_PM_PROFILING=y in
 *        menuconfig, otherwise prints a stub line. Use to diagnose "why is
 *        light-sleep not engaging" -- any NO_LIGHT_SLEEP lock held by a
 *        driver will show up here.
 *
 * Call after boot_hw_init to see the state established by all peripheral
 * inits (I2C, RMT, SDMMC, etc.).
 */
void boot_pm_dump_locks(void);

#ifdef __cplusplus
}
#endif

#endif // BOOT_PM_H
