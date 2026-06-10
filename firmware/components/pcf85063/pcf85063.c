/**
 * @file pcf85063.c
 * @brief PCF85063A RTC driver implementation.
 *
 * Carries forward from legacy:
 *   - BCD helpers (bcd_to_dec, dec_to_bcd)
 *   - 7-byte burst register read/write (seconds through years)
 *   - Oscillator stop flag detection (bit 7 of seconds register)
 *
 * Phase 15 changes:
 *   - Removed pcf85063_sync_from_gps() — it applied tz_offset before writing
 *     to the RTC chip, violating Blueprint 8 §2 ("RTC ALWAYS stores UTC").
 *     This was the primary cause of the double-offset bug (P1).
 *   - Added pcf85063_sync_utc() — writes UTC directly. Weekday computed via
 *     Tomohiko Sakamoto algorithm (no mktime/gmtime dependency).
 *
 * Core 0 only. No LVGL. No direct NVS calls.
 * Data flows: I2C RTC -> this task -> broker_rtc_write() -> Core 1.
 *
 * Architecture: Blueprint 1 §3, Blueprint 5 §3, Blueprint 8
 */

#include "pcf85063.h"
#include "data_broker.h"
#include "boot_hw_init.h"   // g_i2c_mutex
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"       // IRAM_ATTR
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "PCF85063";

// -- Module config --------------------------------------------------------------
#define PCF85063_POLL_MS  1000   // 1 Hz -- RTC has second resolution

// -- Register addresses ---------------------------------------------------------
#define REG_CONTROL_1     0x00
#define REG_CONTROL_2     0x01   // bit 7 = AIE, bit 6 = AF (write 0 to clear)
#define REG_SECONDS       0x04
#define REG_MINUTES       0x05
#define REG_HOURS         0x06
#define REG_DAYS          0x07
#define REG_WEEKDAYS      0x08
#define REG_MONTHS        0x09
#define REG_YEARS         0x0A
#define REG_SECOND_ALARM  0x0B   // bit 7 = AEN_S (1 = disabled)
#define REG_MINUTE_ALARM  0x0C   // bit 7 = AEN_M
#define REG_HOUR_ALARM    0x0D   // bit 7 = AEN_H
#define REG_DAY_ALARM     0x0E   // bit 7 = AEN_D
#define REG_WEEKDAY_ALARM 0x0F   // bit 7 = AEN_W

#define CTRL2_AIE  (1 << 7)
#define CTRL2_AF   (1 << 6)
#define AEN_OFF    (1 << 7)  // setting this bit on an alarm reg DISABLES that field

// -- BCD helpers ----------------------------------------------------------------
static inline uint8_t bcd_to_dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static inline uint8_t dec_to_bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// -- Identity -------------------------------------------------------------------

const char *pcf85063_get_chip_name(void) { return "PCF85063A";          }
const char *pcf85063_get_chip_desc(void) { return "Battery-backed RTC"; }

// -- Tomohiko Sakamoto day-of-week algorithm ------------------------------------
// Returns 0=Sunday .. 6=Saturday for any Gregorian date.
// No mktime/gmtime dependency — pure integer arithmetic.
static uint8_t day_of_week(uint16_t y, uint8_t m, uint8_t d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

// -- Init -----------------------------------------------------------------------

esp_err_t pcf85063_init(i2c_port_t i2c_num)
{
    uint8_t test = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, REG_SECONDS, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &test, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        if (test & 0x80) {
            ESP_LOGW(TAG, "Oscillator stop flag set -- RTC lost power, time invalid");
        }
        ESP_LOGI(TAG, "%s init OK @ 0x%02X", pcf85063_get_chip_name(), PCF85063_ADDR);
    } else {
        ESP_LOGE(TAG, "PCF85063 not found @ 0x%02X: %s", PCF85063_ADDR, esp_err_to_name(ret));
    }
    return ret;
}

// -- Deinit (no-op for RTC) -----------------------------------------------------

void pcf85063_deinit(void)
{
    ESP_LOGI(TAG, "Deinit (no-op)");
}

