/**
 * @file ui_subjects.c
 * @brief LVGL 9 subjects + producer -> UI drain plumbing.
 *
 * See ui_subjects.h for contract + threading rules.
 * See LVGL9_Kompic_Architecture.md §1.6 + §5.4 + §6 for design rationale.
 */

#include "ui_subjects.h"
#include "data_broker.h"       // sensor_status_t, broker_gps_set_enabled
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "UI_SUBJ";

// ---------------------------------------------------------------------------
// Subject storage. lv_subject_init_string wants TWO buffers of the same size
// (current + previous) so LVGL can diff. 48 bytes each -- generous for
// formatted sensor labels; the longest we emit is ~30 chars.
// ---------------------------------------------------------------------------
#define ENV_STR_BUF_LEN  48

lv_subject_t subj_env_temp_str;
lv_subject_t subj_env_hum_str;
lv_subject_t subj_env_press_str;
lv_subject_t subj_env_alt_str;
lv_subject_t subj_env_delta_str;

static char s_buf_temp     [ENV_STR_BUF_LEN];
static char s_buf_temp_prev[ENV_STR_BUF_LEN];
static char s_buf_hum      [ENV_STR_BUF_LEN];
static char s_buf_hum_prev [ENV_STR_BUF_LEN];
static char s_buf_press    [ENV_STR_BUF_LEN];
static char s_buf_press_prev[ENV_STR_BUF_LEN];
static char s_buf_alt      [ENV_STR_BUF_LEN];
static char s_buf_alt_prev [ENV_STR_BUF_LEN];
static char s_buf_delta    [ENV_STR_BUF_LEN];
static char s_buf_delta_prev[ENV_STR_BUF_LEN];

QueueHandle_t g_env_q = NULL;

// ---------------------------------------------------------------------------
// GPS-domain storage (Batch 31.4)
// ---------------------------------------------------------------------------
#define GPS_STR_BUF_LEN  56

lv_subject_t subj_gps_status_str;
lv_subject_t subj_gps_time_str;
lv_subject_t subj_gps_lat_str;
lv_subject_t subj_gps_lon_str;
lv_subject_t subj_gps_altspd_str;
lv_subject_t subj_gps_photo_time_str;
lv_subject_t subj_gps_photo_lat_str;
lv_subject_t subj_gps_photo_lon_str;
lv_subject_t subj_gps_photo_alt_str;
lv_subject_t subj_gps_enabled;
lv_subject_t subj_gps_photo_view;

#define GPS_STR_PAIR(name) \
    static char s_buf_##name[GPS_STR_BUF_LEN]; \
    static char s_buf_##name##_prev[GPS_STR_BUF_LEN]

GPS_STR_PAIR(gps_status);
GPS_STR_PAIR(gps_time);
GPS_STR_PAIR(gps_lat);
GPS_STR_PAIR(gps_lon);
GPS_STR_PAIR(gps_altspd);
GPS_STR_PAIR(gps_photo_time);
GPS_STR_PAIR(gps_photo_lat);
GPS_STR_PAIR(gps_photo_lon);
GPS_STR_PAIR(gps_photo_alt);

QueueHandle_t g_gps_q = NULL;

static const char *k_sentinel_gps    = "--";
static const char *k_sentinel_status = "waiting for GPS module...";

// ---------------------------------------------------------------------------
// IMU-domain storage (Batch 32.1)
// ---------------------------------------------------------------------------
#define IMU_STR_BUF_LEN  32

lv_subject_t subj_imu_accel_x_str;
lv_subject_t subj_imu_accel_y_str;
lv_subject_t subj_imu_accel_z_str;
lv_subject_t subj_imu_gyro_x_str;
lv_subject_t subj_imu_gyro_y_str;
lv_subject_t subj_imu_gyro_z_str;
lv_subject_t subj_imu_roll_str;
lv_subject_t subj_imu_pitch_str;
lv_subject_t subj_imu_temp_str;
lv_subject_t subj_imu_status_str;
lv_subject_t subj_imu_enabled;

#define IMU_STR_PAIR(name) \
    static char s_buf_##name[IMU_STR_BUF_LEN]; \
    static char s_buf_##name##_prev[IMU_STR_BUF_LEN]

IMU_STR_PAIR(imu_ax);
IMU_STR_PAIR(imu_ay);
IMU_STR_PAIR(imu_az);
IMU_STR_PAIR(imu_gx);
IMU_STR_PAIR(imu_gy);
IMU_STR_PAIR(imu_gz);
IMU_STR_PAIR(imu_roll);
IMU_STR_PAIR(imu_pitch);
IMU_STR_PAIR(imu_temp);
IMU_STR_PAIR(imu_status);

QueueHandle_t g_imu_q = NULL;

// ---------------------------------------------------------------------------
// MAG-domain storage (Batch 32.1)
// ---------------------------------------------------------------------------
#define MAG_STR_BUF_LEN  40

