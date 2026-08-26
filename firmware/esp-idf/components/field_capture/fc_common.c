/**
 * @file fc_common.c
 * @brief Shared state + helpers used by every fc_* .c file.
 *
 * Split out of field_capture.c in the Stage 12 refactor. Owns:
 *   - all main-loop state globals (s_mode, s_state, s_boot_seq, ...)
 *   - button + encoder polled state machines
 *   - RGB palette (MODE_INFO) + rgb_off/set_max/pulse
 *   - LSM submenu taxonomy helpers
 *   - NVS mode/boot_seq load/save
 *   - SD mount + directory prep + rtc_iso_now
 *   - wake_sensors_for_mode / park_all_modal_sensors
 */

#include "fc_internal.h"

#include <math.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "data_broker.h"
#include "ws2812.h"
#include "sdcard.h"

static const char *TAG = "FC_COMMON";

// ── Palette ─────────────────────────────────────────────────────────────────
const mode_info_t MODE_INFO[FCM_COUNT] = {
    { "mic",     26,  0,  0 },
    { "env",      0, 26,  0 },
    { "mot",     14, 26,  0 },
    { "ppgbcg",  26,  0, 12 },
    { "fl",      18, 18, 18 },
    { "alarm",    8,  0, 26 },
    { "compass", 26,  0, 26 },
    { "qvar",    26, 26,  0 },
    { "temp",    26,  8,  0 },
    { "bcg",     26, 12,  0 },
    { "lsm",     26, 20,  0 },
    { "steps",    0, 22, 22 },
    { "mlccol",  26,  0,  0 },
    { "tapdbg",  26,  0, 26 },
    { "usb",      0, 26, 26 },
};

const fc_mode_t FC_LSM_SUBMENU[] = {
    FCM_MOTION,
    FCM_BCG,
    FCM_STEPS,
    FCM_MLC_COLLECT,
    FCM_TAP_DBG,
};
const uint8_t FC_LSM_SUBMENU_LEN = sizeof(FC_LSM_SUBMENU) / sizeof(FC_LSM_SUBMENU[0]);

// ── Shared globals ──────────────────────────────────────────────────────────
fc_mode_t    s_mode              = FCM_ENV;
bool         s_in_submenu        = false;
uint32_t     s_last_tap_dbl_count = 0;
app_state_t  s_state             = ST_STANDBY;
uint32_t     s_last_activity_ms  = 0;
uint32_t     s_boot_seq          = 0;
uint32_t     s_rec_seq           = 0;
uint8_t      s_fl_level          = FL_INIT_LEVEL;
bool         s_sd_ready          = false;

enc_poll_t   s_enc = { false, 0, 0 };

btn_sm_t     s_btn_state         = BTN_IDLE;
uint32_t     s_btn_last_change   = 0;
uint32_t     s_btn_release_ms    = 0;
uint32_t     s_btn_press_start_ms = 0;
bool         s_btn_prev_low      = false;

FILE        *s_csv_file          = NULL;
bool         s_recording_early_end = false;

// Compass hard-iron cal (compass mode owns semantics; storage here).
bool  s_mag_cal_done  = false;
float s_mag_offset_x  = 0.0f;
float s_mag_offset_y  = 0.0f;
float s_mag_scale_x   = 1.0f;
float s_mag_scale_y   = 1.0f;

// ── Utility ──────────────────────────────────────────────────────────────────
uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
uint8_t fl_pct_from_level(uint8_t level) {
    if (level > FL_LEVELS) level = FL_LEVELS;
    return (uint8_t)clamp_u32(
        FL_MIN_PCT + (uint32_t)level * (FL_MAX_PCT - FL_MIN_PCT) / FL_LEVELS,
        FL_MIN_PCT, FL_MAX_PCT);
}

