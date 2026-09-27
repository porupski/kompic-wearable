/**
 * @file bme688_drv.c
 * @brief Bosch BME688 driver -- Core 0 only.
 *
 * Wraps the Bosch BME68x sensor API (managed component
 * boschsensortec/bme68x_sensor_api) into our project conventions:
 *   - I2C callbacks talk to ESP-IDF i2c master via the legacy cmd-link
 *     interface (matches the bme280_drv pattern -- caller already holds
 *     g_i2c_mutex when these fire).
 *   - Forced-mode read encapsulated as bme688_read_forced(): trigger,
 *     wait the chip-computed measurement-time, read out, decode.
 *   - Heater profile is fixed at 320 C / 150 ms in v1. Tuning the profile
 *     for VOC discrimination is Phase 4+ work.
 *
 * The Bosch library is a managed component (see idf_component.yml in this
 * directory). The header path is `bme68x.h`; defs live in `bme68x_defs.h`.
 *
 * Read-before-write pattern: task_env_fn reads the broker before writing
 * so the Core-1-owned `home_ref_*` fields are preserved.
 *
 * Architecture: Blueprint 5 §3, Blueprint 14a §4-§5
 */

#include "bme688_drv.h"
#include "bme68x.h"            // Bosch BME68x managed component
#include "bme68x_defs.h"
#include "data_broker.h"
#include "cross_driver.h"
#include "ui_subjects.h"       // Stage 31.2: g_env_q for UI drain path
#include "boot_hw_init.h"      // g_i2c0_bus_handle (Stage 35.3)
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

static const char *TAG = "BME688";

extern SemaphoreHandle_t g_i2c_mutex;

// -- I2C device handle (Stage 35.3) -------------------------------------------
static i2c_master_dev_handle_t s_bme_dev = NULL;

// ── Bosch driver instance (file-static) ───────────────────────────────────────
static struct bme68x_dev   s_dev;
static struct bme68x_conf  s_conf;
static struct bme68x_heatr_conf s_heatr_conf;
static uint8_t             s_i2c_addr = BME68X_I2C_ADDR_LOW;   // 0x76

// ── Task config ───────────────────────────────────────────────────────────────
#define ENV_TASK_PERIOD_MS   2000   // 0.5 Hz -- heater profile + measurement
#define HEATER_TEMP_C        320    // VOC heater plateau
#define HEATER_DURATION_MS   150    // Bosch-typical for VOC

// ── Identity ──────────────────────────────────────────────────────────────────
const char *bme688_get_chip_name(void) { return "BME688";     }
const char *bme688_get_chip_desc(void) { return "T/H/P/Gas"; }

// ─────────────────────────────────────────────────────────────────────────────
// Bosch I2C callbacks. Caller holds g_i2c_mutex when these run.
// Bosch BME68x callback signatures take (uint8_t reg, uint8_t *data, uint32_t
// len, void *intf_ptr) and return int8_t. We forward to the legacy cmd-link
// API which is the same one bme280_drv used -- proven on this hardware.
// ─────────────────────────────────────────────────────────────────────────────

static BME68X_INTF_RET_TYPE bme_i2c_read(uint8_t reg_addr, uint8_t *data,
                                          uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    if (!s_bme_dev) return BME68X_E_COM_FAIL;
    esp_err_t ret = i2c_master_transmit_receive(s_bme_dev, &reg_addr, 1,
                                                 data, len, 100);
    return (ret == ESP_OK) ? BME68X_INTF_RET_SUCCESS : BME68X_E_COM_FAIL;
}

