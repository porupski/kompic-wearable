/**
 * @file data_broker.h
 * @brief Central data broker - the only legal data path between Core 0 and Core 1.
 *
 * ARCHITECTURE (Blueprint 4 §2 - Thin Broker + Fat Driver):
 *   The broker does NOT define data structs. Each driver header owns its own
 *   broker_xxx_data_t typedef and BROKER_XXX_TIMEOUT_MS constant.
 *   This file #includes those headers and exposes the proxy API.
 *
 *   Adding a new module is exactly 3 steps (Blueprint 4 §3):
 *     1. Define broker_foo_data_t + BROKER_FOO_TIMEOUT_MS in foo.h
 *     2. #include "foo.h" here, add API block below
 *     3. BROKER_MODULE_IMPL(foo, FOO, BROKER_FOO_TIMEOUT_MS, true/false) in data_broker.c
 *
 * RULES (Blueprint 1 §4 - non-negotiable):
 *   - Single mutex. Lock held for memcpy only - never during I/O, logic, or LVGL calls.
 *   - No dirty flags. Core 1 polls unconditionally every 200 ms.
 *   - Status derived on read - never stored.
 *   - hw_alive: written once at boot before any task; read-only forever. No mutex.
 *   - enabled: Core 1 writes, Core 0 reads. Guarded by same mutex.
 *
 * ATOMIC GLOBALS - no mutex, single writer enforced by convention (Blueprint 4 §7):
 *   Do not add new atomic globals without explicit review.
 */

#ifndef DATA_BROKER_H
#define DATA_BROKER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// --- Status enum ---------------------------------------------------------------
// Universal across every module. Same enum, no exceptions. (Blueprint 1 Â§4)

typedef enum {
    SENSOR_DISABLED  = 0,  // Grey   - power switch OFF
    SENSOR_OFFLINE   = 1,  // Red    - HW dead / init failed / timeout
    SENSOR_ACQUIRING = 2,  // Yellow - alive but searching / calibrating
    SENSOR_STALE     = 3,  // Orange - data older than module timeout
    SENSOR_ONLINE    = 4,  // Green  - fresh, valid data
    SENSOR_NOTIF     = 5,  // Purple - one-time event (first fix, low battery)
} sensor_status_t;

// --- UI theme enum -------------------------------------------------------------

typedef enum {
    UI_THEME_DARK  = 0,
    UI_THEME_LIGHT = 1,
} ui_theme_t;

// ─── IMU gesture events (stub for future wrist-raise wake) ─────────────────

typedef enum {
    IMU_GESTURE_NONE        = 0,
    IMU_GESTURE_WRIST_RAISE = 1,   // Future: wake screen on wrist raise
    IMU_GESTURE_WRIST_DOWN  = 2,   // Future: turn screen off
    IMU_GESTURE_SHAKE       = 3,   // Future: custom action
} imu_gesture_t;

// --- Atomic globals ------------------------------------------------------------
// Defined in data_broker.c. Single-writer rule - no mutex needed.

extern volatile bool     g_gps_time_seeded;        // Core 0 â†’ Core 1
extern volatile uint8_t  g_saved_brightness;        // Core 1 before lock
// NOTE: g_screen_locked, g_wake_display, g_show_shutdown_overlay,
//       g_shutdown_latched, g_dtap_screen_off moved to boot_power.h
extern volatile uint8_t  g_ui_theme;                // Core 1 only (ui_theme_t)
extern volatile bool     g_blue_light_on;           // Core 1 only

extern volatile int8_t g_tz_offset_hours;

// IMU gesture event (stub for future implementation)
extern volatile uint8_t  g_imu_gesture;         // imu_gesture_t value, Core 0 -> Core 1

// --- Driver data struct imports ------------------------------------------------
// Each driver owns its broker_xxx_data_t. The broker stores and proxies it.
// -- ADD NEW MODULE: #include "mymodule.h" here ---------------------------------

#include "max_m10s.h"    // → broker_gps_data_t,     BROKER_GPS_TIMEOUT_MS
#include "pcf85063.h"    // → broker_rtc_data_t,     BROKER_RTC_TIMEOUT_MS
#include "lis3mdl.h"     // → broker_mag_data_t,     BROKER_MAG_TIMEOUT_MS
#include "bq25619.h"     // → broker_battery_data_t, BROKER_BATTERY_TIMEOUT_MS
#include "veml6030.h"    // → broker_light_data_t,   BROKER_LIGHT_TIMEOUT_MS
#include "drv2605.h"     // → broker_haptic_data_t,  BROKER_HAPTIC_TIMEOUT_MS
#include "lsm6dsv16x.h"  // → broker_imu_data_t,     BROKER_IMU_TIMEOUT_MS
#include "bme688_drv.h"  // → broker_env_data_t,     BROKER_ENV_TIMEOUT_MS
#include "max30101.h"    // → broker_hr_data_t,      BROKER_HR_TIMEOUT_MS
#include "tmp117.h"      // → broker_skin_data_t,    BROKER_SKIN_TIMEOUT_MS

#include "fusion.h"      // → broker_fusion_data_t,  BROKER_FUSION_TIMEOUT_MS

#include "alarm.h"       // → broker_alarm_data_t,   BROKER_ALARM_TIMEOUT_MS

// --- Init ----------------------------------------------------------------------

void broker_init(void);  // Call once from main.c before any driver or task