// ── Mode-taxonomy helpers ───────────────────────────────────────────────────
bool is_lsm_submode(fc_mode_t m) {
    for (uint8_t i = 0; i < FC_LSM_SUBMENU_LEN; i++) {
        if (FC_LSM_SUBMENU[i] == m) return true;
    }
    return false;
}
bool is_top_level(fc_mode_t m) {
    return (m < FCM_COUNT) && !is_lsm_submode(m);
}
fc_mode_t top_mode_step(fc_mode_t m, int dir) {
    int c = (int)m;
    do {
        c += dir;
        if (c < 0)              c += FCM_COUNT;
        if (c >= (int)FCM_COUNT) c -= FCM_COUNT;
    } while (!is_top_level((fc_mode_t)c));
    return (fc_mode_t)c;
}
fc_mode_t lsm_submenu_step(fc_mode_t m, int dir) {
    int idx = 0;
    for (uint8_t i = 0; i < FC_LSM_SUBMENU_LEN; i++) {
        if (FC_LSM_SUBMENU[i] == m) { idx = i; break; }
    }
    int c = idx + dir;
    if (c < 0)                          c += FC_LSM_SUBMENU_LEN;
    if (c >= (int)FC_LSM_SUBMENU_LEN)   c -= FC_LSM_SUBMENU_LEN;
    return FC_LSM_SUBMENU[c];
}

// ── NVS mode / boot_seq ─────────────────────────────────────────────────────
void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open("field", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t m = FCM_ENV;
    nvs_get_u8(h, "mode", &m);
    if (m >= FCM_COUNT) m = FCM_ENV;
    s_mode = (fc_mode_t)m;
    s_in_submenu = is_lsm_submode(s_mode);
    nvs_get_u32(h, "boot_seq", &s_boot_seq);
    s_boot_seq++;
    nvs_set_u32(h, "boot_seq", s_boot_seq);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "boot_seq=%lu restored mode=%s (submenu=%d)",
             (unsigned long)s_boot_seq, MODE_INFO[s_mode].name, s_in_submenu ? 1 : 0);
}
void nvs_save_mode(void) {
    nvs_handle_t h;
    if (nvs_open("field", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "mode", (uint8_t)s_mode);
    nvs_commit(h);
    nvs_close(h);
}

// ── RGB LED helpers ─────────────────────────────────────────────────────────
void rgb_off(void) { ws2812_set_color(0, 0, 0); }
void rgb_set_max(fc_mode_t m) {
    const mode_info_t *mi = &MODE_INFO[m];
    ws2812_set_color(mi->r, mi->g, mi->b);
}
void rgb_pulse(fc_mode_t m, uint32_t t_anchor, uint32_t period_ms) {
    uint32_t now = millis_u32();
    uint32_t elapsed = (now >= t_anchor) ? (now - t_anchor) : 0;
    float phase = (float)(elapsed % period_ms) / (float)period_ms;
    float s = 0.5f * (1.0f + cosf(phase * 2.0f * (float)M_PI));
    const mode_info_t *mi = &MODE_INFO[m];
    ws2812_set_color((uint8_t)(s * mi->r),
                     (uint8_t)(s * mi->g),
                     (uint8_t)(s * mi->b));
}

// ── SD mount + directory prep ───────────────────────────────────────────────
void try_mkdir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir(%s) failed: %s (errno=%d)", path, strerror(errno), errno);
    }
}
void ensure_sd(void) {
    if (s_sd_ready) return;
    if (sdcard_mount() != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed -- recording is a no-op");
        return;
    }
    s_sd_ready = true;
    try_mkdir("/sd/data");
    try_mkdir("/sd/data/mic");
    try_mkdir("/sd/data/env");
    try_mkdir("/sd/data/mot");
    try_mkdir("/sd/data/skin");
    try_mkdir("/sd/data/bcg");
    try_mkdir("/sd/data/qvar");
    try_mkdir("/sd/data/steps");
    try_mkdir("/sd/data/mlc_train");
    ESP_LOGI(TAG, "SD mounted (%ld MiB free)", (long)sdcard_get_free_mib());
}
void rtc_iso_now(char *out, size_t n) {
    if (broker_rtc_hw_alive()) {
        broker_rtc_data_t r; broker_rtc_read(&r);
        if (r.valid) {
            snprintf(out, n, "%04u-%02u-%02uT%02u:%02u:%02u",
                     (unsigned)r.year,  (unsigned)r.month, (unsigned)r.day,
                     (unsigned)r.hour,  (unsigned)r.minute,(unsigned)r.second);
            return;
        }
    }
    snprintf(out, n, "oscstop");
}