static BME68X_INTF_RET_TYPE bme_i2c_write(uint8_t reg_addr, const uint8_t *data,
                                           uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    if (!s_bme_dev) return BME68X_E_COM_FAIL;
    // Prepend reg address to payload -- new API needs a single contiguous write.
    // BME68x writes are typically 1-4 registers so a small stack buffer suffices.
    uint8_t buf[1 + 16];
    if (len > sizeof(buf) - 1) return BME68X_E_COM_FAIL;
    buf[0] = reg_addr;
    memcpy(&buf[1], data, len);
    esp_err_t ret = i2c_master_transmit(s_bme_dev, buf, len + 1, 100);
    return (ret == ESP_OK) ? BME68X_INTF_RET_SUCCESS : BME68X_E_COM_FAIL;
}

static void bme_delay_us(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    uint32_t ms = (period_us + 999U) / 1000U;
    if (ms == 0) ms = 1;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// ─────────────────────────────────────────────────────────────────────────────
// Driver API
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t bme688_drv_init(int i2c_port)
{
    ESP_LOGI(TAG, "driver v%s", BME688_DRIVER_VERSION);
    (void)i2c_port;

    if (!g_i2c0_bus_handle) {
        ESP_LOGE(TAG, "init: g_i2c0_bus_handle NULL -- bringup_bus0 must run first");
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = s_i2c_addr,
        .scl_speed_hz    = BOOT_I2C_FREQ_HZ,
    };
    esp_err_t add = i2c_master_bus_add_device(g_i2c0_bus_handle, &dev_cfg, &s_bme_dev);
    if (add != ESP_OK) {
        ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(add));
        return add;
    }

    s_dev.intf      = BME68X_I2C_INTF;
    s_dev.intf_ptr  = &s_i2c_addr;
    s_dev.read      = bme_i2c_read;
    s_dev.write     = bme_i2c_write;
    s_dev.delay_us  = bme_delay_us;
    s_dev.amb_temp  = 25;   // Bosch needs an ambient-temp seed for the heater
                            // gas-resistance compensation; 25C is a fine default
                            // and gets corrected by the first real temp read.

    int8_t rslt = bme68x_init(&s_dev);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "bme68x_init failed: %d", rslt);
        return ESP_FAIL;
    }

    // T/P/H oversampling + IIR filter off (matches bme280_drv defaults)
    s_conf.os_hum  = BME68X_OS_1X;
    s_conf.os_pres = BME68X_OS_1X;
    s_conf.os_temp = BME68X_OS_1X;
    s_conf.filter  = BME68X_FILTER_OFF;
    s_conf.odr     = BME68X_ODR_NONE;
    rslt = bme68x_set_conf(&s_conf, &s_dev);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "bme68x_set_conf failed: %d", rslt);
        return ESP_FAIL;
    }

    // Heater profile: single set-point, 320 C plateau, 150 ms duration.
    // Bosch-typical for VOC; tuning is Phase 4+.
    s_heatr_conf.enable     = BME68X_ENABLE;
    s_heatr_conf.heatr_temp = HEATER_TEMP_C;
    s_heatr_conf.heatr_dur  = HEATER_DURATION_MS;
    rslt = bme68x_set_heatr_conf(BME68X_FORCED_MODE, &s_heatr_conf, &s_dev);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "bme68x_set_heatr_conf failed: %d", rslt);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BME688 init OK (chip_id=0x%02X, heater %dC/%dms)",
             s_dev.chip_id, HEATER_TEMP_C, HEATER_DURATION_MS);
    return ESP_OK;
}

