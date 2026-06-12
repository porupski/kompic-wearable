/**
 * @file test_drv2605.c
 * @brief Standalone diagnostic for the DRV2605 haptic driver -- I2C bus 2.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C Bus Assignment):
 *   I2C bus 2: SDA=GPIO4, SCL=GPIO5, 400 kHz.
 *   DRV2605 @ 0x5A. LRA: Apple Taptic Engine clone (ELV1411A).
 *   DRV_EN pin: GPIO0 -- driven LOW by boot_power at boot to release the
 *     chip from hardware shutdown. This test sets it LOW directly for
 *     standalone operation.
 *
 * Phases:
 *   1. I2C bus 2 init (note: NOT bus 1).
 *   2. GPIO0 (DRV_EN) configure + drive LOW.
 *   3. drv2605_init -- exits standby, configures LRA open-loop, applies
 *      OD/RTP defaults.
 *   4. Play 4 named effects (CLICK, DOUBLE_CLICK, STRONG_BUZZ, SHARP_TICK)
 *      with 500 ms between each. Logs per-write I2C time + GO bit.
 *   5. Drive a 100 ms RTP burst at the Apple Taptic resonant period (reg=64,
 *      ~157 Hz) to verify the sweep path. Measures wall time.
 *   6. Stop, drive DRV_EN HIGH, stack high-water + heap report.
 *
 * Notes:
 *   - In the full firmware, boot_power.c owns GPIO0. This harness sets it
 *     up directly so the test can run before boot_power lands (see DEFECT-002).
 *   - The chip has internal pull-up on DRV_EN, so without our drive it sits
 *     HIGH = chip in hardware shutdown = no I2C ACK.
 */

#include "drv2605.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "test_drv2605";

// Required by drv2605.c via boot_hw_init.h's expected extern. Defined here
// so the harness links without that header existing yet.
SemaphoreHandle_t g_i2c2_mutex = NULL;

#define TEST_I2C_PORT     I2C_NUM_1     // bus 2
#define TEST_I2C_SDA      4
#define TEST_I2C_SCL      5
#define TEST_I2C_HZ       400000
#define TEST_DRV_EN_GPIO  0

static esp_err_t test_i2c_bus_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = TEST_I2C_SDA,
        .scl_io_num       = TEST_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TEST_I2C_HZ,
    };
    esp_err_t ret = i2c_param_config(TEST_I2C_PORT, &cfg);
    if (ret != ESP_OK) return ret;
    return i2c_driver_install(TEST_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

static void test_drv_en_drive_low(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TEST_DRV_EN_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(TEST_DRV_EN_GPIO, 0);
    // The chip needs >250 us after DRV_EN goes LOW before I2C is responsive.
    vTaskDelay(pdMS_TO_TICKS(5));
}

static void test_drv2605_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             haptic_get_chip_name(), haptic_get_chip_desc());

    if (g_i2c2_mutex == NULL) g_i2c2_mutex = xSemaphoreCreateMutex();

    // ── Phase 1: I2C bus 2 init ─────────────────────────────────────────────
    int64_t t_bus0 = esp_timer_get_time();
    esp_err_t err = test_i2c_bus_init();
    int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "I2C bus 2 init: %lld us -> %s",
             (long long)(t_bus1 - t_bus0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    // ── Phase 2: DRV_EN low ─────────────────────────────────────────────────
    test_drv_en_drive_low();
    ESP_LOGI(TAG, "DRV_EN (GPIO%d) driven LOW", TEST_DRV_EN_GPIO);

    // ── Phase 3: driver init ────────────────────────────────────────────────
    xSemaphoreTake(g_i2c2_mutex, portMAX_DELAY);
    int64_t t_i0 = esp_timer_get_time();
    err = drv2605_init(TEST_I2C_PORT);
    int64_t t_i1 = esp_timer_get_time();
    xSemaphoreGive(g_i2c2_mutex);
    ESP_LOGI(TAG, "drv2605_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ── Phase 4: play 4 named effects ───────────────────────────────────────
    const struct { uint8_t id; const char *name; } effects[] = {
        { HAPTIC_EFFECT_CLICK,         "CLICK"        },
        { HAPTIC_EFFECT_DOUBLE_CLICK,  "DOUBLE_CLICK" },
        { HAPTIC_EFFECT_STRONG_BUZZ,   "STRONG_BUZZ"  },
        { HAPTIC_EFFECT_SHARP_TICK,    "SHARP_TICK"   },
    };
    for (size_t i = 0; i < sizeof(effects) / sizeof(effects[0]); i++) {
        xSemaphoreTake(g_i2c2_mutex, portMAX_DELAY);
        int64_t t_p0 = esp_timer_get_time();
        err = drv2605_play_effect(TEST_I2C_PORT, effects[i].id);
        int64_t t_p1 = esp_timer_get_time();
        xSemaphoreGive(g_i2c2_mutex);
        ESP_LOGI(TAG, "play %s (id=%u): %s in %lld us",
                 effects[i].name, (unsigned)effects[i].id,
                 esp_err_to_name(err), (long long)(t_p1 - t_p0));
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // ── Phase 5: RTP burst at Apple Taptic resonant period ──────────────────
    ESP_LOGI(TAG, "Phase 5: 100 ms RTP burst at period=64 (~157 Hz)");
    int64_t t_s0 = esp_timer_get_time();
    xSemaphoreTake(g_i2c2_mutex, portMAX_DELAY);
    err = drv2605_sweep_step(TEST_I2C_PORT, 64);
    xSemaphoreGive(g_i2c2_mutex);
    int64_t t_s1 = esp_timer_get_time();
    ESP_LOGI(TAG, "sweep_step: %s in %lld us (expected ~120 ms)",
             esp_err_to_name(err), (long long)(t_s1 - t_s0));

    // ── Phase 6: stop + DRV_EN high + memory ────────────────────────────────
    xSemaphoreTake(g_i2c2_mutex, portMAX_DELAY);
    drv2605_stop(TEST_I2C_PORT);
    xSemaphoreGive(g_i2c2_mutex);
    gpio_set_level(TEST_DRV_EN_GPIO, 1);   // back to hardware shutdown

    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_drv2605_run();
}
