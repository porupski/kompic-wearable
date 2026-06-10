/**
 * @file bq25619.c
 * @brief TI BQ25619 driver implementation.
 *
 * See bq25619.h for design notes. This file:
 *   - Wraps the I2C register I/O on bus 2 under g_i2c_mutex.
 *   - Decodes REG_STATUS / REG_FAULT into the broker payload.
 *   - Provides a voltage-LUT SoC mapper + a self-learning observation hook
 *     (stub today; structurally ready for NVS-persisted curve fitting).
 *   - Implements ship-mode entry via REG_MISC.BATFET_DIS.
 *
 * Most register addresses + bit positions are tagged [DSV] (datasheet-verify)
 * pending a bench session with the BQ25619 datasheet in hand. Conservative
 * defaults are used everywhere; see [DEFECT-001] in the porting .md.
 *
 * Core 0 only.
 */

#include "bq25619.h"
#include "data_broker.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

extern SemaphoreHandle_t g_i2c_mutex;

static const char *TAG = "BQ25619";

#define BATTERY_POLL_MS  1000   // 1 Hz -- spec is "polled via I2C"; faster wastes the bus

// -- Identity -----------------------------------------------------------------
const char *bq25619_get_chip_name(void) { return "BQ25619"; }
const char *bq25619_get_chip_desc(void) { return "Li-ion charger + PMID boost"; }

// =============================================================================
// I2C primitives (caller holds g_i2c_mutex)
// =============================================================================

