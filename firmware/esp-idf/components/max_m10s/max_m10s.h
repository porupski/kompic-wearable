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
 * Hardware (master pinout v20 iv7.1 §GPIO ASSIGNMENT, §UART; validated on
 * Mk1b iv8.0 bench 2026-09-20 via Arduino sketch 18c):
 *   Module   : u-blox MAX-M10S (NMEA + UBX on the same UART)
 *   UART     : UART_NUM_1, default 9600 baud (datasheet); reconfigure path TBD
 *   TX GPIO  : 17  (U1TXD IOMUX, ESP -> GPS RXD)
 *   RX GPIO  : 18  (U1RXD IOMUX, GPS TXD -> ESP)
 *   1PPS     : GPIO46 (TimePulse, edge ISR)
 *   No I2C address -- UART-only, excluded from I2C scan table.
 *
 * Core 0 only -- no LVGL includes here.
 *
 * Architecture: Blueprint 1 §8, Blueprint 5 §2, Blueprint 7
 */

#ifndef MAX_M10S_H
#define MAX_M10S_H


// Driver version: MAJOR.MINOR.PATCH -- bump PATCH on any change here,
// MINOR on feature adds, MAJOR on release quality (beta / RC / GA).
#define MAX_M10S_DRIVER_VERSION  "0.2.0"
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
// Pin roles per master pinout (0_Kompic_Pinout_MASTER_v20_iv7.1.md):
//   GPIO17 = U1TXD IOMUX -> ESP drives GPS RXD
//   GPIO18 = U1RXD IOMUX -> ESP receives from GPS TXD
// The earlier reversed defaults (TX=18, RX=17) shipped in this file AND
// in Arduino sketches 18b/18c never matched the PCB. Bench-verified fix
// captured in docs/build_info/Mk1b_build_reports/GPS_M10S_Fixing.md.
#define MAX_M10S_TX_PIN     7 //17 bodged over to MAX_INT (GPIO07)
#define MAX_M10S_RX_PIN     18 //Swapped TX RX from original circuit, bodged
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

// -- Stage 31.4b: runtime UBX-CFG-VALSET writes ------------------------------
//
// Key IDs sourced from u-blox M10 Interface Description (UBX-21035062).
// Only U1/E1 keys used today (single-byte payload).

// Dynamic platform model (CFG-NAVSPG-DYNMODEL, key 0x20110021). WRIST/BIKE
// are "not available in all products" per the manual; MAX-M10S may NAK them.
typedef enum {
    UBX_DYNMODEL_PORTABLE   = 0,
    UBX_DYNMODEL_STATIONARY = 2,
    UBX_DYNMODEL_PEDESTRIAN = 3,
    UBX_DYNMODEL_AUTOMOTIVE = 4,
    UBX_DYNMODEL_SEA        = 5,
    UBX_DYNMODEL_AIR1G      = 6,
    UBX_DYNMODEL_AIR2G      = 7,
    UBX_DYNMODEL_AIR4G      = 8,
    UBX_DYNMODEL_WRIST      = 9,
    UBX_DYNMODEL_BIKE       = 10,
} max_m10s_dynmodel_t;

/** @brief Send CFG-NAVSPG-DYNMODEL VALSET to RAM. Non-blocking. */
esp_err_t           max_m10s_set_dynmodel(max_m10s_dynmodel_t mode);
const char         *max_m10s_dynmodel_name(max_m10s_dynmodel_t mode);
max_m10s_dynmodel_t max_m10s_get_dynmodel(void);   // last one we sent

// UBX-MON-RF (class 0x0A, id 0x38): antenna status + noise + AGC.
// ant_status: 0=INIT, 1=DONTKNOW, 2=OK, 3=SHORT, 4=OPEN.
// ant_power : 0=OFF,  1=ON,       2=DONTKNOW.
typedef struct {
    bool     valid;              // false until first UBX-MON-RF received
    uint32_t last_update_ms;
    uint8_t  ant_status;
    uint8_t  ant_power;
    uint16_t noise_per_ms;
    uint16_t agc_cnt;            // 0..8191 -> 0..100 %
} max_m10s_monrf_t;

esp_err_t   max_m10s_enable_monrf(uint8_t rate);
void        max_m10s_get_monrf(max_m10s_monrf_t *out);
const char *max_m10s_ant_status_name(uint8_t ant_status);

// -- NMEA GSV-derived signal-strength summary --------------------------------
// Populated from NMEA GSV sentences (default output, no UBX config needed).
// A satellite with an SNR entry ages out after ~2.5s so dropped sats stop
// contributing to the bar.
typedef struct {
    uint8_t  sats_in_view;   // total distinct sats reported in the window
    uint8_t  sats_with_snr;  // subset with a non-zero SNR field
    uint8_t  max_cn0;        // dBHz, best single SNR in the window
    uint8_t  top4_avg_cn0;   // dBHz, average of top-4 SNRs (0 if <1 sat)
    uint32_t last_update_ms;
} max_m10s_snr_summary_t;

void max_m10s_get_snr_summary(max_m10s_snr_summary_t *out);

// -- UBX diagnostics ----------------------------------------------------------
// Counters incremented on every valid UBX frame received. Zero-cost when
// unused; primary use is proving whether ESP->GPS TX physically works.
typedef struct {
    uint32_t total;      // any valid UBX frame
    uint32_t mon_ver;    // UBX-MON-VER responses (from GPS_PING)
    uint32_t mon_rf;     // UBX-MON-RF frames (when enabled)
    uint32_t nav_timeutc;// UBX-NAV-TIMEUTC frames
    uint32_t ack_ack;    // UBX-ACK-ACK responses to our CFG-VALSETs
    uint32_t ack_nak;    // UBX-ACK-NAK responses
} max_m10s_ubx_counters_t;

void max_m10s_get_ubx_counters(max_m10s_ubx_counters_t *out);

/** @brief Send UBX-MON-VER poll. Chip responds with a MON-VER frame within
 *         ~10 ms if TX path is alive. Use as a "does ESP->GPS work?" probe.
 *         Non-blocking; caller polls max_m10s_get_ubx_counters().mon_ver. */
esp_err_t max_m10s_ping(void);

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
