/**
 * @file fc_internal.h
 * @brief Shared declarations for the field_capture component's internal .c files.
 *
 * Not part of the public API -- consumers of field_capture should include
 * field_capture.h. This header only lives inside the component and is used
 * to wire the fc_common / fc_recording / fc_modes / fc_modes_lsm /
 * fc_battery_test / fc_shutdown / fc_cli split introduced in the Stage 12
 * refactor.
 */

#ifndef FC_INTERNAL_H
#define FC_INTERNAL_H

#include "field_capture.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ── Component-wide TAG (each .c redefines locally for its own subsystem tag) ──
#ifndef FC_TAG
#define FC_TAG "FIELD"
#endif

// ── Pins / timings ───────────────────────────────────────────────────────────
#define PIN_BUTTON              16
#define PIN_ENC_A               21
#define PIN_ENC_B               43

#define POLL_TICK_MS             5
#define BTN_DEBOUNCE_MS         30
#define BTN_DOUBLE_GAP_MS      350
// Stage 17: any press held longer than this is claimed by the shutdown watcher
// (see fc_shutdown.c SHDN_COMMIT_MS). button_poll() emits neither single-click
// nor double-click for such a press -- prevents the "held to shut down, but
// released too early = single click fires and mode starts" bug.
#define BTN_LONG_PRESS_MS     1000
#define ENC_DETENT_REST_MS      10

#define VOICE_ANNOT_MS        5000
#define BEEP_COUNT               3
#define BEEP_ON_MS             140
#define BEEP_OFF_MS            260
#define PULSE_STANDBY_MS      5000
#define PULSE_RECORD_MS       1000
#define SOLID_ON_ACTIVITY_MS  2000

#define FL_LEVELS               20
#define FL_MIN_PCT               3
#define FL_MAX_PCT              40
#define FL_INIT_LEVEL           10

#ifndef DEBUG_PRINT_SENSORS
#define DEBUG_PRINT_SENSORS      1
#endif
#define SENSOR_PRINT_PERIOD_MS 1000

// ── DRV effect IDs ───────────────────────────────────────────────────────────
#define DRV_STRONG_CLICK         1
#define DRV_MEDIUM_CLICK        20
#define DRV_LONG_BUZZ           47
#define ALARM_DURATION_MS   15000

// ── WS2812 palette ───────────────────────────────────────────────────────────
#define WS_MAX                  26

typedef struct { const char *name; uint8_t r, g, b; } mode_info_t;
extern const mode_info_t MODE_INFO[FCM_COUNT];

extern const fc_mode_t FC_LSM_SUBMENU[];
extern const uint8_t   FC_LSM_SUBMENU_LEN;

// ── App state machine ────────────────────────────────────────────────────────
typedef enum {
    ST_STANDBY = 0,
    ST_FL_ON,
    ST_RECORDING,
    ST_ALARM_FIRING,
} app_state_t;

typedef enum { BTN_IDLE, BTN_PRESSED, BTN_WAIT_DBL, BTN_PRESSED_2 } btn_sm_t;

typedef struct {
    bool     in_motion;
    int8_t   latched_dir;
    uint32_t rest_since_ms;
} enc_poll_t;

// ── Shared globals (defined in fc_common.c) ─────────────────────────────────
extern fc_mode_t    s_mode;
extern bool         s_in_submenu;
extern uint32_t     s_last_tap_dbl_count;
extern app_state_t  s_state;
extern uint32_t     s_last_activity_ms;
extern uint32_t     s_boot_seq;
extern uint32_t     s_rec_seq;
extern uint8_t      s_fl_level;
extern bool         s_sd_ready;

extern enc_poll_t   s_enc;
extern btn_sm_t     s_btn_state;
extern uint32_t     s_btn_last_change;
extern uint32_t     s_btn_release_ms;
extern uint32_t     s_btn_press_start_ms;   // Stage 17: BTN_LONG_PRESS_MS swallow
extern bool         s_btn_prev_low;

extern FILE        *s_csv_file;
extern bool         s_recording_early_end;

// Compass hard-iron cal (fc_modes.c owns these, boot-scoped, no NVS).
extern bool  s_mag_cal_done;
extern float s_mag_offset_x;
extern float s_mag_offset_y;
extern float s_mag_scale_x;
extern float s_mag_scale_y;

// ── Watcher-owned globals (defined in fc_shutdown.c) ────────────────────────
extern volatile bool g_shutdown_hold_active;
extern volatile bool g_recording_active;
extern volatile bool g_watcher_press_reset_pending;
extern volatile bool g_batt_test_active;

// ── Shared I2C bus 0 mutex (defined in boot_hw_init.c) ──────────────────────
extern SemaphoreHandle_t g_i2c_mutex;

