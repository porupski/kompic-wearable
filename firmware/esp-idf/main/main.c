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
#include "boot_hw_init.h"
// #include "boot_display.h"   // TODO: restore when display returns
#include "boot_tasks.h"
#include "data_broker.h"
#include "cross_driver.h"
#include "app_nvs.h"
// #include "ui_broker.h"      // TODO: restore when display returns
// #include "lvgl_ui.h"        // TODO: restore when display returns
// #include "esp_lvgl_port.h"  // TODO: restore when display returns
#include "esp_log.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    // -- 1. Power primitives --------------------------------------------------
    boot_power_init();

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
