/**
 * @file boot_tasks.c
 * @brief Task creation table - all xTaskCreatePinnedToCore calls in one place.
 *
 * This file contains ONLY task creation. No task logic lives here.
 * Task functions are declared extern and defined in their respective modules.
 *
 * CORE ASSIGNMENT (Blueprint 1 §1 - The One Rule):
 *   Core 0 → all sensor acquisition tasks (write to broker)
 *   Core 1 → UI refresh task only (reads from broker, drives LVGL)
 *   tskNO_AFFINITY → utility tasks with no real-time constraint
 *
 * ADDING A NEW SENSOR TASK:
 *   1. Declare its task function extern below.
 *   2. Add one row to the task table.
 *   3. Nothing else changes anywhere.
 *
 * STACK SIZING NOTE:
 *   Stacks live in internal SRAM (fast, interrupt-safe). Do NOT use PSRAM for stacks.
 *   GPS task is 8 KB because NMEA parsing uses local char buffers.
 *   Haptic task is 4 KB: run_sweep() holds I2C mutex across vTaskDelay calls
 *     and IMU amplitude sampling adds stack frames.
 *   IMU task is 4 KB: qmi8658_read() uses 14-byte local buffer + math.
 *   All others sized conservatively. Profile with uxTaskGetStackHighWaterMark()
 *   during integration testing and trim if needed.
 */

#include "boot_tasks.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "BOOT_TASKS";

// --- External task function declarations --------------------------------------
// Each function is defined in the module file that owns the hardware.
// boot_tasks.c only knows their signatures.

// Core 0 - sensor tasks
extern void task_gps_fn(void *arg);          // tu10f.c
extern void task_mag_fn(void *arg);          // qmc5883p.c
extern void task_mag_cal_fn(void *arg);      // qmc5883p.c
extern void task_rtc_fn(void *arg);          // pcf85063.c
extern void task_battery_fn(void *arg);      // app_battery.c
extern void task_light_fn(void *arg);        // bh1750.c
extern void task_haptic_fn(void *arg);       // haptic.c
extern void task_imu_fn(void *arg);          // qmi8658.c
extern void task_env_fn(void *arg);          // bme280.c
extern void task_hr_fn(void *arg);           // max30102.c

extern void task_fusion_fn(void *arg);
extern void task_alarm_fn(void *arg);        // alarm.c

// Core 1 - UI task
extern void task_ui_refresh_fn(void *arg);   // lvgl_ui.c

// Unpinned - utility tasks
extern void task_power_btn_fn(void *arg);       // boot_power.c (v7.2: GPIO16, ISR-driven)
extern void task_settings_saver_fn(void *arg);  // app_nvs.c

// --- Task descriptor table ----------------------------------------------------
// One row per task. Columns: name, function, stack (bytes), priority, core affinity.
//
// Priority scale: 1 (idle+1) … configMAX_PRIORITIES-1. Higher = more urgent.
// tskNO_AFFINITY (-1) = scheduler places freely across both cores.

typedef struct {
    const char    *name;
    TaskFunction_t fn;
    uint32_t       stack_bytes;
    UBaseType_t    priority;
    BaseType_t     core;
} task_entry_t;

static const task_entry_t task_table[] = {

    // -- CORE 0 - Sensor acquisition ------------------------------------------
    { "task_gps",    task_gps_fn,    8192, 4, 0 },  // 8 KB: NMEA char buffers
    { "task_mag",    task_mag_fn,    4096, 4, 0 },
    { "task_magcal", task_mag_cal_fn,4096, 2, 0 },  // Lower pri - background calibration
    { "task_rtc",    task_rtc_fn,    4096, 4, 0 },
    { "task_bat",    task_battery_fn,3072, 3, 0 },  // 10 s interval - low urgency
    { "task_light",  task_light_fn,  3072, 3, 0 },
    { "task_haptic", task_haptic_fn, 4096, 3, 0 },  // 4 KB: sweep holds mutex + IMU sampling
    { "task_imu",    task_imu_fn,    4096, 4, 0 },  // 4 KB: 14-byte raw read + complementary filter
    { "task_env",    task_env_fn,    4096, 2, 0 },
    { "task_hr",     task_hr_fn,     4096, 2, 0 },
    {"task_alarm",   task_alarm_fn,  4096, 2, 0 },

    { "task_fusion", task_fusion_fn, 4096, 2, 0 },
    // -- ADD NEW SENSOR TASK HERE ---------------------------------------------

    // -- CORE 1 - UI refresh ---------------------------------------------------
    { "task_ui",     task_ui_refresh_fn, 8192, 5, 1 },

    // -- UNPINNED - Utility tasks ----------------------------------------------
    { "task_power_btn", task_power_btn_fn,    3072, 5, tskNO_AFFINITY },
    { "task_nvssave",   task_settings_saver_fn, 2048, 2, tskNO_AFFINITY },
};
#define TASK_COUNT (sizeof(task_table) / sizeof(task_table[0]))

// --- Public entry point -------------------------------------------------------

void boot_tasks_start(QueueHandle_t settings_save_q)
{
    ESP_LOGI(TAG, "Creating %d tasks...", (int)TASK_COUNT);

    for (int i = 0; i < (int)TASK_COUNT; i++) {
        const task_entry_t *t = &task_table[i];

        void *param = NULL;
        if (t->fn == task_settings_saver_fn) {
            param = (void *)settings_save_q;
        }

        BaseType_t ret;
        if (t->core == tskNO_AFFINITY) {
            ret = xTaskCreate(t->fn, t->name, t->stack_bytes,
                              param, t->priority, NULL);
        } else {
            ret = xTaskCreatePinnedToCore(t->fn, t->name, t->stack_bytes,
                                          param, t->priority, NULL, t->core);
        }

        if (ret != pdPASS) {
            ESP_LOGE(TAG, "FATAL: Failed to create task '%s' (stack=%lu, pri=%lu, core=%d)",
                     t->name, (unsigned long)t->stack_bytes,
                     (unsigned long)t->priority, (int)t->core);
            configASSERT(false);
        }
    }

    ESP_LOGI(TAG, "All tasks created:");
    ESP_LOGI(TAG, "  Core 0: GPS | MAG | MAG_CAL | RTC | BAT | LIGHT | HAPTIC | IMU | ENV | HR | FUSION | ALARM");
    ESP_LOGI(TAG, "  Core 1: UI_REFRESH");
    ESP_LOGI(TAG, "  Unpinned: POWER_BTN | NVS_SAVE");
}