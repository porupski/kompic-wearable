/**
 * @file pcf85063.h
 * @brief PCF85063A Real-Time Clock driver -- I2C, BCD, UTC storage.
 *
 * TEMPLATE APPLIED:
 *   File names:   pcf85063.h/.c
 *   Header guard: PCF85063_H
 *   Prefix:       pcf85063_
 *   Broker macro: BROKER_RTC (standard -- always-on, no enable toggle)
 *   Chip strings: "PCF85063A" / "Battery-backed RTC"
 *   Timeout:      BROKER_RTC_TIMEOUT_MS = 3000
 *   Poll rate:    1000 ms (second resolution -- no point faster)
 *
 * Hardware:
 *   Chip    : NXP PCF85063A
 *   I2C addr: 0x51 (fixed)
 *   Battery-backed -- maintains time when main power is off.
 *   Year stored as 2-digit BCD internally; broker struct stores full year (2026).
 *
 * Design philosophy (Blueprint 8 §2):
 *   RTC = time authority for watch face.
 *   GPS = calibration source (pcf85063_sync_utc).
 *   RTC ALWAYS stores UTC. Timezone is display-only.
 *
 * Phase 15 changes:
 *   - Removed pcf85063_sync_from_gps() (applied tz offset — violated UTC rule).
 *   - Added pcf85063_sync_utc() — writes UTC directly, no timezone math.
 *
 * Core 0 only -- no LVGL includes here.
 * See rtc_tile.h for the UI side.
 *
 * Architecture: Blueprint 1 §8, Blueprint 5 §2, Blueprint 8
 */

#ifndef PCF85063_H
#define PCF85063_H

#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>

// -- Identity -------------------------------------------------------------------
const char *pcf85063_get_chip_name(void);  // returns "PCF85063A"
const char *pcf85063_get_chip_desc(void);  // returns "Battery-backed RTC"

// -- I2C config -----------------------------------------------------------------
#define PCF85063_ADDR  0x51

// -- Hardware alarm INT ---------------------------------------------------------
// GPIO15 is the PCF85063 INT line. Per v7.2 line 98 / line 128, it is a clean
// RTC GPIO and is the "Wake: alarm" source. The driver's hardware-alarm path is
// independent of the software alarm broker (components/alarm/) -- it is intended
// for future deep-sleep wake-on-alarm, not for in-session alarm UX.
#define PCF85063_INT_GPIO  GPIO_NUM_15

// -- Broker data struct (Blueprint 8 §3) ----------------------------------------
// Defined here (driver owns the shape). Stored in data_broker.c.
typedef struct {
    uint8_t  hour;        // UTC 0-23
    uint8_t  minute;      // 0-59
    uint8_t  second;      // 0-59
    uint8_t  day;         // 1-31
    uint8_t  month;       // 1-12
    uint16_t year;        // full year, e.g. 2026
    uint8_t  weekday;     // 0=Sunday ... 6=Saturday
    bool     valid;       // false if last I2C read failed

    // Mandatory bookkeeping:
    uint32_t last_update_ms;
    bool     enabled;     // always true -- RTC cannot be user-disabled
} broker_rtc_data_t;

#define BROKER_RTC_TIMEOUT_MS  3000U

// -- Internal time struct -------------------------------------------------------
// Used by get_time/set_time. Year is 2-digit (years since 2000).
// Broker struct stores full year -- conversion happens in the task.
typedef struct {
    uint8_t second;   // 0-59 (binary, not BCD)
    uint8_t minute;   // 0-59
    uint8_t hour;     // 0-23
    uint8_t day;      // 1-31
    uint8_t weekday;  // 0=Sunday ... 6=Saturday
    uint8_t month;    // 1-12
    uint8_t year;     // Years since 2000 (e.g. 26 = 2026)
    bool    valid;
} pcf85063_time_t;

// -- Lifecycle ------------------------------------------------------------------

/**
 * @brief Read seconds register to confirm device is alive.
 *        No config needed -- PCF85063 runs immediately after power-on.
 *        Called from boot_hw_init.c after I2C scan confirms ACK at 0x51.
 * @return ESP_OK on success.
 */
esp_err_t pcf85063_init(i2c_port_t i2c_num);

/**
 * @brief FreeRTOS task function. Pinned to Core 0 via boot_tasks.c.
 *        Reads time every 1 s, writes to broker_rtc_write().
 *        Stack: 4096 bytes.
 */
