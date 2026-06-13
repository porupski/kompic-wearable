/**
 * @file main.c
 * @brief Application entry point - orchestrator only.
 *
 * Calls sub-files in strict boot sequence (Blueprint 1 §7, Blueprint 6 §3).
 * Contains ZERO hardware logic. ZERO task definitions. ZERO LVGL calls.
 *
 * If you find yourself adding hardware logic here, it belongs in a sub-file:
 *   Hardware init    → boot_hw_init.c
 *   Display / LVGL   → boot_display.c / lvgl_ui.c
 *   Task creation    → boot_tasks.c
 *   NVS persistence  → app_nvs.c
 *   Power / GPIO     → boot_power.c
 *
 * Boot sequence:
 *  1.  GPIO41 HIGH                    - power latch, must be first
 *  2.  broker_init()                  - mutex + zero structs, before any driver
 *  3.  app_nvs_init()                 - nvs_flash_init, erase if corrupt
 *  4.  app_nvs_load_ui_settings()     - load before applying to atomics
 *  5.  Apply ui_settings to atomics   - g_ui_theme, g_blue_light_on, g_saved_brightness
 *  6.  boot_hw_init()                 - I2C scan + WHO_AM_I + driver init + GPS + ADC
 *  7.  boot_display_init()            - SPI, ST7789, LEDC backlight, PSRAM bufs, LVGL port
 *  8.  lvgl_ui_init()                 - build all screens, apply theme from frame 0
 *  9.  ui_broker_init()               - create async NVS save queue
 *  10. boot_tasks_start()             - all FreeRTOS tasks; scheduler takes over
 */

#include "boot_power.h"
#include "boot_hw_init.h"
#include "boot_display.h"
#include "boot_tasks.h"
#include "data_broker.h"
#include "app_nvs.h"
#include "ui_broker.h"
#include "lvgl_ui.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    // -- 1. Power latch - first thing, always ----------------------------------
    boot_power_init();

    // -- 2. Broker - must exist before any driver or task touches it -----------
    broker_init();

    // -- 3. NVS flash init -----------------------------------------------------
    app_calibration_t cal = {0};
    esp_err_t ret = app_nvs_init(&cal);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed - continuing with defaults");
    }

    // -- 4-5. Load UI settings and apply to atomic globals ---------------------
    ui_settings_t ui_cfg = {
        .theme           = UI_THEME_DARK,
        .brightness      = 70,
        .blue_light_on   = false,
        .auto_brightness = false,
    };
    app_nvs_load_ui_settings(&ui_cfg);

    // Atomics written here (boot context, single writer, no mutex needed)
    g_ui_theme         = ui_cfg.theme;
    g_blue_light_on    = ui_cfg.blue_light_on;
    g_saved_brightness = ui_cfg.brightness;

    ESP_LOGI(TAG, "UI cfg: theme=%u bright=%u bluelit=%d",
             ui_cfg.theme, ui_cfg.brightness, ui_cfg.blue_light_on);

    // -- 6. Hardware bringup ---------------------------------------------------
    // I2C scan → WHO_AM_I verify → driver init → hw_alive flags → GPS → ADC
    boot_hw_init(&cal);

    // -- 7. Display stack ------------------------------------------------------
    // SPI, ST7789, backlight, PSRAM frame buffers, LVGL port + touch
    boot_display_init();
    boot_cst816d_configure();

    // -- 8. UI construction ----------------------------------------------------
    // Must be inside lvgl_port_lock - no LVGL calls before this block
    lvgl_port_lock(portMAX_DELAY);
    lvgl_ui_init(&ui_cfg);
    lvgl_port_unlock();

    // -- 9. Async NVS settings save queue --------------------------------------
    QueueHandle_t settings_q = ui_broker_init();

    // -- 10. Start all tasks ---------------------------------------------------
    // Sensor tasks (Core 0), UI refresh (Core 1), utilities (unpinned)
    boot_tasks_start(settings_q);

    ESP_LOGI(TAG, "Boot complete - firmware running");
    // app_main() returns here. FreeRTOS scheduler owns execution from this point.
}