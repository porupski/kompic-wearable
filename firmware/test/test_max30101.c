/**
 * @file test_max30101.c
 * @brief Standalone diagnostic for the MAX30101 HR / PPG driver.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C bus 1, §GPIO):
 *   SDA=GPIO1, SCL=GPIO2, addr 0x57, 400 kHz.
 *   INT pin = GPIO7 (open-drain, idles HIGH, falling edge on FIFO-A-FULL).
 *
 * Phases:
 *   1. I2C bus 1 init.
 *   2. max30101_init -- Part-ID probe + soft reset + sleep.
 *   3. Configure SpO2 mode (Red+IR), wake, wait the 100 ms settle window.
 *   4. INT ISR install -- gets task notifications, also counted via
 *      max30101_get_int_count(). Drives the read loop in Phase 5.
 *   5. 4-second observation: read FIFO on every task-notify (INT) or on a
 *      40 ms poll fallback. Log raw IR, beat-detect output, INT count.
 *   6. Sleep, ISR uninstall, stack high-water + heap report.
 */

#include "max30101.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "test_max30101";

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

static void test_max30101_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             max30101_get_chip_name(), max30101_get_chip_desc());

    if (g_i2c_mutex == NULL) g_i2c_mutex = xSemaphoreCreateMutex();

    // ── Phase 1 ─────────────────────────────────────────────────────────────
    int64_t t_bus0 = esp_timer_get_time();
    esp_err_t err = test_i2c_bus_init();
    int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "I2C bus 1 init: %lld us -> %s",
             (long long)(t_bus1 - t_bus0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    // ── Phase 2 ─────────────────────────────────────────────────────────────
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    int64_t t_i0 = esp_timer_get_time();
    err = max30101_init(TEST_I2C_PORT);
    int64_t t_i1 = esp_timer_get_time();
    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "max30101_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ── Phase 3 ─────────────────────────────────────────────────────────────
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    err = max30101_set_shutdown(TEST_I2C_PORT, false);
    if (err == ESP_OK) {
        err = max30101_setup_hr_mode(TEST_I2C_PORT, 0x1F, 0x1F);
    }
    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "wake + setup_hr_mode: %s", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(100));   // settle

    // ── Phase 4 ─────────────────────────────────────────────────────────────
    err = max30101_install_int_isr(xTaskGetCurrentTaskHandle());
    ESP_LOGI(TAG, "INT ISR install: %s", esp_err_to_name(err));

    // ── Phase 5 ─────────────────────────────────────────────────────────────
    max30101_beat_detector_t detector;
    max30101_beat_detector_init(&detector);

    int64_t t_obs = esp_timer_get_time();
    uint32_t reads     = 0;
    uint32_t beat_seen = 0;
    while ((esp_timer_get_time() - t_obs) < 4LL * 1000LL * 1000LL) {
        // Wait for INT notification or 40 ms polling tick.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));

        uint8_t avail = 0;
        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            max30101_get_fifo_available(TEST_I2C_PORT, &avail);
            if (avail > 4) avail = 4;
            for (uint8_t i = 0; i < avail; i++) {
                max30101_sample_t s = {0};
                if (max30101_read_fifo(TEST_I2C_PORT, &s, false) == ESP_OK && s.valid) {
                    reads++;
                    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                    bool beat = max30101_check_for_beat(&detector, s.ir, now_ms);
                    if (beat) beat_seen++;
                    if (reads <= 8) {
                        ESP_LOGI(TAG, "sample[%lu] IR=%lu RED=%lu  finger=%d bpm=%u beat=%d",
                                 (unsigned long)reads,
                                 (unsigned long)s.ir, (unsigned long)s.red,
                                 (int)detector.finger_detected,
                                 (unsigned)detector.bpm, (int)beat);
                    }
                }
            }
            xSemaphoreGive(g_i2c_mutex);
        }
    }

    ESP_LOGI(TAG, "[Phase 5] reads=%lu  beats=%lu  INT count=%lu",
             (unsigned long)reads, (unsigned long)beat_seen,
             (unsigned long)max30101_get_int_count());

    // ── Phase 6 ─────────────────────────────────────────────────────────────
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    max30101_set_shutdown(TEST_I2C_PORT, true);
    xSemaphoreGive(g_i2c_mutex);

    max30101_install_int_isr(NULL);

    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_max30101_run();
}
