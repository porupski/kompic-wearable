/**
 * @file pcf85063_cmd.c
 * @brief PCF85063A RTC command surface -- see pcf85063_cmd.h.
 */

#include "pcf85063_cmd.h"
#include "pcf85063.h"
#include "data_broker.h"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

extern SemaphoreHandle_t g_i2c_mutex;   // I2C_NUM_0 -- RTC lives there.

static const char *TAG = "PCF_CMD";

#define PCF_I2C_PORT       I2C_NUM_0
#define PCF_MUTEX_TIMEOUT  pdMS_TO_TICKS(200)

// ── time get / set ──────────────────────────────────────────────────────────

esp_err_t pcf85063_cmd_time_get(pcf85063_time_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(g_i2c_mutex, PCF_MUTEX_TIMEOUT) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t r = pcf85063_get_time(PCF_I2C_PORT, out);
    xSemaphoreGive(g_i2c_mutex);
    return r;
}

esp_err_t pcf85063_cmd_time_set(const pcf85063_time_t *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(g_i2c_mutex, PCF_MUTEX_TIMEOUT) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t r = pcf85063_set_time(PCF_I2C_PORT, t);
    xSemaphoreGive(g_i2c_mutex);
    if (r == ESP_OK) ESP_LOGI(TAG, "time set OK");
    return r;
}

esp_err_t pcf85063_cmd_sync_utc(uint8_t hour, uint8_t minute, uint8_t second,
                                uint8_t day,  uint8_t month,  uint16_t year)
{
    // pcf85063_sync_utc takes the mutex internally.
    return pcf85063_sync_utc(PCF_I2C_PORT, hour, minute, second, day, month, year);
}

// ── ISO timestamps ──────────────────────────────────────────────────────────

void pcf85063_cmd_iso_now(char *out, size_t n)
{
    if (!out || n == 0) return;
    if (broker_rtc_hw_alive()) {
        broker_rtc_data_t r; broker_rtc_read(&r);
        if (r.valid) {
            snprintf(out, n, "%04u-%02u-%02uT%02u:%02u:%02u",
                     (unsigned)r.year,  (unsigned)r.month, (unsigned)r.day,
                     (unsigned)r.hour,  (unsigned)r.minute,(unsigned)r.second);
            return;
        }
    }
    snprintf(out, n, "oscstop");
}

esp_err_t pcf85063_cmd_filename_stamp(char *out, size_t n)
{
    if (!out || n < 20) return ESP_ERR_INVALID_ARG;  // "YYYY-MM-DD_HH-MM-SS" + NUL = 20
    if (!broker_rtc_hw_alive()) return ESP_ERR_INVALID_STATE;
    broker_rtc_data_t r; broker_rtc_read(&r);
    if (!r.valid) return ESP_ERR_INVALID_STATE;
    snprintf(out, n, "%04u-%02u-%02u_%02u-%02u-%02u",
             (unsigned)r.year,  (unsigned)r.month, (unsigned)r.day,
             (unsigned)r.hour,  (unsigned)r.minute,(unsigned)r.second);
    return ESP_OK;
}

// ── dump ────────────────────────────────────────────────────────────────────

static const char *k_pcf_reg_names[18] = {
    "Control_1",   "Control_2",   "Offset",     "RAM_byte",
    "Seconds",     "Minutes",     "Hours",      "Days",
    "Weekdays",    "Months",      "Years",
    "Sec_alarm",   "Min_alarm",   "Hour_alarm", "Day_alarm", "Wday_alarm",
    "Timer_val",   "Timer_mode",
};

void pcf85063_cmd_dump(void)
{
    printf("[RTC] %s @ 0x%02X (I2C bus 0)\n",
           pcf85063_get_chip_name(), PCF85063_ADDR);

    uint8_t regs[18] = {0};
    esp_err_t r = pcf85063_read_regs_raw(PCF_I2C_PORT, 0x00, regs, sizeof(regs));
    if (r != ESP_OK) {
        printf("[RTC]   reg read failed: %s\n", esp_err_to_name(r));
        return;
    }
    printf("[RTC]   PCF85063A registers 0x00..0x11:\n");
    for (int i = 0; i < 18; i++) {
        printf("[RTC]     0x%02X  %-11s = 0x%02X  (%3u)\n",
               i, k_pcf_reg_names[i], regs[i], regs[i]);
    }
    if (regs[4] & 0x80) {
        printf("[RTC]   WARN: Seconds bit 7 (OS) = 1 -- oscillator was stopped, time INVALID\n");
    }

    char iso[32]; pcf85063_cmd_iso_now(iso, sizeof(iso));
    printf("[RTC]   broker now      = %s\n", iso);
}

// ── status summary ──────────────────────────────────────────────────────────

void pcf85063_cmd_status_summary(char *out, size_t max)
{
    if (!out || max == 0) return;
    broker_rtc_data_t r; broker_rtc_read(&r);
    if (r.valid) {
        snprintf(out, max,
                 "%04u-%02u-%02uT%02u:%02u:%02uZ valid=1 wd=%u",
                 (unsigned)r.year,  (unsigned)r.month, (unsigned)r.day,
                 (unsigned)r.hour,  (unsigned)r.minute,(unsigned)r.second,
                 (unsigned)r.weekday);
    } else {
        snprintf(out, max, "invalid (oscstop or not synced)");
    }
}
