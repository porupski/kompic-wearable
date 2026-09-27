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
#include "boot_hw_init.h"        // g_i2c1_bus_handle (Stage 35.2 i2c_master migration)
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t g_i2c2_mutex;

static const char *TAG = "MAX17048";

// -- I2C device handle (Stage 35.2) -------------------------------------------
static i2c_master_dev_handle_t s_max_dev = NULL;

// -- Identity -----------------------------------------------------------------
const char *max17048_get_chip_name(void) { return "MAX17048"; }
const char *max17048_get_chip_desc(void) { return "1-cell Li-ion fuel gauge"; }

// =============================================================================
// 16-bit big-endian I2C read primitive (caller holds g_i2c2_mutex).
// =============================================================================
esp_err_t max17048_read16(i2c_port_t i2c_num, uint8_t reg, uint16_t *out)
{
    (void)i2c_num;
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_max_dev) return ESP_ERR_INVALID_STATE;
    uint8_t rx[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(s_max_dev, &reg, 1, rx, 2, 20);
    if (ret == ESP_OK) *out = ((uint16_t)rx[0] << 8) | rx[1];
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

    // Stage 35.2: add self to the i2c_master bus.
    if (!g_i2c1_bus_handle) {
        ESP_LOGE(TAG, "init: g_i2c1_bus_handle NULL -- bringup_bus1 must run first");
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = MAX17048_ADDR,
        .scl_speed_hz    = BOOT_I2C_FREQ_HZ,
    };
    esp_err_t add = i2c_master_bus_add_device(g_i2c1_bus_handle, &dev_cfg, &s_max_dev);
    if (add != ESP_OK) {
        ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(add));
        return add;
    }

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