void task_rtc_fn(void *arg);

/**
 * @brief No-op for RTC. PCF85063 has no low-power shutdown command.
 */
void pcf85063_deinit(void);

// -- Time read/write ------------------------------------------------------------

/**
 * @brief Read 7 registers (seconds->years) in one burst. BCD->binary internal.
 *        Sets out->valid = false on I2C error.
 */
esp_err_t pcf85063_get_time(i2c_port_t i2c_num, pcf85063_time_t *out);

/**
 * @brief Write time to RTC registers.
 */
esp_err_t pcf85063_set_time(i2c_port_t i2c_num, const pcf85063_time_t *t);

// -- GPS UTC sync (Phase 15 — replaces pcf85063_sync_from_gps) -----------------

/**
 * @brief Set RTC from GPS UTC time — no timezone conversion.
 *
 * Writes UTC directly to the RTC chip. Weekday is computed via
 * Tomohiko Sakamoto's day-of-week algorithm (no mktime dependency).
 * This is the only function GPS sync paths should call. (Blueprint 8 §4)
 *
 * @param i2c_num    I2C port
 * @param utc_hour   GPS UTC hour   (0-23)
 * @param utc_min    GPS UTC minute (0-59)
 * @param utc_sec    GPS UTC second (0-59)
 * @param utc_day    GPS UTC day    (1-31)
 * @param utc_month  GPS UTC month  (1-12)
 * @param utc_year   GPS UTC year   (full 4-digit, e.g. 2026)
 * @return ESP_OK on success
 */
esp_err_t pcf85063_sync_utc(i2c_port_t i2c_num,
                             uint8_t utc_hour,  uint8_t utc_min,  uint8_t utc_sec,
                             uint8_t utc_day,   uint8_t utc_month, uint16_t utc_year);

// -- Hardware alarm (GPIO15 INT) ------------------------------------------------
//
// PCF85063A has H/M/S/D/Weekday alarm registers (0x0B..0x0F). Each register's
// MSB (AEN_*) gates whether that field participates in the match: clear=enabled.
// Control_2 (0x01) bit 7 = AIE (alarm interrupt enable), bit 6 = AF (alarm flag,
// W1C). On match the INT pin goes LOW; firmware must clear AF to release it.
//
// Use case in firmware: future deep-sleep wake-on-alarm. The in-session alarm
// UX is handled by components/alarm/ (polls RTC broker, no INT). This path is
// orthogonal -- enabling it does not interfere with software alarms.

/**
 * @brief Program a one-shot H/M/S alarm. Day and weekday fields are masked
 *        out (AEN_D / AEN_W set HIGH = disabled), so the alarm fires every
 *        day at the specified time-of-day. Sets AIE = 1 and clears AF.
 *        Caller must NOT hold g_i2c_mutex (the function takes it internally).
 *
 * @param i2c_num  I2C port (typically I2C_NUM_0)
 * @param hour     0-23 UTC
 * @param minute   0-59
 * @param second   0-59
 * @return ESP_OK on success
 */
esp_err_t pcf85063_set_alarm(i2c_port_t i2c_num,
                              uint8_t hour, uint8_t minute, uint8_t second);

/**
 * @brief Disable the alarm: clear AIE in control_2 and set AEN bits on all
 *        alarm registers. AF is also cleared.
 */
esp_err_t pcf85063_clear_alarm(i2c_port_t i2c_num);

/**
 * @brief Configure GPIO15 as a falling-edge interrupt and install an ISR that
 *        notifies the given FreeRTOS task. The task is expected to call
 *        pcf85063_clear_alarm_flag() to release the INT line and re-arm.
 *        Idempotent: safe to call once at boot. NOT thread-safe -- call from
 *        boot_hw_init.c before any task is created.
 *
 * @param notify_task  Task to notify via vTaskNotifyGiveFromISR.
 *                     Pass NULL to uninstall the handler.
 * @return ESP_OK on success
 */
esp_err_t pcf85063_install_alarm_isr(TaskHandle_t notify_task);

/**
 * @brief Clear the alarm flag (AF, control_2 bit 6). Releases the INT line.
 *        Caller must NOT hold g_i2c_mutex.
 */
esp_err_t pcf85063_clear_alarm_flag(i2c_port_t i2c_num);

#endif // PCF85063_H