// ── Utility (fc_common.c) ───────────────────────────────────────────────────
static inline uint32_t millis_u32(void) {
    extern int64_t esp_timer_get_time(void);
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}
uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi);
uint8_t  fl_pct_from_level(uint8_t level);

// Mode-taxonomy helpers (fc_common.c). Used by field_capture.c main loop.
bool      is_lsm_submode(fc_mode_t m);
bool      is_top_level(fc_mode_t m);
fc_mode_t top_mode_step(fc_mode_t m, int dir);
fc_mode_t lsm_submenu_step(fc_mode_t m, int dir);

// RGB helpers.
void rgb_off(void);
void rgb_set_max(fc_mode_t m);
void rgb_pulse(fc_mode_t m, uint32_t t_anchor, uint32_t period_ms);

// Mode-signature LED animations (fc_modes.c).
void rgb_compass_alt_red_blue(void);
void rgb_qvar_alt_yellow_purple(void);
void rgb_temp_warm_cycle(void);
void rgb_lsm_submenu_indicator(fc_mode_t sub);

// NVS mode load/save (fc_common.c).
void nvs_load(void);
void nvs_save_mode(void);

// SD helpers (fc_common.c).
void try_mkdir(const char *path);
void ensure_sd(void);
void rtc_iso_now(char *out, size_t n);

// Button + encoder (fc_common.c).
int  button_poll(void);
int  encoder_delta(void);

// Sensor wake/park (fc_common.c).
void wake_sensors_for_mode(fc_mode_t m);
void park_all_modal_sensors(void);

// ── Recording (fc_recording.c) ──────────────────────────────────────────────
void wav_write_header(FILE *f, uint32_t sample_rate, uint16_t bits);
void wav_patch_header(FILE *f, uint32_t data_bytes);
void wav_write_meta_sidecar(const char *wav_path, const char *tag);
uint32_t wav_record_to(const char *path, uint32_t duration_ms);

FILE *csv_open(const char *dir, uint32_t seq, const char *header);
void  csv_row_env(FILE *f);
void  csv_row_motion(FILE *f);

#if DEBUG_PRINT_SENSORS
void debug_dump_mode(fc_mode_t m);
#endif

void voice_annot_lead_in(void);
void run_recording_for_current_mode(void);
void run_alarm_firing(void);

// ── Modes (fc_modes.c) ──────────────────────────────────────────────────────
void run_mag_cal(void);
void run_compass(void);
void run_compass_or_cal(void);
void run_ecg(void);
void run_ecg_session(void);

float read_lsm_die_temp(void);
float read_max_die_temp(void);
void  run_temp_session(void);
void  run_temp_mode(void);

// ── LSM-submenu modes (fc_modes_lsm.c) ──────────────────────────────────────
void run_bcg(void);
void run_bcg_mode(void);
void run_steps_mode(void);
void run_mlc_collect_mode(void);
void run_tap_dbg_mode(void);

// ── PPG+BCG combined raw mode (fc_modes_ppg_bcg.c) ──────────────────────────
// Stage 17: 200 Hz interleaved PPG (MAX30101 raw green) + BCG (LSM accel Z),
// with pc_sync_* header block from fc_sync_write_csv_headers(). See Stage 15
// spec §CSV for the column schema.
void run_ppg_bcg_mode(void);

// ── Battery test + shared telemetry (fc_battery_test.c) ─────────────────────
void     vbat_adc_ensure_init(void);
uint32_t vbat_adc_read_mv(void);
void     esp_ts_ensure_init(void);
float    esp_ts_read_c(void);

typedef struct {
    uint32_t last_idle_c0;
    uint32_t last_idle_c1;
    uint32_t last_wall_us;
    bool     primed;
} idle_pct_state_t;
uint8_t idle_pct_sample(idle_pct_state_t *st);

void run_battery_test_mode(void);
void task_blackbox_fn(void *arg);

// ── Shutdown watcher (fc_shutdown.c) ────────────────────────────────────────
void watcher_ship_mode(void);

// ── Sync (fc_sync.c) ────────────────────────────────────────────────────────
void    fc_sync_init(void);
bool    fc_sync_is_active(void);
void    fc_sync_handle_line(const char *line, int64_t t_recv_us);
bool    fc_sync_offset_valid(void);
int64_t fc_sync_get_offset_us(void);
int64_t fc_sync_get_step_us(void);
bool    fc_sync_step_host_set(void);
void    fc_sync_write_csv_headers(FILE *f);

// ── CLI (fc_cli.c) ──────────────────────────────────────────────────────────
int startswith_ci(const char *s, const char *pfx);

#endif  // FC_INTERNAL_H