esp_err_t bq25619_read_reg(i2c_port_t i2c_num, uint8_t reg, uint8_t *val)
{
    if (!val) return ESP_ERR_INVALID_ARG;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ25619_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ25619_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t bq25619_write_reg(i2c_port_t i2c_num, uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ25619_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t bq25619_read_vbat_mv(i2c_port_t i2c_num, uint16_t *vbat_mv_out)
{
    if (!vbat_mv_out) return ESP_ERR_INVALID_ARG;
    *vbat_mv_out = 0;

    uint8_t raw = 0;
    esp_err_t ret = bq25619_read_reg(i2c_num, BQ25619_REG_VBAT_ADC, &raw);
    if (ret != ESP_OK) return ret;

    // [DSV] Family convention: VBAT_ADC step ~20 mV, offset ~2304 mV.
    // BQ25619 datasheet may use different scaling -- confirm and refine.
    // Output range with these constants: 2304..7404 mV (12 mV per LSB if 8-bit).
    // We use a more conservative 20 mV step and 2304 mV offset; refine later.
    *vbat_mv_out = (uint16_t)(2304U + 20U * (uint16_t)raw);
    return ESP_OK;
}

// =============================================================================
// SoC: voltage LUT + self-learning observation hook
// =============================================================================

// First-pass LUT for a typical 3.7 V single-cell LiPo at light load.
// Concave near full, knee around 3.70-3.75 V, steep drop below 3.40 V.
// See [DEFECT-002] -- refine after bench discharge data.
static const struct { uint16_t mv; uint8_t pct; } s_soc_lut[] = {
    { 4200, 100 },
    { 4100,  90 },
    { 4000,  78 },
    { 3920,  65 },
    { 3850,  50 },
    { 3780,  35 },
    { 3700,  22 },
    { 3600,  12 },
    { 3500,   6 },
    { 3400,   2 },
    { 3300,   0 },
};

uint8_t bq25619_soc_from_mv(uint16_t mv)
{
    const int n = (int)(sizeof(s_soc_lut) / sizeof(s_soc_lut[0]));
    if (mv >= s_soc_lut[0].mv)        return 100;
    if (mv <= s_soc_lut[n - 1].mv)    return 0;

    for (int i = 1; i < n; i++) {
        if (mv >= s_soc_lut[i].mv) {
            // Linear interpolate between i-1 and i (mv: high->low, pct: high->low).
            uint16_t mv_hi  = s_soc_lut[i - 1].mv;
            uint16_t mv_lo  = s_soc_lut[i    ].mv;
            uint8_t  pct_hi = s_soc_lut[i - 1].pct;
            uint8_t  pct_lo = s_soc_lut[i    ].pct;
            uint16_t span_mv = mv_hi - mv_lo;
            uint16_t over_mv = mv - mv_lo;
            int      span_p  = (int)pct_hi - (int)pct_lo;
            int      pct     = (int)pct_lo + (span_p * (int)over_mv) / (int)span_mv;
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;
            return (uint8_t)pct;
        }
    }
    return 0;
}

// -- Self-learning hook (stub) -------------------------------------------------
// Today: just records observed min/max for diagnostics.
// Tomorrow: shifts the s_soc_lut endpoints based on observed cell behaviour
// and persists to NVS so the curve adapts. Hook is in place; algorithm is not.
static uint16_t s_vbat_min_mv = 0xFFFF;  // running observed minimum
static uint16_t s_vbat_max_mv = 0x0000;  // running observed maximum

void bq25619_soc_observe(uint16_t vbat_mv, bool is_charging)
{
    (void)is_charging;  // future: separate charge vs discharge curves
    if (vbat_mv == 0) return;
    if (vbat_mv < s_vbat_min_mv) s_vbat_min_mv = vbat_mv;
    if (vbat_mv > s_vbat_max_mv) s_vbat_max_mv = vbat_mv;
    // TODO: adapt s_soc_lut endpoints when min/max stabilise; persist to NVS.
}

void bq25619_soc_get_observed_extremes(uint16_t *min_mv, uint16_t *max_mv)
{
    if (min_mv) *min_mv = (s_vbat_min_mv == 0xFFFF) ? 0 : s_vbat_min_mv;
    if (max_mv) *max_mv = s_vbat_max_mv;
}

// =============================================================================
// Init
// =============================================================================

esp_err_t bq25619_init(i2c_port_t i2c_num)
{
    int64_t t0 = esp_timer_get_time();

    uint8_t part = 0;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "I2C mutex timeout during init");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = bq25619_read_reg(i2c_num, BQ25619_REG_PART, &part);
    xSemaphoreGive(g_i2c_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "REG_PART read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "%s init OK @ 0x%02X, REG_PART=0x%02X (WHO_AM_I bits [DSV]), boot=%lld us",
             bq25619_get_chip_name(), BQ25619_ADDR, part, (long long)(t1 - t0));
    return ESP_OK;
}

void bq25619_deinit(void)
{
    ESP_LOGI(TAG, "Deinit (no-op)");
}

// =============================================================================
// Boost + ship-mode
// =============================================================================

esp_err_t bq25619_set_boost(i2c_port_t i2c_num, bool enable)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t poc = 0;
    esp_err_t ret = bq25619_read_reg(i2c_num, BQ25619_REG_POC, &poc);
    if (ret == ESP_OK) {
        if (enable) poc |=  BQ25619_POC_BOOST_EN;
        else        poc &= (uint8_t)~BQ25619_POC_BOOST_EN;
        ret = bq25619_write_reg(i2c_num, BQ25619_REG_POC, poc);
    }
    xSemaphoreGive(g_i2c_mutex);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PMID boost %s", enable ? "ENABLED" : "DISABLED");
    } else {
        ESP_LOGE(TAG, "set_boost failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t bq25619_enter_ship_mode(i2c_port_t i2c_num)
{
    ESP_LOGW(TAG, "Entering ship-mode (BATFET_DIS) -- battery disconnect imminent");
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t misc = 0;
    esp_err_t ret = bq25619_read_reg(i2c_num, BQ25619_REG_MISC, &misc);
    if (ret == ESP_OK) {
        misc |= BQ25619_MISC_BATFET_DIS;
        ret = bq25619_write_reg(i2c_num, BQ25619_REG_MISC, misc);
    }
    xSemaphoreGive(g_i2c_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ship-mode write failed: %s", esp_err_to_name(ret));
    }
    // From here, the BQ disconnects the battery within ~10 s (BATFET delay,
    // datasheet-default). USB-C insert or QON long-press wakes the device.
    return ret;
}

// =============================================================================
// Polling task
// =============================================================================

void task_battery_fn(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(BATTERY_POLL_MS);
    TickType_t       last   = xTaskGetTickCount();

    ESP_LOGI(TAG, "Task started on Core %d", xPortGetCoreID());

    while (1) {
        if (!broker_battery_hw_alive()) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        uint8_t  status = 0, fault = 0, poc = 0;
        uint16_t vbat_mv = 0;

        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "I2C mutex timeout (poll)");
            vTaskDelayUntil(&last, period);
            continue;
        }
        bq25619_read_reg(I2C_NUM_1, BQ25619_REG_STATUS,   &status);
        bq25619_read_reg(I2C_NUM_1, BQ25619_REG_FAULT,    &fault);
        bq25619_read_reg(I2C_NUM_1, BQ25619_REG_POC,      &poc);
        bq25619_read_vbat_mv(I2C_NUM_1, &vbat_mv);
        xSemaphoreGive(g_i2c_mutex);

        uint8_t chrg = (status & BQ25619_STATUS_CHRG_MASK) >> BQ25619_STATUS_CHRG_SHIFT;
        bool    pg   = (status & BQ25619_STATUS_PG_GOOD) != 0;
        bool    is_charging = (chrg == BQ25619_CHRG_PRE_CHARGE) ||
                              (chrg == BQ25619_CHRG_FAST_CHARGE);

        bq25619_soc_observe(vbat_mv, is_charging);

        broker_battery_data_t bd = {
            .voltage       = (float)vbat_mv / 1000.0f,
            .percentage    = bq25619_soc_from_mv(vbat_mv),
            .charging      = is_charging,
            .power_good    = pg,
            .charge_state  = chrg,
            .fault         = fault,
            .boost_enabled = (poc & BQ25619_POC_BOOST_EN) != 0,
        };
        broker_battery_write(&bd);

        vTaskDelayUntil(&last, period);
    }
}
