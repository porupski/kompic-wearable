/**
 * @file boot_hw_init.c
 * @brief Hardware bringup implementation. See boot_hw_init.h.
 */

#include "boot_hw_init.h"
#include "boot_display.h"

#include "data_broker.h"
#include "cross_driver.h"

// Driver headers -- each provides an _init(i2c_port_t) or _init(void).
#include "bme688_drv.h"
#include "lsm6dsv16x.h"
// STUBBED Stage 35: lis3mdl / veml6030 / max30101 / tmp117 drivers not
// initialised. Component sources still build; boot-time activation only.
// See Stage_35_Mk1b_I2C_Master_And_Sensor_Trim.md.
#include "pcf85063.h"
#include "drv2605.h"
#include "bq25619.h"
#include "max17048.h"
#include "haptic.h"
#include "encoder.h"
#include "ws2812.h"
#include "rgb_policy.h"
#include "flashlight.h"
#include "sdcard.h"
#include "mic_pdm.h"
#include "max_m10s.h"

#include "driver/i2c_master.h"   // Stage 35: sole I2C API for this binary
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BOOT_HW";

// -- I2C mutex handles --------------------------------------------------------
SemaphoreHandle_t g_i2c_mutex  = NULL;
SemaphoreHandle_t g_i2c2_mutex = NULL;

// -- I2C master bus handles (Stage 35 migration) ------------------------------
i2c_master_bus_handle_t g_i2c0_bus_handle = NULL;
i2c_master_bus_handle_t g_i2c1_bus_handle = NULL;

// backlight_set_brightness() now lives in boot_display.c and forwards to the
// real CO5300 WRDISBV register when the panel is present. No-op otherwise.
// lvgl_ui / settings-screen callers see the same API.

// -- I2C address probe (Bus 0 helper, Stage 35.3) ----------------------------
static bool i2c0_probe_master(uint8_t addr)
{
    return i2c_master_probe(g_i2c0_bus_handle, addr, pdMS_TO_TICKS(20)) == ESP_OK;
}

// -- Bus 0 (I2C_NUM_0): sensors + RTC on GPIO1/2 -----------------------------
// Stage 35.3: migrated to driver/i2c_master.h alongside Bus 1. Legacy
// driver/i2c.h is gone from the whole binary now.
static void bringup_bus0(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source                    = I2C_CLK_SRC_DEFAULT,
        .i2c_port                      = I2C_NUM_0,
        .scl_io_num                    = BOOT_I2C0_SCL_GPIO,
        .sda_io_num                    = BOOT_I2C0_SDA_GPIO,
        .glitch_ignore_cnt             = 7,
        .flags.enable_internal_pullup  = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_i2c0_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C0 install failed (i2c_master): %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "I2C0 up on SDA=%d SCL=%d (i2c_master API, per-device speed)",
             BOOT_I2C0_SDA_GPIO, BOOT_I2C0_SCL_GPIO);

    // ---- Probe + init each chip on bus 0 ------------------------------------
    if (i2c0_probe_master(0x76)) {  // BME688
        if (bme688_drv_init(I2C_NUM_0) == ESP_OK) {
            broker_env_set_hw_status(true);
            ESP_LOGI(TAG, "  BME688  0x76 OK");
        } else ESP_LOGW(TAG, "  BME688 init failed");
    } else ESP_LOGW(TAG, "  BME688  0x76 NAK (absent or bus fault)");

    if (i2c0_probe_master(0x6B)) {  // LSM6DSV16X
        if (lsm6dsv16x_init(I2C_NUM_0) == ESP_OK) {
            broker_imu_set_hw_status(true);
            // Advanced features live on the same chip -- once init passes,
            // pedometer + tap-Z + activity/inactivity auto-sleep all come up
            // as a baseline. Activity/inactivity is a no-op at the current
            // firmware-forced HP 240 Hz accel (see Stage_10_LSM_report §3);
            // configured now so it becomes free once we ODR-mode-switch.
            (void)lsm6dsv16x_pedometer_enable(true);
            (void)lsm6dsv16x_tap_z_enable(true);
            (void)lsm6dsv16x_activity_sleep_enable(true);
            broker_steps_set_hw_status(true);
            broker_steps_set_enabled(true);
            ESP_LOGI(TAG, "  LSM6DSV 0x6B OK  (pedo+tap-Z+act-sleep armed)");
        } else ESP_LOGW(TAG, "  LSM6DSV init failed");
    } else ESP_LOGW(TAG, "  LSM6DSV 0x6B NAK (absent or bus fault)");

    // STUBBED Stage 35: LIS3MDL (0x1C), VEML6030 (0x10), MAX30101 (0x57),
    // TMP117 (0x48/0x49) drivers not initialised. Chips remain on the bus;
    // WHOAMI still probes them. Un-stub by restoring the init blocks +
    // task entries + tile registrations. See Stage_35 doc.

    if (i2c0_probe_master(0x51)) {  // PCF85063A RTC
        if (pcf85063_init(I2C_NUM_0) == ESP_OK) {
            broker_rtc_set_hw_status(true);
            ESP_LOGI(TAG, "  PCF85063 0x51 OK");
        } else ESP_LOGW(TAG, "  PCF85063 init failed");
    } else ESP_LOGW(TAG, "  PCF85063 0x51 NAK (absent or bus fault)");
}

