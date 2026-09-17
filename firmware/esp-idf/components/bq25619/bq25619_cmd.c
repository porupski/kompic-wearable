/**
 * @file bq25619_cmd.c
 * @brief BQ25619 command surface -- see bq25619_cmd.h.
 */

#include "bq25619_cmd.h"
#include "bq25619.h"
#include "data_broker.h"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

extern SemaphoreHandle_t g_i2c2_mutex;

static const char *TAG = "BQ_CMD";

#define BQ_I2C_PORT       I2C_NUM_1
#define BQ_MUTEX_TIMEOUT  pdMS_TO_TICKS(300)

// ── enable (REG_POC.CHG_CONFIG) ─────────────────────────────────────────────

esp_err_t bq_cmd_enable_set(bool on)
{
    if (xSemaphoreTake(g_i2c2_mutex, BQ_MUTEX_TIMEOUT) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t poc = 0;
    esp_err_t ret = bq25619_read_reg(BQ_I2C_PORT, BQ25619_REG_POC, &poc);
    if (ret == ESP_OK) {
        uint8_t next = on ? (poc | BQ25619_POC_CHG_CONFIG)
                          : (poc & (uint8_t)~BQ25619_POC_CHG_CONFIG);
        if (next != poc) {
            ret = bq25619_write_reg(BQ_I2C_PORT, BQ25619_REG_POC, next);
        }
    }
    xSemaphoreGive(g_i2c2_mutex);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "charger %s", on ? "ENABLED" : "DISABLED");
    } else {
        ESP_LOGW(TAG, "enable_set(%d) failed: %s", on ? 1 : 0, esp_err_to_name(ret));
    }
    return ret;
}

bool bq_cmd_enable_get(void)
{
    if (xSemaphoreTake(g_i2c2_mutex, BQ_MUTEX_TIMEOUT) != pdTRUE) return false;
    uint8_t poc = 0;
    esp_err_t ret = bq25619_read_reg(BQ_I2C_PORT, BQ25619_REG_POC, &poc);
    xSemaphoreGive(g_i2c2_mutex);
    return (ret == ESP_OK) && ((poc & BQ25619_POC_CHG_CONFIG) != 0);
}

// ── boost (PMID 5 V) ────────────────────────────────────────────────────────

esp_err_t bq_cmd_boost_set(bool on) { return bq25619_set_boost(BQ_I2C_PORT, on); }

bool bq_cmd_boost_get(void)
{
    broker_battery_data_t bd = {0};
    broker_battery_read(&bd);
    return bd.boost_enabled;
}

// ── ship-mode (raw charger action) ──────────────────────────────────────────

esp_err_t bq_cmd_ship_mode(void)
{
    return bq25619_enter_ship_mode(BQ_I2C_PORT);
}

// ── dump ────────────────────────────────────────────────────────────────────

static const struct { uint8_t reg; const char *name; } k_dump_regs[] = {
    { BQ25619_REG_IINDPM,   "IINDPM  " },
    { BQ25619_REG_POC,      "POC     " },
    { BQ25619_REG_ICHG,     "ICHG    " },
    { BQ25619_REG_PRECHRG,  "PRECHRG " },
    { BQ25619_REG_VREG,     "VREG    " },
    { BQ25619_REG_TIMER,    "TIMER   " },
    { BQ25619_REG_BOOSTV,   "BOOSTV  " },
    { BQ25619_REG_MISC,     "MISC    " },
    { BQ25619_REG_STATUS,   "STATUS  " },
    { BQ25619_REG_FAULT,    "FAULT   " },
    { BQ25619_REG_PART,     "PART    " },
    { BQ25619_REG_VBAT_ADC, "VBAT_ADC" },
};

void bq_cmd_dump(void)
{
    printf("[BQ] %s @ 0x%02X (I2C bus 1)\n",
           bq25619_get_chip_name(), BQ25619_ADDR);

    if (xSemaphoreTake(g_i2c2_mutex, BQ_MUTEX_TIMEOUT) != pdTRUE) {
        printf("[BQ] mutex timeout -- bus busy\n");
        return;
    }
    for (size_t i = 0; i < sizeof(k_dump_regs) / sizeof(k_dump_regs[0]); i++) {
        uint8_t v = 0xFF;
        esp_err_t r = bq25619_read_reg(BQ_I2C_PORT, k_dump_regs[i].reg, &v);
        if (r == ESP_OK) {
            printf("[BQ]   0x%02X %s = 0x%02X\n",
                   k_dump_regs[i].reg, k_dump_regs[i].name, v);
        } else {
            printf("[BQ]   0x%02X %s = READ_FAIL (%s)\n",
                   k_dump_regs[i].reg, k_dump_regs[i].name,
                   esp_err_to_name(r));
        }
    }
    xSemaphoreGive(g_i2c2_mutex);

    uint16_t min_mv = 0, max_mv = 0;
    bq25619_soc_get_observed_extremes(&min_mv, &max_mv);
    printf("[BQ] observed vbat range: min=%u mV  max=%u mV\n",
           (unsigned)min_mv, (unsigned)max_mv);

    broker_battery_data_t bd = {0};
    broker_battery_read(&bd);
    printf("[BQ] broker: v=%.2fV pct=%u chg=%d pg=%d fault=0x%02X boost=%d age=%u ms\n",
           (double)bd.voltage, (unsigned)bd.percentage,
           bd.charging ? 1 : 0, bd.power_good ? 1 : 0,
           bd.fault, bd.boost_enabled ? 1 : 0,
           (unsigned)bd.last_update_ms);
}

// ── status summary (STATUS one-liner) ───────────────────────────────────────

void bq_cmd_status_summary(char *out, size_t max)
{
    if (!out || max == 0) return;
    broker_battery_data_t bd = {0};
    broker_battery_read(&bd);
    snprintf(out, max,
             "v=%.2fV pct=%u chg=%d pg=%d fault=0x%02X boost=%d",
             (double)bd.voltage, (unsigned)bd.percentage,
             bd.charging ? 1 : 0, bd.power_good ? 1 : 0,
             bd.fault, bd.boost_enabled ? 1 : 0);
}
