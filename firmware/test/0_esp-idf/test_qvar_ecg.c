/**
 * @file test_qvar_ecg.c
 * @brief Standalone diagnostic for the LSM6DSV16X Qvar / ECG channel.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md):
 *   I2C bus 1 (GPIO1 SDA, GPIO2 SCL), LSM6DSV16X @ 0x6B.
 *   Qvar inputs: pogo pin 1 (skin), pogo pin 2 (crown).
 *
 * Prereq:
 *   The LSM6DSV16X is shared with the IMU driver. This test brings up
 *   bus 1 + the IMU's bare-minimum reset so the Qvar channel can be
 *   enabled. In production, lsm6dsv16x_init() runs first and this
 *   driver simply calls qvar_ecg_init() afterwards.
 *
 * Phases:
 *   1. Init I2C bus 1 + the shared g_i2c_mutex.
 *   2. Probe WHO_AM_I @ 0x0F (expect 0x70 -- LSM6DSV16X).
 *   3. Light reset of the LSM6DSV16X (CTRL3.SW_RESET).
 *   4. qvar_ecg_init -> enable AH_QVAR with ZIN=235M.
 *   5. Read 5 single samples via qvar_ecg_read_sample (sanity).
 *   6. Capture 1 s (240 samples) via qvar_ecg_capture; log
 *      min/max/avg/peak-to-peak.
 *   7. qvar_ecg_has_contact() -- expected FALSE on a bare board
 *      (electrodes floating).
 *   8. qvar_ecg_deinit, stack high-water + heap.
 *
 * The bench operator can short Qvar1 + Qvar2 with a wet finger for
 * Phase 6 to see a non-trivial ECG-like waveform.
 */

#include "qvar_ecg.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "test_qvar_ecg";

#define BUS1_I2C_NUM    I2C_NUM_0
#define BUS1_SDA        1
#define BUS1_SCL        2
#define LSM_ADDR        0x6B
#define LSM_REG_WAI     0x0F
#define LSM_WAI_VAL     0x70
#define LSM_REG_CTRL3   0x12
#define LSM_CTRL3_SWRST 0x01

// The test owns its own mutex -- production code uses the one the
// IMU driver expects (extern SemaphoreHandle_t g_i2c_mutex).
SemaphoreHandle_t g_i2c_mutex = NULL;

static esp_err_t test_bus_init(void)
{
    i2c_config_t cfg = {
        .mode          = I2C_MODE_MASTER,
        .sda_io_num    = BUS1_SDA,
        .scl_io_num    = BUS1_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = 400000 },
    };
    esp_err_t r = i2c_param_config(BUS1_I2C_NUM, &cfg);
    if (r != ESP_OK) return r;
    r = i2c_driver_install(BUS1_I2C_NUM, cfg.mode, 0, 0, 0);
    if (r == ESP_ERR_INVALID_STATE) r = ESP_OK;  // already installed
    return r;
}

static esp_err_t imu_probe_and_reset(void)
{
    uint8_t reg = LSM_REG_WAI, who = 0;
    esp_err_t r = i2c_master_write_read_device(BUS1_I2C_NUM, LSM_ADDR,
                                                &reg, 1, &who, 1,
                                                pdMS_TO_TICKS(20));
    if (r != ESP_OK) return r;
    ESP_LOGI(TAG, "LSM6DSV16X WHO_AM_I=0x%02X (expected 0x%02X)",
             who, LSM_WAI_VAL);
    if (who != LSM_WAI_VAL) return ESP_FAIL;

    uint8_t buf[2] = { LSM_REG_CTRL3, LSM_CTRL3_SWRST };
    r = i2c_master_write_to_device(BUS1_I2C_NUM, LSM_ADDR, buf, 2,
                                    pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(30));
    return r;
}

static void test_qvar_ecg_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             qvar_ecg_get_chip_name(), qvar_ecg_get_chip_desc());

    // Phase 1: bus + mutex
    g_i2c_mutex = xSemaphoreCreateMutex();
    if (!g_i2c_mutex) { ESP_LOGE(TAG, "mutex alloc failed"); return; }

    int64_t t_b0 = esp_timer_get_time();
    esp_err_t err = test_bus_init();
    int64_t t_b1 = esp_timer_get_time();
    ESP_LOGI(TAG, "i2c bus 1 init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_b1 - t_b0));
    if (err != ESP_OK) return;

    // Phase 2-3: WHO_AM_I + soft reset
    err = imu_probe_and_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IMU probe/reset failed: %s", esp_err_to_name(err));
        return;
    }

    // Phase 4: enable Qvar
    int64_t t_i0 = esp_timer_get_time();
    err = qvar_ecg_init(BUS1_I2C_NUM);
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "qvar_ecg_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // Phase 5: 5 sanity samples (no rate enforced)
    for (int i = 0; i < 5; i++) {
        int16_t s = 0;
        int64_t tr0 = esp_timer_get_time();
        err = qvar_ecg_read_sample(&s);
        int64_t tr1 = esp_timer_get_time();
        ESP_LOGI(TAG, "single sample %d: %s, value=%6d, read_us=%lld",
                 i, esp_err_to_name(err), (int)s, (long long)(tr1 - tr0));
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Phase 6: 1 s capture at 240 Hz
    static int16_t trace[QVAR_DEFAULT_CAPTURE_N];
    size_t got = 0;
    int64_t tc0 = esp_timer_get_time();
    err = qvar_ecg_capture(trace, QVAR_DEFAULT_CAPTURE_N, &got);
    int64_t tc1 = esp_timer_get_time();
    ESP_LOGI(TAG, "qvar_ecg_capture(%d): %s, got=%u in %lld us",
             (int)QVAR_DEFAULT_CAPTURE_N, esp_err_to_name(err),
             (unsigned)got, (long long)(tc1 - tc0));

    if (got > 0) {
        int32_t sum = 0;
        int16_t mn = trace[0], mx = trace[0];
        for (size_t i = 0; i < got; i++) {
            int16_t v = trace[i];
            sum += v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        int32_t avg = sum / (int32_t)got;
        ESP_LOGI(TAG, "trace: n=%u  min=%6d  max=%6d  avg=%6ld  pp=%6d",
                 (unsigned)got, (int)mn, (int)mx, (long)avg, (int)(mx - mn));
    }

    // Phase 7: contact heuristic
    bool contact = qvar_ecg_has_contact();
    ESP_LOGI(TAG, "qvar_ecg_has_contact() = %s",
             contact ? "TRUE (probably touching electrodes)"
                     : "FALSE (electrodes appear floating)");

    // Phase 8: deinit + memory
    qvar_ecg_deinit();
    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_qvar_ecg_run();
}