// -- Bus 1 (I2C_NUM_1): DRV2605 + BQ25619 + MAX17048 on GPIO4/5 --------------
// Stage 35.2: migrated to driver/i2c_master.h. Kills the chronic
// bq25619_read_reg -> i2c_hw_fsm_reset -> IRQ WDT crash that plagued
// fw 0.4.68..0.4.89 under any nav stress.
static bool i2c1_probe_master(uint8_t addr)
{
    return i2c_master_probe(g_i2c1_bus_handle, addr, pdMS_TO_TICKS(20)) == ESP_OK;
}

static void bringup_bus1(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source                    = I2C_CLK_SRC_DEFAULT,
        .i2c_port                      = I2C_NUM_1,
        .scl_io_num                    = BOOT_I2C1_SCL_GPIO,
        .sda_io_num                    = BOOT_I2C1_SDA_GPIO,
        .glitch_ignore_cnt             = 7,
        .flags.enable_internal_pullup  = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_i2c1_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C1 install failed (i2c_master): %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "I2C1 up on SDA=%d SCL=%d (i2c_master API, per-device speed)",
             BOOT_I2C1_SDA_GPIO, BOOT_I2C1_SCL_GPIO);

    if (i2c1_probe_master(0x5A)) {  // DRV2605L
        // Probe-only: haptic_init() below adds the device + runs auto-cal.
        broker_haptic_set_hw_status(true);
        ESP_LOGI(TAG, "  DRV2605 0x5A ACK (init deferred to haptic_init)");
    } else ESP_LOGW(TAG, "  DRV2605 0x5A NAK (absent or bus fault)");

    if (i2c1_probe_master(0x6A)) {  // BQ25619
        if (bq25619_init(I2C_NUM_1) == ESP_OK) {
            broker_battery_set_hw_status(true);
            ESP_LOGI(TAG, "  BQ25619 0x6A OK");
        } else ESP_LOGW(TAG, "  BQ25619 init failed");
    } else ESP_LOGW(TAG, "  BQ25619 0x6A NAK (absent or bus fault)");

    // MAX17048 fuel gauge -- self-powered from CELL. NAK without a battery
    // is expected; ACK confirms both wiring AND that VBAT+ reaches the chip.
    if (i2c1_probe_master(0x36)) {
        if (max17048_init(I2C_NUM_1) == ESP_OK) {
            ESP_LOGI(TAG, "  MAX17048 0x36 OK");
        } else ESP_LOGW(TAG, "  MAX17048 init failed");
    } else ESP_LOGW(TAG, "  MAX17048 0x36 NAK (absent, no cell, or bus fault)");
}

