/**
 * @file boot_tasks.c
 * @brief Task creation table - all xTaskCreatePinnedToCore calls in one place.
 *
 * Phase 1 (display-less field-capture port): only sensor + haptic + power-button
 * tasks are started. task_ui_refresh_fn, task_touch_fn, task_gps_fn, and
 * task_settings_saver_fn are held out until the display path returns; adding
 * them back is one line each.
 *
 * task_field_capture_fn is not created here yet — the field_capture component
 * lands in Phase 2 and its extern goes in the Core 1 slot below.
 *
 * CORE ASSIGNMENT (Blueprint 1 §1):
 *   Core 0 → sensor acquisition
 *   Core 1 → application logic (field capture; UI when display returns)
 *   tskNO_AFFINITY → utility tasks with no real-time constraint
 */

#include "boot_tasks.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "BOOT_TASKS";

// --- External task function declarations --------------------------------------
// One line per task; symbol lives in the module that owns the hardware.

// Core 0 - sensor tasks (iv7.1 chip set)
extern void task_env_fn(void *arg);          // bme688/bme688_drv.c
extern void task_imu_fn(void *arg);          // lsm6dsv16x/lsm6dsv16x.c
extern void task_mag_fn(void *arg);          // lis3mdl/lis3mdl.c
extern void task_mag_cal_fn(void *arg);      // lis3mdl/lis3mdl.c
extern void task_hr_fn(void *arg);           // max30101/max30101.c
extern void task_skin_fn(void *arg);         // tmp117/tmp117.c
extern void task_light_fn(void *arg);        // veml6030/veml6030.c
extern void task_battery_fn(void *arg);      // bq25619/bq25619.c
extern void task_rtc_fn(void *arg);          // pcf85063/pcf85063.c
extern void task_haptic_fn(void *arg);       // drv2605/haptic.c
extern void task_alarm_fn(void *arg);        // alarm/alarm.c

// Core 1 - application logic
extern void task_field_capture_fn(void *arg);      // field_capture/field_capture.c

// Priority shutdown watcher -- owns double-click, force-ships. Independent
// of field_capture so a stuck recording / mic capture cannot block it.
extern void task_shutdown_watcher_fn(void *arg);   // field_capture/field_capture.c

// Small stdin-reader for RTC CLI: SET_TIME <ISO> and GET_TIME on USB-CDC.
extern void task_rtc_cli_fn(void *arg);            // field_capture/field_capture.c

// BLACKBOX telemetry logger. Self-terminates at startup if cfg_sys.blackbox
// is off OR batt_test is on -- unconditionally created here.
extern void task_blackbox_fn(void *arg);           // field_capture/field_capture.c

// -- Held out for later phases (uncomment when re-enabling) -------------------
// extern void task_power_btn_fn(void *arg);    // GPIO16 owned by field_capture in Phase 2
// extern void task_fusion_fn(void *arg);        // depends on IMU + MAG + BARO stability
// extern void task_gps_fn(void *arg);           // GPS module offline (broken connector)
// extern void task_ui_refresh_fn(void *arg);    // display path down
// extern void task_touch_fn(void *arg);         // display path down
// extern void task_settings_saver_fn(void *arg);// ui_broker save queue not wired in Phase 2

// --- Task descriptor table ----------------------------------------------------

typedef struct {
    const char    *name;
    TaskFunction_t fn;
    uint32_t       stack_bytes;
    UBaseType_t    priority;
    BaseType_t     core;
} task_entry_t;

static const task_entry_t task_table[] = {

    // -- CORE 0 - Sensor acquisition ------------------------------------------
    { "task_env",    task_env_fn,    4096, 2, 0 },
    { "task_imu",    task_imu_fn,    4096, 4, 0 },
    { "task_mag",    task_mag_fn,    4096, 4, 0 },
    { "task_magcal", task_mag_cal_fn,4096, 2, 0 },
    { "task_hr",     task_hr_fn,     4096, 2, 0 },
    { "task_skin",   task_skin_fn,   3072, 3, 0 },
    { "task_light",  task_light_fn,  3072, 3, 0 },
    { "task_bat",    task_battery_fn,3072, 3, 0 },
    { "task_rtc",    task_rtc_fn,    4096, 4, 0 },
    { "task_haptic", task_haptic_fn, 4096, 3, 0 },
    { "task_alarm",  task_alarm_fn,  4096, 2, 0 },

    // -- CORE 1 - Application logic -------------------------------------------
    { "task_field",  task_field_capture_fn, 8192, 4, 1 },  // owns button + encoder + SD I/O

    // -- UNPINNED - Utility tasks ---------------------------------------------
    // task_power_btn omitted: GPIO16 is owned by task_field_capture_fn
    // (single-click) and task_shutdown_watcher_fn (double-click).
    //
    // Shutdown watcher runs at priority 6 (higher than field_capture=4) so
    // even if the app task is mid-blocking-loop, the watcher preempts and
    // fires ship mode within one 5 ms poll cycle.
    { "task_shutdn", task_shutdown_watcher_fn, 3072, 6, tskNO_AFFINITY },

    // RTC CLI on stdin. Low priority; only wakes on serial input.
    { "task_rtccli", task_rtc_cli_fn,          3072, 2, tskNO_AFFINITY },

    // BLACKBOX -- Stage 11 background telemetry logger. Pinned Core 0
    // (utility side). Priority 1 so it never contends with sensors. Stack
    // 4096 (fprintf into ~35 fields is generous). Task self-exits when
    // cfg_sys.blackbox=0.
    { "task_bbox",   task_blackbox_fn,         4096, 1, 0 },
};
#define TASK_COUNT (sizeof(task_table) / sizeof(task_table[0]))

// --- Public entry point -------------------------------------------------------

void boot_tasks_start(QueueHandle_t settings_save_q)
{
    (void)settings_save_q;  // reserved for Phase 2 field_capture / settings saver

    ESP_LOGI(TAG, "Creating %d tasks...", (int)TASK_COUNT);

    for (int i = 0; i < (int)TASK_COUNT; i++) {
        const task_entry_t *t = &task_table[i];

        BaseType_t ret;
        if (t->core == tskNO_AFFINITY) {
            ret = xTaskCreate(t->fn, t->name, t->stack_bytes,
                              NULL, t->priority, NULL);
        } else {
            ret = xTaskCreatePinnedToCore(t->fn, t->name, t->stack_bytes,
                                          NULL, t->priority, NULL, t->core);
        }

        if (ret != pdPASS) {
            ESP_LOGE(TAG, "FATAL: Failed to create task '%s' (stack=%lu, pri=%lu, core=%d)",
                     t->name, (unsigned long)t->stack_bytes,
                     (unsigned long)t->priority, (int)t->core);
            configASSERT(false);
        }
    }

    ESP_LOGI(TAG, "All tasks created:");
    ESP_LOGI(TAG, "  Core 0: ENV | IMU | MAG | MAG_CAL | HR | SKIN | LIGHT | BAT | RTC | HAPTIC | ALARM");
    ESP_LOGI(TAG, "  Core 1: FIELD_CAPTURE");
    ESP_LOGI(TAG, "  Unpinned: SHUTDOWN_WATCHER (priority double-click ship mode) | RTC_CLI (stdin SET_TIME/GET_TIME)");
}
