/**
 * @file app_nvs.c
 * @brief NVS persistence — sensor calibration and UI settings.
 *
 * UI settings async flow (Blueprint 1 §5):
 *   Core 1  →  ui_settings_save_async()  →  xQueueOverwrite (non-blocking)
 *   task_settings_saver (unpinned, pri 2)  →  xQueueReceive  →  app_nvs_save_ui_settings()
 *
 * Float storage convention:
 *   NVS has no float type. All floats are stored as int32_t scaled by ×1000,
 *   giving three decimal places of precision (sufficient for mag calibration).
 *
 * Error handling convention:
 *   Every public function returns esp_err_t. On ESP_ERR_NVS_NOT_FOUND the caller
 *   receives safe defaults and ESP_OK — this is not an error, it is a first-boot
 *   condition. All other errors are returned to the caller and logged here.
 */

#include "app_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include <string.h>

static const char *TAG = "APP_NVS";

// ─── NVS namespaces (max 15 chars per ESP-IDF constraint) ─────────────────────

// #define NVS_CALIBRATION_NS   "calibration"   // Sensor calibration data //replaced with NVS_CALIBRATION_NS
#define NS_UI    "ui_cfg"        // UI appearance settings

// ─── NVS keys — calibration namespace ─────────────────────────────────────────

#define KEY_MAG_OFF_X    "mag_off_x"
#define KEY_MAG_OFF_Y    "mag_off_y"
#define KEY_MAG_SCL_X    "mag_scl_x"
#define KEY_MAG_SCL_Y    "mag_scl_y"
#define KEY_MAG_SCL_AVG  "mag_scl_avg"
#define KEY_MAG_CAL      "mag_cal"       // u8: 0=uncal, 1=calibrated
#define KEY_GPS_YEAR     "gps_year"
#define KEY_GPS_MONTH    "gps_month"
#define KEY_GPS_DAY      "gps_day"
// #define KEY_HEIGHT_REF   "height_ref"    // int32: altitude_m × 100
// #define KEY_HEIGHT_ZERO  "height_zero"   // u8: 0=not set, 1=zeroed
#define NVS_CALIBRATION_NS  "calibration"
#define NVS_KEY_HEIGHT_REF  "height_ref"    // int32, alt_m × 100
#define NVS_KEY_HEIGHT_ZERO "height_zero"   // uint8, 1 = set

// ─── NVS keys — UI namespace ───────────────────────────────────────────────────

#define KEY_UI_THEME     "theme"
#define KEY_UI_BRIGHT    "bright"
#define KEY_UI_BLUELIT   "bluelit"
#define KEY_UI_AUTOBR    "autobr"

// ─── Async settings queue ──────────────────────────────────────────────────────
// Depth-1 queue with xQueueOverwrite semantics. Only one pending write at a time.
// The saver task always processes the most recently requested state.

static QueueHandle_t s_settings_q = NULL;

QueueHandle_t app_nvs_settings_queue_create(void)
{
    s_settings_q = xQueueCreate(1, sizeof(ui_settings_t));
    configASSERT(s_settings_q);  // Unrecoverable if allocation fails
    return s_settings_q;
}

// void ui_settings_save_async(const ui_settings_t *s)
// {
//     if (!s_settings_q) {
//         ESP_LOGW(TAG, "ui_settings_save_async: queue not created");
//         return;
//     }
//     // Overwrite any pending save with the latest values — last write wins.
//     xQueueOverwrite(s_settings_q, s);
// }

// ─── Init ─────────────────────────────────────────────────────────────────────