lv_subject_t subj_mag_xyz_str;
lv_subject_t subj_mag_heading_str;
lv_subject_t subj_mag_cardinal_str;
lv_subject_t subj_mag_cal_btn_str;
lv_subject_t subj_mag_enabled;

#define MAG_STR_PAIR(name) \
    static char s_buf_##name[MAG_STR_BUF_LEN]; \
    static char s_buf_##name##_prev[MAG_STR_BUF_LEN]

MAG_STR_PAIR(mag_xyz);
MAG_STR_PAIR(mag_heading);
MAG_STR_PAIR(mag_cardinal);
MAG_STR_PAIR(mag_cal_btn);

QueueHandle_t g_mag_q = NULL;

static bool         s_initialised = false;
static bool         s_drain_started = false;
static lv_timer_t  *s_drain_timer = NULL;

// Sentinel string shown before first sample arrives.
static const char *k_sentinel_env = "---";

// ---------------------------------------------------------------------------
// Drain callback -- runs on LVGL task under the port lock (lv_timer
// callbacks always run there per LVGL 9 threading model).
// ---------------------------------------------------------------------------
static void env_format_and_publish(const broker_env_data_t *d)
{
    char tmp[ENV_STR_BUF_LEN];

    // Data validity gate mirrors the pre-31.2 env_tile logic.
    // We only get here if the producer pushed a sample, so treat the
    // presence of enabled+valid-ish fields as "publishable"; the tile
    // shows "---" when the sensor is offline (broker status handles that,
    // but we still fall back here defensively).

    // Temp: "Temp:      23.4 °C"
    int t_w = (int)d->temperature_c;
    int t_d = (int)((d->temperature_c - t_w) * 10);
    if (t_d < 0) t_d = -t_d;
    snprintf(tmp, sizeof(tmp), "Temp:      %d.%d \xc2\xb0""C", t_w, t_d);
    lv_subject_copy_string(&subj_env_temp_str, tmp);

    // Hum: "Hum:       48.2 %"
    int h_w = (int)d->humidity_pct;
    int h_d = (int)((d->humidity_pct - h_w) * 10);
    if (h_d < 0) h_d = -h_d;
    snprintf(tmp, sizeof(tmp), "Hum:       %d.%d %%", h_w, h_d);
    lv_subject_copy_string(&subj_env_hum_str, tmp);

    // Press: "Press:     1012.3 hPa"
    int p_w = (int)d->pressure_hpa;
    int p_d = (int)((d->pressure_hpa - p_w) * 10);
    if (p_d < 0) p_d = -p_d;
    snprintf(tmp, sizeof(tmp), "Press:     %d.%d hPa", p_w, p_d);
    lv_subject_copy_string(&subj_env_press_str, tmp);

    // Alt: "Alt:       34 m"
    snprintf(tmp, sizeof(tmp), "Alt:       %d m", (int)d->altitude_m);
    lv_subject_copy_string(&subj_env_alt_str, tmp);

    // Δ Height: "Δ Height:  +1.2 m" or "-- not zeroed --"
    if (d->home_ref_valid) {
        float delta = d->altitude_m - d->home_ref_altitude_m;
        char sign = (delta >= 0.0f) ? '+' : '-';
        int d_w = (int)delta;
        int d_f = (int)((delta - (float)d_w) * 10);
        if (d_f < 0) d_f = -d_f;
        if (d_w < 0)  d_w = -d_w;
        snprintf(tmp, sizeof(tmp), "\xce\x94 Height:  %c%d.%d m", sign, d_w, d_f);
    } else {
        snprintf(tmp, sizeof(tmp), "\xce\x94 Height:  -- not zeroed --");
    }
    lv_subject_copy_string(&subj_env_delta_str, tmp);

    ESP_LOGV(TAG, "env published: T=%.1f H=%.1f P=%.1f Alt=%.1f",
             (double)d->temperature_c, (double)d->humidity_pct,
             (double)d->pressure_hpa, (double)d->altitude_m);
}

