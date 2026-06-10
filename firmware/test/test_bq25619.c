/**
 * @file test_bq25619.c
 * @brief Standalone diagnostic for the BQ25619 charger driver.
 *
 * Wiring: see Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C bus 2.
 *   SDA=GPIO4, SCL=GPIO5, addr 0x6A, 400 kHz.
 *
 * Phases:
 *   1. I2C bus 2 init + WHO_AM_I (REG_PART) read.
 *   2. Status register dump (REG_STATUS, REG_FAULT, REG_POC, REG_MISC).
 *   3. Battery voltage + SoC + charge state.
 *   4. Boost toggle: enable -> read back POC -> disable -> read back.
 *   5. Ship-mode WRITE IS PRINTED ONLY -- not executed (would shut down the
 *      test device with no easy way back except USB-C insert or QON press).
 *
 * Expected output (real hw):
 *   I (xx) test_bq25619: Chip: BQ25619 -- Li-ion charger + PMID boost
 *   I (xx) test_bq25619: I2C bus 2 init: <us>
 *   I (xx) test_bq25619: init: ESP_OK in <us>
 *   I (xx) test_bq25619: regs  STATUS=0x.. FAULT=0x00 POC=0x.. MISC=0x..
 *   I (xx) test_bq25619: Vbat=3.92 V  SoC=72%  state=Fast charge  power=USB OK  boost=OFF
 *   I (xx) test_bq25619: boost ENABLE  -> POC=0x..  OK
 *   I (xx) test_bq25619: boost DISABLE -> POC=0x..  OK
 *   I (xx) test_bq25619: ship-mode write WOULD be: REG_MISC |= 0x20 (SKIPPED)
 */

#include "bq25619.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "test_bq25619";

SemaphoreHandle_t g_i2c_mutex = NULL;

#define TEST_I2C_PORT  I2C_NUM_1   // bus 2
#define TEST_I2C_SDA   4
#define TEST_I2C_SCL   5
#define TEST_I2C_HZ    400000

static const char *k_chrg[] = { "Idle", "Pre-charge", "Fast charge", "Done" };

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

static void dump_regs(void)
{
    uint8_t status = 0, fault = 0, poc = 0, misc = 0;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    bq25619_read_reg(TEST_I2C_PORT, BQ25619_REG_STATUS, &status);
    bq25619_read_reg(TEST_I2C_PORT, BQ25619_REG_FAULT,  &fault);
    bq25619_read_reg(TEST_I2C_PORT, BQ25619_REG_POC,    &poc);
    bq25619_read_reg(TEST_I2C_PORT, BQ25619_REG_MISC,   &misc);
    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "regs  STATUS=0x%02X FAULT=0x%02X POC=0x%02X MISC=0x%02X",
             status, fault, poc, misc);
}

static void test_bq25619_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             bq25619_get_chip_name(), bq25619_get_chip_desc());

    if (g_i2c_mutex == NULL) {
        g_i2c_mutex = xSemaphoreCreateMutex();
        configASSERT(g_i2c_mutex != NULL);
    }

    int64_t t_bus0 = esp_timer_get_time();
    esp_err_t err = test_i2c_bus_init();
    int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "I2C bus 2 init: %lld us -> %s",
             (long long)(t_bus1 - t_bus0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    int64_t t_i0 = esp_timer_get_time();
    err = bq25619_init(TEST_I2C_PORT);
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ---- Phase 2: register dump ----
    dump_regs();

    // ---- Phase 3: Vbat / SoC ----
    uint16_t vbat_mv = 0;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    int64_t t_v0 = esp_timer_get_time();
    bq25619_read_vbat_mv(TEST_I2C_PORT, &vbat_mv);
    int64_t t_v1 = esp_timer_get_time();
    uint8_t status = 0;
    bq25619_read_reg(TEST_I2C_PORT, BQ25619_REG_STATUS, &status);
    xSemaphoreGive(g_i2c_mutex);

    uint8_t chrg = (status & BQ25619_STATUS_CHRG_MASK) >> BQ25619_STATUS_CHRG_SHIFT;
    bool    pg   = (status & BQ25619_STATUS_PG_GOOD) != 0;
    uint8_t soc  = bq25619_soc_from_mv(vbat_mv);
    ESP_LOGI(TAG, "Vbat=%u mV (%.2f V) read_dt=%lld us  SoC=%u%%  state=%s  power=%s",
             vbat_mv, (double)vbat_mv / 1000.0, (long long)(t_v1 - t_v0),
             soc, (chrg < 4) ? k_chrg[chrg] : "???",
             pg ? "USB OK" : "Battery only");

    bq25619_soc_observe(vbat_mv, chrg == BQ25619_CHRG_FAST_CHARGE || chrg == BQ25619_CHRG_PRE_CHARGE);
    uint16_t v_min = 0, v_max = 0;
    bq25619_soc_get_observed_extremes(&v_min, &v_max);
    ESP_LOGI(TAG, "observed extremes (this run): min=%u mV max=%u mV", v_min, v_max);

    // ---- Phase 4: boost toggle ----
    err = bq25619_set_boost(TEST_I2C_PORT, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    dump_regs();
    err = bq25619_set_boost(TEST_I2C_PORT, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    dump_regs();

    // ---- Phase 5: ship-mode is PRINTED ONLY ----
    ESP_LOGW(TAG, "ship-mode write WOULD be: REG_MISC |= 0x%02X  (SKIPPED in test harness)",
             BQ25619_MISC_BATFET_DIS);
    ESP_LOGW(TAG, "To verify ship-mode on bench, call bq25619_enter_ship_mode() manually.");

    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "free heap after test: %u bytes", (unsigned)heap_after);
    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_bq25619_run();
}