esp_err_t app_nvs_init(app_calibration_t *cal)
{
    if (!cal) return ESP_ERR_INVALID_ARG;

    // Initialise NVS flash; handle corruption or version mismatch gracefully.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupt or version mismatch — erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));

    // ── Load magnetometer calibration ──────────────────────────────────────────
    ret = app_nvs_load_mag_calibration(
              &cal->mag_offset_x, &cal->mag_offset_y,
              &cal->mag_scale_x,  &cal->mag_scale_y,
              &cal->mag_scale_avg, &cal->mag_calibrated);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Mag cal loaded: off=(%.3f, %.3f) scale=(%.3f, %.3f, avg=%.3f)",
                 (double)cal->mag_offset_x, (double)cal->mag_offset_y,
                 (double)cal->mag_scale_x,  (double)cal->mag_scale_y,
                 (double)cal->mag_scale_avg);
    } else {
        // Not found — first boot or after a factory erase. Use safe defaults.
        cal->mag_offset_x   = 0.0f;
        cal->mag_offset_y   = 0.0f;
        cal->mag_scale_x    = 1.0f;
        cal->mag_scale_y    = 1.0f;
        cal->mag_scale_avg  = 1.0f;
        cal->mag_calibrated = false;
        ESP_LOGW(TAG, "Mag cal not found — using defaults (first boot?)");
    }

    // ── Load height reference ──────────────────────────────────────────────────
    ret = app_nvs_load_height_reference(&cal->height_reference_altitude,
                                        &cal->height_zeroed);
    if (ret != ESP_OK) {
        cal->height_reference_altitude = 0.0f;
        cal->height_zeroed = false;
    }

    // ── Load GPS last-known date (informational at this stage) ─────────────────
    uint16_t y = 0; uint8_t mo = 0, d = 0;
    if (app_nvs_load_gps_date(&y, &mo, &d) == ESP_OK) {
        ESP_LOGI(TAG, "Last GPS date stored: %04u-%02u-%02u", y, mo, d);
    }

    ESP_LOGI(TAG, "NVS init complete");
    return ESP_OK;
}

// ─── UI Settings ──────────────────────────────────────────────────────────────

esp_err_t app_nvs_load_ui_settings(ui_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    // Set safe defaults first — a partial NVS read still yields a valid struct.
    out->theme           = 0;      // UI_THEME_DARK
    out->brightness      = 70;     // Comfortable default
    out->blue_light_on   = false;
    out->auto_brightness = false;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS_UI, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "ui_cfg namespace not found — using defaults");
        return ESP_OK;  // Not an error — first boot
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "ui_cfg open failed");

    uint8_t v;
    if (nvs_get_u8(h, KEY_UI_THEME,   &v) == ESP_OK) out->theme           = v;
    if (nvs_get_u8(h, KEY_UI_BRIGHT,  &v) == ESP_OK) out->brightness      = v;
    if (nvs_get_u8(h, KEY_UI_BLUELIT, &v) == ESP_OK) out->blue_light_on   = (v != 0);
    if (nvs_get_u8(h, KEY_UI_AUTOBR,  &v) == ESP_OK) out->auto_brightness = (v != 0);
    nvs_close(h);

    // Clamp brightness to valid operating range
    if (out->brightness < 1)   out->brightness = 1;
    if (out->brightness > 100) out->brightness = 100;

    ESP_LOGI(TAG, "UI settings loaded: theme=%u bright=%u bluelit=%d autobr=%d",
             out->theme, out->brightness, out->blue_light_on, out->auto_brightness);
    return ESP_OK;
}

esp_err_t app_nvs_save_ui_settings(const ui_settings_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS_UI, NVS_READWRITE, &h);
    ESP_RETURN_ON_ERROR(ret, TAG, "ui_cfg open (write) failed");

    ret = nvs_set_u8(h, KEY_UI_THEME,   s->theme);
    if (ret == ESP_OK) ret = nvs_set_u8(h, KEY_UI_BRIGHT,  s->brightness);
    if (ret == ESP_OK) ret = nvs_set_u8(h, KEY_UI_BLUELIT, s->blue_light_on   ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_set_u8(h, KEY_UI_AUTOBR,  s->auto_brightness ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "UI settings saved: theme=%u bright=%u", s->theme, s->brightness);
    } else {
        ESP_LOGE(TAG, "UI settings save failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

// ─── Magnetometer calibration ─────────────────────────────────────────────────

esp_err_t app_nvs_save_mag_calibration(float offset_x, float offset_y,
                                        float scale_x,  float scale_y,
                                        float scale_avg)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READWRITE, &h);
    ESP_RETURN_ON_ERROR(ret, TAG, "calibration open (write) failed");

    // Store as ×1000 scaled int32 — three decimal places of precision.
    ret = nvs_set_i32(h, KEY_MAG_OFF_X,   (int32_t)(offset_x  * 1000.0f));
    if (ret == ESP_OK) ret = nvs_set_i32(h, KEY_MAG_OFF_Y,   (int32_t)(offset_y  * 1000.0f));
    if (ret == ESP_OK) ret = nvs_set_i32(h, KEY_MAG_SCL_X,   (int32_t)(scale_x   * 1000.0f));
    if (ret == ESP_OK) ret = nvs_set_i32(h, KEY_MAG_SCL_Y,   (int32_t)(scale_y   * 1000.0f));
    if (ret == ESP_OK) ret = nvs_set_i32(h, KEY_MAG_SCL_AVG, (int32_t)(scale_avg * 1000.0f));
    if (ret == ESP_OK) ret = nvs_set_u8 (h, KEY_MAG_CAL, 1);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);

    if (ret == ESP_OK)
        ESP_LOGI(TAG, "Mag cal saved: off=(%.3f, %.3f) scale=(%.3f, %.3f, avg=%.3f)",
                 (double)offset_x, (double)offset_y,
                 (double)scale_x,  (double)scale_y, (double)scale_avg);
    else
        ESP_LOGE(TAG, "Mag cal save failed: %s", esp_err_to_name(ret));

    return ret;
}

