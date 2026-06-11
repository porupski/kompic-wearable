/**
 * @file max_m10s.h
 * @brief u-blox MAX-M10S GNSS receiver -- UART NMEA + UBX driver.
 *
 * Replaces the TU10F driver at the chip layer. The TU10F was a u-blox M10
 * module too (NMEA-compatible), so the NMEA parser carries forward verbatim;
 * the new ground in this driver is:
 *
 *   1. UBX-NAV-TIMEUTC parser  -- atomic UTC time WITHOUT waiting for a fix.
 *      The chip emits this binary frame as soon as the receiver has timing
 *      lock, which is many seconds before a position fix is available. Used
 *      to seed the RTC at cold boot.
 *
 *   2. 1PPS edge ISR on GPIO46 -- TimePulse output for future RTC discipline.
 *      Today we only count edges; Phase 2+ work will use the rising edge to
 *      slew-correct the PCF85063.
 *
 * The broker_gps_data_t shape is preserved field-for-field so gps_tile.c
 * builds with only an include path swap and identity-call rename.
 *
 * Hardware (v7.2 §GPIO ASSIGNMENT, §UART):
 *   Module   : u-blox MAX-M10S (NMEA + UBX on the same UART)
 *   UART     : UART_NUM_1, default 9600 baud (datasheet); reconfigure path TBD
 *   TX GPIO  : 17  (ESP -> GPS RX)
 *   RX GPIO  : 18  (GPS TX -> ESP)
 *   1PPS     : GPIO46 (TimePulse, edge ISR)
 *   No I2C address -- UART-only, excluded from I2C scan table.
 *
 * Core 0 only -- no LVGL includes here.
 *
 * Architecture: Blueprint 1 §8, Blueprint 5 §2, Blueprint 7
 */

#ifndef MAX_M10S_H
#define MAX_M10S_H

#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// -- Identity -----------------------------------------------------------------
const char *max_m10s_get_chip_name(void);   // returns "MAX-M10S"
const char *max_m10s_get_chip_desc(void);   // returns "u-blox M10 GNSS"

// -- Pin / UART config --------------------------------------------------------
// [DSV] Datasheet default for MAX-M10S is 9600 baud; older TU10F module used
// 38400. Leaving 9600 as the start baud here; bench bring-up confirms.
#define MAX_M10S_UART_NUM   UART_NUM_1
#define MAX_M10S_BAUD_RATE  9600
#define MAX_M10S_TX_PIN     17
#define MAX_M10S_RX_PIN     18
#define MAX_M10S_PPS_PIN    GPIO_NUM_46

// -- Fix type enum ------------------------------------------------------------
typedef enum {
    GPS_FIX_NONE = 0,
    GPS_FIX_2D,
    GPS_FIX_3D,
} gps_fix_type_t;

// -- Broker data struct (Blueprint 7 §2) --------------------------------------
// Field-for-field preserved from the old tu10f.h so gps_tile.c builds with
// only the include swap.
typedef struct {
    double         latitude;          // decimal degrees, negative = S
    double         longitude;         // decimal degrees, negative = W
    float          altitude_m;
    float          speed_kmh;
    float          course_deg;
    gps_fix_type_t fix;               // GPS_FIX_NONE / GPS_FIX_2D / GPS_FIX_3D
    uint8_t        sats_in_use;
    float          hdop;
    uint8_t        utc_hour;
    uint8_t        utc_minute;
    uint8_t        utc_second;
    uint8_t        utc_day;
    uint8_t        utc_month;
    uint16_t       utc_year;          // full year, e.g. 2026
    bool           time_valid;
    bool           position_valid;
    bool           first_fix_notified;

    // Mandatory bookkeeping:
    uint32_t       last_update_ms;
    bool           enabled;
} broker_gps_data_t;

#define BROKER_GPS_TIMEOUT_MS  5000U

// -- Debug sentences (raw NMEA for overlay display) ---------------------------
typedef struct {
    char gga[96];
    char rmc[96];
} max_m10s_debug_sentences_t;

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Install UART driver, configure pins, enable RX pullup, install the
 *        1PPS ISR on GPIO46 (rising-edge, count-only).
 *        Called from boot_hw_init.c (not in I2C scan -- UART path).
 */
esp_err_t max_m10s_init(void);

/**
 * @brief FreeRTOS task. Pinned to Core 0 via boot_tasks.c. Stack ~8 KB.
 *        Drains UART, parses NMEA + UBX, writes to broker, seeds RTC on
 *        the first UBX-TIMEUTC (or NMEA fallback) message.
 */
void task_gps_fn(void *arg);

/** @brief Delete UART driver + remove 1PPS ISR. Call on shutdown. */
void max_m10s_deinit(void);

// -- Data access --------------------------------------------------------------

/**
 * @brief Drain UART RX buffer, parse all available NMEA + UBX frames.
 *        Updates internal state. Called from task_gps_fn.
 * @return ESP_OK if at least one frame was parsed, ESP_ERR_NOT_FOUND if empty.
 */
esp_err_t max_m10s_update(void);

/** @brief Copy internal GPS state into broker-shaped struct. */
void max_m10s_get_snapshot(broker_gps_data_t *out);

/** @brief True iff we have a valid fix (fix != GPS_FIX_NONE). */
bool max_m10s_has_fix(void);

/** @brief Flush UART RX buffer. Use after UBX config to clear stale data. */
void max_m10s_flush(void);

/** @brief Copy the last raw GGA and RMC sentences for the debug overlay. */
void max_m10s_get_debug_sentences(char *gga_buf, size_t gga_sz,
                                   char *rmc_buf, size_t rmc_sz);

/** @brief Extract last-known UTC time. Returns ESP_ERR_INVALID_STATE if
 *         time_valid is false. */
esp_err_t max_m10s_get_utc_time(uint16_t *year, uint8_t *month, uint8_t *day,
                                 uint8_t *hour, uint8_t *minute, uint8_t *second);

// -- 1PPS access (test / RTC discipline) --------------------------------------

/** @brief Cumulative count of 1PPS rising edges since boot. */
uint32_t max_m10s_get_pps_count(void);

/** @brief Microsecond timestamp (esp_timer_get_time()) of the most recent
 *         1PPS rising edge. 0 if no edge yet. */
int64_t max_m10s_get_last_pps_us(void);

// -- UBX parser entry point (exposed for test harness simulation) -------------

/**
 * @brief Feed one byte into the UBX state machine. When a complete
 *        NAV-TIMEUTC frame arrives, the internal time_valid flag is set.
 *        Test harnesses can drive this directly to validate the parser
 *        without hardware.
 */
void max_m10s_feed_ubx_byte(uint8_t byte);

#endif // MAX_M10S_H