// ---------------------------------------------------------------------------
// GPS drain formatter (Batch 31.4)
// ---------------------------------------------------------------------------
static void gps_format_and_publish(const broker_gps_data_t *d)
{
    char tmp[GPS_STR_BUF_LEN];
    sensor_status_t st = broker_gps_get_status();

    // ── Status line (primary user-facing text; robust to all states) ──
    // Priority order: disabled -> offline -> stale -> fix -> acquiring.
    // "no data" distinguishes "chip silent" from "chip alive, no fix yet".
    // Stage 31.4b: append MON-RF antenna hint when snapshot is valid --
    // "3D fix (8 sats) · ant OK" or "acquiring... · ant OPEN" makes the
    // diagnostic obvious at a glance.
    int used = 0;
    if (!d->enabled) {
        used = snprintf(tmp, sizeof(tmp), "GPS disabled");
    } else if (st == SENSOR_OFFLINE) {
        used = snprintf(tmp, sizeof(tmp), "no data -- check antenna/wiring");
    } else if (d->fix == GPS_FIX_3D) {
        used = snprintf(tmp, sizeof(tmp), "3D fix (%u sats)", d->sats_in_use);
    } else if (d->fix == GPS_FIX_2D) {
        used = snprintf(tmp, sizeof(tmp), "2D fix (%u sats)", d->sats_in_use);
    } else if (st == SENSOR_STALE) {
        used = snprintf(tmp, sizeof(tmp), "fix lost (last known shown)");
    } else if (d->time_valid) {
        used = snprintf(tmp, sizeof(tmp), "acquiring... (time only)");
    } else {
        used = snprintf(tmp, sizeof(tmp), "acquiring... (no signal yet)");
    }
    // Append antenna status when MON-RF snapshot has fresh data.
    max_m10s_monrf_t rf;
    max_m10s_get_monrf(&rf);
    if (rf.valid && used > 0 && used < (int)sizeof(tmp) - 12) {
        snprintf(tmp + used, sizeof(tmp) - used, " \xc2\xb7 ant %s",
                 max_m10s_ant_status_name(rf.ant_status));
    }
    lv_subject_copy_string(&subj_gps_status_str, tmp);

    // ── Time + date, single line ──
    if (d->time_valid) {
        snprintf(tmp, sizeof(tmp), "%04u-%02u-%02u %02u:%02u:%02u UTC",
                 d->utc_year, d->utc_month, d->utc_day,
                 d->utc_hour, d->utc_minute, d->utc_second);
    } else {
        snprintf(tmp, sizeof(tmp), "-- no time --");
    }
    lv_subject_copy_string(&subj_gps_time_str, tmp);

    // Position validity gate: show live fix OR "(last known)" during
    // SENSOR_STALE with real coords, per the earlier tile's Bug-2 pattern.
    bool pos_live  = d->position_valid && (st == SENSOR_ONLINE || st == SENSOR_NOTIF);
    bool pos_stale = !d->position_valid && (st == SENSOR_STALE) &&
                     (d->latitude != 0.0 || d->longitude != 0.0);
    bool pos_any   = pos_live || pos_stale;

    // ── LAT ──
    if (pos_any) {
        double lat = d->latitude;
        char   ns  = (lat >= 0.0) ? 'N' : 'S';
        if (lat < 0.0) lat = -lat;
        int lat_w = (int)lat;
        int lat_d = (int)((lat - lat_w) * 10000);
        snprintf(tmp, sizeof(tmp), "LAT: %d.%04d\xc2\xb0 %c%s",
                 lat_w, lat_d, ns, pos_stale ? " (last)" : "");
    } else {
        snprintf(tmp, sizeof(tmp), "LAT: --");
    }
    lv_subject_copy_string(&subj_gps_lat_str, tmp);

    // ── LON ──
    if (pos_any) {
        double lon = d->longitude;
        char   ew  = (lon >= 0.0) ? 'E' : 'W';
        if (lon < 0.0) lon = -lon;
        int lon_w = (int)lon;
        int lon_d = (int)((lon - lon_w) * 10000);
        snprintf(tmp, sizeof(tmp), "LON: %d.%04d\xc2\xb0 %c%s",
                 lon_w, lon_d, ew, pos_stale ? " (last)" : "");
    } else {
        snprintf(tmp, sizeof(tmp), "LON: --");
    }
    lv_subject_copy_string(&subj_gps_lon_str, tmp);

    // ── ALT + SPD ──
    if (pos_any) {
        int alt_w = (int)d->altitude_m;
        int spd_w = (int)d->speed_kmh;
        int spd_d = (int)((d->speed_kmh - spd_w) * 10);
        if (spd_d < 0) spd_d = -spd_d;
        if (pos_stale) {
            snprintf(tmp, sizeof(tmp), "ALT: %d m (last)", alt_w);
        } else {
            snprintf(tmp, sizeof(tmp), "ALT: %d m  SPD: %d.%d km/h",
                     alt_w, spd_w, spd_d);
        }
    } else {
        snprintf(tmp, sizeof(tmp), "ALT: --  SPD: --");
    }
    lv_subject_copy_string(&subj_gps_altspd_str, tmp);

    // ── Photo view strings ──
    if (d->time_valid) {
        snprintf(tmp, sizeof(tmp), "%02u:%02u:%02u UTC \xc2\xb7 %04u-%02u-%02u",
                 d->utc_hour, d->utc_minute, d->utc_second,
                 d->utc_year, d->utc_month, d->utc_day);
    } else {
        snprintf(tmp, sizeof(tmp), "--:--:-- UTC \xc2\xb7 ----/--/--");
    }
    lv_subject_copy_string(&subj_gps_photo_time_str, tmp);

    if (pos_any) {
        double lat = d->latitude;
        char   ns  = (lat >= 0.0) ? 'N' : 'S';
        if (lat < 0.0) lat = -lat;
        int lat_w = (int)lat;
        int lat_d = (int)((lat - lat_w) * 100000);
        snprintf(tmp, sizeof(tmp), "%d.%05d\xc2\xb0 %c", lat_w, lat_d, ns);
        lv_subject_copy_string(&subj_gps_photo_lat_str, tmp);

        double lon = d->longitude;
        char   ew  = (lon >= 0.0) ? 'E' : 'W';
        if (lon < 0.0) lon = -lon;
        int lon_w = (int)lon;
        int lon_d = (int)((lon - lon_w) * 100000);
        snprintf(tmp, sizeof(tmp), "%d.%05d\xc2\xb0 %c", lon_w, lon_d, ew);
        lv_subject_copy_string(&subj_gps_photo_lon_str, tmp);

        int alt_w = (int)d->altitude_m;
        snprintf(tmp, sizeof(tmp), "ALT %d m \xc2\xb7 Sats %u%s",
                 alt_w, d->sats_in_use, pos_stale ? " \xc2\xb7 (last)" : "");
        lv_subject_copy_string(&subj_gps_photo_alt_str, tmp);
    } else {
        lv_subject_copy_string(&subj_gps_photo_lat_str, "no fix");
        lv_subject_copy_string(&subj_gps_photo_lon_str, "");
        lv_subject_copy_string(&subj_gps_photo_alt_str, "waiting for GPS");
    }

    // ── Two-way switch state: mirror broker into subject ──
    // Only fires observer + updates the switch if the value actually
    // changed (LVGL subject_set semantics), so no feedback loop.
    lv_subject_set_int(&subj_gps_enabled, d->enabled ? 1 : 0);

    ESP_LOGV(TAG, "gps published: fix=%d sats=%u lat=%.5f lon=%.5f enabled=%d",
             (int)d->fix, d->sats_in_use,
             d->latitude, d->longitude, d->enabled ? 1 : 0);
}