// --- GPS API  (custom get_status - has_fix + first_fix_notif logic) ------------

void            broker_gps_write(const broker_gps_data_t *data);
void            broker_gps_read(broker_gps_data_t *out);
sensor_status_t broker_gps_get_status(void);
void            broker_gps_set_enabled(bool en);
bool            broker_gps_get_enabled(void);
void            broker_gps_set_hw_status(bool ok);
bool            broker_gps_hw_alive(void);

// --- MAG API  (custom get_status - calibrating/calibrated state) ---------------

void            broker_mag_write(const broker_mag_data_t *data);
void            broker_mag_read(broker_mag_data_t *out);
sensor_status_t broker_mag_get_status(void);
void            broker_mag_set_enabled(bool en);
bool            broker_mag_get_enabled(void);
void            broker_mag_set_hw_status(bool ok);
bool            broker_mag_hw_alive(void);

void            broker_mag_set_calibrating(bool calibrating);
bool            broker_mag_get_calibrating(void);

// --- RTC API  (macro-generated - always-on, no enable toggle) -----------------

void            broker_rtc_write(const broker_rtc_data_t *data);
void            broker_rtc_read(broker_rtc_data_t *out);
sensor_status_t broker_rtc_get_status(void);
void            broker_rtc_set_enabled(bool en);
bool            broker_rtc_get_enabled(void);
void            broker_rtc_set_hw_status(bool ok);
bool            broker_rtc_hw_alive(void);

// --- Battery API  (custom get_status - NOTIF at <10%) -------------------------

void            broker_battery_write(const broker_battery_data_t *data);
void            broker_battery_read(broker_battery_data_t *out);
sensor_status_t broker_battery_get_status(void);
void            broker_battery_set_enabled(bool en);
bool            broker_battery_get_enabled(void);
void            broker_battery_set_hw_status(bool ok);
bool            broker_battery_hw_alive(void);

// --- Light API  (macro-generated - toggleable) --------------------------------

void            broker_light_write(const broker_light_data_t *data);
void            broker_light_read(broker_light_data_t *out);
sensor_status_t broker_light_get_status(void);
void            broker_light_set_enabled(bool en);
bool            broker_light_get_enabled(void);
void            broker_light_set_hw_status(bool ok);
bool            broker_light_hw_alive(void);

// --- Haptic API  (macro-generated - toggleable) --------------------------------

void            broker_haptic_write(const broker_haptic_data_t *data);
void            broker_haptic_read(broker_haptic_data_t *out);
sensor_status_t broker_haptic_get_status(void);
void            broker_haptic_set_enabled(bool en);
bool            broker_haptic_get_enabled(void);
void            broker_haptic_set_hw_status(bool ok);
bool            broker_haptic_hw_alive(void);

// --- IMU API (macro-generated - toggleable) -----------------------------------

void            broker_imu_write(const broker_imu_data_t *data);
void            broker_imu_read(broker_imu_data_t *out);
sensor_status_t broker_imu_get_status(void);
void            broker_imu_set_enabled(bool en);
bool            broker_imu_get_enabled(void);
void            broker_imu_set_hw_status(bool ok);
bool            broker_imu_hw_alive(void);

// ── Fusion API ──────────────────────────────────────────

void            broker_fusion_write(const broker_fusion_data_t *data);
void            broker_fusion_read(broker_fusion_data_t *out);
sensor_status_t broker_fusion_get_status(void);
void            broker_fusion_set_enabled(bool en);
bool            broker_fusion_get_enabled(void);
void            broker_fusion_set_hw_status(bool ok);
bool            broker_fusion_hw_alive(void);

// ── Enviorment API ──────────────────────────────────────────
void            broker_env_write(const broker_env_data_t *data);
void            broker_env_read(broker_env_data_t *out);
sensor_status_t broker_env_get_status(void);
void            broker_env_set_enabled(bool en);
bool            broker_env_get_enabled(void);
void            broker_env_set_hw_status(bool ok);
bool            broker_env_hw_alive(void);

// ── Health API ──────────────────────────────────────────
void            broker_hr_write(const broker_hr_data_t *data);
void            broker_hr_read(broker_hr_data_t *out);
sensor_status_t broker_hr_get_status(void);
void            broker_hr_set_enabled(bool en);
bool            broker_hr_get_enabled(void);
void            broker_hr_set_hw_status(bool ok);
bool            broker_hr_hw_alive(void);

// ── Skin temperature API (TMP117) ────────────────────────
void            broker_skin_write(const broker_skin_data_t *data);
void            broker_skin_read(broker_skin_data_t *out);
sensor_status_t broker_skin_get_status(void);
void            broker_skin_set_enabled(bool en);
bool            broker_skin_get_enabled(void);
void            broker_skin_set_hw_status(bool ok);
bool            broker_skin_hw_alive(void);

// --- Alarm API (macro-generated - always-on, no enable toggle) ---------
void            broker_alarm_write(const broker_alarm_data_t *data);
void            broker_alarm_read(broker_alarm_data_t *out);
sensor_status_t broker_alarm_get_status(void);
void            broker_alarm_set_enabled(bool en);
bool            broker_alarm_get_enabled(void);
void            broker_alarm_set_hw_status(bool ok);
bool            broker_alarm_hw_alive(void);

// -- ADD NEW MODULE API BLOCK HERE (7 lines) ------------------------------------

#endif // DATA_BROKER_H
