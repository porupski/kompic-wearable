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
#include "boot_display.h"      // Stage 21 §4.1a: CO5300 bring-up with touch-probe skip
#include "boot_tasks.h"
#include "data_broker.h"
#include "cross_driver.h"
#include "app_nvs.h"
#include "nvs_cfg.h"
#include "firmware_version.h"    // KOMPIC_FW_VERSION for nvs_cfg_sys_check_fw_version
#include "driver/i2c.h"
#include "lvgl_ui_display.h"     // Stage 22 §4.1b: lvgl port + CO5300 flush
#include "esp_log.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    // -- 0. Provenance banner ------------------------------------------------
    // Prints hw + fw at the top of every boot so any log capture is
    // self-identifying. Individual drivers print their <NAME>_DRIVER_VERSION
    // as the first line of their _init() for the same reason.
    ESP_LOGI(TAG, "KOMPIC hw=%s fw=%s", KOMPIC_HW_VERSION, KOMPIC_FW_VERSION);

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

    // -- 6. Display bring-up (Stage 21 §4.1a) --------------------------------
    // Probes CST9217 on I2C0; if the touch chip is absent (iv7.1 today) the
    // panel init is skipped and the rest of boot continues unchanged. Return
    // value inspected only for the boot log line.
    {
        esp_err_t disp_err = boot_display_init();
        if (disp_err == ESP_OK) {
            ESP_LOGI(TAG, "display ready");
        } else if (disp_err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "display absent -- running headless");
        } else {
            ESP_LOGW(TAG, "display init returned %s", esp_err_to_name(disp_err));
        }
    }

    // -- 6b. LVGL port + display (Stage 22 §4.1b) ----------------------------
    // Runs when the panel is present, OR when the bench-only NVS override
    // "LVGL_FORCE ON" was set on the previous boot. In force mode the flush
    // callback is a no-op so iv7.1 can exercise the tile pipeline without a
    // real CO5300. See lvgl_ui_display.{c,h} and Stage 22 §4.1b.
    {
        const bool present   = boot_display_is_present();
        const bool forced    = nvs_cfg_sys_get_lvgl_force_on();
        if (present || forced) {
            esp_err_t lvgl_err = lvgl_ui_display_setup(!present && forced);
            if (lvgl_err != ESP_OK) {
                ESP_LOGW(TAG, "LVGL setup failed: %s (continuing headless)",
                         esp_err_to_name(lvgl_err));
            } else {
                // §4.3: build screens + register tiles once the port is up,
                // then spawn the LVGL refresh + settings-saver tasks. The
                // refresh task reads from broker; sensor tasks aren't started
                // yet, so the first few refreshes render empty state -- that's
                // fine, widgets self-populate on the next tick.
                (void)lvgl_ui_display_boot_screens();
                lvgl_ui_display_start_tasks();
            }
        } else {
            ESP_LOGI(TAG, "LVGL skipped -- no panel and LVGL_FORCE=OFF");
        }
    }

    // -- 7. Kick tasks --------------------------------------------------------
    boot_tasks_start(NULL);

    ESP_LOGI(TAG, "Phase 2 boot complete - field_capture running on Core 1");
}
