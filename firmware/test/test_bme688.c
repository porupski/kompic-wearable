/**
 * @file test_bme688.c
 * @brief Standalone diagnostic for the BME688 environment + gas driver.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C bus 1):
 *   SDA=GPIO1, SCL=GPIO2, addr 0x76, 400 kHz.
 *
 * Phases:
 *   1. I2C bus 1 init.
 *   2. bme688_drv_init -- verify the Bosch library's chip-ID handshake.
 *   3. 5 successive forced-mode measurements at 2 s intervals -- print T/P/H,
 *      gas resistance, gas_valid flag, heater-blocking time per cycle.
 *   4. Heater-blocking quantification: time the full mutex-hold of one
 *      forced-mode cycle so we can flag if it ever drifts above 250 ms.
 *   5. Stack high-water + heap report.
 */

#include "bme688_drv.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "test_bme688";

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

static void test_bme688_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             bme688_get_chip_name(), bme688_get_chip_desc());

    if (g_i2c_mutex == NULL) g_i2c_mutex = xSemaphoreCreateMutex();

    int64_t t_bus0 = esp_timer_get_time();
    esp_err_t err = test_i2c_bus_init();
    int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "I2C bus 1 init: %lld us -> %s",
             (long long)(t_bus1 - t_bus0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    // ── Phase 2: driver init ────────────────────────────────────────────────
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    int64_t t_i0 = esp_timer_get_time();
    err = bme688_drv_init(TEST_I2C_PORT);
    int64_t t_i1 = esp_timer_get_time();
    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "bme688_drv_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ── Phase 3+4: 5 forced-mode reads ──────────────────────────────────────
    int64_t worst_mutex_us = 0;
    for (int i = 0; i < 5; i++) {
        float temp = 0.0f, hum = 0.0f, press = 0.0f, gas = 0.0f;
        bool  gas_valid = false;

        int64_t t_lock0 = esp_timer_get_time();
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        int64_t t_read0 = esp_timer_get_time();
        err = bme688_read_forced(&temp, &hum, &press, &gas, &gas_valid);
        int64_t t_read1 = esp_timer_get_time();
        xSemaphoreGive(g_i2c_mutex);
        int64_t t_lock1 = esp_timer_get_time();

        int64_t hold_us = t_lock1 - t_lock0;
        if (hold_us > worst_mutex_us) worst_mutex_us = hold_us;

        if (err == ESP_OK) {
            float alt = bme688_pressure_to_altitude(press, 1013.25f);
            ESP_LOGI(TAG, "#%d  T=%.2fC  H=%.1f%%  P=%.1fhPa  alt=%.1fm  "
                          "gas=%.0fohm valid=%d  read=%lldus  hold=%lldus",
                     i, (double)temp, (double)hum, (double)press, (double)alt,
                     (double)gas, (int)gas_valid,
                     (long long)(t_read1 - t_read0),
                     (long long)hold_us);
        } else {
            ESP_LOGW(TAG, "#%d  read failed (hold=%lldus)",
                     i, (long long)hold_us);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGI(TAG, "Worst-case I2C mutex hold over %d cycles: %lld us",
             5, (long long)worst_mutex_us);
    if (worst_mutex_us > 250000) {
        ESP_LOGW(TAG, "Mutex hold > 250 ms -- touch/button starvation risk");
    }

    // ── Phase 5: memory snapshot ────────────────────────────────────────────
    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_bme688_run();
}
