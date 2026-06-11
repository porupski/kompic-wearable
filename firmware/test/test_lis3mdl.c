/**
 * @file test_lis3mdl.c
 * @brief Standalone diagnostic for the LIS3MDLTR magnetometer driver.
 *
 * Wiring: see Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C bus 1.
 *   SDA=GPIO1, SCL=GPIO2, addr 0x1C, 400 kHz.
 *
 * Phases:
 *   1. I2C bus 1 init + WHO_AM_I read (expect 0x3D).
 *   2. Soft-reset + configure (continuous, 10 Hz, ±4 G, UHP XYZ, BDU).
 *   3. 10 raw reads at 100 ms intervals -- print XYZ in µT + per-read dt.
 *   4. Heading derivation for each sample.
 *   5. LRA offset measurement -- average of 20 samples over ~1 s.
 *      Expect ~1 G (= 100 µT) magnitude per v7.2 line 432.
 *
 * Note: hard-iron calibration via figure-8 sweep is NOT exercised here --
 * that's a tile-driven flow that requires user interaction (and the broker).
 */

#include "lis3mdl.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>

static const char *TAG = "test_lis3mdl";

SemaphoreHandle_t g_i2c_mutex = NULL;

#define TEST_I2C_PORT  I2C_NUM_0
#define TEST_I2C_SDA   1
#define TEST_I2C_SCL   2
#define TEST_I2C_HZ    400000

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

static void test_lis3mdl_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             lis3mdl_get_chip_name(), lis3mdl_get_chip_desc());

    if (g_i2c_mutex == NULL) g_i2c_mutex = xSemaphoreCreateMutex();

    int64_t t_bus0 = esp_timer_get_time();
    esp_err_t err = test_i2c_bus_init();
    int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "I2C bus 1 init: %lld us -> %s",
             (long long)(t_bus1 - t_bus0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    int64_t t_i0 = esp_timer_get_time();
    err = lis3mdl_init(TEST_I2C_PORT);
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ---- Phase 3+4: 10 reads + heading per sample ----
    for (int i = 0; i < 10; i++) {
        float x, y, z;
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        int64_t t_r0 = esp_timer_get_time();
        err = lis3mdl_read_raw(TEST_I2C_PORT, &x, &y, &z);
        int64_t t_r1 = esp_timer_get_time();
        xSemaphoreGive(g_i2c_mutex);

        if (err == ESP_OK) {
            float mag    = sqrtf(x * x + y * y + z * z);
            float headg  = lis3mdl_calculate_heading(x, y, 0.0f);
            ESP_LOGI(TAG, "#%2d  X=%+7.1f Y=%+7.1f Z=%+7.1f uT  |B|=%6.1f  hdg=%5.1f deg  dt=%lld us",
                     i, (double)x, (double)y, (double)z, (double)mag, (double)headg,
                     (long long)(t_r1 - t_r0));
        } else {
            ESP_LOGE(TAG, "#%2d read failed: %s", i, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // ---- Phase 5: LRA offset measurement ----
    ESP_LOGI(TAG, "Measuring LRA DC offset (place device flat, keep still) ...");
    float ox = 0, oy = 0, oz = 0;
    err = lis3mdl_measure_lra_offset(TEST_I2C_PORT, &ox, &oy, &oz);
    if (err == ESP_OK) {
        float mag = sqrtf(ox * ox + oy * oy + oz * oz);
        ESP_LOGI(TAG, "LRA offset = (%.2f, %.2f, %.2f) uT  |B|=%.2f uT  (~%.2f gauss)",
                 (double)ox, (double)oy, (double)oz, (double)mag, (double)(mag / 100.0f));
        // Sanity: pure-Earth field magnitude is ~25-65 uT; LRA pushes us above that.
        if (mag < 30.0f) {
            ESP_LOGW(TAG, "Magnitude below typical Earth field -- LRA offset may not be present");
        }
    } else {
        ESP_LOGE(TAG, "LRA offset measurement failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_lis3mdl_run();
}