esp_err_t app_nvs_load_mag_calibration(float *offset_x, float *offset_y,
                                        float *scale_x,  float *scale_y,
                                        float *scale_avg, bool *calibrated)
{
    if (!offset_x || !offset_y || !scale_x || !scale_y || !scale_avg || !calibrated)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ret;  // Caller handles defaults
    ESP_RETURN_ON_ERROR(ret, TAG, "calibration open (ro) failed");

    int32_t a = 0, b = 0, c = 0, d = 0, e = 0;
    uint8_t f = 0;

    ret = nvs_get_i32(h, KEY_MAG_OFF_X,   &a);
    if (ret == ESP_OK) ret = nvs_get_i32(h, KEY_MAG_OFF_Y,   &b);
    if (ret == ESP_OK) ret = nvs_get_i32(h, KEY_MAG_SCL_X,   &c);
    if (ret == ESP_OK) ret = nvs_get_i32(h, KEY_MAG_SCL_Y,   &d);
    if (ret == ESP_OK) ret = nvs_get_i32(h, KEY_MAG_SCL_AVG, &e);
    if (ret == ESP_OK) ret = nvs_get_u8 (h, KEY_MAG_CAL,     &f);
    nvs_close(h);

    if (ret == ESP_OK) {
        *offset_x   = a / 1000.0f;
        *offset_y   = b / 1000.0f;
        *scale_x    = c / 1000.0f;
        *scale_y    = d / 1000.0f;
        *scale_avg  = e / 1000.0f;
        *calibrated = (f == 1);
    }
    return ret;
}

esp_err_t app_nvs_clear_mag_calibration(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READWRITE, &h);
    ESP_RETURN_ON_ERROR(ret, TAG, "calibration open (write) failed");

    // Only clear the flag — leave offset values intact in case of re-use.
    ret = nvs_set_u8(h, KEY_MAG_CAL, 0);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);

    if (ret == ESP_OK)
        ESP_LOGI(TAG, "Mag cal flag cleared");

    return ret;
}

// ─── GPS last-known date ──────────────────────────────────────────────────────

esp_err_t app_nvs_save_gps_date(uint16_t year, uint8_t month, uint8_t day)
{
    // Guard: only overwrite if the new date is strictly newer.
    uint16_t sy = 0; uint8_t smo = 0, sd = 0;
    if (app_nvs_load_gps_date(&sy, &smo, &sd) == ESP_OK) {
        if (year < sy)  return ESP_ERR_INVALID_STATE;
        if (year == sy && month < smo) return ESP_ERR_INVALID_STATE;
        if (year == sy && month == smo && day <= sd) return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READWRITE, &h);
    ESP_RETURN_ON_ERROR(ret, TAG, "calibration open (write) failed");

    ret = nvs_set_u16(h, KEY_GPS_YEAR,  year);
    if (ret == ESP_OK) ret = nvs_set_u8(h, KEY_GPS_MONTH, month);
    if (ret == ESP_OK) ret = nvs_set_u8(h, KEY_GPS_DAY,   day);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);

    if (ret == ESP_OK)
        ESP_LOGI(TAG, "GPS date saved: %04u-%02u-%02u", year, month, day);

    return ret;
}