// ── Button polling / debounce / single-double click ─────────────────────────
int button_poll(void) {
    uint32_t now = millis_u32();
    bool low = (gpio_get_level(PIN_BUTTON) == 0);
    int event = 0;
    if (low != s_btn_prev_low && (now - s_btn_last_change) >= BTN_DEBOUNCE_MS) {
        s_btn_last_change = now;
        s_btn_prev_low    = low;
        if (low) {
            if (s_btn_state == BTN_WAIT_DBL) {
                s_btn_state = BTN_PRESSED_2;
            } else {
                s_btn_state          = BTN_PRESSED;
                s_btn_press_start_ms = now;   // Stage 17: track for long-press swallow
            }
        } else {
            if (s_btn_state == BTN_PRESSED) {
                uint32_t held = now - s_btn_press_start_ms;
                if (held >= BTN_LONG_PRESS_MS) {
                    // Held past the shutdown-commit threshold. The shutdown
                    // watcher owns this press; do NOT queue a delayed single-
                    // click that would fire after the user's finger came up.
                    s_btn_state = BTN_IDLE;
                } else {
                    s_btn_release_ms = now;
                    s_btn_state      = BTN_WAIT_DBL;
                }
            } else if (s_btn_state == BTN_PRESSED_2) {
                s_btn_state = BTN_IDLE;
                event = 2;
            }
        }
    }
    if (s_btn_state == BTN_WAIT_DBL && (now - s_btn_release_ms) > BTN_DOUBLE_GAP_MS) {
        s_btn_state = BTN_IDLE;
        event = 1;
    }
    return event;
}

// Polled detent-rest encoder state machine (see feedback_encoder_polling.md:
// ALPS EC05E settle bounce lands >15 ms after leading edge with B flipped;
// PCNT/ISR oscillates +1/-1).
int encoder_delta(void) {
    uint8_t a = gpio_get_level(PIN_ENC_A);
    uint8_t b = gpio_get_level(PIN_ENC_B);
    bool at_rest = (a && b);
    uint32_t now = millis_u32();
    int emit = 0;

    if (at_rest) {
        if (s_enc.in_motion) {
            if (s_enc.rest_since_ms == 0) {
                s_enc.rest_since_ms = now;
            } else if ((now - s_enc.rest_since_ms) >= ENC_DETENT_REST_MS) {
                if (s_enc.latched_dir != 0) emit = s_enc.latched_dir;
                s_enc.latched_dir  = 0;
                s_enc.in_motion    = false;
            }
        }
    } else {
        s_enc.rest_since_ms = 0;
        if (!s_enc.in_motion) {
            if      (a == 0 && b == 1) s_enc.latched_dir = +1;
            else if (a == 1 && b == 0) s_enc.latched_dir = -1;
            s_enc.in_motion = true;
        }
    }
    return emit;
}

// ── Sensor wake / park (only the sensors used by the given mode) ────────────
void wake_sensors_for_mode(fc_mode_t m) {
    switch (m) {
    case FCM_ENV:
        broker_env_set_enabled(true);
        broker_light_set_enabled(true);
        break;
    case FCM_MOTION:
        broker_imu_set_enabled(true);
        broker_mag_set_enabled(true);
        break;
    case FCM_PPG_BCG:
        // Stage 17: PPG+BCG combined raw. HR task stays disabled -- we drive
        // MAX30101 directly (MULTI_LED, raw green) inside the 200 Hz tick loop
        // in run_ppg_bcg_mode() to avoid FIFO-drain contention.
        broker_imu_set_enabled(true);
        break;
    case FCM_COMPASS:
        broker_mag_set_enabled(true);
        break;
    case FCM_TEMP:
        // ENV has die + air (BME688); SKIN has TMP117; IMU has on-die
        // temp (LSM6DSV16X) published on broker_imu_data_t.temperature.
        // The MAX30101 die-temp is read directly via one-shot register
        // poke in read_max_die_temp() -- we do NOT enable the HR task,
        // because that would kick the MAX out of shutdown into PPG mode
        // and turn the green LED on (unnecessary + adds thermal load).
        broker_env_set_enabled(true);
        broker_imu_set_enabled(true);
        broker_skin_set_enabled(true);
        break;
    case FCM_BCG:
        broker_imu_set_enabled(true);
        break;
    case FCM_MLC_COLLECT:
        // Stage 10 bug fix: MLC training capture reads broker_imu on every
        // tick. Without this the IMU task stays parked and every row lands
        // as zeros -- silently useless training data.
        broker_imu_set_enabled(true);
        break;
    default: break;
    }
}
void park_all_modal_sensors(void) {
    broker_env_set_enabled(false);
    broker_light_set_enabled(false);
    broker_imu_set_enabled(false);
    broker_mag_set_enabled(false);
    broker_hr_set_enabled(false);
    broker_skin_set_enabled(false);
}