// ---------------------------------------------------------------------------
// GPS two-way switch: observer syncs subject -> broker on user-tap changes
// ---------------------------------------------------------------------------
static void obs_gps_enabled_to_broker(lv_observer_t *obs, lv_subject_t *subj)
{
    (void)obs;
    int32_t v = lv_subject_get_int(subj);
    bool desired = (v != 0);
    // Guard against feedback: only write when broker differs. The drain
    // then mirrors this back (no-op on next tick because value matches).
    if (broker_gps_get_enabled() != desired) {
        broker_gps_set_enabled(desired);
        ESP_LOGD(TAG, "gps switch -> broker_gps_set_enabled(%d)", desired ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------
// GPS view API (mirrors subj_gps_photo_view). Raw int values (0/1) so we
// don't collide with gps_tile.h's gps_tile_view_t enum.
// ---------------------------------------------------------------------------
void kw_ui_gps_view_set(int photo_view)
{
    lv_subject_set_int(&subj_gps_photo_view, photo_view ? 1 : 0);
}

int kw_ui_gps_view_get(void)
{
    return lv_subject_get_int(&subj_gps_photo_view) == 1 ? 1 : 0;
}

void kw_ui_gps_view_toggle(void)
{
    kw_ui_gps_view_set(kw_ui_gps_view_get() ? 0 : 1);
}

// ---------------------------------------------------------------------------
// IMU drain formatter (Batch 32.1)
// ---------------------------------------------------------------------------
static void imu_format_and_publish(const broker_imu_data_t *d)
{
    char tmp[IMU_STR_BUF_LEN];

    // Status text derived from broker (matches env/gps pattern where the
    // formatter reads live broker state to build the primary status line).
    bool hw_alive = broker_imu_hw_alive();
    if (!hw_alive) {
        snprintf(tmp, sizeof(tmp), "OFFLINE");
    } else if (!d->enabled) {
        snprintf(tmp, sizeof(tmp), "Disabled");
    } else {
        sensor_status_t st = broker_imu_get_status();
        if (st == SENSOR_STALE || st == SENSOR_OFFLINE) {
            snprintf(tmp, sizeof(tmp), "Stale");
        } else {
            snprintf(tmp, sizeof(tmp), "Online");
        }
    }
    lv_subject_copy_string(&subj_imu_status_str, tmp);

    // On disabled/offline, blank the numeric labels; the drain still runs
    // (producer pushes a "disabled" snapshot) so the tile updates instantly.
    if (!hw_alive || !d->enabled) {
        lv_subject_copy_string(&subj_imu_accel_x_str, "X: ---");
        lv_subject_copy_string(&subj_imu_accel_y_str, "Y: ---");
        lv_subject_copy_string(&subj_imu_accel_z_str, "Z: ---");
        lv_subject_copy_string(&subj_imu_gyro_x_str,  "X: ---");
        lv_subject_copy_string(&subj_imu_gyro_y_str,  "Y: ---");
        lv_subject_copy_string(&subj_imu_gyro_z_str,  "Z: ---");
        lv_subject_copy_string(&subj_imu_roll_str,    "Roll:  ---");
        lv_subject_copy_string(&subj_imu_pitch_str,   "Pitch: ---");
        lv_subject_copy_string(&subj_imu_temp_str,    "Temp: ---");
        lv_subject_set_int(&subj_imu_enabled, d->enabled ? 1 : 0);
        return;
    }

    snprintf(tmp, sizeof(tmp), "X: %.2f", (double)d->accel_x);
    lv_subject_copy_string(&subj_imu_accel_x_str, tmp);
    snprintf(tmp, sizeof(tmp), "Y: %.2f", (double)d->accel_y);
    lv_subject_copy_string(&subj_imu_accel_y_str, tmp);
    snprintf(tmp, sizeof(tmp), "Z: %.2f", (double)d->accel_z);
    lv_subject_copy_string(&subj_imu_accel_z_str, tmp);

    snprintf(tmp, sizeof(tmp), "X: %.1f", (double)d->gyro_x);
    lv_subject_copy_string(&subj_imu_gyro_x_str, tmp);
    snprintf(tmp, sizeof(tmp), "Y: %.1f", (double)d->gyro_y);
    lv_subject_copy_string(&subj_imu_gyro_y_str, tmp);
    snprintf(tmp, sizeof(tmp), "Z: %.1f", (double)d->gyro_z);
    lv_subject_copy_string(&subj_imu_gyro_z_str, tmp);

    snprintf(tmp, sizeof(tmp), "Roll:  %.1f", (double)d->roll_deg);
    lv_subject_copy_string(&subj_imu_roll_str, tmp);
    snprintf(tmp, sizeof(tmp), "Pitch: %.1f", (double)d->pitch_deg);
    lv_subject_copy_string(&subj_imu_pitch_str, tmp);

    snprintf(tmp, sizeof(tmp), "Temp: %.1f C", (double)d->temperature);
    lv_subject_copy_string(&subj_imu_temp_str, tmp);

    lv_subject_set_int(&subj_imu_enabled, 1);

    ESP_LOGV(TAG, "imu published: ax=%.2f gx=%.1f roll=%.1f enabled=1",
             (double)d->accel_x, (double)d->gyro_x, (double)d->roll_deg);
}

static void obs_imu_enabled_to_broker(lv_observer_t *obs, lv_subject_t *subj)
{
    (void)obs;
    bool desired = (lv_subject_get_int(subj) != 0);
    if (broker_imu_get_enabled() != desired) {
        broker_imu_set_enabled(desired);
        ESP_LOGD(TAG, "imu switch -> broker_imu_set_enabled(%d)", desired ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------
// MAG drain formatter (Batch 32.1)
// ---------------------------------------------------------------------------
static const char *k_cardinal[8] = { "N","NE","E","SE","S","SW","W","NW" };

static void mag_format_and_publish(const broker_mag_data_t *d)
{
    char tmp[MAG_STR_BUF_LEN];
    sensor_status_t st = broker_mag_get_status();
    bool data_ok = (st == SENSOR_ONLINE || st == SENSOR_STALE || st == SENSOR_ACQUIRING);

    // XYZ line
    if (data_ok && d->enabled) {
        snprintf(tmp, sizeof(tmp),
                 "X:%.0f  Y:%.0f  Z:%.0f  \xc2\xb5T",
                 (double)d->x_ut, (double)d->y_ut, (double)d->z_ut);
    } else {
        snprintf(tmp, sizeof(tmp), "X:---  Y:---  Z:---  \xc2\xb5T");
    }
    lv_subject_copy_string(&subj_mag_xyz_str, tmp);

    // Heading + cardinal (only meaningful when we have a live/stale fix)
    if (data_ok && (st == SENSOR_ONLINE || st == SENSOR_STALE) && d->enabled) {
        snprintf(tmp, sizeof(tmp), "%.0f\xc2\xb0", (double)d->heading_deg);
        lv_subject_copy_string(&subj_mag_heading_str, tmp);
        uint8_t c = (d->cardinal < 8) ? d->cardinal : 0;
        lv_subject_copy_string(&subj_mag_cardinal_str, k_cardinal[c]);
    } else {
        lv_subject_copy_string(&subj_mag_heading_str,  "---");
        lv_subject_copy_string(&subj_mag_cardinal_str, "---");
    }

    // Calibrate button label mirrors calibration state.
    if (d->calibrating) {
        snprintf(tmp, sizeof(tmp), "CAL %ds", (int)d->cal_countdown);
        lv_subject_copy_string(&subj_mag_cal_btn_str, tmp);
    } else {
        lv_subject_copy_string(&subj_mag_cal_btn_str, "CALIBRATE");
    }

    lv_subject_set_int(&subj_mag_enabled, d->enabled ? 1 : 0);

    ESP_LOGV(TAG, "mag published: xyz=(%.0f,%.0f,%.0f) hdg=%.0f cal=%d enabled=%d",
             (double)d->x_ut, (double)d->y_ut, (double)d->z_ut,
             (double)d->heading_deg, d->calibrating ? 1 : 0,
             d->enabled ? 1 : 0);
}

static void obs_mag_enabled_to_broker(lv_observer_t *obs, lv_subject_t *subj)
{
    (void)obs;
    bool desired = (lv_subject_get_int(subj) != 0);
    if (broker_mag_get_enabled() != desired) {
        broker_mag_set_enabled(desired);
        ESP_LOGD(TAG, "mag switch -> broker_mag_set_enabled(%d)", desired ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------
// Master drain (runs on LVGL task under port lock every 200 ms)
// ---------------------------------------------------------------------------
static void drain_cb(lv_timer_t *t)
{
    (void)t;

    broker_env_data_t env_sample;
    if (g_env_q && xQueueReceive(g_env_q, &env_sample, 0) == pdTRUE) {
        env_format_and_publish(&env_sample);
        ESP_LOGD(TAG, "drain: env sample consumed");
    }

    broker_gps_data_t gps_sample;
    if (g_gps_q && xQueueReceive(g_gps_q, &gps_sample, 0) == pdTRUE) {
        gps_format_and_publish(&gps_sample);
        ESP_LOGD(TAG, "drain: gps sample consumed");
    }

    broker_imu_data_t imu_sample;
    if (g_imu_q && xQueueReceive(g_imu_q, &imu_sample, 0) == pdTRUE) {
        imu_format_and_publish(&imu_sample);
        ESP_LOGD(TAG, "drain: imu sample consumed");
    }

    broker_mag_data_t mag_sample;
    if (g_mag_q && xQueueReceive(g_mag_q, &mag_sample, 0) == pdTRUE) {
        mag_format_and_publish(&mag_sample);
        ESP_LOGD(TAG, "drain: mag sample consumed");
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void kw_ui_subjects_init(void)
{
    if (s_initialised) {
        ESP_LOGD(TAG, "init: already initialised -- no-op");
        return;
    }

    // Initialise every string buffer to the sentinel so bound labels
    // render "---" until the first producer sample arrives.
    strncpy(s_buf_temp,  k_sentinel_env, ENV_STR_BUF_LEN - 1);
    strncpy(s_buf_hum,   k_sentinel_env, ENV_STR_BUF_LEN - 1);
    strncpy(s_buf_press, k_sentinel_env, ENV_STR_BUF_LEN - 1);
    strncpy(s_buf_alt,   k_sentinel_env, ENV_STR_BUF_LEN - 1);
    strncpy(s_buf_delta, k_sentinel_env, ENV_STR_BUF_LEN - 1);

    lv_subject_init_string(&subj_env_temp_str,
                           s_buf_temp,  s_buf_temp_prev,
                           ENV_STR_BUF_LEN, k_sentinel_env);
    lv_subject_init_string(&subj_env_hum_str,
                           s_buf_hum,   s_buf_hum_prev,
                           ENV_STR_BUF_LEN, k_sentinel_env);
    lv_subject_init_string(&subj_env_press_str,
                           s_buf_press, s_buf_press_prev,
                           ENV_STR_BUF_LEN, k_sentinel_env);
    lv_subject_init_string(&subj_env_alt_str,
                           s_buf_alt,   s_buf_alt_prev,
                           ENV_STR_BUF_LEN, k_sentinel_env);
    lv_subject_init_string(&subj_env_delta_str,
                           s_buf_delta, s_buf_delta_prev,
                           ENV_STR_BUF_LEN, k_sentinel_env);

    if (g_env_q == NULL) {
        // Overwrite semantics: producer never blocks; UI always gets latest.
        g_env_q = xQueueCreate(1, sizeof(broker_env_data_t));
        if (g_env_q == NULL) {
            ESP_LOGE(TAG, "init: xQueueCreate(g_env_q) FAILED");
            return;
        }
    }

    // -- GPS domain (Batch 31.4) --
    strncpy(s_buf_gps_status, k_sentinel_status, GPS_STR_BUF_LEN - 1);
    strncpy(s_buf_gps_time,      k_sentinel_gps, GPS_STR_BUF_LEN - 1);
    strncpy(s_buf_gps_lat,       k_sentinel_gps, GPS_STR_BUF_LEN - 1);
    strncpy(s_buf_gps_lon,       k_sentinel_gps, GPS_STR_BUF_LEN - 1);
    strncpy(s_buf_gps_altspd,    k_sentinel_gps, GPS_STR_BUF_LEN - 1);
    strncpy(s_buf_gps_photo_time, k_sentinel_gps, GPS_STR_BUF_LEN - 1);
    strncpy(s_buf_gps_photo_lat,  k_sentinel_gps, GPS_STR_BUF_LEN - 1);
    strncpy(s_buf_gps_photo_lon,  k_sentinel_gps, GPS_STR_BUF_LEN - 1);
    strncpy(s_buf_gps_photo_alt,  k_sentinel_gps, GPS_STR_BUF_LEN - 1);

    lv_subject_init_string(&subj_gps_status_str, s_buf_gps_status,
                           s_buf_gps_status_prev, GPS_STR_BUF_LEN,
                           k_sentinel_status);
    lv_subject_init_string(&subj_gps_time_str, s_buf_gps_time,
                           s_buf_gps_time_prev, GPS_STR_BUF_LEN,
                           k_sentinel_gps);
    lv_subject_init_string(&subj_gps_lat_str, s_buf_gps_lat,
                           s_buf_gps_lat_prev, GPS_STR_BUF_LEN,
                           k_sentinel_gps);
    lv_subject_init_string(&subj_gps_lon_str, s_buf_gps_lon,
                           s_buf_gps_lon_prev, GPS_STR_BUF_LEN,
                           k_sentinel_gps);
    lv_subject_init_string(&subj_gps_altspd_str, s_buf_gps_altspd,
                           s_buf_gps_altspd_prev, GPS_STR_BUF_LEN,
                           k_sentinel_gps);
    lv_subject_init_string(&subj_gps_photo_time_str, s_buf_gps_photo_time,
                           s_buf_gps_photo_time_prev, GPS_STR_BUF_LEN,
                           k_sentinel_gps);
    lv_subject_init_string(&subj_gps_photo_lat_str, s_buf_gps_photo_lat,
                           s_buf_gps_photo_lat_prev, GPS_STR_BUF_LEN,
                           k_sentinel_gps);
    lv_subject_init_string(&subj_gps_photo_lon_str, s_buf_gps_photo_lon,
                           s_buf_gps_photo_lon_prev, GPS_STR_BUF_LEN,
                           k_sentinel_gps);
    lv_subject_init_string(&subj_gps_photo_alt_str, s_buf_gps_photo_alt,
                           s_buf_gps_photo_alt_prev, GPS_STR_BUF_LEN,
                           k_sentinel_gps);

    lv_subject_init_int(&subj_gps_enabled,    0);  // reflects broker
    lv_subject_init_int(&subj_gps_photo_view, KW_GPS_VIEW_NORMAL);

    // Two-way switch: subject change -> broker. Drain writes broker->subject.
    lv_subject_add_observer(&subj_gps_enabled,
                            obs_gps_enabled_to_broker, NULL);

    if (g_gps_q == NULL) {
        g_gps_q = xQueueCreate(1, sizeof(broker_gps_data_t));
        if (g_gps_q == NULL) {
            ESP_LOGE(TAG, "init: xQueueCreate(g_gps_q) FAILED");
            return;
        }
    }

    // -- IMU domain (Batch 32.1) --
    strncpy(s_buf_imu_ax,     "X: ---",     IMU_STR_BUF_LEN - 1);
    strncpy(s_buf_imu_ay,     "Y: ---",     IMU_STR_BUF_LEN - 1);
    strncpy(s_buf_imu_az,     "Z: ---",     IMU_STR_BUF_LEN - 1);
    strncpy(s_buf_imu_gx,     "X: ---",     IMU_STR_BUF_LEN - 1);
    strncpy(s_buf_imu_gy,     "Y: ---",     IMU_STR_BUF_LEN - 1);
    strncpy(s_buf_imu_gz,     "Z: ---",     IMU_STR_BUF_LEN - 1);
    strncpy(s_buf_imu_roll,   "Roll:  ---", IMU_STR_BUF_LEN - 1);
    strncpy(s_buf_imu_pitch,  "Pitch: ---", IMU_STR_BUF_LEN - 1);
    strncpy(s_buf_imu_temp,   "Temp: ---",  IMU_STR_BUF_LEN - 1);
    strncpy(s_buf_imu_status, "Acquiring...", IMU_STR_BUF_LEN - 1);

    lv_subject_init_string(&subj_imu_accel_x_str, s_buf_imu_ax,
                           s_buf_imu_ax_prev, IMU_STR_BUF_LEN, s_buf_imu_ax);
    lv_subject_init_string(&subj_imu_accel_y_str, s_buf_imu_ay,
                           s_buf_imu_ay_prev, IMU_STR_BUF_LEN, s_buf_imu_ay);
    lv_subject_init_string(&subj_imu_accel_z_str, s_buf_imu_az,
                           s_buf_imu_az_prev, IMU_STR_BUF_LEN, s_buf_imu_az);
    lv_subject_init_string(&subj_imu_gyro_x_str,  s_buf_imu_gx,
                           s_buf_imu_gx_prev, IMU_STR_BUF_LEN, s_buf_imu_gx);
    lv_subject_init_string(&subj_imu_gyro_y_str,  s_buf_imu_gy,
                           s_buf_imu_gy_prev, IMU_STR_BUF_LEN, s_buf_imu_gy);
    lv_subject_init_string(&subj_imu_gyro_z_str,  s_buf_imu_gz,
                           s_buf_imu_gz_prev, IMU_STR_BUF_LEN, s_buf_imu_gz);
    lv_subject_init_string(&subj_imu_roll_str,    s_buf_imu_roll,
                           s_buf_imu_roll_prev, IMU_STR_BUF_LEN, s_buf_imu_roll);
    lv_subject_init_string(&subj_imu_pitch_str,   s_buf_imu_pitch,
                           s_buf_imu_pitch_prev, IMU_STR_BUF_LEN, s_buf_imu_pitch);
    lv_subject_init_string(&subj_imu_temp_str,    s_buf_imu_temp,
                           s_buf_imu_temp_prev, IMU_STR_BUF_LEN, s_buf_imu_temp);
    lv_subject_init_string(&subj_imu_status_str,  s_buf_imu_status,
                           s_buf_imu_status_prev, IMU_STR_BUF_LEN,
                           s_buf_imu_status);

    lv_subject_init_int(&subj_imu_enabled, 0);
    lv_subject_add_observer(&subj_imu_enabled,
                            obs_imu_enabled_to_broker, NULL);

    if (g_imu_q == NULL) {
        g_imu_q = xQueueCreate(1, sizeof(broker_imu_data_t));
        if (g_imu_q == NULL) {
            ESP_LOGE(TAG, "init: xQueueCreate(g_imu_q) FAILED");
            return;
        }
    }

    // -- MAG domain (Batch 32.1) --
    strncpy(s_buf_mag_xyz,      "X:---  Y:---  Z:---  \xc2\xb5T", MAG_STR_BUF_LEN - 1);
    strncpy(s_buf_mag_heading,  "---",       MAG_STR_BUF_LEN - 1);
    strncpy(s_buf_mag_cardinal, "---",       MAG_STR_BUF_LEN - 1);
    strncpy(s_buf_mag_cal_btn,  "CALIBRATE", MAG_STR_BUF_LEN - 1);

    lv_subject_init_string(&subj_mag_xyz_str,      s_buf_mag_xyz,
                           s_buf_mag_xyz_prev, MAG_STR_BUF_LEN, s_buf_mag_xyz);
    lv_subject_init_string(&subj_mag_heading_str,  s_buf_mag_heading,
                           s_buf_mag_heading_prev, MAG_STR_BUF_LEN,
                           s_buf_mag_heading);
    lv_subject_init_string(&subj_mag_cardinal_str, s_buf_mag_cardinal,
                           s_buf_mag_cardinal_prev, MAG_STR_BUF_LEN,
                           s_buf_mag_cardinal);
    lv_subject_init_string(&subj_mag_cal_btn_str,  s_buf_mag_cal_btn,
                           s_buf_mag_cal_btn_prev, MAG_STR_BUF_LEN,
                           s_buf_mag_cal_btn);

    lv_subject_init_int(&subj_mag_enabled, 0);
    lv_subject_add_observer(&subj_mag_enabled,
                            obs_mag_enabled_to_broker, NULL);

    if (g_mag_q == NULL) {
        g_mag_q = xQueueCreate(1, sizeof(broker_mag_data_t));
        if (g_mag_q == NULL) {
            ESP_LOGE(TAG, "init: xQueueCreate(g_mag_q) FAILED");
            return;
        }
    }

    s_initialised = true;
    ESP_LOGI(TAG, "init: env(5) + gps(9 str + 2 int) + imu(10 str + 1 int) + "
                  "mag(4 str + 1 int) subjects created; "
                  "g_env_q + g_gps_q + g_imu_q + g_mag_q up");
}

void kw_ui_subjects_start_drain(void)
{
    if (!s_initialised) {
        ESP_LOGW(TAG, "start_drain: not initialised -- forwarding to init");
        kw_ui_subjects_init();
    }
    if (s_drain_started) {
        ESP_LOGD(TAG, "start_drain: already started -- no-op");
        return;
    }

    s_drain_timer = lv_timer_create(drain_cb, 200, NULL);
    if (s_drain_timer == NULL) {
        ESP_LOGE(TAG, "start_drain: lv_timer_create FAILED");
        return;
    }
    s_drain_started = true;
    ESP_LOGI(TAG, "start_drain: LVGL drain timer running (200 ms)");
}