esp_err_t app_nvs_load_gps_date(uint16_t *year, uint8_t *month, uint8_t *day)
{
    if (!year || !month || !day) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ret;
    ESP_RETURN_ON_ERROR(ret, TAG, "calibration open (ro) failed");

    ret = nvs_get_u16(h, KEY_GPS_YEAR,  year);
    if (ret == ESP_OK) ret = nvs_get_u8(h, KEY_GPS_MONTH, month);
    if (ret == ESP_OK) ret = nvs_get_u8(h, KEY_GPS_DAY,   day);
    nvs_close(h);
    return ret;
}

// ─── Height reference ──────────────────────────────────────────────────────────
esp_err_t app_nvs_load_height_reference(float *alt_m, bool *valid)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READONLY, &h);
    if (ret != ESP_OK) { *valid = false; return ret; }

    uint8_t zeroed = 0;
    nvs_get_u8(h, NVS_KEY_HEIGHT_ZERO, &zeroed);
    int32_t raw = 0;
    ret = nvs_get_i32(h, NVS_KEY_HEIGHT_REF, &raw);
    nvs_close(h);

    if (ret == ESP_OK && zeroed == 1) {
        *alt_m = raw / 100.0f;
        *valid = true;
    } else {
        *alt_m = 0.0f;
        *valid = false;
    }
    return ESP_OK;
}

esp_err_t app_nvs_save_height_reference(float alt_m)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    int32_t raw = (int32_t)(alt_m * 100.0f);
    nvs_set_i32(h, NVS_KEY_HEIGHT_REF, raw);
    nvs_set_u8 (h, NVS_KEY_HEIGHT_ZERO, 1);
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}
esp_err_t app_nvs_clear_height_reference(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READWRITE, &h);
    ESP_RETURN_ON_ERROR(ret, TAG, "calibration open (write) failed");

    ret = nvs_set_u8(h, NVS_KEY_HEIGHT_ZERO, 0);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

// esp_err_t app_nvs_save_height_reference(float altitude_m)
// {
//     nvs_handle_t h;
//     esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READWRITE, &h);
//     ESP_RETURN_ON_ERROR(ret, TAG, "calibration open (write) failed");

//     // Store as ×100 scaled int32 (two decimal places = ~1 cm precision).
//     ret = nvs_set_i32(h, KEY_HEIGHT_REF,  (int32_t)(altitude_m * 100.0f));
//     if (ret == ESP_OK) ret = nvs_set_u8(h, KEY_HEIGHT_ZERO, 1);
//     if (ret == ESP_OK) ret = nvs_commit(h);
//     nvs_close(h);
//     return ret;
// }

// esp_err_t app_nvs_load_height_reference(float *altitude_m, bool *zeroed)
// {
//     if (!altitude_m || !zeroed) return ESP_ERR_INVALID_ARG;

//     nvs_handle_t h;
//     esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READONLY, &h);
//     if (ret == ESP_ERR_NVS_NOT_FOUND) return ret;
//     ESP_RETURN_ON_ERROR(ret, TAG, "calibration open (ro) failed");

//     int32_t v = 0; uint8_t z = 0;
//     ret = nvs_get_i32(h, KEY_HEIGHT_REF,  &v);
//     if (ret == ESP_OK) ret = nvs_get_u8(h, KEY_HEIGHT_ZERO, &z);
//     nvs_close(h);

//     if (ret == ESP_OK) {
//         *altitude_m = v / 100.0f;
//         *zeroed     = (z == 1);
//     }
//     return ret;
// }

// esp_err_t app_nvs_clear_height_reference(void)
// {
//     nvs_handle_t h;
//     esp_err_t ret = nvs_open(NVS_CALIBRATION_NS, NVS_READWRITE, &h);
//     ESP_RETURN_ON_ERROR(ret, TAG, "calibration open (write) failed");

//     ret = nvs_set_u8(h, KEY_HEIGHT_ZERO, 0);
//     if (ret == ESP_OK) ret = nvs_commit(h);
//     nvs_close(h);
//     return ret;
// }