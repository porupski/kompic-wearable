/**
 * @file main.c
 * @brief Application entry point - orchestrator only.
 *
 * PHASE 2 STATE (display-less field-capture port):
 *   1. boot_power_init()    - GPIO0 LOW (DRV_EN), GPIO16 button config
 *   2. broker_init()        - mutex + zero structs + g_ui_event_q
 *   3. cross_driver_init()  - clear listener tables
 *   4. app_nvs_init()       - flash init + calibration load
 *   5. boot_hw_init(&cal)   - I2C0/I2C1 install, WHO_AM_I probe, driver
 *                              init, broker_xxx_set_hw_status, peripherals
 *                              (encoder / WS2812 / flashlight / SD / mic PDM /
 *                              haptic queue)
 *   6. boot_tasks_start()   - sensor tasks (Core 0) + field_capture (Core 1)
 *
 * Display / touch / LVGL scaffolding stays out of the boot path until the
 * FPC connector returns. Their code compiles (dead) so re-enabling is a
 * matter of restoring the commented lines.
 */

#include "boot_power.h"
#include "boot_pm.h"
#include "boot_hw_init.h"
// #include "boot_display.h"   // TODO: restore when display returns
#include "boot_tasks.h"
#include "data_broker.h"
#include "cross_driver.h"
#include "app_nvs.h"
#include "nvs_cfg.h"
#include "firmware_version.h"    // KOMPIC_FW_VERSION for nvs_cfg_sys_check_fw_version
#include "driver/i2c.h"
// #include "ui_broker.h"      // TODO: restore when display returns
// #include "lvgl_ui.h"        // TODO: restore when display returns
// #include "esp_lvgl_port.h"  // TODO: restore when display returns
#include "esp_log.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    // -- 1. Power primitives --------------------------------------------------
    boot_power_init();

    // -- 1b. Dynamic frequency scaling + light-sleep (Stage 11 Item D) --------
    // No-op unless CONFIG_PM_ENABLE=y is set in menuconfig. Safe to leave in
    // regardless. Must run before any task grabs a PM lock.
    boot_pm_init();

    // -- 2. Broker ------------------------------------------------------------
    broker_init();

    // -- 3. Cross-driver event bus -------------------------------------------
    cross_driver_init();

    // -- 4. NVS flash + calibration ------------------------------------------
    app_calibration_t cal = {0};
    esp_err_t ret = app_nvs_init(&cal);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed - continuing with defaults");
    }

    // -- 5. Hardware bringup --------------------------------------------------
    boot_hw_init(&cal);

    // -- 5a. Eager telemetry init (Stage 17) ---------------------------------
    // vbat_adc_ensure_init() and esp_ts_ensure_init() used to lazy-init on
    // first STATUS call, which surfaced their log lines mid-CLI output long
    // after boot. Firing them here places their log lines in the right slot
    // and shaves ~2 ms off the first STATUS invocation.
    {
        extern void vbat_adc_ensure_init(void);
        extern void esp_ts_ensure_init(void);
        vbat_adc_ensure_init();
        esp_ts_ensure_init();
    }

    // -- 5b. Boot-time NVS command-state printout ----------------------------
    // Toggle with the "NVS_PRINT ON/OFF" serial command. Reads PCF85063A
    // RAM_byte (0x03) for redundancy cross-check against ESP NVS.
    //
    // Also compare-and-update the last-known fw version (Stage 11): if the
    // stored value differs from KOMPIC_FW_VERSION, log "upgraded X -> Y".
    nvs_cfg_sys_check_fw_version(KOMPIC_FW_VERSION);
    nvs_cfg_boot_print(I2C_NUM_0);

    // -- 5c. PM lock inventory (Item D diagnosis) ----------------------------
    // Enable CONFIG_PM_PROFILING=y in menuconfig to see actual lock holders.
    // Without profiling this is a stub print. Runs after boot_hw_init so all
    // driver-initiated locks (I2C/RMT/SDMMC) are visible.
    boot_pm_dump_locks();

    // -- 6. Display + LVGL UI (deferred until display returns) ---------------
    // TODO: restore when display returns.
    //   boot_display_init();
    //   boot_cst816d_configure();
    //   lvgl_port_lock(portMAX_DELAY);
    //   lvgl_ui_init(&ui_cfg);
    //   lvgl_port_unlock();
    //   QueueHandle_t settings_q = ui_broker_init();

    // -- 7. Kick tasks --------------------------------------------------------
    boot_tasks_start(NULL);

    ESP_LOGI(TAG, "Phase 2 boot complete - field_capture running on Core 1");
}
