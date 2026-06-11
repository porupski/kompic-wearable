/**
 * @file test_veml6030.c
 * @brief Standalone diagnostic for the VEML6030 ambient light driver.
 *
 * Wiring: see Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C bus 1.
 *   SDA=GPIO1, SCL=GPIO2, addr 0x10, 400 kHz.
 *
 * Phases:
 *   1. I2C bus 1 init + power-down + configure (gain=1/4x, IT=100 ms).
 *   2. 10 successive raw reads at 500 ms intervals -- print raw + lux.
 *   3. Auto-range probe: at each read, print whether the range changed.
 *   4. Lux-to-brightness curve sanity: log mapping at decadal lux levels.
 */

#include "veml6030.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "test_veml6030";

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

static const char *gain_name(veml6030_gain_t g)
{
    switch (g) {
        case VEML6030_GAIN_1_8: return "1/8x";
        case VEML6030_GAIN_1_4: return "1/4x";
        case VEML6030_GAIN_1:   return "1x";
        case VEML6030_GAIN_2:   return "2x";
        default: return "?";
    }
}

static void test_veml6030_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             veml6030_get_chip_name(), veml6030_get_chip_desc());

    if (g_i2c_mutex == NULL) g_i2c_mutex = xSemaphoreCreateMutex();

    int64_t t_bus0 = esp_timer_get_time();
    esp_err_t err = test_i2c_bus_init();
    int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "I2C bus 1 init: %lld us -> %s",
             (long long)(t_bus1 - t_bus0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    int64_t t_i0 = esp_timer_get_time();
    err = veml6030_init(TEST_I2C_PORT);
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ---- Phase 2+3: 10 reads with auto-range observation ----
    veml6030_gain_t prev_gain;
    veml6030_it_t   prev_it;
    veml6030_get_range(&prev_gain, &prev_it);
    ESP_LOGI(TAG, "starting range: gain=%s IT=%d res=%.4f lx/cnt",
             gain_name(prev_gain), (int)prev_it,
             (double)veml6030_current_resolution());

    for (int i = 0; i < 10; i++) {
        uint16_t raw = 0;
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        int64_t t_r0 = esp_timer_get_time();
        err = veml6030_read_raw(TEST_I2C_PORT, &raw);
        int64_t t_r1 = esp_timer_get_time();
        xSemaphoreGive(g_i2c_mutex);

        veml6030_gain_t g; veml6030_it_t it;
        veml6030_get_range(&g, &it);
        float res = veml6030_current_resolution();
        float lux = raw * res;

        const char *range_changed = (g != prev_gain || it != prev_it) ? " [RANGE CHANGED]" : "";
        ESP_LOGI(TAG, "#%2d  raw=%5u  lux=%9.2f  gain=%s IT=%d  dt=%lld us%s",
                 i, (unsigned)raw, (double)lux, gain_name(g), (int)it,
                 (long long)(t_r1 - t_r0), range_changed);
        prev_gain = g; prev_it = it;

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // ---- Phase 4: lux-to-brightness curve sanity ----
    const float sample_lux[] = { 0.0f, 1.0f, 10.0f, 50.0f, 200.0f, 1000.0f, 5000.0f, 20000.0f };
    ESP_LOGI(TAG, "Lux -> brightness curve sanity:");
    for (size_t i = 0; i < sizeof(sample_lux) / sizeof(sample_lux[0]); i++) {
        uint8_t pct = veml6030_lux_to_brightness(sample_lux[i]);
        ESP_LOGI(TAG, "  %8.1f lux -> %3u%%", (double)sample_lux[i], (unsigned)pct);
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
    test_veml6030_run();
}
