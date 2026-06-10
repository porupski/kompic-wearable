/**
 * @file boot_tasks.h
 * @brief Task creation — all xTaskCreatePinnedToCore calls in one place.
 *
 * RESPONSIBILITY:
 *   boot_tasks_start() is the single call that creates every FreeRTOS task.
 *   main.c calls it as one line after all hardware and UI are initialised.
 *
 * DOES NOT:
 *   Implement any task function. Task functions live in the file that owns
 *   the relevant hardware or subsystem (see Blueprint 6 §5):
 *
 *     task_gps_fn          → tu10f.c
 *     task_mag_fn          → qmc5883p.c
 *     task_mag_cal_fn      → qmc5883p.c
 *     task_rtc_fn          → pcf85063.c
 *     task_battery_fn      → app_battery.c
 *     task_light_fn        → bh1750.c  (or veml7700.c on premium board)
 *     task_dtap_verify_fn  → boot_hw_init.c  (owns CST816S I2C read)
 *     task_ui_refresh_fn   → lvgl_ui.c
 *     task_power_monitor_fn→ boot_power.c
 *     task_boot_button_fn  → boot_power.c
 *     task_settings_saver_fn → app_nvs.c (drains ui_settings_save_q)
 *
 * ADDING A NEW MODULE:
 *   1. Declare the task function extern here.
 *   2. Add one row to the task table in boot_tasks.c.
 *   That is all.
 */

#ifndef BOOT_TASKS_H
#define BOOT_TASKS_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief Create and start all application tasks.
 *
 * Tasks that depend on hw_alive flags check broker_xxx_hw_alive() at runtime
 * and idle gracefully if their hardware is absent — no conditional task creation.
 *
 * @param settings_save_q  The async NVS save queue created by
 *                         app_nvs_settings_queue_create(). Passed as the
 *                         pvParameters argument to task_settings_saver_fn.
 */
void boot_tasks_start(QueueHandle_t settings_save_q);

#endif // BOOT_TASKS_H