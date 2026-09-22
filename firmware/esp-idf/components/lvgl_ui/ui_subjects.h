/**
 * @file ui_subjects.h
 * @brief LVGL 9 observer/subject singletons + producer -> UI drain plumbing.
 *
 * Stage 31.2 -- foundations rebuild. Producer tasks (bme688, max_m10s,
 * lsm6dsv16x, ...) push samples to per-domain FreeRTOS queues from ANY
 * core. A single lv_timer_create() callback runs on the LVGL task,
 * drains every queue, formats the values, and calls
 * lv_subject_copy_string / lv_subject_set_int on the corresponding
 * subject. Tiles use lv_label_bind_text / lv_obj_bind_* against these
 * subjects and refresh automatically -- no per-tile update() poll.
 *
 * Threading contract:
 *   Producers  (any core, any task) -> xQueueOverwrite(g_<domain>_q, ...)
 *   Drain cb   (LVGL task, one thread) -> lv_subject_set_* / _copy_string
 *   Consumers  (widgets bound via observer API)
 *
 * This is the ONLY place lv_subject_set_* / lv_subject_copy_string is
 * called; tile code binds only.
 *
 * Log tiers (per feedback_esp_log_verbosity_practice.md):
 *   LOGI -- init done + timer started (one-shot)
 *   LOGD -- per-drain-cycle summary when a queue had a fresh item
 *   LOGV -- per-subject write firehose (off by default)
 */

#ifndef UI_SUBJECTS_H
#define UI_SUBJECTS_H

#include "lvgl.h"
#include "bme688_drv.h"        // broker_env_data_t (queue payload)
#include "max_m10s.h"          // broker_gps_data_t (queue payload)
#include "lsm6dsv16x.h"        // broker_imu_data_t (queue payload)
#include "lis3mdl.h"           // broker_mag_data_t (queue payload)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ---------------------------------------------------------------------------
// Env-domain subjects (Batch 31.2 pilot)
// ---------------------------------------------------------------------------
// String subjects hold formatted display strings; drain cb calls
// lv_subject_copy_string with a fresh format. Bind labels with
// lv_label_bind_text(lbl, &subj_env_*, NULL).
extern lv_subject_t subj_env_temp_str;    // "Temp:      23.4 °C"
extern lv_subject_t subj_env_hum_str;     // "Hum:       48.2 %"
extern lv_subject_t subj_env_press_str;   // "Press:     1012.3 hPa"
extern lv_subject_t subj_env_alt_str;     // "Alt:       34 m"
extern lv_subject_t subj_env_delta_str;   // "Δ Height:  +1.2 m"

// Producer queue -- bme688's task_env_fn does xQueueOverwrite on this
// after broker_env_write(). Depth 1: overwrite semantics; UI only ever
// cares about the freshest sample. Owned by ui_subjects module.
extern QueueHandle_t g_env_q;

// ---------------------------------------------------------------------------
// GPS-domain subjects (Batch 31.4)
// ---------------------------------------------------------------------------
// Robust status-first display: subj_gps_status_str carries the primary
// user-facing line ("3D fix (8 sats)", "acquiring...", "no data --
// check antenna", etc.). Tile shows that prominently regardless of
// device liveness. Rest of the strings hold the data fields when
// available.
extern lv_subject_t subj_gps_status_str;   // "3D fix (8 sats · HDOP 1.2)"
extern lv_subject_t subj_gps_time_str;     // "2026-09-18 14:32:07 UTC"
extern lv_subject_t subj_gps_lat_str;      // "LAT: 46.0511° N"
extern lv_subject_t subj_gps_lon_str;      // "LON: 14.5051° E"
extern lv_subject_t subj_gps_altspd_str;   // "ALT: 302 m · SPD: 0.0 km/h"

// Photo-view formatted strings (larger / different layout than normal)
extern lv_subject_t subj_gps_photo_time_str; // "14:32:07 UTC · 2026-09-18"
extern lv_subject_t subj_gps_photo_lat_str;  // "46.05110° N"
extern lv_subject_t subj_gps_photo_lon_str;  // "14.50510° E"
extern lv_subject_t subj_gps_photo_alt_str;  // "ALT 302 m · Sats 8"

