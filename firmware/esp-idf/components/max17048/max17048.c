/**
 * @file max17048.c
 * @brief Maxim MAX17048 driver implementation -- stub for Stage 26 Batch 26.1.
 *
 * See max17048.h for design notes. This file:
 *   - Wraps 16-bit big-endian register I/O on I2C bus 2 (I2C_NUM_1) under
 *     g_i2c2_mutex (shared with BQ25619 + DRV2605).
 *   - Provides an init that reads VERSION and logs SW/family match.
 *   - Provides raw VCELL (mV) and SOC (%*100) accessors.
 *
 * No broker payload, no polling task, no self-learning. Those land in a
 * follow-up batch once Ivan has a real cell attached and voltage-vs-SOC
 * curves to fit.
 */

#include "max17048.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t g_i2c2_mutex;

static const char *TAG = "MAX17048";

// -- Identity -----------------------------------------------------------------
const char *max17048_get_chip_name(void) { return "MAX17048"; }
const char *max17048_get_chip_desc(void) { return "1-cell Li-ion fuel gauge"; }

// =============================================================================
// 16-bit big-endian I2C read primitive (caller holds g_i2c2_mutex).
// =============================================================================
esp_err_t max17048_read16(i2c_port_t i2c_num, uint8_t reg, uint16_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t hi = 0, lo = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MAX17048_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MAX17048_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &hi, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &lo, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    if (ret == ESP_OK) *out = ((uint16_t)hi << 8) | lo;
    return ret;
}

esp_err_t max17048_read_vcell_mv(i2c_port_t i2c_num, uint16_t *vcell_mv_out)
{
    if (!vcell_mv_out) return ESP_ERR_INVALID_ARG;
    uint16_t raw = 0;
    esp_err_t ret = max17048_read16(i2c_num, MAX17048_REG_VCELL, &raw);
    if (ret != ESP_OK) return ret;
    // 78.125 uV/LSB -> mV. Use integer math: mv = raw * 78125 / 1e6 = raw * 5 / 64.
    *vcell_mv_out = (uint16_t)((uint32_t)raw * 5U / 64U);
    return ESP_OK;
}

esp_err_t max17048_read_soc_pct100(i2c_port_t i2c_num, uint16_t *soc_pct100_out)
{
    if (!soc_pct100_out) return ESP_ERR_INVALID_ARG;
    uint16_t raw = 0;
    esp_err_t ret = max17048_read16(i2c_num, MAX17048_REG_SOC, &raw);
    if (ret != ESP_OK) return ret;
    // MSB = integer %, LSB = 1/256 %. -> pct * 100 = (hi * 25600 + lo * 100) / 256.
    uint32_t hi = (raw >> 8) & 0xFF;
    uint32_t lo = raw & 0xFF;
    uint32_t pct100 = (hi * 25600U + lo * 100U) / 256U;
    if (pct100 > 10000U) pct100 = 10000U;
    *soc_pct100_out = (uint16_t)pct100;
    return ESP_OK;
}

// =============================================================================
// Init -- read VERSION, log family match.
// =============================================================================
esp_err_t max17048_init(i2c_port_t i2c_num)
{
    ESP_LOGI(TAG, "driver v%s", MAX17048_DRIVER_VERSION);

    esp_err_t ret = ESP_FAIL;
    if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(TAG, "init: g_i2c2_mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    uint16_t version = 0;
    ret = max17048_read16(i2c_num, MAX17048_REG_VERSION, &version);
    xSemaphoreGive(g_i2c2_mutex);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "VERSION read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Per datasheet: VERSION reset value is 0x001_ where the lower nibble
    // is the IC production version. Bits 15-4 identify the family (0x001);
    // bits 3-0 are the production revision.
    if ((version & 0xFFF0) == 0x0010) {
        ESP_LOGI(TAG, "MAX17048 init OK @ 0x%02X (VERSION=0x%04X, prod_rev=0x%X)",
                 MAX17048_ADDR, version, version & 0x0F);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "MAX17048 VERSION=0x%04X does not match family 0x001_ mask",
             version);
    return ESP_ERR_INVALID_RESPONSE;
}