// -- Time read/write ------------------------------------------------------------

esp_err_t pcf85063_get_time(i2c_port_t i2c_num, pcf85063_time_t *t)
{
    uint8_t data[7] = {0};

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, REG_SECONDS, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 7, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        t->second  = bcd_to_dec(data[0] & 0x7F);
        t->minute  = bcd_to_dec(data[1] & 0x7F);
        t->hour    = bcd_to_dec(data[2] & 0x3F);
        t->day     = bcd_to_dec(data[3] & 0x3F);
        t->weekday = bcd_to_dec(data[4] & 0x07);
        t->month   = bcd_to_dec(data[5] & 0x1F);
        t->year    = bcd_to_dec(data[6]);
        t->valid   = true;
    } else {
        t->valid = false;
        ESP_LOGW(TAG, "pcf85063_get_time failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t pcf85063_set_time(i2c_port_t i2c_num, const pcf85063_time_t *t)
{
    uint8_t data[7] = {
        dec_to_bcd(t->second),
        dec_to_bcd(t->minute),
        dec_to_bcd(t->hour),
        dec_to_bcd(t->day),
        dec_to_bcd(t->weekday),
        dec_to_bcd(t->month),
        dec_to_bcd(t->year),
    };

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, REG_SECONDS, true);
    i2c_master_write(cmd, data, 7, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "RTC set: %02d:%02d:%02d %04d-%02d-%02d (wd=%d)",
                 t->hour, t->minute, t->second,
                 2000 + t->year, t->month, t->day, t->weekday);
    } else {
        ESP_LOGE(TAG, "pcf85063_set_time failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

// -- GPS UTC sync (Phase 15 — replaces pcf85063_sync_from_gps) -----------------

esp_err_t pcf85063_sync_utc(i2c_port_t i2c_num,
                             uint8_t utc_hour,  uint8_t utc_min,  uint8_t utc_sec,
                             uint8_t utc_day,   uint8_t utc_month, uint16_t utc_year)
{
    pcf85063_time_t rtc = {
        .hour    = utc_hour,
        .minute  = utc_min,
        .second  = utc_sec,
        .day     = utc_day,
        .month   = utc_month,
        .year    = (uint8_t)(utc_year % 100),  // 2026 -> 26
        .weekday = day_of_week(utc_year, utc_month, utc_day),
        .valid   = true,
    };

    ESP_LOGI(TAG, "GPS UTC sync: %02u:%02u:%02u %04u-%02u-%02u (wd=%u)",
             utc_hour, utc_min, utc_sec,
             utc_year, utc_month, utc_day, (unsigned)rtc.weekday);

    return pcf85063_set_time(i2c_num, &rtc);
}

// -- Task (Blueprint 8 §6) -----------------------------------------------------

void task_rtc_fn(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(PCF85063_POLL_MS);
    TickType_t       last   = xTaskGetTickCount();

    ESP_LOGI(TAG, "Task started on Core %d", xPortGetCoreID());

    while (1) {
        if (!broker_rtc_hw_alive()) {
            vTaskDelayUntil(&last, period);
            continue;
        }

        pcf85063_time_t t = {0};

        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            pcf85063_get_time(I2C_NUM_0, &t);
            xSemaphoreGive(g_i2c_mutex);
        } else {
            ESP_LOGW(TAG, "I2C mutex timeout");
            vTaskDelayUntil(&last, period);
            continue;
        }

        broker_rtc_data_t bd = {
            .hour    = t.hour,
            .minute  = t.minute,
            .second  = t.second,
            .day     = t.day,
            .month   = t.month,
            .year    = (uint16_t)(2000 + t.year),
            .weekday = t.weekday,
            .valid   = t.valid,
        };
        broker_rtc_write(&bd);

        vTaskDelayUntil(&last, period);
    }
}

// =============================================================================
// Hardware alarm (GPIO15 INT) -- see pcf85063.h for design notes.
// Independent of components/alarm/ (which polls broker_rtc for in-session UX).
// =============================================================================

static TaskHandle_t s_alarm_notify_task = NULL;
static volatile bool s_alarm_isr_installed = false;

static esp_err_t i2c_write_reg(i2c_port_t i2c_num, uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t i2c_read_reg(i2c_port_t i2c_num, uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void IRAM_ATTR pcf85063_alarm_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    if (s_alarm_notify_task) {
        vTaskNotifyGiveFromISR(s_alarm_notify_task, &hpw);
    }
    if (hpw) portYIELD_FROM_ISR();
}

esp_err_t pcf85063_set_alarm(i2c_port_t i2c_num,
                              uint8_t hour, uint8_t minute, uint8_t second)
{
    if (hour > 23 || minute > 59 || second > 59) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // H/M/S enabled (AEN bit = 0); day + weekday disabled (AEN bit = 1).
    ret |= i2c_write_reg(i2c_num, REG_SECOND_ALARM,  dec_to_bcd(second));
    ret |= i2c_write_reg(i2c_num, REG_MINUTE_ALARM,  dec_to_bcd(minute));
    ret |= i2c_write_reg(i2c_num, REG_HOUR_ALARM,    dec_to_bcd(hour));
    ret |= i2c_write_reg(i2c_num, REG_DAY_ALARM,     AEN_OFF);
    ret |= i2c_write_reg(i2c_num, REG_WEEKDAY_ALARM, AEN_OFF);

    // Enable AIE, clear AF in control_2.
    ret |= i2c_write_reg(i2c_num, REG_CONTROL_2, CTRL2_AIE);

    xSemaphoreGive(g_i2c_mutex);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Alarm armed: %02u:%02u:%02u UTC (daily)", hour, minute, second);
    } else {
        ESP_LOGE(TAG, "set_alarm I2C failure (mask): %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pcf85063_clear_alarm(i2c_port_t i2c_num)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_OK;
    ret |= i2c_write_reg(i2c_num, REG_SECOND_ALARM,  AEN_OFF);
    ret |= i2c_write_reg(i2c_num, REG_MINUTE_ALARM,  AEN_OFF);
    ret |= i2c_write_reg(i2c_num, REG_HOUR_ALARM,    AEN_OFF);
    ret |= i2c_write_reg(i2c_num, REG_DAY_ALARM,     AEN_OFF);
    ret |= i2c_write_reg(i2c_num, REG_WEEKDAY_ALARM, AEN_OFF);
    ret |= i2c_write_reg(i2c_num, REG_CONTROL_2,     0x00);   // AIE off, AF cleared
    xSemaphoreGive(g_i2c_mutex);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Alarm cleared");
    return ret;
}

esp_err_t pcf85063_clear_alarm_flag(i2c_port_t i2c_num)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    // Read-modify-write: clear AF (bit 6) but keep AIE (bit 7) intact.
    uint8_t c2 = 0;
    esp_err_t ret = i2c_read_reg(i2c_num, REG_CONTROL_2, &c2);
    if (ret == ESP_OK) {
        c2 &= (uint8_t)~CTRL2_AF;  // W1C semantics: writing 0 clears
        ret = i2c_write_reg(i2c_num, REG_CONTROL_2, c2);
    }
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t pcf85063_install_alarm_isr(TaskHandle_t notify_task)
{
    if (notify_task == NULL) {
        // Uninstall.
        if (s_alarm_isr_installed) {
            gpio_isr_handler_remove(PCF85063_INT_GPIO);
            s_alarm_isr_installed = false;
        }
        s_alarm_notify_task = NULL;
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PCF85063_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    // INT idles HIGH
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) return ret;

    esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) return isr_ret;

    s_alarm_notify_task = notify_task;

    ret = gpio_isr_handler_add(PCF85063_INT_GPIO, pcf85063_alarm_isr, NULL);
    if (ret != ESP_OK) {
        s_alarm_notify_task = NULL;
        return ret;
    }
    s_alarm_isr_installed = true;
    ESP_LOGI(TAG, "Alarm ISR installed on GPIO%d (falling edge)", PCF85063_INT_GPIO);
    return ESP_OK;
}