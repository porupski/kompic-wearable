/**
 * @file app_nvs.h
 * @brief NVS persistence -- sensor calibration data and UI settings.
 *
 * NAMESPACE LAYOUT:
 *   "calibration"  -- mag offsets/scale, GPS last date, height reference
 *   "ui_cfg"       -- theme, brightness, blue-light, auto-brightness
 *   ("battery" namespace -- formerly owned by v5 app_battery.c; v7.2 battery
 *    lifetime voltage tracking lives in components/bq25619/. See
 *    DataBroker_2026-06-13 DEFECT-001 for current persistence state.)
 *
 * ASYNC SAVE PATTERN (Blueprint 1 §5):
 *   Core 1 never calls NVS directly -- flash I/O would cause missed LVGL frames.
 *   Instead, Core 1 calls ui_settings_save_async() which is a non-blocking
 *   xQueueOverwrite onto a depth-1 queue. task_settings_saver (unpinned, pri 2)
 *   drains the queue and calls app_nvs_save_ui_settings(). Last-write-wins.
 *
 * RULES:
 *   - Every function returns esp_err_t. Callers check the return value.
 *   - No silent failures. Log warnings on soft errors, errors on fatal.
 *   - NVS writes only happen from task_settings_saver or boot context.
 *   - app_nvs_init() must be called before any other function in this file.
 *
 * NOTE: ui_settings_t is defined in ui_broker.h (Blueprint 3 §7).
 *       This file only declares functions that operate on it.
 */

#ifndef APP_NVS_H
#define APP_NVS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// FIX: Blocker #3 -- ui_settings_t removed from here.
//      It lives exclusively in ui_broker.h per Blueprint 3 §7.
//      Include ui_broker.h wherever both are needed.
#include "ui_broker.h"   // ui_settings_t

// --- Calibration struct --------------------------------------------------------
// Populated by app_nvs_init(). Passed to boot_hw_init for driver seeding.
// All fields have safe defaults if NVS keys are absent (first boot).

typedef struct {
    // Magnetometer hard-iron calibration
    float mag_offset_x;
    float mag_offset_y;
    float mag_scale_x;
    float mag_scale_y;
    float mag_scale_avg;
    bool  mag_calibrated;

    // Barometric height reference (future env module use)
    float height_reference_altitude;  // metres
    bool  height_zeroed;
} app_calibration_t;

// --- Init ----------------------------------------------------------------------

/**
 * @brief Initialise NVS flash and load all calibration data into *cal.
 *
 * Handles NVS_ERR_NO_FREE_PAGES and NVS_ERR_NEW_VERSION_FOUND by erasing
 * and reinitialising -- all stored data will be lost in that case (logged).
 * Calibration fields in *cal are set to safe defaults if their keys are absent.
 *
 * Call once from main.c before any tasks are created.
 *
 * @param cal  Output struct. Must not be NULL. Always fully populated on return.
 * @return ESP_OK on success. Fatal init failure returns the ESP-IDF error code.
 */
esp_err_t app_nvs_init(app_calibration_t *cal);

// --- Async settings queue ------------------------------------------------------

/**
 * @brief Create the async UI settings save queue (depth 1, overwrite semantics).
 *        Call once from main.c. Returns the handle to pass to task_settings_saver.
 */
QueueHandle_t app_nvs_settings_queue_create(void);

/**
 * @brief Non-blocking enqueue of the current UI settings for async NVS write.
 *        Safe to call from Core 1 / LVGL context. Uses xQueueOverwrite -- if a
 *        write is already pending, it is replaced with the latest values.
 */
// void ui_settings_save_async(const ui_settings_t *s);

// --- UI Settings ---------------------------------------------------------------

/**
 * @brief Load UI settings from NVS. Returns safe defaults if keys are absent.
 *        Call from main.c before lvgl_ui_init(), after app_nvs_init().
 */
esp_err_t app_nvs_load_ui_settings(ui_settings_t *out);

/**
 * @brief Save UI settings to NVS synchronously.
 *        ONLY call from task_settings_saver -- never from Core 1 / LVGL context.
 */
esp_err_t app_nvs_save_ui_settings(const ui_settings_t *s);

// --- Magnetometer calibration --------------------------------------------------

/**
 * @brief Persist magnetometer hard-iron calibration to NVS.
 *        Called from task_mag_cal after a successful calibration run.
 *        Floats are stored as scaled int32 (x1000) -- NVS has no float type.
 */
esp_err_t app_nvs_save_mag_calibration(float offset_x, float offset_y,
                                        float scale_x,  float scale_y,
                                        float scale_avg);

/**
 * @brief Load magnetometer calibration from NVS.
 *        All output pointers are set to safe defaults on ESP_ERR_NVS_NOT_FOUND.
 */
esp_err_t app_nvs_load_mag_calibration(float *offset_x, float *offset_y,
                                        float *scale_x,  float *scale_y,
                                        float *scale_avg, bool *calibrated);

/**
 * @brief Clear the mag calibration flag in NVS (does not erase offset values).
 *        Next boot will treat the sensor as uncalibrated.
 */
esp_err_t app_nvs_clear_mag_calibration(void);

// --- GPS last-known date -------------------------------------------------------

/**
 * @brief Persist GPS date only if it is strictly newer than the stored value.
 *        Used to seed the RTC on cold boot before GPS fix is acquired.
 *        Returns ESP_ERR_INVALID_STATE if the new date is not newer.
 */
esp_err_t app_nvs_save_gps_date(uint16_t year, uint8_t month, uint8_t day);

/**
 * @brief Load the last-known GPS date from NVS.
 *        Returns ESP_ERR_NVS_NOT_FOUND if never stored.
 */
esp_err_t app_nvs_load_gps_date(uint16_t *year, uint8_t *month, uint8_t *day);

// --- Height reference ----------------------------------------------------------
// Reserved for future barometric env module. Stored in "calibration" namespace.

esp_err_t app_nvs_load_height_reference(float *alt_m, bool *valid);
esp_err_t app_nvs_save_height_reference(float alt_m);
esp_err_t app_nvs_clear_height_reference(void);

#endif // APP_NVS_H