esp_err_t bme688_read_forced(float *temp_c, float *hum_pct,
                              float *press_hpa, float *gas_ohm,
                              bool  *gas_valid)
{
    // Trigger forced-mode measurement
    int8_t rslt = bme68x_set_op_mode(BME68X_FORCED_MODE, &s_dev);
    if (rslt != BME68X_OK) {
        ESP_LOGW(TAG, "set_op_mode FORCED failed: %d", rslt);
        return ESP_FAIL;
    }

    // Wait the chip-computed measurement duration. bme68x_get_meas_dur()
    // returns microseconds; for OSR=1x T/P/H + 150 ms heater that's ~170 ms.
    uint32_t meas_us = bme68x_get_meas_dur(BME68X_FORCED_MODE, &s_conf, &s_dev);
    meas_us += (uint32_t)s_heatr_conf.heatr_dur * 1000U;
    s_dev.delay_us(meas_us, s_dev.intf_ptr);

    struct bme68x_data data = {0};
    uint8_t n_fields = 0;
    rslt = bme68x_get_data(BME68X_FORCED_MODE, &data, &n_fields, &s_dev);
    if (rslt < BME68X_OK || n_fields == 0) {
        ESP_LOGW(TAG, "get_data failed: %d (n_fields=%u)", rslt, (unsigned)n_fields);
        return ESP_FAIL;
    }

    *temp_c    = (float)data.temperature;
    *hum_pct   = (float)data.humidity;
    *press_hpa = (float)data.pressure / 100.0f;   // Pa -> hPa
    *gas_ohm   = (float)data.gas_resistance;
    *gas_valid = ((data.status & BME68X_GASM_VALID_MSK) != 0) &&
                 ((data.status & BME68X_HEAT_STAB_MSK)  != 0);

    // Keep the Bosch driver's ambient-temp seed honest for the next heater
    // gas-resistance compensation. Costs us nothing.
    s_dev.amb_temp = (int8_t)data.temperature;

    return ESP_OK;
}

float bme688_pressure_to_altitude(float pressure_hpa, float sea_level_hpa)
{
    return 44330.0f * (1.0f - powf(pressure_hpa / sea_level_hpa, 0.190295f));
}

// ─────────────────────────────────────────────────────────────────────────────
// Core 0 task
// ─────────────────────────────────────────────────────────────────────────────

void task_env_fn(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(ENV_TASK_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();

    ESP_LOGI(TAG, "Task started on Core %d", xPortGetCoreID());

    while (1) {
        vTaskDelayUntil(&last, period);

        if (!broker_env_hw_alive())    continue;
        if (!broker_env_get_enabled()) continue;

        float temp = 0.0f, hum = 0.0f, press = 0.0f, gas = 0.0f;
        bool  gas_valid = false;
        esp_err_t err = ESP_FAIL;

        // Take mutex for the entire forced-mode cycle (~200 ms with the heater).
        // This is the longest single mutex hold in the system; the env task
        // runs at the lowest sensor priority for exactly this reason -- touch
        // and button must not be starved.
        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            err = bme688_read_forced(&temp, &hum, &press, &gas, &gas_valid);
            xSemaphoreGive(g_i2c_mutex);
        } else {
            ESP_LOGW(TAG, "I2C mutex timeout");
        }

        if (err != ESP_OK) continue;   // Let timestamp age -> STALE

        float alt = bme688_pressure_to_altitude(press, 1013.25f);

        // Read-before-write: preserve home_ref_* (Core 1 owns these).
        broker_env_data_t bd = {0};
        broker_env_read(&bd);

        bd.temperature_c      = temp;
        bd.humidity_pct       = hum;
        bd.pressure_hpa       = press;
        bd.altitude_m         = alt;
        bd.gas_resistance_ohm = gas;
        bd.gas_valid          = gas_valid;
        bd.enabled            = broker_env_get_enabled();
        // bd.home_ref_altitude_m / bd.home_ref_valid preserved from read.

        broker_env_write(&bd);

        // Stage 31.2: publish to UI drain queue in addition to the broker
        // write above. Overwrite semantics: producer never blocks; the UI
        // drain timer (ui_subjects.c) picks up the freshest sample every
        // 200 ms on the LVGL task. Broker path stays for CLI (STATUS,
        // WHOAMI) and other consumers not yet subject-bound.
        if (g_env_q) {
            (void)xQueueOverwrite(g_env_q, &bd);
        }

        cross_driver_fire(XD_EVENT_ENV_UPDATED, &bd);
    }
}
