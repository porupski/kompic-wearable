/**
 * @file qvar_ecg.c
 * @brief LSM6DSV16X Qvar / ECG capture, shares I2C bus 1 with the IMU.
 *
 * What is Qvar?
 *   The LSM6DSV16X embeds an "Analog Hub / Qvar" channel -- a
 *   high-impedance, single-ended analog-to-digital path intended for
 *   electrical biopotential sensing (skin charge variation, ECG-class
 *   signals). The bare-die supports four selectable input impedances
 *   (2.4 GOhm / 730 MOhm / 300 MOhm / 235 MOhm) and the same ODR family
 *   as the IMU's primary acceleration channel (15.625 Hz - 7.68 kHz).
 *
 *   See LSM6DSV16X datasheet (ST DocID DS13176) §Analog hub /Qvar.
 *
 * Why share the IMU's bus mutex?
 *   The Qvar channel IS the LSM6DSV16X -- same chip, same I2C address
 *   (0x6B), same bus 1. The IMU driver already declares
 *   `extern SemaphoreHandle_t g_i2c_mutex` and the LSM6DSV16X's primary
 *   loop is on Core 0; this driver must serialise against that loop.
 *
 * Concurrency story:
 *   - Init / sample / capture all take g_i2c_mutex briefly.
 *   - We never hold the mutex across vTaskDelay -- only across the I2C
 *     register read/write itself.
 *   - Cadence in capture() is driven by vTaskDelayUntil; the actual
 *     I2C read takes <1 ms at 400 kHz, leaving ~3 ms of slack per 4.17
 *     ms sample period at 240 Hz.
 *
 * What this driver does NOT do:
 *   - It does not init the LSM6DSV16X chip. lsm6dsv16x_init() handles
 *     the WHO_AM_I / reset / accel+gyro config / etc. We assume the
 *     IMU driver has been brought up first.
 *   - It does not read OUT_TEMP / accel / gyro -- those belong to the
 *     IMU driver.
 *   - It does not surface samples into the data broker. ECG is captured
 *     on-demand and streamed elsewhere (SD card, BLE) by higher modules.
 */

#include "qvar_ecg.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t g_i2c_mutex;  // shared with lsm6dsv16x.c (bus 1)

static const char *TAG = "QVAR_ECG";

const char *qvar_ecg_get_chip_name(void) { return "QvarECG";                       }
const char *qvar_ecg_get_chip_desc(void) { return "LSM6DSV16X AH_QVAR @ 240 Hz";   }

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────

static bool          s_enabled   = false;
static i2c_port_t    s_i2c_num   = I2C_NUM_0;

// ─────────────────────────────────────────────────────────────────────────────
// I2C primitives (caller holds g_i2c_mutex)
// ─────────────────────────────────────────────────────────────────────────────

static esp_err_t write_reg(i2c_port_t port, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(port, QVAR_I2C_ADDR, buf, 2,
                                      pdMS_TO_TICKS(20));
}

static esp_err_t read_reg(i2c_port_t port, uint8_t reg, uint8_t *out)
{
    return i2c_master_write_read_device(port, QVAR_I2C_ADDR, &reg, 1,
                                         out, 1, pdMS_TO_TICKS(20));
}

