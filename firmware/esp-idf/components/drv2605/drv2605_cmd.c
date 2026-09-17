/**
 * @file drv2605_cmd.c
 * @brief DRV2605 command surface -- see drv2605_cmd.h.
 */

#include "drv2605_cmd.h"
#include "drv2605.h"
#include "haptic.h"
#include "data_broker.h"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

extern SemaphoreHandle_t g_i2c2_mutex;

static const char *TAG = "DRV_CMD";

#define DRV_I2C_PORT       I2C_NUM_1
#define DRV_MUTEX_TIMEOUT  pdMS_TO_TICKS(300)

// ── enable ──────────────────────────────────────────────────────────────────

esp_err_t drv2605_cmd_enable_set(bool on)
{
    broker_haptic_set_enabled(on);
    ESP_LOGI(TAG, "haptic %s", on ? "ENABLED" : "DISABLED");
    return ESP_OK;
}

bool drv2605_cmd_enable_get(void) { return broker_haptic_get_enabled(); }

// ── play ────────────────────────────────────────────────────────────────────

esp_err_t drv2605_cmd_play_effect(uint8_t effect_id)
{
    if (effect_id < 1 || effect_id > 123) return ESP_ERR_INVALID_ARG;
    haptic_play(effect_id);
    return ESP_OK;
}

// ── calibrate ───────────────────────────────────────────────────────────────

void drv2605_cmd_calibrate(void) { haptic_request_calibration(); }

// ── sweep ───────────────────────────────────────────────────────────────────

void drv2605_cmd_sweep_start(void) { haptic_sweep_start(); }

// Latches the currently-playing sweep step and ends the sweep.
// haptic_sweep_set() is what haptic_tile's "SET" button already calls.
void drv2605_cmd_sweep_stop(void)  { haptic_sweep_set();   }

// ── UI effect ───────────────────────────────────────────────────────────────

uint8_t drv2605_cmd_ui_effect_get(void)         { return haptic_get_ui_effect(); }
void    drv2605_cmd_ui_effect_set(uint8_t id)   { haptic_set_ui_effect(id);      }

// ── dump ────────────────────────────────────────────────────────────────────

static const struct { uint8_t reg; const char *name; } k_dump_regs[] = {
    { DRV2605_REG_STATUS,     "STATUS    " },
    { DRV2605_REG_MODE,       "MODE      " },
    { DRV2605_REG_LIBRARY,    "LIBRARY   " },
    { DRV2605_REG_RTP,        "RTP       " },
    { DRV2605_REG_WAVESEQ1,   "WAVESEQ1  " },
    { DRV2605_REG_GO,         "GO        " },
    { DRV2605_REG_RATED_VOLT, "RATED_VOLT" },
    { DRV2605_REG_OD_CLAMP,   "OD_CLAMP  " },
    { DRV2605_REG_CAL_COMP,   "CAL_COMP  " },
    { DRV2605_REG_CAL_BEMF,   "CAL_BEMF  " },
    { DRV2605_REG_FEEDBACK,   "FEEDBACK  " },
    { DRV2605_REG_LRA_PERIOD, "LRA_PERIOD" },
};

// Local minimal register reader -- drv2605.c doesn't expose a public
// read_reg helper, and adding one would touch the driver. Duplicating
// the 6-line I2C read is cheaper than growing the driver surface for
// a bench-only dump. Migrates to the new API in Batch 1b.
static esp_err_t dump_read_reg(uint8_t reg, uint8_t *val)
{
    if (!val) return ESP_ERR_INVALID_ARG;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DRV2605_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DRV2605_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(DRV_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

void drv2605_cmd_dump(void)
{
    printf("[HAPTIC] %s @ 0x%02X (I2C bus 1)\n",
           haptic_get_chip_name(), DRV2605_I2C_ADDR);

    if (xSemaphoreTake(g_i2c2_mutex, DRV_MUTEX_TIMEOUT) != pdTRUE) {
        printf("[HAPTIC] mutex timeout -- bus busy\n");
        return;
    }
    for (size_t i = 0; i < sizeof(k_dump_regs) / sizeof(k_dump_regs[0]); i++) {
        uint8_t v = 0xFF;
        esp_err_t r = dump_read_reg(k_dump_regs[i].reg, &v);
        if (r == ESP_OK) {
            printf("[HAPTIC]   0x%02X %s = 0x%02X\n",
                   k_dump_regs[i].reg, k_dump_regs[i].name, v);
        } else {
            printf("[HAPTIC]   0x%02X %s = READ_FAIL (%s)\n",
                   k_dump_regs[i].reg, k_dump_regs[i].name,
                   esp_err_to_name(r));
        }
    }
    xSemaphoreGive(g_i2c2_mutex);

    broker_haptic_data_t hd = {0};
    broker_haptic_read(&hd);
    printf("[HAPTIC] broker: en=%d cal=%d f=%.0fHz last_eff=%u ui_eff=%u "
           "sweep_active=%d step=%u age=%u ms\n",
           broker_haptic_get_enabled() ? 1 : 0,
           hd.calibrated ? 1 : 0, (double)hd.resonant_freq_hz,
           (unsigned)hd.last_effect, (unsigned)haptic_get_ui_effect(),
           hd.sweep_active ? 1 : 0, (unsigned)hd.sweep_step,
           (unsigned)hd.last_update_ms);
}

// ── status summary ──────────────────────────────────────────────────────────

void drv2605_cmd_status_summary(char *out, size_t max)
{
    if (!out || max == 0) return;
    broker_haptic_data_t hd = {0};
    broker_haptic_read(&hd);
    snprintf(out, max,
             "en=%d ui_eff=%u cal=%d f=%.0fHz",
             broker_haptic_get_enabled() ? 1 : 0,
             (unsigned)haptic_get_ui_effect(),
             hd.calibrated ? 1 : 0,
             (double)hd.resonant_freq_hz);
}
