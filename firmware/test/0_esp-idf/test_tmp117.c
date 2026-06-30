/**
 * @file test_tmp117.c
 * @brief Standalone diagnostic for the TMP117 skin-temperature driver.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C bus 1):
 *   SDA=GPIO1, SCL=GPIO2, addr 0x48, 400 kHz.
 *
 * Phases:
 *   1. I2C bus 1 init.
 *   2. tmp117_init -- DEVICE_ID probe + soft reset.
 *   3. 6 successive temperature reads at 1 s intervals -- print signed
 *      temperature + per-read I2C wire time.
 *   4. Sanity: confirm the readings sit in a believable human-skin range
 *      (20-40 C) when the test is run on a wrist; warn otherwise.
 *   5. Stack high-water + heap report.
 */

#include "tmp117.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>

static const char *TAG = "test_tmp117";

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

static void test_tmp117_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             tmp117_get_chip_name(), tmp117_get_chip_desc());

    if (g_i2c_mutex == NULL) g_i2c_mutex = xSemaphoreCreateMutex();

    // ── Phase 1 ─────────────────────────────────────────────────────────────
    int64_t t_bus0 = esp_timer_get_time();
    esp_err_t err = test_i2c_bus_init();
    int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "I2C bus 1 init: %lld us -> %s",
             (long long)(t_bus1 - t_bus0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    // ── Phase 2 ─────────────────────────────────────────────────────────────
    int64_t t_i0 = esp_timer_get_time();
    err = tmp117_init(TEST_I2C_PORT);
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "tmp117_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ── Phase 3+4: 6 reads at 1 s ───────────────────────────────────────────
    int  in_range = 0;
    for (int i = 0; i < 6; i++) {
        float temp = NAN;
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        int64_t t_r0 = esp_timer_get_time();
        err = tmp117_read_temp_c(TEST_I2C_PORT, &temp);
        int64_t t_r1 = esp_timer_get_time();
        xSemaphoreGive(g_i2c_mutex);

        if (err == ESP_OK) {
            const char *flag = (temp > 20.0f && temp < 40.0f) ? " <- skin range"
                             : (temp > 10.0f && temp < 50.0f) ? " <- ambient-ish"
                             : "";
            if (temp > 20.0f && temp < 40.0f) in_range++;
            ESP_LOGI(TAG, "#%d  T=%.4f C  dt=%lld us%s",
                     i, (double)temp, (long long)(t_r1 - t_r0), flag);
        } else {
            ESP_LOGW(TAG, "#%d  read failed: %s", i, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "[Phase 4] readings in skin range (20-40 C): %d / 6", in_range);
    ESP_LOGI(TAG, "tmp117_get_last_temp_c() = %.4f C",
             (double)tmp117_get_last_temp_c());

    // ── Phase 5 ─────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_tmp117_run();
}