static esp_err_t read_regs(i2c_port_t port, uint8_t reg,
                           uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(port, QVAR_I2C_ADDR, &reg, 1,
                                         buf, len, pdMS_TO_TICKS(20));
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t qvar_ecg_init(i2c_port_t i2c_num)
{
    s_i2c_num = i2c_num;

    if (!g_i2c_mutex) {
        ESP_LOGE(TAG, "g_i2c_mutex not initialised -- imu driver must run first");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Read CTRL7 first so we OR in our bits (read-before-write Blueprint 4 §3).
    uint8_t ctrl7 = 0;
    esp_err_t r = read_reg(i2c_num, QVAR_REG_CTRL7, &ctrl7);
    if (r != ESP_OK) {
        xSemaphoreGive(g_i2c_mutex);
        ESP_LOGE(TAG, "CTRL7 read failed: %s", esp_err_to_name(r));
        return r;
    }

    uint8_t new_ctrl7 = (ctrl7 & 0x0E) // preserve any IMU-set bits we don't own
                      | QVAR_CTRL7_AH_QVAR_EN
                      | QVAR_CTRL7_ZIN_235M;
    r = write_reg(i2c_num, QVAR_REG_CTRL7, new_ctrl7);
    xSemaphoreGive(g_i2c_mutex);

    if (r != ESP_OK) {
        ESP_LOGE(TAG, "CTRL7 write failed: %s", esp_err_to_name(r));
        return r;
    }

    // The Qvar channel needs a moment to settle after enable.
    vTaskDelay(pdMS_TO_TICKS(10));

    s_enabled = true;
    ESP_LOGI(TAG, "%s enabled (CTRL7 0x%02X -> 0x%02X, ZIN=235M, ODR=%d Hz)",
             qvar_ecg_get_chip_name(), ctrl7, new_ctrl7, QVAR_ODR_HZ);
    return ESP_OK;
}

esp_err_t qvar_ecg_deinit(void)
{
    if (!g_i2c_mutex || !s_enabled) {
        s_enabled = false;
        return ESP_OK;
    }
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t ctrl7 = 0;
    esp_err_t r = read_reg(s_i2c_num, QVAR_REG_CTRL7, &ctrl7);
    if (r == ESP_OK) {
        ctrl7 &= ~QVAR_CTRL7_AH_QVAR_EN;
        r = write_reg(s_i2c_num, QVAR_REG_CTRL7, ctrl7);
    }
    xSemaphoreGive(g_i2c_mutex);
    s_enabled = false;
    ESP_LOGI(TAG, "Qvar disabled");
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sample API
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t qvar_ecg_read_sample(int16_t *out)
{
    if (!out)        return ESP_ERR_INVALID_ARG;
    if (!s_enabled)  return ESP_ERR_INVALID_STATE;
    if (!g_i2c_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t raw[2];
    esp_err_t r = read_regs(s_i2c_num, QVAR_REG_OUT_AH_QVAR_L, raw, 2);
    xSemaphoreGive(g_i2c_mutex);
    if (r != ESP_OK) return r;

    *out = (int16_t)((raw[1] << 8) | raw[0]);
    return ESP_OK;
}

esp_err_t qvar_ecg_capture(int16_t *dst, size_t n_samples, size_t *out_n)
{
    if (!dst || !n_samples) return ESP_ERR_INVALID_ARG;
    if (!s_enabled)         return ESP_ERR_INVALID_STATE;

    const TickType_t period_ticks = pdMS_TO_TICKS(1000 / QVAR_ODR_HZ);
    TickType_t wake = xTaskGetTickCount();
    size_t i = 0;
    for (; i < n_samples; i++) {
        esp_err_t r = qvar_ecg_read_sample(&dst[i]);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "capture short at sample %u: %s",
                     (unsigned)i, esp_err_to_name(r));
            break;
        }
        vTaskDelayUntil(&wake, period_ticks > 0 ? period_ticks : 1);
    }
    if (out_n) *out_n = i;
    return (i == n_samples) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

bool qvar_ecg_has_contact(void)
{
    if (!s_enabled) return false;
    int16_t window[QVAR_CONTACT_WINDOW_SAMPLES];
    size_t got = 0;
    if (qvar_ecg_capture(window, QVAR_CONTACT_WINDOW_SAMPLES, &got) != ESP_OK) {
        // partial; treat what we got as the dataset
    }
    if (got < (QVAR_CONTACT_WINDOW_SAMPLES / 2)) return false;

    int floating = 0;
    for (size_t i = 0; i < got; i++) {
        int v = window[i] < 0 ? -window[i] : window[i];
        if (v >= QVAR_FLOAT_THRESHOLD) floating++;
    }
    // Majority of samples railing -> no contact.
    return (floating < (int)(got / 2));
}