// -- Public entry point -------------------------------------------------------
void boot_hw_init(const app_calibration_t *cal)
{
    ESP_LOGI(TAG, "boot_logic v%s", BOOT_LOGIC_DRIVER_VERSION);
    (void)cal;  // reserved for driver seeding (mag hard-iron, height ref)

    // -- I2C bus mutexes ------------------------------------------------------
    g_i2c_mutex  = xSemaphoreCreateMutex();
    g_i2c2_mutex = xSemaphoreCreateMutex();
    configASSERT(g_i2c_mutex && g_i2c2_mutex);


    // -- Non-I2C peripherals --------------------------------------------------
    // encoder_init() intentionally NOT called: field_capture polls the pins
    // directly with a detent-rest state machine. Auto-memory:
    // feedback_encoder_polling.md -- PCNT/ISR approach oscillates +1/-1 on
    // the ALPS EC05E's settle bounce.
    // -- MAX-M10S GNSS (UART_NUM_1 + 1PPS) -----------------------------------
    // Stage 25 Batch A: bring the GPS driver up on Mk1b. Chip was populated but
    // never exercised. task_gps_fn is added to boot_tasks.c in the same batch.
    if (max_m10s_init() == ESP_OK) {
        broker_gps_set_hw_status(true);
        broker_gps_set_enabled(true);
        ESP_LOGI(TAG, "  MAX-M10S UART1 OK (GPS enabled, will search for fix)");
    } else {
        ESP_LOGW(TAG, "  MAX-M10S init failed -- GPS offline");
    }

    if (ws2812_init()     == ESP_OK) ESP_LOGI(TAG, "ws2812   OK");
    // Stage 24 side quest 2026-09-10: rgb_policy was defined but never
    // init'd, so no animations ran. This is the fix for the "RGB dark at
    // boot" iv7.1 bench observation. On Mk1b bring-up (Phase A step 6)
    // this init should give us the bright-then-fade-with-idle behaviour
    // the rgb_policy header documents.
    if (rgb_policy_init()  == ESP_OK) ESP_LOGI(TAG, "rgb_policy OK");
    if (flashlight_init() == ESP_OK) ESP_LOGI(TAG, "flashlight OK");
    if (sdcard_init()     == ESP_OK) ESP_LOGI(TAG, "sdcard   mutex up (mount deferred)");
    if (mic_pdm_init()    == ESP_OK) ESP_LOGI(TAG, "mic PDM  channel installed");

    // -- I2C bringup ----------------------------------------------------------
    bringup_bus0();
    bringup_bus1();

    // -- Sensor enable policy -------------------------------------------------
    // Always-on: RTC (timestamps), battery (ship-mode + monitor), haptic
    //            (encoder feedback). Their tasks poll at low duty anyway.
    // Parked   : env / imu / mag / hr / skin / light. field_capture wakes
    //            only the pair(s) for the currently-selected mode when a
    //            recording starts and parks them again on close. Keeps the
    //            bus quiet in STANDBY (no I2C mutex contention, no MAX30101
    //            LED current draw) and matches the sketch's behaviour.
    if (broker_rtc_hw_alive())     broker_rtc_set_enabled(true);
    if (broker_battery_hw_alive()) broker_battery_set_enabled(true);
    if (broker_haptic_hw_alive())  broker_haptic_set_enabled(true);
    ESP_LOGI(TAG, "Always-on sensors enabled (RTC/battery/haptic); modal sensors parked");

    // -- Haptic command queue LAST (Stage 17) ---------------------------------
    // Placed at the very end of boot_hw_init so a successful boot-OK buzz below
    // implicitly confirms every prior step. drv2605_init() runs auto-cal (up to
    // 1.5 s poll window); on COLD boot the chip needs settling time after
    // DRV_EN goes high or the BEMF loop fails to converge (STATUS bit 3 =
    // DIAG_RESULT = 1, bit 2 = OVER_TEMP flag set) -- warm reboots work
    // because the chip stayed powered. A 2 s pre-delay lets VDD/BEMF fully
    // settle before we poke GO=1. Bench-confirmed 2026-08-26.
    if (broker_haptic_hw_alive()) {
        ESP_LOGI(TAG, "DRV: settling 2000 ms before auto-cal (cold-boot BEMF stabilisation)");
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (haptic_init() == ESP_OK) {
            ESP_LOGI(TAG, "haptic queue OK");
            // Boot-OK confirmation: queue two STRONG_CLICKs. They play as the
            // haptic task drains after boot_tasks_start; the task's own
            // per-effect wait_go pacing separates them audibly. If the LRA
            // moves, everything above this line ran; if it stays quiet, watch
            // the log for the failing step.
            haptic_play_forced(1);   // DRV_STRONG_CLICK (ROM effect index 1)
            haptic_play_forced(1);
        } else {
            ESP_LOGW(TAG, "haptic queue init FAILED -- no boot buzz");
        }
    } else {
        ESP_LOGW(TAG, "DRV2605 not alive -- no boot buzz");
    }

    ESP_LOGI(TAG, "boot_hw_init complete");
}
