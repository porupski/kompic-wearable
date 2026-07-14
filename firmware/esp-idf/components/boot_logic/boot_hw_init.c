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
#include "lis3mdl.h"
#include "veml6030.h"
#include "max30101.h"
#include "tmp117.h"
#include "pcf85063.h"
#include "drv2605.h"
#include "bq25619.h"
#include "haptic.h"
#include "encoder.h"
#include "ws2812.h"
#include "flashlight.h"
#include "sdcard.h"
#include "mic_pdm.h"

#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "BOOT_HW";

// -- I2C mutex handles --------------------------------------------------------
SemaphoreHandle_t g_i2c_mutex  = NULL;
SemaphoreHandle_t g_i2c2_mutex = NULL;

// Phase-1 backlight stub kept for lvgl_ui dead-code references.
void backlight_set_brightness(uint8_t pct) { (void)pct; }

// -- I2C bus install helper ---------------------------------------------------
static esp_err_t install_i2c_bus(i2c_port_t port, int sda, int scl)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = sda,
        .scl_io_num       = scl,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BOOT_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(port, &cfg);
    if (err != ESP_OK) return err;
    return i2c_driver_install(port, cfg.mode, 0, 0, 0);
}

// -- I2C address probe (single-byte write, ACK check) -------------------------
static bool i2c_probe(i2c_port_t port, uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

// -- Bus 0 (I2C_NUM_0): sensors + RTC on GPIO1/2 -----------------------------
static void bringup_bus0(void)
{
    if (install_i2c_bus(I2C_NUM_0, BOOT_I2C0_SDA_GPIO, BOOT_I2C0_SCL_GPIO) != ESP_OK) {
        ESP_LOGE(TAG, "I2C0 install failed");
        return;
    }
    ESP_LOGI(TAG, "I2C0 up on SDA=%d SCL=%d @%d Hz",
             BOOT_I2C0_SDA_GPIO, BOOT_I2C0_SCL_GPIO, BOOT_I2C_FREQ_HZ);

    // ---- Probe + init each chip on bus 0 ------------------------------------
    if (i2c_probe(I2C_NUM_0, 0x76)) {  // BME688
        if (bme688_drv_init(I2C_NUM_0) == ESP_OK) {
            broker_env_set_hw_status(true);
            ESP_LOGI(TAG, "  BME688  0x76 OK");
        } else ESP_LOGW(TAG, "  BME688 init failed");
    }

    if (i2c_probe(I2C_NUM_0, 0x6B)) {  // LSM6DSV16X
        if (lsm6dsv16x_init(I2C_NUM_0) == ESP_OK) {
            broker_imu_set_hw_status(true);
            ESP_LOGI(TAG, "  LSM6DSV 0x6B OK");
        } else ESP_LOGW(TAG, "  LSM6DSV init failed");
    }

    if (i2c_probe(I2C_NUM_0, 0x1C)) {  // LIS3MDLTR
        if (lis3mdl_init(I2C_NUM_0) == ESP_OK) {
            broker_mag_set_hw_status(true);
            ESP_LOGI(TAG, "  LIS3MDL 0x1C OK");
        } else ESP_LOGW(TAG, "  LIS3MDL init failed");
    }

    if (i2c_probe(I2C_NUM_0, 0x10)) {  // VEML6030
        if (veml6030_init(I2C_NUM_0) == ESP_OK) {
            broker_light_set_hw_status(true);
            ESP_LOGI(TAG, "  VEML6030 0x10 OK");
        } else ESP_LOGW(TAG, "  VEML6030 init failed");
    }

    if (i2c_probe(I2C_NUM_0, 0x57)) {  // MAX30101
        if (max30101_init(I2C_NUM_0) == ESP_OK) {
            broker_hr_set_hw_status(true);
            ESP_LOGI(TAG, "  MAX30101 0x57 OK");
        } else ESP_LOGW(TAG, "  MAX30101 init failed");
    }

    // TMP117 at 0x48 or 0x49 (ADDR strap dependent)
    uint8_t tmp_addr = i2c_probe(I2C_NUM_0, 0x48) ? 0x48
                      : (i2c_probe(I2C_NUM_0, 0x49) ? 0x49 : 0);
    if (tmp_addr) {
        if (tmp117_init(I2C_NUM_0) == ESP_OK) {
            broker_skin_set_hw_status(true);
            ESP_LOGI(TAG, "  TMP117  0x%02X OK", tmp_addr);
        } else ESP_LOGW(TAG, "  TMP117 init failed");
    }

    if (i2c_probe(I2C_NUM_0, 0x51)) {  // PCF85063A RTC
        if (pcf85063_init(I2C_NUM_0) == ESP_OK) {
            broker_rtc_set_hw_status(true);
            ESP_LOGI(TAG, "  PCF85063 0x51 OK");
        } else ESP_LOGW(TAG, "  PCF85063 init failed");
    }
}

// -- Bus 1 (I2C_NUM_1): DRV2605 + BQ25619 on GPIO4/5 -------------------------
static void bringup_bus1(void)
{
    if (install_i2c_bus(I2C_NUM_1, BOOT_I2C1_SDA_GPIO, BOOT_I2C1_SCL_GPIO) != ESP_OK) {
        ESP_LOGE(TAG, "I2C1 install failed");
        return;
    }
    ESP_LOGI(TAG, "I2C1 up on SDA=%d SCL=%d @%d Hz",
             BOOT_I2C1_SDA_GPIO, BOOT_I2C1_SCL_GPIO, BOOT_I2C_FREQ_HZ);

    if (i2c_probe(I2C_NUM_1, 0x5A)) {  // DRV2605L
        // Probe-only: haptic_init() below runs the full drv2605_init with
        // auto-cal exactly once, holding g_i2c2_mutex, so we skip it here.
        broker_haptic_set_hw_status(true);
        ESP_LOGI(TAG, "  DRV2605 0x5A ACK (init deferred to haptic_init)");
    }

    if (i2c_probe(I2C_NUM_1, 0x6A)) {  // BQ25619
        if (bq25619_init(I2C_NUM_1) == ESP_OK) {
            broker_battery_set_hw_status(true);
            ESP_LOGI(TAG, "  BQ25619 0x6A OK");
        } else ESP_LOGW(TAG, "  BQ25619 init failed");
    }
}

// -- Public entry point -------------------------------------------------------
void boot_hw_init(const app_calibration_t *cal)
{
    (void)cal;  // reserved for driver seeding (mag hard-iron, height ref)

    // -- I2C bus mutexes ------------------------------------------------------
    g_i2c_mutex  = xSemaphoreCreateMutex();
    g_i2c2_mutex = xSemaphoreCreateMutex();
    configASSERT(g_i2c_mutex && g_i2c2_mutex);

    // -- I2C bringup ----------------------------------------------------------
    bringup_bus0();
    bringup_bus1();

    // -- Non-I2C peripherals --------------------------------------------------
    // encoder_init() intentionally NOT called: field_capture polls the pins
    // directly with a detent-rest state machine. Auto-memory:
    // feedback_encoder_polling.md -- PCNT/ISR approach oscillates +1/-1 on
    // the ALPS EC05E's settle bounce.
    if (ws2812_init()     == ESP_OK) ESP_LOGI(TAG, "ws2812   OK");
    if (flashlight_init() == ESP_OK) ESP_LOGI(TAG, "flashlight OK");
    if (sdcard_init()     == ESP_OK) ESP_LOGI(TAG, "sdcard   mutex up (mount deferred)");
    if (mic_pdm_init()    == ESP_OK) ESP_LOGI(TAG, "mic PDM  channel installed");

    // -- Haptic command queue (requires DRV2605 alive) ------------------------
    if (broker_haptic_hw_alive()) {
        if (haptic_init() == ESP_OK) ESP_LOGI(TAG, "haptic queue OK");
    }

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

    ESP_LOGI(TAG, "boot_hw_init complete");
}