// Two-way + view-toggle int subjects.
//   subj_gps_enabled     -- 0/1, mirrors broker_gps_get_enabled().
//                           lv_obj_bind_checked on the power switch drives
//                           BOTH directions: user tap sets subject -> observer
//                           calls broker_gps_set_enabled; producer state
//                           reflected back via drain.
//   subj_gps_photo_view  -- 0=NORMAL, 1=PHOTO. Drives HIDDEN flag on both
//                           the photo container and the normal-view widgets.
extern lv_subject_t subj_gps_enabled;
extern lv_subject_t subj_gps_photo_view;

// Producer queue -- max_m10s's task_gps_fn does xQueueOverwrite after
// broker_gps_write().
extern QueueHandle_t g_gps_q;

// -- GPS view API (mirrored on subj_gps_photo_view).
//    Raw int values (0 = normal, 1 = photo) instead of an enum -- gps_tile.h
//    already defines a gps_tile_view_t with matching values and we don't
//    want the type collision. Callers pass 0/1 or their own enum casts.
#define KW_GPS_VIEW_NORMAL  0
#define KW_GPS_VIEW_PHOTO   1
void kw_ui_gps_view_set(int photo_view);
int  kw_ui_gps_view_get(void);
void kw_ui_gps_view_toggle(void);

// ---------------------------------------------------------------------------
// IMU-domain subjects (Batch 32.1 -- lsm6dsv16x)
// ---------------------------------------------------------------------------
// Nine per-value labels + one primary status line + a two-way power int.
// Formatter derives status text from broker_imu_hw_alive/get_status like
// gps_format_and_publish does; tile only owns LED colour.
extern lv_subject_t subj_imu_accel_x_str;   // "X: 0.00"
extern lv_subject_t subj_imu_accel_y_str;
extern lv_subject_t subj_imu_accel_z_str;
extern lv_subject_t subj_imu_gyro_x_str;    // "X: 0.0"
extern lv_subject_t subj_imu_gyro_y_str;
extern lv_subject_t subj_imu_gyro_z_str;
extern lv_subject_t subj_imu_roll_str;      // "Roll:  0.0"
extern lv_subject_t subj_imu_pitch_str;     // "Pitch: 0.0"
extern lv_subject_t subj_imu_temp_str;      // "Temp: 24.5 C"
extern lv_subject_t subj_imu_status_str;    // "Online" / "Disabled" / "OFFLINE" / "Stale"
extern lv_subject_t subj_imu_enabled;       // 0/1, mirrors broker_imu_get_enabled()

extern QueueHandle_t g_imu_q;               // producer -> UI queue (depth 1)

// ---------------------------------------------------------------------------
// MAG-domain subjects (Batch 32.1 -- lis3mdl)
// ---------------------------------------------------------------------------
// Value labels (XYZ combined on one line to match the current tile layout).
// Needle rotation + heading colour + calibrate-button state stay in
// compass_tile_update() because they're numeric/style ops, not text.
extern lv_subject_t subj_mag_xyz_str;       // "X:-12  Y:45  Z:8  µT"
extern lv_subject_t subj_mag_heading_str;   // "127°"
extern lv_subject_t subj_mag_cardinal_str;  // "SE"
extern lv_subject_t subj_mag_cal_btn_str;   // "CALIBRATE" / "CAL 15s"
extern lv_subject_t subj_mag_enabled;       // 0/1, mirrors broker_mag_get_enabled()

extern QueueHandle_t g_mag_q;               // producer -> UI queue (depth 1)

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * @brief Initialise every subject singleton + create producer queues.
 *        Idempotent. Must be called BEFORE any tile init that binds
 *        against these subjects.
 *        Safe to call outside lvgl_port_lock() (only queue create + subject
 *        init, no widget touches).
 */
void kw_ui_subjects_init(void);

/**
 * @brief Create the LVGL timer that drains producer queues and pushes
 *        into subjects. Idempotent (second call is a no-op).
 *        Must be called inside lvgl_port_lock() (creates an lv_timer).
 *        Drain period is 200 ms -- matches the pre-Stage-31 poll cadence
 *        of task_ui_refresh_fn so cosmetic behaviour is unchanged.
 */
void kw_ui_subjects_start_drain(void);

#endif // UI_SUBJECTS_H
