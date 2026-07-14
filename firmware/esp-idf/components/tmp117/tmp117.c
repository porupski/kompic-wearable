/**
 * @file tmp117.c
 * @brief Texas Instruments TMP117 driver -- Core 0 only.
 *
 * Simple I2C temperature sensor. Default power-up mode is "continuous
 * conversion @ 1 Hz with 8-sample average"; we don't reconfigure, just
 * read TEMP_RESULT every 5 s.
 *
 * Wire format: TMP117 registers are 16-bit, MSB first. Reads are issued
 * as one-byte addr write + two-byte data read (no addr-auto-increment).
 *
 * The conversion is `temp_c = (int16_t)raw / 128.0`. The raw value is
 * signed two's complement; negative readings represent below-freezing
 * skin contact, which would be... noteworthy.
 *
 * Architecture: Blueprint 5 §3, Blueprint 14b
 */

#include "tmp117.h"
#include "data_broker.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

static const char *TAG = "TMP117";

extern SemaphoreHandle_t g_i2c_mutex;

static float s_last_temp_c = NAN;

const char *tmp117_get_chip_name(void) { return "TMP117";          }
const char *tmp117_get_chip_desc(void) { return "Skin temperature"; }

// ── I2C helpers (caller holds g_i2c_mutex) ───────────────────────────────────

static esp_err_t read_reg16(i2c_port_t port, uint8_t reg, uint16_t *out)
{
    uint8_t buf[2] = { 0, 0 };
    esp_err_t ret = i2c_master_write_read_device(port, TMP117_ADDR,
                                                  &reg, 1, buf, 2,
                                                  pdMS_TO_TICKS(20));
    if (ret == ESP_OK && out) {
        // TMP117 emits MSB first on the wire.
        *out = ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
    }
    return ret;
}

static esp_err_t write_reg16(i2c_port_t port, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_write_to_device(port, TMP117_ADDR,
                                      buf, 3, pdMS_TO_TICKS(20));
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

esp_err_t tmp117_init(i2c_port_t port)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint16_t dev_id = 0;
    esp_err_t ret = read_reg16(port, TMP117_REG_DEVICE_ID, &dev_id);
    if (ret != ESP_OK) {
        xSemaphoreGive(g_i2c_mutex);
        ESP_LOGE(TAG, "DEVICE_ID read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    uint16_t did = dev_id & 0x0FFFu;
    if (did != 0x0117u) {
        ESP_LOGW(TAG, "Unexpected DEVICE_ID: 0x%04X (expected 0x?117) [DSV]",
                 (unsigned)dev_id);
    } else {
        ESP_LOGI(TAG, "DEVICE_ID = 0x%04X (TMP117 family)", (unsigned)dev_id);
    }

    // Soft reset -- CONFIG.bit1 self-clears, datasheet says "wait 1.5 ms".
    (void)write_reg16(port, TMP117_REG_CONFIG, TMP117_CONFIG_RESET);
    xSemaphoreGive(g_i2c_mutex);
    vTaskDelay(pdMS_TO_TICKS(5));

    ESP_LOGI(TAG, "%s init OK @ 0x%02X (continuous mode @ default ODR)",
             tmp117_get_chip_name(), TMP117_ADDR);
    return ESP_OK;
}

esp_err_t tmp117_read_temp_c(i2c_port_t port, float *temp_c)
{
    uint16_t raw = 0;
    esp_err_t ret = read_reg16(port, TMP117_REG_TEMP_RESULT, &raw);
    if (ret != ESP_OK) return ret;

    // Two's complement decode then convert: 1 LSB = 1/128 C.
    int16_t signed_raw = (int16_t)raw;
    if (temp_c) *temp_c = (float)signed_raw / (float)TMP117_LSB_PER_C;
    return ESP_OK;
}

float tmp117_get_last_temp_c(void) { return s_last_temp_c; }

// ── Core 0 task ──────────────────────────────────────────────────────────────

void task_skin_fn(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(TMP117_POLL_MS);
    TickType_t last = xTaskGetTickCount();

    ESP_LOGI(TAG, "Task started on Core %d", xPortGetCoreID());

    while (1) {
        vTaskDelayUntil(&last, period);

        if (!broker_skin_hw_alive())    continue;
        if (!broker_skin_get_enabled()) continue;

        float temp = NAN;
        esp_err_t ret = ESP_FAIL;
        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ret = tmp117_read_temp_c(TMP117_I2C_PORT, &temp);
            xSemaphoreGive(g_i2c_mutex);
        }
        if (ret != ESP_OK || !isfinite(temp)) continue;

        s_last_temp_c = temp;

        // Read-before-write to preserve the UI-owned `enabled` field.
        broker_skin_data_t bd = {0};
        broker_skin_read(&bd);
        bd.skin_temp_c = temp;
        bd.valid       = true;
        broker_skin_write(&bd);
    }
}
