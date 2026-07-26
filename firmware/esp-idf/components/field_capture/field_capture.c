/**
 * @file field_capture.c
 * @brief Port of 7_demo_field_capture. See field_capture.h.
 */

#include "field_capture.h"
#include "firmware_version.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>       // atoi() for BLACKBOX_CADENCE / LED
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>       // FS_LS command
#include <unistd.h>     // fsync() -- FatFs f_sync bridge (see SD durability rule)

// ═════════════════════════════════════════════════════════════════════════════
// SD durability rule (added 2026-07-24 after batt_0025.csv came back empty).
//
// fflush() alone is NOT enough. It flushes stdio buffer to VFS but FatFs's
// directory entry (file size + mtime) only gets committed on f_sync (via
// fsync(fd)) or f_close. If the device dies between fflushes -- as batt_0025
// did -- the file appears on disk with size 0 and NO data is recoverable.
//
// Every writer that survives loss-of-power must pick ONE of these patterns:
//
//   PATTERN A -- open-append-close per row.
//     Use for LOW rate writers (say <10 Hz). Simplest to reason about.
//     Each fclose() commits the directory entry. Cost ~1-5 ms per row.
//     Example: run_battery_test_mode (10 s cadence).
//
//   PATTERN B -- keep open, fsync per row.
//     Use for HIGH rate writers (say >=10 Hz) that can't afford fopen/fclose
//     overhead. Header written + fclose on entry; on each row: open("a"), or
//     hold file open + fflush() + fsync(fileno(f)). fsync forces f_sync
//     which commits the directory entry. Cost ~0.5-2 ms per fsync.
//     Example: run_mlc_collect_mode (50 Hz).
//
//   NEVER USE: fflush() alone. Under power loss, expect zero-byte files.
// ═════════════════════════════════════════════════════════════════════════════

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"      // esp_restart / esp_get_free_heap_size
#include "esp_pm.h"           // esp_pm_lock_create/acquire/release for VBUS-aware batt_test
#include "soc/rtc.h"          // rtc_clk_cpu_freq_get_config -- CPU freq snapshot under DFS
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "data_broker.h"
// encoder.h intentionally not included -- field_capture polls the encoder
// pins directly (see feedback_encoder_polling.md: ALPS EC05E needs the
// state-machine approach, PCNT/ISR oscillates +1/-1 on settle bounce).
#include "ws2812.h"
#include "pcf85063.h"                     // for the RTC CLI task
#include "nvs_cfg.h"                      // RTC command persistence + boot printout
#include "usb_msc.h"                      // Stage 11: cyan-tile lazy USB MSC
#include "driver/usb_serial_jtag.h"       // reliable stdin over USB-CDC

// ─── TEMPORARY (mk1): battery-voltage ADC via 5k1-5k1 hack on screen GPIO9 ───
// The BQ25619 has no real VBAT ADC -- the "voltage" it reports is a stuck
// threshold setpoint. While the screen is not on the FPC, we borrow its
// GPIO9 (screen D2 pad) as ADC1_CH8 and read a 1:2 divider from Vbat.
// Multiplied by 2 in firmware. Remove this block once the screen returns
// OR once we swap in BQ25896 / MAX17048 for mk2.
// Wiring: Vbat -- 5k1 -- GPIO9 -- 5k1 -- GND (optional 100nF at GPIO9 to GND).
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
// The qvar_ecg component IS built, but ESP-IDF's transitive REQUIRES
// linking places libqvar_ecg.a BEFORE libfield_capture.a in every pass
// of the multi-scan (no --start-group is emitted). GNU ld therefore
// never resolves the field_capture -> qvar_ecg_* references. Fighting
// the build system for one 3-function driver is not worth it; the
// register I/O is inlined below via the shared g_i2c_mutex. Header is
// still pulled in for the register / bit macros.
#include "qvar_ecg.h"
#include "flashlight.h"
#include "sdcard.h"
#include "mic_pdm.h"
#include "bme688_drv.h"     // BME688_DRIVER_VERSION exists; used for WHOAMI
#include "haptic.h"
#include "bq25619.h"

static const char *TAG = "FIELD";

// Shared I2C bus 0 mutex, defined in boot_logic/boot_hw_init.c. Declared
// extern here so we can serialize the QVAR register access with the IMU
// task and other bus-0 users.
extern SemaphoreHandle_t g_i2c_mutex;

// ── Inline QVAR block (bypass qvar_ecg component due to link-order bug) ─────
// Bit-layout fixes verified on-bench with firmware/arduino/9_test_ecg:
//   * AH_QVAR_EN is CTRL7 bit 7 (0x80), NOT bit 0 (0x01) as the qvar_ecg.h
//     macro claims. The old value was actually enabling LPF1_G_EN (a
//     gyro filter), so the analog hub never turned on.
//   * CTRL1 layout is OP_MODE_XL[6:4] + ODR_XL[3:0]. Do not use the
//     lsm6dsv16x.h CTRL1_ODR_* macros -- they have the fields reversed
//     and would drop the accel into low-power mode.
// Use these direct hex values instead:
#define ECG_LSM_ADDR            0x6B
#define ECG_REG_CTRL1           0x10
#define ECG_REG_CTRL2           0x11
#define ECG_REG_CTRL7           0x16
#define ECG_REG_OUT_AH_L        0x3A
#define ECG_CTRL7_AH_QVAR_EN    (1 << 7)   // bit 7 -- correct
#define ECG_CTRL7_ZIN_2G4       (0x00 << 4) // 2.4 GOhm (highest impedance)
#define ECG_CTRL1_HP_240HZ      0x07       // OP_MODE=HP (000<<4) + ODR=240Hz (0111<<0)

static esp_err_t qvar_reg_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t r = i2c_master_write_to_device(I2C_NUM_0, ECG_LSM_ADDR,
                                             b, 2, pdMS_TO_TICKS(20));
    xSemaphoreGive(g_i2c_mutex);
    return r;
}
static esp_err_t qvar_reg_read_word(uint8_t reg_lo, int16_t *out) {
    uint8_t rx[2] = {0};
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t r = i2c_master_write_read_device(I2C_NUM_0, ECG_LSM_ADDR,
                                               &reg_lo, 1, rx, 2,
                                               pdMS_TO_TICKS(20));
    xSemaphoreGive(g_i2c_mutex);
    if (r == ESP_OK) *out = (int16_t)((uint16_t)rx[1] << 8 | rx[0]);
    return r;
}
static esp_err_t qvar_local_enable(void) {
    // Datasheet §9.20 sequence: power down accel + gyro, flip AH_QVAR_EN,
    // then re-enable accel in HP mode so QVAR has a sample clock.
    esp_err_t r;
    r = qvar_reg_write(ECG_REG_CTRL1, 0x00);              // accel PD
    if (r != ESP_OK) return r;
    r = qvar_reg_write(ECG_REG_CTRL2, 0x00);              // gyro  PD
    if (r != ESP_OK) return r;
    vTaskDelay(pdMS_TO_TICKS(5));
    r = qvar_reg_write(ECG_REG_CTRL7,
                       ECG_CTRL7_AH_QVAR_EN | ECG_CTRL7_ZIN_2G4);
    if (r != ESP_OK) return r;
    vTaskDelay(pdMS_TO_TICKS(5));
    r = qvar_reg_write(ECG_REG_CTRL1, ECG_CTRL1_HP_240HZ);
    if (r != ESP_OK) return r;
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}
static esp_err_t qvar_local_disable(void) {
    // Clear AH_QVAR_EN. Accel stays configured; imu task will re-init if
    // it's re-enabled after this.
    return qvar_reg_write(ECG_REG_CTRL7, 0x00);
}

// ── Pins / timings ──────────────────────────────────────────────────────────
#define PIN_BUTTON             16
#define PIN_ENC_A              21
#define PIN_ENC_B              43

#define POLL_TICK_MS            5   // 5 ms to catch fast encoder rotations
#define BTN_DEBOUNCE_MS        30
#define BTN_DOUBLE_GAP_MS      350
#define ENC_DETENT_REST_MS     10   // both A+B HIGH continuously to confirm rest
// Recording sessions run until button click -- no fixed duration. The 15-min
// uptime cap in task_shutdown_watcher_fn covers the "left running" case.
#define VOICE_ANNOT_MS         5000     // fixed voice-annotation prelude (WAV)
#define BEEP_COUNT             3
#define BEEP_ON_MS             140
#define BEEP_OFF_MS            260
#define PULSE_STANDBY_MS       5000
#define PULSE_RECORD_MS        1000
#define SOLID_ON_ACTIVITY_MS   2000

#define FL_LEVELS              20
#define FL_MIN_PCT              3
#define FL_MAX_PCT             40   // 75 % was still too bright to the eye
#define FL_INIT_LEVEL          10

// Set to 1 to log a broker snapshot of every alive sensor once per second
// during ST_STANDBY. Cheap; useful for bench sanity-checking.
#ifndef DEBUG_PRINT_SENSORS
#define DEBUG_PRINT_SENSORS     1
#endif
#define SENSOR_PRINT_PERIOD_MS 1000

// ── DRV effect IDs (subset from sketch) ─────────────────────────────────────
#define DRV_STRONG_CLICK        1
#define DRV_MEDIUM_CLICK       20
#define DRV_LONG_BUZZ          47
#define ALARM_DURATION_MS   15000

// ── Palette (WS2812 max per channel, dimmed for eyes) ───────────────────────
#define WS_MAX 26
typedef struct { const char *name; uint8_t r, g, b; } mode_info_t;
static const mode_info_t MODE_INFO[FCM_COUNT] = {
    { "mic",     26,  0,  0 },
    { "env",      0, 26,  0 },
    { "mot",     14, 26,  0 },   // LSM submenu -- palette shown alternating with LSM yellow
    { "skin",    26,  0, 12 },
    { "fl",      18, 18, 18 },
    { "alarm",    8,  0, 26 },
    // COMPASS palette isn't used for solid/pulse in standby -- it renders
    // as a 2 Hz red<->blue alternation (matches the live N/S gradient).
    // Kept here for logging + parity with the enum.
    { "compass", 26,  0, 26 },
    // QVAR touch-button demo (was "ecg" through Stage 6 -- renamed
    // Stage 7 § 4 after QVAR was confirmed as a touch sensor, not ECG).
    // Rendered as a 2 Hz yellow<->purple alternation in standby.
    { "qvar",    26, 26,  0 },
    // TEMP -- rendered as fire strobing (yellow/orange/red flicker,
    // Stage 10 upgrade), handled specially in the standby LED path.
    { "temp",    26,  8,  0 },
    // BCG -- LSM submenu entry. Rendered as a 2 Hz yellow<->red alternation
    // when the actual mode is active.
    { "bcg",     26, 12,  0 },
    // -- Stage 10 additions ------------------------------------------------
    // LSM gateway (yellow slot at top level). Enter with single-click; on
    // entry, s_in_submenu goes true and s_mode advances to first submenu entry.
    { "lsm",     26, 20,  0 },
    // STEPS -- pedometer. Cyan indicator (paired with yellow when in submenu).
    { "steps",    0, 22, 22 },
    // MLC_COLLECT -- raw accel+gyro training data. Red (danger colour -- reminds
    // you not to walk away with recording live). S5 stub for now.
    { "mlccol",  26,  0,  0 },
    // TAP_DBG -- host-side magnitude tap detector + chip tap echo. Magenta
    // in the submenu; red when out-of-zone, magnitude-colored while active.
    { "tapdbg",  26,  0, 26 },
    // -- Stage 11 additions ------------------------------------------------
    // USB_MSC -- cyan top-level tile. Solid/pulsing cyan in standby; while
    // active, cyan slowly breathes and the LED palette is owned by usb_msc.c.
    { "usb",      0, 26, 26 },
};

// LSM submenu presentation order. Extend here to add more LSM sub-modes.
const fc_mode_t FC_LSM_SUBMENU[] = {
    FCM_MOTION,
    FCM_BCG,
    FCM_STEPS,
    FCM_MLC_COLLECT,
    FCM_TAP_DBG,
};
const uint8_t FC_LSM_SUBMENU_LEN = sizeof(FC_LSM_SUBMENU) / sizeof(FC_LSM_SUBMENU[0]);

// True if `m` is only reachable via the LSM submenu (skip in top-level cycle).
static inline bool is_lsm_submode(fc_mode_t m) {
    for (uint8_t i = 0; i < FC_LSM_SUBMENU_LEN; i++) {
        if (FC_LSM_SUBMENU[i] == m) return true;
    }
    return false;
}
// True if `m` should appear in the top-level encoder cycle.
static inline bool is_top_level(fc_mode_t m) {
    return (m < FCM_COUNT) && !is_lsm_submode(m);
}
// Advance/retreat `m` through the top-level cycle, skipping submodes.
static fc_mode_t top_mode_step(fc_mode_t m, int dir) {
    int c = (int)m;
    do {
        c += dir;
        if (c < 0)              c += FCM_COUNT;
        if (c >= (int)FCM_COUNT) c -= FCM_COUNT;
    } while (!is_top_level((fc_mode_t)c));
    return (fc_mode_t)c;
}
// Advance/retreat `m` through the LSM submenu.
static fc_mode_t lsm_submenu_step(fc_mode_t m, int dir) {
    int idx = 0;
    for (uint8_t i = 0; i < FC_LSM_SUBMENU_LEN; i++) {
        if (FC_LSM_SUBMENU[i] == m) { idx = i; break; }
    }
    int c = idx + dir;
    if (c < 0)                          c += FC_LSM_SUBMENU_LEN;
    if (c >= (int)FC_LSM_SUBMENU_LEN)   c -= FC_LSM_SUBMENU_LEN;
    return FC_LSM_SUBMENU[c];
}

typedef enum {
    ST_STANDBY = 0,
    ST_FL_ON,
    ST_RECORDING,
    ST_ALARM_FIRING,
} app_state_t;

// ── Forward-decl of watcher-owned globals (defined near task_shutdown_watcher_fn
//    at the bottom of the file). Needed so MLC_COLLECT (defined higher up)
//    can reference them. ─────────────────────────────────────────────────────
extern volatile bool g_shutdown_hold_active;
extern volatile bool g_recording_active;
extern volatile bool g_watcher_press_reset_pending;

// ── Globals ──────────────────────────────────────────────────────────────────
static fc_mode_t   s_mode              = FCM_ENV;
static bool        s_in_submenu        = false;  // Stage 10: LSM submenu latch
static uint32_t    s_last_tap_dbl_count = 0;     // for tap-Z-double dedup (see loop)
static app_state_t s_state             = ST_STANDBY;
static uint32_t    s_last_activity_ms  = 0;
static uint32_t    s_boot_seq          = 0;
static uint32_t    s_rec_seq           = 0;
static uint8_t     s_fl_level          = FL_INIT_LEVEL;
static bool        s_sd_ready          = false;

// Encoder polled detent-rest state machine (see auto-memory
// feedback_encoder_polling.md: ALPS EC05E's settle bounce lands >15 ms
// after leading edge with B flipped; PCNT/ISR approach oscillates +1/-1).
// One click emitted per full detent-rest → motion → detent-rest cycle.
// Direction latched from the FIRST line to go LOW out of rest.
typedef struct {
    bool     in_motion;      // set when we leave both-HIGH rest
    int8_t   latched_dir;    // 0, +1 (CW), -1 (CCW); latched at leave-rest
    uint32_t rest_since_ms;  // when both lines last became HIGH (0 = not yet)
} enc_poll_t;
static enc_poll_t  s_enc = { false, 0, 0 };

// Button state machine
typedef enum { BTN_IDLE, BTN_PRESSED, BTN_WAIT_DBL, BTN_PRESSED_2 } btn_sm_t;
static btn_sm_t    s_btn_state         = BTN_IDLE;
static uint32_t    s_btn_last_change   = 0;
static uint32_t    s_btn_release_ms    = 0;
static bool        s_btn_prev_low      = false;

// Recording session
static FILE       *s_csv_file          = NULL;
static bool        s_recording_early_end = false;

// -- FCM_COMPASS state (ESP-IDF-only feature, not present in the sketch).
// Hard-iron calibration is boot-scoped: valid until the next reboot, no
// NVS persistence. First press in compass mode runs the 10 s figure-8;
// subsequent presses (same boot) skip cal and enter the compass directly.
static bool  s_mag_cal_done  = false;
static float s_mag_offset_x  = 0.0f;
static float s_mag_offset_y  = 0.0f;
static float s_mag_scale_x   = 1.0f;
static float s_mag_scale_y   = 1.0f;

// ── Utility ──────────────────────────────────────────────────────────────────
static inline uint32_t millis_u32(void) {
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}
static inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static uint8_t fl_pct_from_level(uint8_t level) {
    if (level > FL_LEVELS) level = FL_LEVELS;
    return (uint8_t)clamp_u32(
        FL_MIN_PCT + (uint32_t)level * (FL_MAX_PCT - FL_MIN_PCT) / FL_LEVELS,
        FL_MIN_PCT, FL_MAX_PCT);
}

// ── NVS (field/mode + field/boot_seq + field/lsm_sub) ───────────────────────
static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open("field", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t m = FCM_ENV;
    nvs_get_u8(h, "mode", &m);
    if (m >= FCM_COUNT) m = FCM_ENV;
    s_mode = (fc_mode_t)m;
    // Stage 10: if the persisted mode is only reachable via the LSM submenu,
    // implicitly restore the submenu latch too. Handles both intentional
    // save-in-submenu and pre-Stage-10 firmware that saved FCM_MOTION/BCG
    // as a top-level mode (would otherwise deadlock the top-level cycle).
    s_in_submenu = is_lsm_submode(s_mode);
    nvs_get_u32(h, "boot_seq", &s_boot_seq);
    s_boot_seq++;
    nvs_set_u32(h, "boot_seq", s_boot_seq);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "boot_seq=%lu restored mode=%s (submenu=%d)",
             (unsigned long)s_boot_seq, MODE_INFO[s_mode].name, s_in_submenu ? 1 : 0);
}
static void nvs_save_mode(void) {
    nvs_handle_t h;
    if (nvs_open("field", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "mode", (uint8_t)s_mode);
    nvs_commit(h);
    nvs_close(h);
}

uint32_t field_capture_get_boot_seq(void) {
    return s_boot_seq;
}

static int button_poll(void);  // forward decl (definition ~line 610)
static void watcher_ship_mode(void);  // forward decl (definition further down)

// Callback for usb_msc_run_until_exit -- returns true on a single click.
// Runs from the usb_msc loop tick (~50 ms), not from the main state machine.
static bool usb_msc_button_click_cb(void) {
    return button_poll() == 1;
}

// ── RGB LED helpers ──────────────────────────────────────────────────────────
static void rgb_off(void) { ws2812_set_color(0, 0, 0); }
static void rgb_set_max(fc_mode_t m) {
    const mode_info_t *mi = &MODE_INFO[m];
    ws2812_set_color(mi->r, mi->g, mi->b);
}
// Anchored breathe: phase=0 at `t_anchor` starts at MAX brightness,
// dips to 0 at half period, back to MAX. Prevents "resume mid-downturn"
// when transitioning from solid-on-activity to the standby pulse.
static void rgb_pulse(fc_mode_t m, uint32_t t_anchor, uint32_t period_ms) {
    uint32_t now = millis_u32();
    uint32_t elapsed = (now >= t_anchor) ? (now - t_anchor) : 0;
    float phase = (float)(elapsed % period_ms) / (float)period_ms;
    float s = 0.5f * (1.0f + cosf(phase * 2.0f * (float)M_PI));
    const mode_info_t *mi = &MODE_INFO[m];
    ws2812_set_color((uint8_t)(s * mi->r),
                     (uint8_t)(s * mi->g),
                     (uint8_t)(s * mi->b));
}

// ── SD mount + directory prep ────────────────────────────────────────────────
// Best-effort mkdir; EEXIST is not an error worth logging.
static void try_mkdir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir(%s) failed: %s (errno=%d)", path, strerror(errno), errno);
    }
}
static void ensure_sd(void) {
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
    try_mkdir("/sd/data/bcg");    // Stage 7: BCG session recordings
    try_mkdir("/sd/data/qvar");   // Stage 7: QVAR session recordings
    try_mkdir("/sd/data/steps");        // Stage 10: pedometer session recordings
    try_mkdir("/sd/data/mlc_train");    // Stage 10: MLC training data collection
    ESP_LOGI(TAG, "SD mounted (%ld MiB free)", (long)sdcard_get_free_mib());
}
static void rtc_iso_now(char *out, size_t n) {
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

// ── WAV header (44 B canonical PCM RIFF) ────────────────────────────────────
static void wav_write_header(FILE *f, uint32_t sample_rate, uint16_t bits) {
    uint8_t hdr[44] = {0};
    memcpy(hdr,    "RIFF", 4);
    memcpy(hdr+8,  "WAVEfmt ", 8);
    hdr[16] = 16;   // fmt chunk size
    hdr[20] = 1;    // PCM
    hdr[22] = 1;    // mono
    hdr[24] = sample_rate         & 0xFF;
    hdr[25] = (sample_rate >>  8) & 0xFF;
    hdr[26] = (sample_rate >> 16) & 0xFF;
    hdr[27] = (sample_rate >> 24) & 0xFF;
    uint32_t byte_rate = sample_rate * (bits / 8);
    hdr[28] = byte_rate         & 0xFF;
    hdr[29] = (byte_rate >>  8) & 0xFF;
    hdr[30] = (byte_rate >> 16) & 0xFF;
    hdr[31] = (byte_rate >> 24) & 0xFF;
    hdr[32] = (bits / 8);        // block align
    hdr[34] = (uint8_t)bits;
    memcpy(hdr+36, "data", 4);
    fwrite(hdr, 1, sizeof(hdr), f);
}
static void wav_patch_header(FILE *f, uint32_t data_bytes) {
    uint32_t riff_size = 36 + data_bytes;
    uint8_t v[4];
    fseek(f, 4, SEEK_SET);
    v[0]= riff_size&0xFF; v[1]=(riff_size>>8)&0xFF;
    v[2]=(riff_size>>16)&0xFF; v[3]=(riff_size>>24)&0xFF;
    fwrite(v, 1, 4, f);
    fseek(f, 40, SEEK_SET);
    v[0]= data_bytes&0xFF; v[1]=(data_bytes>>8)&0xFF;
    v[2]=(data_bytes>>16)&0xFF; v[3]=(data_bytes>>24)&0xFF;
    fwrite(v, 1, 4, f);
    fseek(f, 0, SEEK_END);
}

// ── Blocking WAV recorder ────────────────────────────────────────────────────
// Forward declarations for helpers defined further down (button state machine
// lives with the encoder logic; wav_record_to calls into it to allow the MIC
// mode to exit on click).
static int button_poll(void);

// Write a "<wav_path>.meta.txt" sidecar next to a WAV file with the same
// provenance line the CSV files carry. WAV lacks a standard text-comment
// field we can rely on, so we keep the metadata as a separate small text
// file with a matching stem.
static void wav_write_meta_sidecar(const char *wav_path, const char *tag) {
    char meta[128];
    snprintf(meta, sizeof(meta), "%s.meta.txt", wav_path);
    FILE *f = fopen(meta, "w");
    if (!f) return;
    char now[32]; rtc_iso_now(now, sizeof(now));
    fprintf(f, "rtc_start=%s hw=%s fw=%s boot=%lu seq=%lu tag=%s\n",
            now, KOMPIC_HW_VERSION, KOMPIC_FW_VERSION,
            (unsigned long)s_boot_seq, (unsigned long)s_rec_seq, tag);
    fclose(f);
}

// Record a WAV file.
//   duration_ms == 0  -> record until the button is clicked (or the 15-min
//                        global uptime cap fires). Suitable for MIC mode.
//   duration_ms  > 0  -> hard-capped duration (used for the 5 s voice-annotation
//                        prelude of the CSV modes). Button click also exits.
static uint32_t wav_record_to(const char *path, uint32_t duration_ms) {
    ensure_sd();
    if (!s_sd_ready) return 0;
    if (mic_pdm_start() != ESP_OK) {
        ESP_LOGW(TAG, "mic_pdm_start failed");
        return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s failed: %s (errno=%d)", path, strerror(errno), errno);
        mic_pdm_stop();
        return 0;
    }
    wav_write_header(f, MIC_PDM_SAMPLE_RATE_HZ, MIC_PDM_BITS_PER_SAMPLE);

    static uint8_t frame[MIC_PDM_FRAME_BYTES];
    uint32_t written = 0;
    uint32_t start = millis_u32();
    for (;;) {
        if (s_recording_early_end) break;
        if (duration_ms > 0 && (millis_u32() - start) >= duration_ms) break;
        if (button_poll() == 1) { s_recording_early_end = true; break; }

        size_t got = 0;
        if (mic_pdm_read(frame, sizeof(frame), &got, 40) == ESP_OK && got) {
            fwrite(frame, 1, got, f);
            written += got;
        }
        rgb_pulse(FCM_MIC, start, PULSE_RECORD_MS);
    }
    wav_patch_header(f, written);
    fclose(f);
    mic_pdm_stop();
    ESP_LOGI(TAG, "WAV %s -> %lu bytes", path, (unsigned long)written);
    return written;
}

// ── CSV open + per-mode row writer ───────────────────────────────────────────
static FILE *csv_open(const char *dir, uint32_t seq, const char *header) {
    ensure_sd();
    if (!s_sd_ready) return NULL;
    char path[96];
    snprintf(path, sizeof(path), "/sd/data/%s/s%04lu_r%04lu.csv",
             dir, (unsigned long)s_boot_seq, (unsigned long)seq);
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "csv fopen %s failed: %s (errno=%d)",
                 path, strerror(errno), errno);
        return NULL;
    }
    char now[32]; rtc_iso_now(now, sizeof(now));
    // Provenance line -- every SD file starts with this. Traces the recording
    // back to (a) the RTC wall-clock at file open, (b) the physical hardware
    // revision, (c) the firmware version compiled into this binary, and
    // (d) the boot / session sequence numbers.
    fprintf(f, "# rtc_start=%s hw=%s fw=%s boot=%lu seq=%lu mode=%s\n",
            now, KOMPIC_HW_VERSION, KOMPIC_FW_VERSION,
            (unsigned long)s_boot_seq, (unsigned long)seq, dir);
    fprintf(f, "%s\n", header);
    ESP_LOGI(TAG, "CSV %s opened", path);
    return f;
}
static void csv_row_env(FILE *f) {
    broker_env_data_t e;   broker_env_read(&e);
    broker_light_data_t l; broker_light_read(&l);
    fprintf(f, "%lu,%.2f,%.2f,%.2f,%.0f,%.1f\n",
            (unsigned long)millis_u32(),
            e.temperature_c, e.humidity_pct, e.pressure_hpa,
            e.gas_resistance_ohm, l.lux);
}
static void csv_row_motion(FILE *f) {
    broker_imu_data_t im; broker_imu_read(&im);
    broker_mag_data_t mg; broker_mag_read(&mg);
    fprintf(f, "%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f\n",
            (unsigned long)millis_u32(),
            im.accel_x, im.accel_y, im.accel_z,
            im.gyro_x,  im.gyro_y,  im.gyro_z,
            mg.x_ut,    mg.y_ut,    mg.z_ut);
}
static void csv_row_skin(FILE *f) {
    broker_skin_data_t s; broker_skin_read(&s);
    broker_hr_data_t   h; broker_hr_read(&h);
    fprintf(f, "%lu,%.2f,%u,%u,%.1f\n",
            (unsigned long)millis_u32(),
            s.skin_temp_c,
            (unsigned)h.bpm, (unsigned)h.finger_detected, h.spo2_pct);
}

// ── Debug: print ONLY the sensors relevant to the current recording mode,
// throttled to once per second. Matches the sketch's per-mode Serial dump.
#if DEBUG_PRINT_SENSORS
static void debug_dump_mode(fc_mode_t m) {
    static uint32_t s_next_ms = 0;
    uint32_t now = millis_u32();
    if (now < s_next_ms) return;
    s_next_ms = now + SENSOR_PRINT_PERIOD_MS;

    switch (m) {
    case FCM_ENV: {
        broker_env_data_t e;   broker_env_read(&e);
        broker_light_data_t l; broker_light_read(&l);
        ESP_LOGI(TAG, "[ENV] T=%.2fC H=%.1f%% P=%.2fhPa gas=%.0fOhm alt=%.1fm lux=%.1f",
                 e.temperature_c, e.humidity_pct, e.pressure_hpa,
                 e.gas_resistance_ohm, e.altitude_m, l.lux);
        break;
    }
    case FCM_MOTION: {
        broker_imu_data_t im; broker_imu_read(&im);
        broker_mag_data_t mg; broker_mag_read(&mg);
        ESP_LOGI(TAG, "[MOT] acc=%.2f,%.2f,%.2f  gyro=%.1f,%.1f,%.1f  mag=%.1f,%.1f,%.1f uT",
                 im.accel_x, im.accel_y, im.accel_z,
                 im.gyro_x,  im.gyro_y,  im.gyro_z,
                 mg.x_ut,    mg.y_ut,    mg.z_ut);
        break;
    }
    case FCM_SKIN: {
        broker_skin_data_t s; broker_skin_read(&s);
        broker_hr_data_t   h; broker_hr_read(&h);
        ESP_LOGI(TAG, "[SKN] skin=%.2fC  bpm=%u finger=%d spo2=%.1f%% quality=%u",
                 s.skin_temp_c,
                 (unsigned)h.bpm, h.finger_detected, h.spo2_pct,
                 (unsigned)h.signal_quality);
        break;
    }
    default: break;
    }
}
#endif

// ── Button polling / debounce / single-double click ──────────────────────────
// Return: 0 = none, 1 = single click, 2 = double click
static int button_poll(void) {
    uint32_t now = millis_u32();
    bool low = (gpio_get_level(PIN_BUTTON) == 0);
    int event = 0;
    if (low != s_btn_prev_low && (now - s_btn_last_change) >= BTN_DEBOUNCE_MS) {
        s_btn_last_change = now;
        s_btn_prev_low    = low;
        if (low) {
            s_btn_state = (s_btn_state == BTN_WAIT_DBL) ? BTN_PRESSED_2 : BTN_PRESSED;
        } else {
            if (s_btn_state == BTN_PRESSED) {
                s_btn_release_ms = now;
                s_btn_state = BTN_WAIT_DBL;
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

// Polled detent-rest state machine. Returns +1 / -1 on a completed
// detent-rest → motion → detent-rest cycle, otherwise 0.
static int encoder_delta(void) {
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
            // Latch direction from whichever line went LOW first.
            if      (a == 0 && b == 1) s_enc.latched_dir = +1;
            else if (a == 1 && b == 0) s_enc.latched_dir = -1;
            // Both LOW: skip; will catch it on the next entry to motion.
            s_enc.in_motion = true;
        }
    }
    return emit;
}

// (Old double-click enter_ship_mode() + abort_recording_and_flush() lived
// here. Ship-mode is now owned by task_shutdown_watcher_fn (4 s hold, Stage 7
// § 3), which calls watcher_ship_mode() directly and does not attempt to
// flush open files. If a future feature wants clean file wind-down on
// shutdown, add it back and call from watcher_ship_mode() before BATFET.)

// ── Sensor wake / park (only the sensors used by the given mode) ────────────
// RTC + battery + haptic are always-on (enabled in boot_hw_init.c).
// The others sit parked until wake_sensors_for_mode() flips their enabled
// flag; their tasks then wake up and start driving I2C. On park, tasks
// idle again and (in the MAX30101 case) the driver puts the chip to sleep.
static void wake_sensors_for_mode(fc_mode_t m) {
    switch (m) {
    case FCM_ENV:
        broker_env_set_enabled(true);
        broker_light_set_enabled(true);
        break;
    case FCM_MOTION:
        broker_imu_set_enabled(true);
        broker_mag_set_enabled(true);
        break;
    case FCM_SKIN:
        broker_hr_set_enabled(true);
        broker_skin_set_enabled(true);
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
        // as zeros -- silently useless training data. Confirmed on bench
        // 2026-07-24 (fw=0.2.8): first recording session produced an all-
        // zeros CSV, no ax/ay/az/gx/gy/gz signal at all.
        broker_imu_set_enabled(true);
        break;
    default: break;  // MIC / FL / ALARM don't wake I2C sensors
    }
}
static void park_all_modal_sensors(void) {
    broker_env_set_enabled(false);
    broker_light_set_enabled(false);
    broker_imu_set_enabled(false);
    broker_mag_set_enabled(false);
    broker_hr_set_enabled(false);
    broker_skin_set_enabled(false);
}

// ── Compass mode (ESP-IDF-only, not in the sketch) ──────────────────────────
//
// Coordinate assumption: with the watch flat and the LIS3MDL "dot" corner
// at top-right, the chip's +X axis points AWAY from the wearer (forward).
// heading is computed with atan2f(-y, x) so that CW rotation of the watch
// (looking down) increases the reported bearing 0-360. If the compass
// spins the wrong way for you on the bench, flip the -y to +y in
// run_compass() -- that's the single sign to change.
//
// The N/S red-blue gradient uses cos(heading): +1 at N (red = max), 0 at
// E/W (LED off), -1 at S (blue = max). Smooth curve between.

// LED helpers specific to compass/cal
static void rgb_compass_alt_red_blue(void) {
    // 2 Hz alternation, 250 ms per colour. Same red/blue palette as the
    // live compass gradient -- the alternation just distinguishes "waiting
    // to enter compass" from "in compass mode" (where hue depends on heading).
    uint32_t t = millis_u32();
    bool red = ((t / 250) % 2) == 0;
    if (red) ws2812_set_color(26, 0,  0);
    else     ws2812_set_color( 0, 0, 26);
}
static void rgb_flash_purple(void) {
    // 5 Hz on/off flash during the 10 s figure-8 cal.
    uint32_t t = millis_u32();
    bool on = ((t / 100) % 2) == 0;
    if (on) ws2812_set_color(12, 0, 26);
    else    ws2812_set_color( 0, 0,  0);
}
// Compass LED gradient (simple version, iv7.1 baseline):
// cos(heading) drives a red<->blue mix. +1 at N (pure red), -1 at S (pure
// blue), 0 at E/W (LED off). No tilt overlay -- watch must be roughly flat
// for meaningful readings. Tilt-comp + tilt-hue overlay left for a future
// pass once LSM/LIS axis-alignment is confirmed on the bench.
static void rgb_compass_gradient(float heading_deg) {
    float rad = heading_deg * (float)M_PI / 180.0f;
    float c   = cosf(rad);
    uint8_t r = c > 0.0f ? (uint8_t)( c * (float)WS_MAX) : 0;
    uint8_t b = c < 0.0f ? (uint8_t)(-c * (float)WS_MAX) : 0;
    ws2812_set_color(r, 0, b);
}

// True when heading is within +/-5 deg of N (0) or S (180).
static bool heading_on_ns(float heading_deg) {
    float d_n = fminf(heading_deg, 360.0f - heading_deg);
    float d_s = fabsf(heading_deg - 180.0f);
    return d_n < 5.0f || d_s < 5.0f;
}

// 10 s figure-8 hard-iron calibration. Tracks per-axis min/max on X and Y.
// Stage 7 § 9: outlier rejection cuts the "one spike ruined my cal" failure
// mode by clamping input samples to a plausible local-field range before the
// min/max update. Local Earth field on the surface is ~30-70 uT vector
// magnitude, so any per-axis reading outside +/-500 uT is either a nearby
// magnet or a chip glitch -- skip it.
#define MAG_CAL_MAX_ABS_UT   500.0f
static void run_mag_cal(void) {
    ESP_LOGI(TAG, "[COMPASS] calibration START -- figure-8 for 10 seconds");
    haptic_play_forced(DRV_MEDIUM_CLICK);   // "get moving" tick
    float min_x =  1e9f, max_x = -1e9f;
    float min_y =  1e9f, max_y = -1e9f;
    uint32_t rejected = 0;
    uint32_t accepted = 0;
    uint32_t start = millis_u32();
    bool aborted = false;
    while ((millis_u32() - start) < 10000) {
        rgb_flash_purple();
        broker_mag_data_t m; broker_mag_read(&m);

        // Outlier rejection: skip samples with per-axis magnitudes outside
        // physically plausible Earth-field range. Kills single spikes from
        // passing magnets / laptop / clasp interference.
        if (fabsf(m.x_ut) > MAG_CAL_MAX_ABS_UT ||
            fabsf(m.y_ut) > MAG_CAL_MAX_ABS_UT) {
            rejected++;
        } else {
            if (m.x_ut < min_x) min_x = m.x_ut;
            if (m.x_ut > max_x) max_x = m.x_ut;
            if (m.y_ut < min_y) min_y = m.y_ut;
            if (m.y_ut > max_y) max_y = m.y_ut;
            accepted++;
        }

        if (button_poll() == 1) { aborted = true; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (aborted) {
        ESP_LOGW(TAG, "[COMPASS] cal aborted -- press again in compass mode to retry");
        return;
    }
    if (accepted < 50) {
        ESP_LOGW(TAG, "[COMPASS] cal REJECTED -- only %u accepted samples (%u rejected as outliers). retry.",
                 (unsigned)accepted, (unsigned)rejected);
        // "cal failed" feedback: three short clicks.
        for (int i = 0; i < 3; i++) {
            haptic_play_forced(DRV_STRONG_CLICK);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        return;
    }
    s_mag_offset_x = (min_x + max_x) * 0.5f;
    s_mag_offset_y = (min_y + max_y) * 0.5f;
    float rx = (max_x - min_x) * 0.5f;
    float ry = (max_y - min_y) * 0.5f;
    // Guard divide-by-zero if user didn't rotate enough on an axis.
    s_mag_scale_x = (rx > 1.0f) ? rx : 1.0f;
    s_mag_scale_y = (ry > 1.0f) ? ry : 1.0f;
    s_mag_cal_done = true;
    ESP_LOGI(TAG, "[COMPASS] cal DONE  off=(%.1f, %.1f) uT  span=(%.1f, %.1f) uT  (%u kept / %u rejected)",
             s_mag_offset_x, s_mag_offset_y, rx * 2.0f, ry * 2.0f,
             (unsigned)accepted, (unsigned)rejected);
    haptic_play_forced(DRV_LONG_BUZZ);
}

// Live compass -- runs until single button press. LED red<->blue on the
// N-S axis, DRV pulses at 1 Hz whenever heading is within +/-5 deg of
// N or S.
//
// TODO(iv8.0): on-demand recal is intentionally NOT bound to a button
// hold here. Single-click exits, and any hold >= 2 s belongs to the
// shutdown watcher's warn/fire ladder -- there is no clean window in
// between. Recal will move to a 3-second sustained QVAR-electrode touch
// once the always-on QVAR dispatcher lands (see Stage 8 §5). Until then,
// recal only happens on cold boot / after mag disturbance -> reset.
static void run_compass(void) {
    ESP_LOGI(TAG, "[COMPASS] compass ACTIVE  (single-click: exit)");
    uint32_t last_ns_pulse = 0;
    uint32_t last_print    = 0;
    while (1) {
        if (button_poll() == 1) break;
        uint32_t now = millis_u32();

        broker_mag_data_t m; broker_mag_read(&m);
        float nx = (m.x_ut - s_mag_offset_x) / s_mag_scale_x;
        float ny = (m.y_ut - s_mag_offset_y) / s_mag_scale_y;
        // 2D heading from X/Y only. Watch must be roughly flat for accuracy.
        // Sign of +ny flips the CW/CCW direction if the mag axis orientation
        // on the PCB is mirrored -- flip if compass spins the wrong way.
        float heading = atan2f(+ny, nx) * 180.0f / (float)M_PI;
        if (heading < 0.0f) heading += 360.0f;

        rgb_compass_gradient(heading);

        if (heading_on_ns(heading) && (now - last_ns_pulse) >= 1000) {
            last_ns_pulse = now;
            haptic_play(DRV_MEDIUM_CLICK);
        }
        if ((now - last_print) >= 500) {
            last_print = now;
            ESP_LOGI(TAG, "[COMPASS] heading=%5.1f deg   raw=(%.1f, %.1f) uT",
                     heading, m.x_ut, m.y_ut);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "[COMPASS] exit -> STANDBY");
}

// Single click on FCM_COMPASS: first press this boot -> cal then compass;
// subsequent presses -> straight to compass.
static void run_compass_or_cal(void) {
    wake_sensors_for_mode(FCM_COMPASS);
    vTaskDelay(pdMS_TO_TICKS(200));   // let LIS3MDL settle after wake
    if (!s_mag_cal_done) run_mag_cal();
    if (s_mag_cal_done)  run_compass();
    park_all_modal_sensors();
}

// ── ECG mode (ESP-IDF-only, not in the sketch) ──────────────────────────────
//
// LSM6DSV16X Qvar electrostatic sensor between two body electrodes (one on
// each arm, chest-loop path via ESD diodes). Two responsibilities:
//   1. Stream raw 16-bit samples to serial as `<value>\n` -- one number per
//      line, ASCII decimal. The Arduino IDE Serial Plotter running on the
//      same USB-CDC port will parse and plot these; ESP_LOG lines from
//      other tasks appear as gaps but don't crash the plotter.
//   2. Fire DRV on each detected beat so the user feels their pulse.
//
// Beat detector is the same shape as the PPG one in the sketch: DC-remove
// via a fast-then-slow EMA baseline, local-max of AC above adaptive
// threshold, 300 ms refractory (200 BPM cap).

// QVAR LED signature: 2 Hz yellow<->purple alternation. Distinct from
// every other mode at a glance (mic=red pulse, env=green, mot=yellow-green,
// skin=pink, temp=warm cycle, compass=red/blue alt, bcg=yellow/red alt).
static void rgb_qvar_alt_yellow_purple(void) {
    uint32_t t = millis_u32();
    bool yellow = ((t / 250) % 2) == 0;
    if (yellow) ws2812_set_color(26, 26,  0);   // yellow
    else        ws2812_set_color(14,  0, 26);   // purple
}

// TEMP mode signature (Stage 10 upgrade): fire strobing -- a red-orange-yellow
// palette cycle with per-frame pseudorandom brightness jitter to mimic a
// campfire / candle flame. Distinct from every other mode's signature at a
// glance and reads as "warmth / temperature" without needing a screen.
//
// Cadence: palette advances at ~5 Hz (200 ms/step); jitter refreshes every
// ~32 ms. Amplitude jitter deliberately never dips to zero so the LED
// never fully blinks off (that would read as "fault", not "fire").
static void rgb_temp_warm_cycle(void) {
    uint32_t t = millis_u32();
    // Deterministic pseudorandom flicker (Knuth multiplicative hash, no PRNG
    // state to maintain). New jitter value ~30 Hz.
    uint32_t h = (t / 32) * 2654435761U;
    uint8_t  jitter = (uint8_t)((h >> 26) & 0x0F);   // 0..15
    uint8_t  amp    = 18 + jitter;                   // 18..33 (clipped below)
    if (amp > WS_MAX) amp = WS_MAX;
    // Palette phase: 0=deep red, 1=orange, 2=yellow. ~5 Hz cycle.
    uint32_t phase = (t / 200) % 3;
    uint8_t  g_full;
    switch (phase) {
    case 0:  g_full = 0;         break;
    case 1:  g_full = amp / 4;   break;   // orange (r + 25% g)
    default: g_full = amp / 2;   break;   // yellow (r + 50% g)
    }
    ws2812_set_color(amp, g_full, 0);
}

// LSM submenu signature: half-second alternation between LSM yellow and the
// currently-selected submode's color. Reads at a glance as "in the LSM
// menu, currently on <mot|bcg|steps|mlccol>". Distinct from the 250 ms
// QVAR alt-pattern (yellow/purple) since the period is 2x slower.
static void rgb_lsm_submenu_indicator(fc_mode_t sub) {
    uint32_t t = millis_u32();
    bool show_yellow = ((t / 500) % 2) == 0;
    if (show_yellow) {
        const mode_info_t *mi = &MODE_INFO[FCM_LSM];
        ws2812_set_color(mi->r, mi->g, mi->b);
    } else {
        const mode_info_t *mi = &MODE_INFO[sub];
        ws2812_set_color(mi->r, mi->g, mi->b);
    }
}

// QVAR button demo. Physics recap:
//   The LSM6DSV16X QVAR is a charge-variometer, not a voltage sensor. It
//   integrates the current flowing on/off the two Qvar pins. When a finger
//   touches an electrode, charge redistributes: big transient spike (onset),
//   then internal leakage equalizes the amp back toward baseline within ~1s.
//   Release produces another transient in the opposite direction.
//
// Three observed states on iv7.1 (Stage 7 bench, 2026-07-17):
//   idle           slow rail-to-rail sawtooth (amp drift, seconds-long period)
//   one electrode  raw swings full-scale, spiky, oscillating (mains + body AC)
//   both touched   raw sits railed on one side, tight envelope near the rail
//
// The idle sawtooth produces |raw| ≈ 30 k for long stretches -- naive
// envelope-of-|raw| detection would false-trigger on it. Fix: a 1st-order
// high-pass filter kills the slow drift so only the AC content of a real
// touch feeds into the envelope. Idle -> HPF output ≈ 0, one-electrode -> big
// HPF output, both-touched -> small HPF output (railed = derivative near 0).
//
// Plotter output (tab-separated):
//   raw      -- the 16-bit signed differential (post-HPF, for plot clarity)
//   touch_p  -- +10000 while envelope > THRESH AND last onset was positive
//   touch_n  -- -10000 while envelope > THRESH AND last onset was negative
//
// HPF: 1st-order at ~2 Hz cutoff (fs = 250 Hz -> alpha = tau/(tau+dt) with
//      tau = 1/(2*pi*2) = 0.0796 s -> alpha = 0.0796/(0.0796+0.004) = 0.952).
#define QVAR_HPF_ALPHA          0.952f
#define QVAR_ONSET_THRESH_RAW   800.0f
#define QVAR_TOUCH_THRESH_RAW   400.0f
#define QVAR_ENV_DECAY          0.985f    // per-sample decay at 250 Hz => ~66 ms tau

// Both-touched detection: when both electrodes are held, HPF-envelope is
// LOW (railed = flat derivative) but the raw signal sits pegged at one of
// the amp rails. Track the slow raw mean; if it's near a rail AND the HPF
// envelope is quiet for a sustained window, treat as "both touched."
#define QVAR_RAIL_ABS_MIN       15000.0f  // |raw_mean| this large = near a rail
#define QVAR_QUIET_ENV_MAX      150.0f    // HPF envelope this small = flat
#define QVAR_RAW_MEAN_ALPHA     0.02f     // ~50-sample EMA of raw

// Output mode: 0 = human-readable monitor (default, ~1 Hz INFO logs);
//              1 = Arduino-Serial-Plotter (tab-separated numbers).
#define QVAR_PLOTTER_MODE   0

// Sessions run until the button is pressed -- no auto-timeout. The 15-min
// global uptime cap in task_shutdown_watcher_fn is the final safety net.

typedef enum {
    QVAR_NO_TOUCH = 0,
    QVAR_QVAR1_TOUCH,
    QVAR_QVAR2_TOUCH,
    QVAR_BOTH_TOUCH,
} qvar_touch_state_t;

static const char *qvar_state_name(qvar_touch_state_t s) {
    switch (s) {
    case QVAR_QVAR1_TOUCH: return "Qvar1";
    case QVAR_QVAR2_TOUCH: return "Qvar2";
    case QVAR_BOTH_TOUCH:  return "both";
    case QVAR_NO_TOUCH:
    default:               return "none";
    }
}

static void run_ecg(void) {
    if (qvar_local_enable() != ESP_OK) {
        ESP_LOGE(TAG, "[QVAR] enable failed -- skipping");
        return;
    }
#if QVAR_PLOTTER_MODE
    ESP_LOGI(TAG, "[QVAR] active (PLOTTER mode). plot: raw touch_p touch_n. exit: click.");
    esp_log_level_set(TAG, ESP_LOG_WARN);
#else
    ESP_LOGI(TAG, "[QVAR] active (MONITOR mode). recording -> SD until click.");
#endif

    s_rec_seq++;
    FILE *csv = csv_open("qvar", s_rec_seq, "time_ms,raw,hpf,state");
    uint32_t rows_written = 0;

    float    hp_prev_x   = 0.0f;
    float    hp_prev_y   = 0.0f;
    float    envelope    = 0.0f;
    float    raw_mean    = 0.0f;   // slow EMA of raw for "both-touched" detect
    int8_t   last_sign   = 0;
    bool     prev_touching = false;
    uint32_t last_led_tick  = 0;
    uint32_t last_status_ms = 0;
    qvar_touch_state_t state      = QVAR_NO_TOUCH;
    qvar_touch_state_t last_logged_state = QVAR_NO_TOUCH;

    uint32_t session_start = millis_u32();

    while (1) {
        if (button_poll() == 1) break;
        uint32_t now = millis_u32();

        int16_t raw = 0;
        if (qvar_reg_read_word(ECG_REG_OUT_AH_L, &raw) == ESP_OK) {
            float xin = (float)raw;
            float y   = QVAR_HPF_ALPHA * (hp_prev_y + xin - hp_prev_x);
            hp_prev_x = xin;
            hp_prev_y = y;

            float abs_y = fabsf(y);
            envelope *= QVAR_ENV_DECAY;
            if (abs_y > envelope) envelope = abs_y;

            // Slow mean of raw for rail-detection.
            raw_mean = raw_mean + QVAR_RAW_MEAN_ALPHA * (xin - raw_mean);

            if (abs_y > QVAR_ONSET_THRESH_RAW) {
                last_sign = (y > 0.0f) ? +1 : -1;
            }

            // State classification. Priority:
            //   (1) big AC envelope     -> one-electrode touch
            //   (2) railed raw + quiet  -> both-electrode touch
            //   (3) otherwise           -> no touch
            bool touching_one = envelope > QVAR_TOUCH_THRESH_RAW;
            bool railed = fabsf(raw_mean) > QVAR_RAIL_ABS_MIN;
            bool quiet  = envelope < QVAR_QUIET_ENV_MAX;
            if (touching_one) {
                state = (last_sign > 0) ? QVAR_QVAR1_TOUCH : QVAR_QVAR2_TOUCH;
            } else if (railed && quiet) {
                state = QVAR_BOTH_TOUCH;
            } else {
                state = QVAR_NO_TOUCH;
            }

            // Haptic on rising edge of any-touch (state transitions from
            // NO_TOUCH into any active state).
            bool touching_now = (state != QVAR_NO_TOUCH);
            if (touching_now && !prev_touching) {
                haptic_play_forced(DRV_MEDIUM_CLICK);
            }
            prev_touching = touching_now;

            if (csv) {
                fprintf(csv, "%lu,%d,%d,%d\n",
                        (unsigned long)(now - session_start),
                        (int)raw, (int)y, (int)state);
                rows_written++;
            }

#if QVAR_PLOTTER_MODE
            int touch_p_out = (state == QVAR_QVAR1_TOUCH) ?  10000 : 0;
            int touch_n_out = (state == QVAR_QVAR2_TOUCH) ? -10000 : 0;
            printf("%d\t%d\t%d\n", (int)y, touch_p_out, touch_n_out);
#else
            // Monitor mode: log on state change immediately, and a 1 Hz
            // heartbeat so the user knows the session is still running.
            if (state != last_logged_state) {
                ESP_LOGI(TAG, "[QVAR] state -> %s   raw_mean=%.0f  env=%.0f",
                         qvar_state_name(state), (double)raw_mean, (double)envelope);
                last_logged_state = state;
            }
            if ((now - last_status_ms) >= 1000) {
                last_status_ms = now;
                uint32_t elapsed = (now - session_start) / 1000;
                ESP_LOGI(TAG, "[QVAR] t=%us  state=%s  raw=%d  rows=%u",
                         (unsigned)elapsed,
                         qvar_state_name(state),
                         (int)raw,
                         (unsigned)rows_written);
            }
#endif
        }

        if ((now - last_led_tick) >= 50) {
            last_led_tick = now;
            rgb_qvar_alt_yellow_purple();
        }

        vTaskDelay(pdMS_TO_TICKS(4));   // ~250 Hz poll (driver ODR 240 Hz)
    }

    if (csv) {
        fflush(csv);
        fclose(csv);
        ESP_LOGI(TAG, "[QVAR] CSV closed (%u rows)", (unsigned)rows_written);
    }
#if QVAR_PLOTTER_MODE
    esp_log_level_set(TAG, ESP_LOG_INFO);
#endif
    qvar_local_disable();
    ESP_LOGI(TAG, "[QVAR] exit -> STANDBY");
}

// ═════════════════════════════════════════════════════════════════════════════
// FCM_TEMP -- aggregate thermal map of every onboard temp source
// ═════════════════════════════════════════════════════════════════════════════
//
// Reads:
//   - TMP117 skin temp (via broker_skin_data_t.skin_temp_c)
//   - BME688 die/air temp (via broker_env_data_t.temperature_c)
//   - LSM6DSV16X on-die temp (via broker_imu_data_t.temperature)
//   - MAX30101 die temp (direct register poke -- one-shot TEMP_EN trigger)
//   - ESP32-S3 SoC junction temp (temperature_sensor API)
//
// Print cadence: 1 Hz. Single-click during the mode exits back to STANDBY.

#include "driver/temperature_sensor.h"

// LSM6DSV16X OUT_TEMP register layout: signed 16-bit at 256 LSB/degC with
// a +25 degC zero offset. Same address (0x6B) as the IMU. Requires the
// accel to be at HP or Normal mode.
#define LSM_ADDR              0x6B
#define LSM_REG_OUT_TEMP_L    0x20

// MAX30101 die-temp: write 1 to TEMP_CONFIG (0x21), wait ~30 ms, read
// TINT (0x1F, signed int8) and TFRAC (0x20, bits 3:0). Temp = TINT + TFRAC * 0.0625.
// Note: the temperature ADC does NOT update while the chip is in shutdown
// (SHDN=1 in MODE_CONFIG). read_max_die_temp() below wakes the chip with all
// LED currents zeroed, triggers the conversion, then returns to shutdown.
#define MAX_ADDR              0x57
#define MAX_REG_MODE_CONFIG   0x09
#define MAX_REG_LED1_PA       0x0C   // Red
#define MAX_REG_LED2_PA       0x0D   // IR
#define MAX_REG_LED3_PA       0x0E   // Green
#define MAX_REG_TEMP_INT      0x1F
#define MAX_REG_TEMP_FRAC     0x20
#define MAX_REG_TEMP_CONFIG   0x21
#define MAX_MODE_HR           0x02   // MODE[2:0] = 010 -- lowest-power active mode
#define MAX_MODE_SHDN         0x80   // SHDN = 1

// Take g_i2c_mutex for these reads -- shares bus 0 with all other sensors.
static float read_lsm_die_temp(void) {
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -273.15f;
    uint8_t rx[2] = {0};
    uint8_t reg = LSM_REG_OUT_TEMP_L;
    esp_err_t r = i2c_master_write_read_device(I2C_NUM_0, LSM_ADDR,
                                               &reg, 1, rx, 2, pdMS_TO_TICKS(20));
    xSemaphoreGive(g_i2c_mutex);
    if (r != ESP_OK) return -273.15f;
    int16_t raw = (int16_t)((uint16_t)rx[1] << 8 | rx[0]);
    return 25.0f + (float)raw / 256.0f;
}

static float read_max_die_temp(void) {
    // Step 1: force all LED currents to 0, then wake from shutdown into HR
    // mode. LED_PA = 0 guarantees no LED fires during the brief wake window.
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return -273.15f;
    uint8_t buf_led1[] = { MAX_REG_LED1_PA, 0x00 };
    uint8_t buf_led2[] = { MAX_REG_LED2_PA, 0x00 };
    uint8_t buf_led3[] = { MAX_REG_LED3_PA, 0x00 };
    uint8_t buf_wake[] = { MAX_REG_MODE_CONFIG, MAX_MODE_HR };
    i2c_master_write_to_device(I2C_NUM_0, MAX_ADDR, buf_led1, 2, pdMS_TO_TICKS(20));
    i2c_master_write_to_device(I2C_NUM_0, MAX_ADDR, buf_led2, 2, pdMS_TO_TICKS(20));
    i2c_master_write_to_device(I2C_NUM_0, MAX_ADDR, buf_led3, 2, pdMS_TO_TICKS(20));
    i2c_master_write_to_device(I2C_NUM_0, MAX_ADDR, buf_wake, 2, pdMS_TO_TICKS(20));
    xSemaphoreGive(g_i2c_mutex);

    vTaskDelay(pdMS_TO_TICKS(5));   // analog wake settling

    // Step 2: trigger one-shot temp conversion.
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return -273.15f;
    uint8_t buf_trig[] = { MAX_REG_TEMP_CONFIG, 0x01 };
    i2c_master_write_to_device(I2C_NUM_0, MAX_ADDR, buf_trig, 2, pdMS_TO_TICKS(20));
    xSemaphoreGive(g_i2c_mutex);

    vTaskDelay(pdMS_TO_TICKS(35));   // TEMP_RDY typically clears in <30 ms

    // Step 3: read TINT/TFRAC and return the chip to shutdown.
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return -273.15f;
    int8_t  tint  = 0;
    uint8_t tfrac = 0;
    uint8_t reg;
    reg = MAX_REG_TEMP_INT;
    i2c_master_write_read_device(I2C_NUM_0, MAX_ADDR, &reg, 1, (uint8_t *)&tint, 1, pdMS_TO_TICKS(20));
    reg = MAX_REG_TEMP_FRAC;
    i2c_master_write_read_device(I2C_NUM_0, MAX_ADDR, &reg, 1, &tfrac, 1, pdMS_TO_TICKS(20));
    uint8_t buf_shdn[] = { MAX_REG_MODE_CONFIG, MAX_MODE_SHDN };
    i2c_master_write_to_device(I2C_NUM_0, MAX_ADDR, buf_shdn, 2, pdMS_TO_TICKS(20));
    xSemaphoreGive(g_i2c_mutex);

    return (float)tint + (float)(tfrac & 0x0F) * 0.0625f;
}

// Forward decls for shared telemetry helpers (definitions further down in
// the file, near run_battery_test_mode).
static void     esp_ts_ensure_init(void);
static float    esp_ts_read_c(void);
static void     vbat_adc_ensure_init(void);
static uint32_t vbat_adc_read_mv(void);

static void run_temp_session(void) {
    ESP_LOGI(TAG, "[TEMP] active. single-click to exit.");

    // Use the shared esp_ts helpers (Stage 11 refactor). No more per-function
    // static handle -- BLACKBOX + BATT_TEST + TEMP all share one install and
    // one enable, avoiding the "Already installed" ESP-IDF driver rejection.
    esp_ts_ensure_init();

    uint32_t last_print = 0;
    while (1) {
        // Exit on single click.
        if (button_poll() == 1) break;

        uint32_t now = millis_u32();
        rgb_temp_warm_cycle();

        if ((now - last_print) >= 1000) {
            last_print = now;

            broker_env_data_t  e;  broker_env_read(&e);
            broker_skin_data_t s;  broker_skin_read(&s);
            broker_imu_data_t  im; broker_imu_read(&im);

            float t_lsm  = read_lsm_die_temp();
            float t_max  = read_max_die_temp();
            float t_soc  = esp_ts_read_c();

            (void)im;   // broker_imu_data_t.temperature also available; direct read (t_lsm) preferred for freshness.
            printf("[TEMP] skin(TMP117)=%.2fC  air(BME688)=%.2fC  imu(LSM)=%.1fC  ppg(MAX)=%.1fC  soc(ESP32)=%.1fC\n",
                   s.skin_temp_c, e.temperature_c, t_lsm, t_max, t_soc);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "[TEMP] exit -> STANDBY");
}

// Called from the button-press dispatcher for FCM_TEMP.
static void run_temp_mode(void) {
    wake_sensors_for_mode(FCM_TEMP);
    vTaskDelay(pdMS_TO_TICKS(200));   // let sensor tasks publish first reads
    run_temp_session();
    park_all_modal_sensors();
}

// ═════════════════════════════════════════════════════════════════════════════
// FCM_BCG -- ballistocardiography on LSM6DSV16X accelerometer
// ═════════════════════════════════════════════════════════════════════════════
//
// Reads accel_z from the broker (LSM publishes at ~240 Hz). Filter chain:
//   HPF @ 1 Hz  (removes gravity + baseline drift)
//   LPF @ 15 Hz (removes motion artifact + high-freq noise)
// Both 1st-order IIR RC-cascade. Combined = 40 dB/decade band edges.
//
// Peak detector: adaptive threshold on the filtered signal envelope, with a
// 400 ms refractory window (150 BPM cap).
//
// Works best worn tightly on the wrist during rest / sleep -- BCG amplitude
// (~10-50 mg wrist-side at rest) is 20-35 dB above the LSM noise floor
// there. Any motion completely swamps the signal; that's a physics limit,
// not a chip limit.
//
// Plotter output (tab-separated): filtered   beat_marker
//   filtered      -- the bandpassed accel_z in milli-g
//   beat_marker   -- 100 for one sample after each detected peak, else 0

// 1st-order IIR coefficients precomputed for fs = 240 Hz.
// alpha_hp = tau / (tau + dt), tau = 1/(2*pi*fc). At fc=1Hz, tau=0.1592s
//   alpha_hp = 0.1592 / (0.1592 + 0.00417) ≈ 0.9745
// alpha_lp = dt / (dt + tau), tau = 1/(2*pi*fc). At fc=15Hz, tau=0.01061s
//   alpha_lp = 0.00417 / (0.00417 + 0.01061) ≈ 0.2822
#define BCG_HPF_ALPHA   0.9745f
#define BCG_LPF_ALPHA   0.2822f
#define BCG_REFRACTORY_MS   400
#define BCG_THRESH_FLOOR    0.003f   // 3 mg minimum peak (wrist-BCG amplitude
                                     // observed at rest, iv7.1 Stage 7: ~5-13 mg
                                     // with occasional excursions to ±20 mg)

// Output mode: 0 = human-readable monitor (default, ~1 Hz INFO logs);
//              1 = Arduino-Serial-Plotter (tab-separated numbers, per sample).
// Toggle at compile time. Monitor mode still writes SD; plotter mode adds
// the serial trace but SD recording remains identical.
#define BCG_PLOTTER_MODE     0

// Sessions run until the button is pressed -- no auto-timeout. The 15-min
// global uptime cap in task_shutdown_watcher_fn is the final safety net.

static void rgb_bcg_alt_yellow_red(void) {
    // 2 Hz yellow<->red alternation. Distinct from every other mode.
    uint32_t t = millis_u32();
    bool yellow = ((t / 250) % 2) == 0;
    if (yellow) ws2812_set_color(26, 26, 0);
    else        ws2812_set_color(26,  0, 0);
}

// BPM smoothing: median-of-N interval samples avoids spurious BPM jumps
// from any single skipped/added beat. N=5 samples => 5 heartbeats of latency
// on the reported number (a few seconds), which is fine for a resting HR.
#define BCG_BPM_MEDIAN_N   5
// If no beat detected for this long, the reported BPM is stale -- clear it
// so the log prints "---" instead of a leftover reading (mirrors sketch 12
// BCG behaviour). Also resets the interval ring so the first beat AFTER
// staleness anchors cleanly rather than treating the gap as an interval.
#define BCG_STALE_MS  2000

static void run_bcg(void) {
    // In plotter mode we silence our own INFO logs so the plotter stream
    // stays clean. Monitor mode keeps INFO on (1 Hz status line).
#if BCG_PLOTTER_MODE
    ESP_LOGI(TAG, "[BCG] active (PLOTTER mode). plot: filtered_mg  beat_marker. exit: click.");
    esp_log_level_set(TAG, ESP_LOG_WARN);
#else
    ESP_LOGI(TAG, "[BCG] active (MONITOR mode). recording -> SD until click.");
#endif

    // Open the recording CSV. If SD isn't ready we just skip the write and
    // still run the session -- the LED / beat detect / BPM still work.
    s_rec_seq++;
    FILE *csv = csv_open("bcg", s_rec_seq,
                         "time_ms,accel_z_g,filt_mg,beat");
    uint32_t rows_written = 0;

    // Filter + peak state.
    float hp_prev_x = 0.0f, hp_prev_y = 0.0f, lp_prev_y = 0.0f;
    float envelope  = 0.0f, prev_af   = 0.0f;
    uint32_t last_beat_ms  = 0;
    uint32_t last_led_tick = 0;
    uint32_t last_status_ms = 0;
    uint32_t total_beats   = 0;

    // Rolling ring of beat intervals for the median BPM estimate.
    uint32_t intervals[BCG_BPM_MEDIAN_N] = {0};
    uint8_t  int_idx    = 0;
    uint8_t  int_filled = 0;
    float    bpm_last   = 0.0f;

    uint32_t session_start = millis_u32();

    while (1) {
        if (button_poll() == 1) break;
        uint32_t now = millis_u32();

        broker_imu_data_t im; broker_imu_read(&im);
        float x = im.accel_z / 9.81f;

        float y_hp = BCG_HPF_ALPHA * (hp_prev_y + x - hp_prev_x);
        hp_prev_x = x; hp_prev_y = y_hp;
        float y_lp = BCG_LPF_ALPHA * y_hp + (1.0f - BCG_LPF_ALPHA) * lp_prev_y;
        lp_prev_y = y_lp;

        float filtered = y_lp;
        float filt_mg  = filtered * 1000.0f;

        float af = fabsf(filtered);
        envelope *= 0.998f;
        if (af > envelope) envelope = af;
        float thresh = envelope * 0.4f;
        if (thresh < BCG_THRESH_FLOOR) thresh = BCG_THRESH_FLOOR;

        int beat_marker = 0;
        if (prev_af > af && prev_af > thresh &&
            (now - last_beat_ms) > BCG_REFRACTORY_MS) {
            // On the very first beat of the session, last_beat_ms is still 0
            // -- the "interval" would be the whole boot-time-so-far. Skip
            // the BPM update, just anchor last_beat_ms.
            if (last_beat_ms != 0) {
                uint32_t interval = now - last_beat_ms;
                intervals[int_idx] = interval;
                int_idx = (int_idx + 1) % BCG_BPM_MEDIAN_N;
                if (int_filled < BCG_BPM_MEDIAN_N) int_filled++;

                uint32_t sorted[BCG_BPM_MEDIAN_N];
                for (int i = 0; i < int_filled; i++) sorted[i] = intervals[i];
                for (int i = 1; i < int_filled; i++) {
                    uint32_t k = sorted[i]; int j = i - 1;
                    while (j >= 0 && sorted[j] > k) { sorted[j+1] = sorted[j]; j--; }
                    sorted[j+1] = k;
                }
                uint32_t med = sorted[int_filled / 2];
                bpm_last = (med > 0) ? (60000.0f / (float)med) : 0.0f;
            }
            last_beat_ms = now;
            total_beats++;
            beat_marker = 100;
        }
        prev_af = af;

        // Stale-BPM reset: if we haven't seen a beat in BCG_STALE_MS, blank
        // the reported BPM and drop the interval ring so the next real beat
        // anchors cleanly. Matches sketch 12 behaviour.
        if (last_beat_ms != 0 && (now - last_beat_ms) > BCG_STALE_MS) {
            bpm_last   = 0.0f;
            int_filled = 0;
            int_idx    = 0;
        }

        if (csv) {
            fprintf(csv, "%lu,%.4f,%.2f,%d\n",
                    (unsigned long)(now - session_start),
                    im.accel_z / 9.81f, (double)filt_mg, beat_marker);
            rows_written++;
        }

#if BCG_PLOTTER_MODE
        printf("%d\t%d\n", (int)filt_mg, beat_marker);
#else
        // Monitor mode: one status line per second at INFO. BPM prints as
        // "---" when stale (no beat within the last BCG_STALE_MS).
        if ((now - last_status_ms) >= 1000) {
            last_status_ms = now;
            uint32_t elapsed = (now - session_start) / 1000;
            if (bpm_last > 0.0f) {
                ESP_LOGI(TAG, "[BCG] t=%us  BPM=%3.0f  beats=%u  rows=%u",
                         (unsigned)elapsed, (double)bpm_last,
                         (unsigned)total_beats, (unsigned)rows_written);
            } else {
                ESP_LOGI(TAG, "[BCG] t=%us  BPM=---  beats=%u  rows=%u",
                         (unsigned)elapsed,
                         (unsigned)total_beats, (unsigned)rows_written);
            }
        }
#endif

        if ((now - last_led_tick) >= 50) {
            last_led_tick = now;
            rgb_bcg_alt_yellow_red();
        }

        vTaskDelay(pdMS_TO_TICKS(4));   // ~250 Hz poll
    }

    if (csv) {
        fflush(csv);
        fclose(csv);
        ESP_LOGI(TAG, "[BCG] CSV closed (%u rows)", (unsigned)rows_written);
    }
#if BCG_PLOTTER_MODE
    esp_log_level_set(TAG, ESP_LOG_INFO);
#endif
    ESP_LOGI(TAG, "[BCG] exit -> STANDBY  (%u beats total)", (unsigned)total_beats);
}

// Called from the button-press dispatcher for FCM_BCG.
static void run_bcg_mode(void) {
    wake_sensors_for_mode(FCM_BCG);
    vTaskDelay(pdMS_TO_TICKS(200));   // let LSM task publish first reads
    run_bcg();
    park_all_modal_sensors();
}

// ── FCM_STEPS (LSM submenu): pedometer step counter session ─────────────────
// The pedometer runs always-on (armed in boot_hw_init after LSM init). This
// mode is a live-display + optional CSV recording session. Encoder rotation
// while in-session is unused (no brightness / no channel select); button
// single-click ends the session. Broker channel: broker_steps.
static void run_steps_mode(void) {
    ESP_LOGI(TAG, "STEPS session start");
    ensure_sd();

    // Reset the on-chip counter so the session starts at zero. Users see a
    // clean count for whatever activity they're recording (walk, stair test).
    (void)lsm6dsv16x_pedometer_reset();

    FILE *f = NULL;
    if (s_sd_ready) {
        char path[64];
        s_rec_seq++;
        snprintf(path, sizeof(path), "/sd/data/steps/s%04lu_r%04lu.csv",
                 (unsigned long)s_boot_seq, (unsigned long)s_rec_seq);
        try_mkdir("/sd/data/steps");
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "# kompic mk1 iv7.1 fw=%s hw=%s mode=steps boot=%lu seq=%lu\n",
                    KOMPIC_FW_VERSION, KOMPIC_HW_VERSION,
                    (unsigned long)s_boot_seq, (unsigned long)s_rec_seq);
            fprintf(f, "t_ms,step_count,delta\n");
            fflush(f);
        } else {
            ESP_LOGW(TAG, "STEPS: fopen(%s) failed -- live display only", path);
        }
    }

    // Live loop: 1 Hz status print + optional CSV row. Ends on button click.
    uint32_t start_ms = millis_u32();
    uint32_t last_row = 0;
    uint32_t prev_steps = 0;
    bool     have_prev = false;
    while (button_poll() != 1) {
        uint32_t now = millis_u32();
        if ((now - last_row) >= 1000) {
            last_row = now;
            broker_steps_data_t sd = {0};
            broker_steps_read(&sd);
            uint32_t delta = have_prev ? (sd.step_count - prev_steps) : 0;
            have_prev = true;
            prev_steps = sd.step_count;
            ESP_LOGI(TAG, "STEPS t=%lus count=%lu +%lu",
                     (unsigned long)((now - start_ms) / 1000),
                     (unsigned long)sd.step_count, (unsigned long)delta);
            if (f) {
                fprintf(f, "%lu,%lu,%lu\n",
                        (unsigned long)(now - start_ms),
                        (unsigned long)sd.step_count,
                        (unsigned long)delta);
                fflush(f);
            }
            // Pulse yellow (LSM) briefly per second as a heartbeat.
            rgb_set_max(FCM_LSM);
        } else {
            // Between prints -- solid cyan (STEPS color).
            rgb_set_max(FCM_STEPS);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (f) fclose(f);
    ESP_LOGI(TAG, "STEPS session end");
}

// ── FCM_MLC_COLLECT (LSM submenu): raw accel+gyro CSV for MLC training ───
// Two-phase state machine so the operator can pick a label without accidentally
// starting a recording, and no encoder input during recording can pollute the
// label column. Session per file, single label per file (cleanest for the
// MEMS Studio training pipeline).
//
// Phase 1: LABEL_PICK  (LED dim label color)
//   encoder rotate       -> cycle label 0..3, LED to new color, (label+1)
//                           quick DRV pulses so you can hear the label
//                           without looking
//   button short-click   -> CONFIRM label, transition to RECORDING.
//                           Opens the CSV file, plays a distinct 3-click
//                           cue, LED goes bright.
//   button hold >= 2 s   -> exit mode without recording (long buzz).
//
// Phase 2: RECORDING   (LED bright label color)
//   encoder rotate       -> IGNORED (locked to prevent accidental relabel)
//   button short-click   -> mark next sample row (mark=1). One strong DRV
//                           click. Subsequent rows go back to mark=0.
//   button hold >= 2 s   -> end recording, close CSV, exit mode. Long DRV
//                           buzz. Shutdown watcher press timer is RESET so
//                           continuing to hold gives a fresh 4 s to ship
//                           mode (Ivan spec: 2 s stop + 4 s ship = 6 s).
//
// While in RECORDING: uptime auto-shutoff cap is paused, resumes as a fresh
// 15-min countdown from recording end.
//
// LED palette (Ivan's spec):
//   0 = WHITE     (still / rest)
//   1 = YELLOW    (walking)
//   2 = RED       (running / brisk)
//   3 = PURPLE    (reserved / other)
//
// CSV columns: t_ms,label,mark,ax,ay,az,gx,gy,gz
// (accel m/s^2, gyro dps, from broker_imu_data_t)
#define MLC_LABEL_COUNT      4
#define MLC_HOLD_STOP_MS  2000
#define MLC_PICK_LED_DIVIDER  3   // dim = WS_MAX / N in the pick phase

static const uint8_t MLC_LABEL_COLORS[MLC_LABEL_COUNT][3] = {
    { WS_MAX,   WS_MAX,   WS_MAX },   // 0 -- white  (still)
    { WS_MAX,   WS_MAX,   0      },   // 1 -- yellow (walking)
    { WS_MAX,   0,        0      },   // 2 -- red    (running)
    { WS_MAX/2, 0,        WS_MAX },   // 3 -- purple (other)
};

// Play N quick DRV strong clicks with a short gap between so the operator
// can count them cleanly.
static void mlc_haptic_pulses(uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        haptic_play_forced(DRV_STRONG_CLICK);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}
// Set the LED for the current label. divider = 1 (bright, RECORDING) or
// MLC_PICK_LED_DIVIDER (dim, LABEL_PICK).
static void mlc_led_for_label(uint8_t label, uint8_t divider) {
    if (g_shutdown_hold_active) return;   // watcher owns LED during red hold
    ws2812_set_color(MLC_LABEL_COLORS[label][0] / divider,
                     MLC_LABEL_COLORS[label][1] / divider,
                     MLC_LABEL_COLORS[label][2] / divider);
}

static void run_mlc_collect_mode(void) {
    ESP_LOGI(TAG, "MLC_COLLECT: LABEL_PICK phase -- encoder cycles label, "
                  "short-click confirms + starts recording, hold 2 s exits");
    ensure_sd();
    wake_sensors_for_mode(FCM_MLC_COLLECT);
    // Small settle so the IMU task publishes at least one broker sample before
    // the first CSV row hits disk. Without this, the first ~10 rows can still
    // read zeros while the task spins up.
    vTaskDelay(pdMS_TO_TICKS(120));

    // Two-phase state machine.
    typedef enum { MLC_PICK, MLC_REC } mlc_phase_t;
    mlc_phase_t phase = MLC_PICK;

    uint8_t  label            = 0;
    FILE    *f                = NULL;
    uint32_t start_ms         = 0;
    uint32_t rows             = 0;
    uint32_t marks            = 0;
    bool     mark_next_row    = false;
    bool     exit_mode        = false;

    // Direct button GPIO polling (bypass button_poll(), which is single/double-
    // click biased and won't emit a "hold" event).
    bool     btn_low          = (gpio_get_level(PIN_BUTTON) == 0);
    uint32_t press_start_ms   = 0;
    bool     press_hold_fired = false;

    // Initial LED: DIM label-0 (white). User picks first, THEN clicks to start.
    mlc_led_for_label(label, MLC_PICK_LED_DIVIDER);
    ESP_LOGI(TAG, "MLC label -> 0 (pulses=1)  [pick]");

    while (!exit_mode) {
        uint32_t now = millis_u32();

        // -- Encoder: only meaningful in PICK phase (locked during REC) --
        int enc = encoder_delta();
        if (enc != 0) {
            if (phase == MLC_PICK) {
                int nl = ((int)label + enc) % MLC_LABEL_COUNT;
                if (nl < 0) nl += MLC_LABEL_COUNT;
                label = (uint8_t)nl;
                ESP_LOGI(TAG, "MLC label -> %u (pulses=%u)  [pick]",
                         (unsigned)label, (unsigned)(label + 1));
                mlc_haptic_pulses(label + 1);
                mlc_led_for_label(label, MLC_PICK_LED_DIVIDER);
            } else {
                // Locked -- soft tick so the operator knows it registered.
                haptic_play(DRV_MEDIUM_CLICK);
                ESP_LOGD(TAG, "MLC encoder ignored (locked while recording)");
            }
        }

        // -- Button: direct GPIO. Short-click / long-hold in both phases. --
        bool now_low = (gpio_get_level(PIN_BUTTON) == 0);
        if (now_low && !btn_low) {
            press_start_ms   = now;
            press_hold_fired = false;
            btn_low = true;
        } else if (!now_low && btn_low) {
            uint32_t held = now - press_start_ms;
            if (!press_hold_fired && held < MLC_HOLD_STOP_MS) {
                // SHORT CLICK
                if (phase == MLC_PICK) {
                    // Confirm label -> start recording.
                    if (s_sd_ready) {
                        char path[80];
                        s_rec_seq++;
                        snprintf(path, sizeof(path),
                                 "/sd/data/mlc_train/s%04lu_r%04lu_L%u.csv",
                                 (unsigned long)s_boot_seq,
                                 (unsigned long)s_rec_seq,
                                 (unsigned)label);
                        f = fopen(path, "w");
                        if (f) {
                            fprintf(f, "# kompic mk1 iv7.1 fw=%s hw=%s mode=mlc_collect "
                                       "boot=%lu seq=%lu label=%u\n",
                                    KOMPIC_FW_VERSION, KOMPIC_HW_VERSION,
                                    (unsigned long)s_boot_seq, (unsigned long)s_rec_seq,
                                    (unsigned)label);
                            fprintf(f, "# label fixed for this file (single class per file)\n");
                            fprintf(f, "# mark = 1 on the sample flagged by a short click\n");
                            fprintf(f, "# units: accel m/s^2 (broker scale), gyro dps\n");
                            fprintf(f, "t_ms,label,mark,ax,ay,az,gx,gy,gz\n");
                            fflush(f);
                            ESP_LOGI(TAG, "MLC_COLLECT: file open %s", path);
                        } else {
                            ESP_LOGW(TAG, "MLC_COLLECT: fopen(%s) failed", path);
                        }
                    }
                    phase = MLC_REC;
                    start_ms = now;
                    g_recording_active = true;
                    // Session-start cue: 3 quick clicks + LED brightens to full.
                    mlc_haptic_pulses(3);
                    mlc_led_for_label(label, 1);
                    ESP_LOGI(TAG, "MLC_COLLECT: RECORDING label=%u -- click=mark, "
                                  "hold 2 s to end", (unsigned)label);
                } else {
                    // MLC_REC: short click = mark
                    mark_next_row = true;
                    marks++;
                    haptic_play_forced(DRV_STRONG_CLICK);
                    ESP_LOGI(TAG, "MLC MARK #%u at t=%lu ms",
                             (unsigned)marks, (unsigned long)(now - start_ms));
                }
            }
            btn_low = false;
        } else if (now_low && btn_low) {
            // 2 s hold: exit mode (either from PICK without recording, or from
            // REC ending a recording).
            if (!press_hold_fired && (now - press_start_ms) >= MLC_HOLD_STOP_MS) {
                press_hold_fired = true;
                exit_mode        = true;
                haptic_play_forced(DRV_LONG_BUZZ);
                if (phase == MLC_REC) {
                    // Reset watcher press timer so ship-mode countdown starts
                    // fresh from here (2 s stop + 4 s ship = 6 s total).
                    g_watcher_press_reset_pending = true;
                    ESP_LOGI(TAG, "MLC_COLLECT: RECORDING end (%lu rows, %u marks, "
                                  "label=%u) -- watcher press reset",
                             (unsigned long)rows, (unsigned)marks, (unsigned)label);
                } else {
                    ESP_LOGI(TAG, "MLC_COLLECT: exit LABEL_PICK (no recording)");
                }
            }
        }

        // -- Sample only during RECORDING --
        if (phase == MLC_REC) {
            broker_imu_data_t bd = {0};
            broker_imu_read(&bd);
            uint32_t t = now - start_ms;
            uint8_t  mark_col = mark_next_row ? 1 : 0;
            if (f) {
                fprintf(f, "%lu,%u,%u,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f\n",
                        (unsigned long)t, (unsigned)label, (unsigned)mark_col,
                        bd.accel_x, bd.accel_y, bd.accel_z,
                        bd.gyro_x,  bd.gyro_y,  bd.gyro_z);
                rows++;
                // PATTERN B (see SD durability rule): fflush() + fsync() every
                // row. fflush pushes stdio buffer to VFS; fsync forces FatFs
                // f_sync which commits the directory entry (file size + mtime)
                // to disk. If we relied on periodic fflush + fclose-on-exit,
                // every row since the last flush would evaporate on a battery
                // drop or hang. See batt_0025 empty-file incident 2026-07-24.
                fflush(f);
                (void)fsync(fileno(f));
            }
            mark_next_row = false;
            mlc_led_for_label(label, 1);
        } else {
            mlc_led_for_label(label, MLC_PICK_LED_DIVIDER);
        }

        vTaskDelay(pdMS_TO_TICKS(LSM6DSV16X_POLL_MS));
    }

    // Wait for release so button-up doesn't re-trigger the parent state.
    while (gpio_get_level(PIN_BUTTON) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Clear recording flag (watcher re-arms 15-min cap from here).
    g_recording_active = false;

    if (f) fclose(f);
    park_all_modal_sensors();
    ESP_LOGI(TAG, "MLC_COLLECT: mode exit");
}

// ── FCM_TAP_DBG (LSM submenu): tap debug + host-side magnitude detector ─
// Purpose: prove the tap subsystem end-to-end. Shows on the log:
//   * Live orientation (pitch/roll from accel) + in-zone status
//   * Live magnitude, HP magnitude, peak-hold per axis
//   * HOST-SIDE tap detector (magnitude-based, orientation-invariant per
//     Ivan's ask): fires on |mag - baseline| spikes above threshold with
//     refractory
//   * CHIP-SIDE tap events polled from lsm6dsv16x_tap_z_{single,double}_count()
//     -- lets us see if the chip's per-axis tap subsystem is firing OR silent
//
// LED: RED out of zone, MAGENTA in-zone-idle, WHITE flash on tap.
//
// Controls: single click = exit (matches BCG/MOTION pattern).
static void run_tap_dbg_mode(void) {
    // Wake the IMU explicitly -- prior modes (BCG/ECG) leave it parked.
    // Without this, broker_imu_read returns zeros and the whole diagnostic
    // is useless (Ivan bench log 2026-07-23).
    wake_sensors_for_mode(FCM_BCG);   // BCG wakes broker_imu; TAP_DBG needs same
    vTaskDelay(pdMS_TO_TICKS(200));   // let IMU task publish first sample

    ESP_LOGI(TAG, "TAP_DBG start -- magnitude-based tap detector + chip-tap echo. "
                  "single-click to exit.");

    // Detector parameters
    const float    mag_alpha    = 0.05f;
    const float    thresh_ms2   = 3.0f;
    const uint32_t refr_ms      = 200;
    const float    pitch_min    = -80.0f;
    const float    pitch_max    =  10.0f;
    const float    roll_lim     =  45.0f;

    float    baseline_mag = 9.81f;
    float    prev_hp      = 0.0f;
    float    peak_ax = 0.0f, peak_ay = 0.0f, peak_az = 0.0f;
    uint32_t last_host_beat_ms = 0;
    uint32_t taps_host          = 0;
    uint32_t last_chip_single   = lsm6dsv16x_tap_z_single_count();
    uint32_t last_chip_double   = lsm6dsv16x_tap_z_double_count();
    uint32_t taps_chip_single   = 0;
    uint32_t taps_chip_double   = 0;
    uint32_t tap_led_flash_ms   = 0;
    uint32_t last_heartbeat_ms  = 0;

    while (1) {
        // Exit on single click -- standard pattern (BCG, TEMP, COMPASS all
        // do this via button_poll() == 1). No hold-2s complexity.
        if (button_poll() == 1) break;

        uint32_t now = millis_u32();

        broker_imu_data_t im;
        broker_imu_read(&im);

        // -- Orientation from accel (quasi-static) --
        float pitch = atan2f(-im.accel_x,
                             sqrtf(im.accel_y * im.accel_y + im.accel_z * im.accel_z))
                      * (180.0f / (float)M_PI);
        float roll  = atan2f(im.accel_y, im.accel_z) * (180.0f / (float)M_PI);
        bool in_zone = (pitch >= pitch_min && pitch <= pitch_max &&
                        roll  >= -roll_lim && roll <= roll_lim);

        // -- Magnitude-based host tap detector --
        float mag = sqrtf(im.accel_x * im.accel_x +
                          im.accel_y * im.accel_y +
                          im.accel_z * im.accel_z);
        baseline_mag = baseline_mag * (1.0f - mag_alpha) + mag * mag_alpha;
        float hp = mag - baseline_mag;

        // -- Peak-hold per axis --
        float absx = fabsf(im.accel_x), absy = fabsf(im.accel_y), absz = fabsf(im.accel_z);
        if (absx > peak_ax) peak_ax = absx; else peak_ax *= 0.985f;
        if (absy > peak_ay) peak_ay = absy; else peak_ay *= 0.985f;
        if (absz > peak_az) peak_az = absz; else peak_az *= 0.985f;

        // -- Fire tap on rising edge above threshold (gated by in-zone) --
        if (in_zone && hp > thresh_ms2 && prev_hp <= thresh_ms2 &&
            (now - last_host_beat_ms) > refr_ms) {
            taps_host++;
            last_host_beat_ms = now;
            tap_led_flash_ms  = now + 150;
            ESP_LOGI(TAG, "[TAP-HOST] #%u  delta=%.2f m/s^2  peaks x=%.2f y=%.2f z=%.2f  "
                          "pitch=%+.0f roll=%+.0f",
                     (unsigned)taps_host, hp, peak_ax, peak_ay, peak_az, pitch, roll);
        }
        prev_hp = hp;

        // -- Poll chip tap counters (dedup on strict increase) --
        uint32_t cs = lsm6dsv16x_tap_z_single_count();
        uint32_t cd = lsm6dsv16x_tap_z_double_count();
        if (cs != last_chip_single) {
            taps_chip_single += (cs - last_chip_single);
            last_chip_single  = cs;
            tap_led_flash_ms  = now + 150;
            ESP_LOGI(TAG, "[TAP-CHIP] SINGLE  cnt=%u", (unsigned)taps_chip_single);
        }
        if (cd != last_chip_double) {
            taps_chip_double += (cd - last_chip_double);
            last_chip_double  = cd;
            tap_led_flash_ms  = now + 150;
            ESP_LOGI(TAG, "[TAP-CHIP] DOUBLE  cnt=%u", (unsigned)taps_chip_double);
        }

        // -- 2 Hz heartbeat --
        if ((now - last_heartbeat_ms) >= 500) {
            last_heartbeat_ms = now;
            ESP_LOGI(TAG, "[TAP-HB ] %s  pitch=%+6.1f roll=%+6.1f  "
                          "mag=%.2f base=%.2f hp=%+.2f  peaks x=%.2f y=%.2f z=%.2f  "
                          "host=%u  chip(s/d)=%u/%u",
                     in_zone ? "IN " : "OUT",
                     pitch, roll,
                     mag, baseline_mag, hp,
                     peak_ax, peak_ay, peak_az,
                     (unsigned)taps_host,
                     (unsigned)taps_chip_single, (unsigned)taps_chip_double);
        }

        // -- LED --
        if (!g_shutdown_hold_active) {
            if (now < tap_led_flash_ms) {
                ws2812_set_color(WS_MAX, WS_MAX, WS_MAX);
            } else if (!in_zone) {
                ws2812_set_color(WS_MAX, 0, 0);
            } else {
                ws2812_set_color(WS_MAX, 0, WS_MAX);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    park_all_modal_sensors();
    ESP_LOGI(TAG, "[TAP-DBG] exit -- host=%u chip s/d=%u/%u",
             (unsigned)taps_host, (unsigned)taps_chip_single, (unsigned)taps_chip_double);
}

// FCM_ECG single click: enable QVAR block, stream + detect beats, park on exit.
static void run_ecg_session(void) {
    // QVAR shares the LSM6DSV16X with the IMU driver. To avoid CTRL1/CTRL7
    // being rewritten from under us while ECG is running, force-park the
    // IMU for the duration of the session (100 ms gives any in-flight
    // read time to finish and release the mutex). qvar_local_enable()
    // then owns accel/QVAR config for the ECG window.
    broker_imu_set_enabled(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    run_ecg();
    // Leave IMU parked -- matches boot policy. User re-enters MOTION
    // mode to wake it if needed.
}

// ── Recording orchestrator (blocking) ────────────────────────────────────────
static void voice_annot_lead_in(void) {
    for (int i = 0; i < BEEP_COUNT; i++) {
        ws2812_set_color(WS_MAX, 0, 0);
        haptic_play_forced(DRV_STRONG_CLICK);
        vTaskDelay(pdMS_TO_TICKS(BEEP_ON_MS));
        rgb_off();
        vTaskDelay(pdMS_TO_TICKS(BEEP_OFF_MS));
    }
}
static void run_recording_for_current_mode(void) {
    s_recording_early_end = false;
    s_rec_seq++;

    // Wake only the sensors we're about to record. Give them a moment to
    // settle before the CSV loop starts (MAX30101 exits sleep, LEDs light).
    wake_sensors_for_mode(s_mode);
    vTaskDelay(pdMS_TO_TICKS(200));

    bool needs_annot = (s_mode == FCM_ENV || s_mode == FCM_MOTION || s_mode == FCM_SKIN);
    if (needs_annot) {
        voice_annot_lead_in();
        char path[96];
        snprintf(path, sizeof(path), "/sd/data/mic/s%04lu_r%04lu_annot.wav",
                 (unsigned long)s_boot_seq, (unsigned long)s_rec_seq);
        wav_record_to(path, VOICE_ANNOT_MS);
        wav_write_meta_sidecar(path, "annot");
    }

    switch (s_mode) {
    case FCM_MIC: {
        char path[96];
        snprintf(path, sizeof(path), "/sd/data/mic/s%04lu_r%04lu.wav",
                 (unsigned long)s_boot_seq, (unsigned long)s_rec_seq);
        rgb_set_max(FCM_MIC);
        wav_record_to(path, 0);   // 0 = record until button click
        wav_write_meta_sidecar(path, "mic");
        break;
    }
    case FCM_ENV:
        s_csv_file = csv_open("env",  s_rec_seq,
                              "millis,temp_c,hum_pct,press_hpa,gas_r_ohm,lux");
        break;
    case FCM_MOTION:
        s_csv_file = csv_open("mot",  s_rec_seq,
                              "millis,ax,ay,az,gx,gy,gz,mx_ut,my_ut,mz_ut");
        break;
    case FCM_SKIN:
        s_csv_file = csv_open("skin", s_rec_seq,
                              "millis,skin_c,bpm,finger,spo2");
        break;
    default: break;
    }

    if (s_csv_file) {
        uint32_t start = millis_u32();
        const uint32_t row_period_ms = (s_mode == FCM_ENV) ? 500 : 100;
        // Record indefinitely -- exit only on button click. The 15-min
        // global uptime cap in task_shutdown_watcher_fn is the final safety net.
        while (!s_recording_early_end) {
            rgb_pulse(s_mode, start, PULSE_RECORD_MS);
            switch (s_mode) {
            case FCM_ENV:    csv_row_env(s_csv_file);    break;
            case FCM_MOTION: csv_row_motion(s_csv_file); break;
            case FCM_SKIN:   csv_row_skin(s_csv_file);   break;
            default: break;
            }
#if DEBUG_PRINT_SENSORS
            debug_dump_mode(s_mode);   // once per second, mode-specific
#endif
            if (button_poll() == 1) s_recording_early_end = true;
            vTaskDelay(pdMS_TO_TICKS(row_period_ms));
        }
        fflush(s_csv_file);
        fclose(s_csv_file);
        s_csv_file = NULL;
        ESP_LOGI(TAG, "CSV closed (click-exit)");
    }

    // End-of-recording DRV signal + park modal sensors again.
    haptic_play_forced(DRV_LONG_BUZZ);
    park_all_modal_sensors();
}

// ── Alarm firing (simplified vs sketch: 15s buzz + click pattern) ───────────
static void run_alarm_firing(void) {
    uint32_t start = millis_u32();
    uint32_t last_click = 0;
    bool     buzz_fired = false;
    ESP_LOGI(TAG, "alarm firing (%ums)", (unsigned)ALARM_DURATION_MS);
    while ((millis_u32() - start) < ALARM_DURATION_MS) {
        rgb_pulse(FCM_ALARM, start, PULSE_RECORD_MS);
        uint32_t now = millis_u32();
        if ((now - last_click) >= 900) {
            last_click = now;
            ESP_LOGI(TAG, "alarm click -> DRV effect %d", DRV_STRONG_CLICK);
            haptic_play_forced(DRV_STRONG_CLICK);
        }
        if (!buzz_fired && (now - start) > ALARM_DURATION_MS * 2 / 3) {
            haptic_play_forced(DRV_LONG_BUZZ);
            buzz_fired = true;
        }
        if (button_poll() == 1) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    rgb_off();
}

// ═════════════════════════════════════════════════════════════════════════════
// RTC serial CLI -- port of the sketch's SET_TIME / GET_TIME handler.
// Reads lines from stdin (USB-Serial-JTAG); commands are case-insensitive:
//   SET_TIME 2026-07-11T14:30:00     -- write UTC into PCF85063A
//   GET_TIME                          -- read back and print ISO
// Line terminator: '\n' (LF) or '\r'. Silently ignores unknown commands.
// ═════════════════════════════════════════════════════════════════════════════

static int try_parse_iso(const char *s, int *yr, int *mo, int *da,
                         int *hr, int *mi, int *se) {
    // Accept "YYYY-MM-DDTHH:MM:SS" or "YYYY-MM-DD HH:MM:SS".
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", yr, mo, da, hr, mi, se) == 6) return 1;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", yr, mo, da, hr, mi, se) == 6) return 1;
    return 0;
}

// Simple case-insensitive prefix compare.
static int startswith_ci(const char *s, const char *pfx) {
    while (*pfx) {
        char a = *s++, b = *pfx++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static void rtc_cli_print_now(void) {
    broker_rtc_data_t r; broker_rtc_read(&r);
    if (r.valid) {
        printf("[RTC] %04u-%02u-%02uT%02u:%02u:%02u UTC\n",
               (unsigned)r.year, (unsigned)r.month, (unsigned)r.day,
               (unsigned)r.hour, (unsigned)r.minute,(unsigned)r.second);
    } else {
        printf("[RTC] oscstop -- run SET_TIME <YYYY-MM-DDTHH:MM:SS> to seed\n");
    }
}

// UTC (y,m,d,h,mi,s) -> Unix seconds. Valid for 1970..2099.
static uint64_t civil_to_unix(int yr, int mo, int da, int hr, int mi, int se) {
    static const int dpm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint64_t days = 0;
    for (int y = 1970; y < yr; y++) {
        days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
    }
    for (int m = 0; m < mo - 1; m++) {
        int dm = dpm[m];
        if (m == 1 && ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0)) dm = 29;
        days += dm;
    }
    days += (da - 1);
    return days * 86400ULL + (uint64_t)hr * 3600 + (uint64_t)mi * 60 + (uint64_t)se;
}

static void rtc_cli_dump_nvs(void) {
    // Full dump of everything in NVS -- mirrors nvs_cfg_boot_print but
    // triggered on demand via NVS_PRINT (no args). Includes RTC command
    // state, PCF RAM_byte redundancy channel, and every sys knob.
    nvs_cfg_rtc_t r;
    if (nvs_cfg_rtc_load(&r) != ESP_OK || !r.valid) {
        printf("[NVS] cfg_rtc empty (SET_TIME has not been called since NVS erase)\n");
    } else {
        printf("[NVS] cfg_rtc.wall_ts       = %llu\n", (unsigned long long)r.wall_ts);
        printf("[NVS] cfg_rtc.wr_ms         = %llu\n", (unsigned long long)r.wr_ms);
        printf("[NVS] cfg_rtc.boot_seq      = %lu\n",  (unsigned long)r.boot_seq);
        printf("[NVS] cfg_rtc.last_set_time = \"%s\"\n", r.last_set_time);
    }
    uint8_t ram = 0;
    if (pcf85063_ram_byte_read(I2C_NUM_0, &ram) == ESP_OK) {
        printf("[PCF] RAM_byte (0x03)       = 0x%02X\n", ram);
    } else {
        printf("[PCF] RAM_byte read failed\n");
    }
    // cfg_sys knobs.
    printf("[SYS] print_on_boot         = %d\n",
           nvs_cfg_sys_get_print_on_boot() ? 1 : 0);
    printf("[SYS] batt_test             = %d  (reboot to apply; VBUS-in keeps serial alive)\n",
           nvs_cfg_sys_get_batt_test() ? 1 : 0);
    printf("[SYS] blackbox              = %d  cadence=%u s\n",
           nvs_cfg_sys_get_blackbox() ? 1 : 0,
           (unsigned)nvs_cfg_sys_get_bb_cadence_s());
    char last_fw[NVS_CFG_FW_STR_MAX] = {0};
    (void)nvs_cfg_sys_get_last_fw(last_fw, sizeof(last_fw));
    printf("[SYS] last_fw               = \"%s\"  (current=%s)\n",
           last_fw[0] ? last_fw : "(none)", KOMPIC_FW_VERSION);
}

// Forward decl: boot_pm_dump_locks lives in components/boot_logic/boot_pm.c.
// Not adding boot_logic to field_capture's REQUIRES (would form a cycle);
// use an extern here.
extern void boot_pm_dump_locks(void);

// Printed at boot AND on the HELP command.
static void rtc_cli_print_help(void) {
    printf("  Commands:\n");
    printf("    HELP                             this list\n");
    printf("    STATUS                           one-shot state dump (uptime, sensors, batt, heap)\n");
    printf("    GET_TIME [-v]                    read RTC now (-v also dumps NVS + RAM_byte)\n");
    printf("    SET_TIME YYYY-MM-DDTHH:MM:SS     write UTC + persist to NVS + PCF RAM_byte\n");
    printf("    RTC_DUMP                         hex dump of all 18 PCF85063A registers\n");
    printf("    NVS_PRINT [ON|OFF]               toggle boot-time NVS printout (no arg = dump)\n");
    printf("    BATT_TEST [ON|OFF]               enter battery-test mode on next boot (no arg = state)\n");
    printf("    BLACKBOX [ON|OFF]                background telemetry logger (reboot to start/stop)\n");
    printf("    BLACKBOX_CADENCE <s>             sample cadence in seconds (default 10, range 1..3600)\n");
    printf("    WHOAMI                           I2C sensor identification + hw_alive status\n");
    printf("    PM_DUMP                          dump PM lock inventory now\n");
    printf("    RGB <r> <g> <b> | RGB AUTO       bench-poke WS2812 (0..255) / release override\n");
    printf("    FS_LS [/sd/path]                 list SD directory\n");
    printf("    FS_CAT </sd/path>                dump a file to console\n");
    printf("    SHIPMODE                         drop BATFET now (escape when button stuck)\n");
    printf("    REBOOT                           esp_restart() -- clean SW reset\n");
}

// One-shot dump of everything a human might want at a glance.
static void rtc_cli_dump_status(void) {
    uint32_t up_s = millis_u32() / 1000U;
    uint32_t h = up_s / 3600; uint32_t m = (up_s % 3600) / 60; uint32_t s = up_s % 60;

    rtc_cpu_freq_config_t cfg;
    rtc_clk_cpu_freq_get_config(&cfg);
    uint32_t heap_kb = esp_get_free_heap_size() / 1024;
    uint32_t min_kb  = esp_get_minimum_free_heap_size() / 1024;

    broker_battery_data_t bat; broker_battery_read(&bat);

    esp_ts_ensure_init();
    float t_soc = esp_ts_read_c();
    vbat_adc_ensure_init();
    uint32_t v_adc = vbat_adc_read_mv();

    uint32_t sens_on = 0;
    if (broker_imu_get_enabled())     sens_on |= (1 << 0);
    if (broker_mag_get_enabled())     sens_on |= (1 << 1);
    if (broker_env_get_enabled())     sens_on |= (1 << 2);
    if (broker_light_get_enabled())   sens_on |= (1 << 3);
    if (broker_hr_get_enabled())      sens_on |= (1 << 4);
    if (broker_skin_get_enabled())    sens_on |= (1 << 5);
    if (broker_battery_get_enabled()) sens_on |= (1 << 6);
    if (broker_rtc_get_enabled())     sens_on |= (1 << 7);

    printf("[STATUS]\n");
    printf("  fw           = %s   hw = %s\n", KOMPIC_FW_VERSION, KOMPIC_HW_VERSION);
    printf("  uptime       = %luh %02lum %02lus   boot_seq = %lu\n",
           (unsigned long)h, (unsigned long)m, (unsigned long)s,
           (unsigned long)s_boot_seq);
    printf("  fcm_mode     = %s   state = %s\n",
           MODE_INFO[s_mode].name,
           (s_state == 0) ? "STANDBY" :
           (s_state == 1) ? "FL_ON"   :
           (s_state == 2) ? "RECORDING" : "ALARM");
    printf("  sensors_on   = 0x%02lX  (IMU|MAG|ENV|LIGHT|HR|SKIN|BAT|RTC, LSB=IMU)\n",
           (unsigned long)sens_on);
    printf("  cpu_mhz      = %lu   heap = %lu KB (min %lu KB)\n",
           (unsigned long)cfg.freq_mhz, (unsigned long)heap_kb,
           (unsigned long)min_kb);
    printf("  vbat_adc     = %lu mV   soc_temp = %.1f C\n",
           (unsigned long)v_adc, t_soc);
    printf("  bq_v (fake!) = %.3f V   pct = %u  charging = %d  pg = %d  fault = 0x%02X\n",
           bat.voltage, (unsigned)bat.percentage,
           bat.charging ? 1 : 0, bat.power_good ? 1 : 0, bat.fault);
    printf("  NVS: print_boot=%d  batt_test=%d  blackbox=%d  bb_cadence=%u s\n",
           nvs_cfg_sys_get_print_on_boot() ? 1 : 0,
           nvs_cfg_sys_get_batt_test()     ? 1 : 0,
           nvs_cfg_sys_get_blackbox()      ? 1 : 0,
           (unsigned)nvs_cfg_sys_get_bb_cadence_s());
}

// Read one byte from an I2C register on either bus, protected by
// g_i2c_mutex. Returns 0xFF on failure. Used by WHOAMI.
static uint8_t whoami_read_reg8(i2c_port_t port, uint8_t addr, uint8_t reg,
                                 esp_err_t *out_err) {
    uint8_t val = 0xFF;
    esp_err_t r = ESP_FAIL;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg, true);
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, &val, I2C_MASTER_NACK);
        i2c_master_stop(cmd);
        r = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        xSemaphoreGive(g_i2c_mutex);
    }
    if (out_err) *out_err = r;
    return val;
}

// Read a 16-bit big-endian register (needed for TMP117's DEVICE_ID).
static uint16_t whoami_read_reg16be(i2c_port_t port, uint8_t addr, uint8_t reg,
                                     esp_err_t *out_err) {
    uint8_t hi = 0xFF, lo = 0xFF;
    esp_err_t r = ESP_FAIL;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg, true);
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, &hi, I2C_MASTER_ACK);
        i2c_master_read_byte(cmd, &lo, I2C_MASTER_NACK);
        i2c_master_stop(cmd);
        r = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        xSemaphoreGive(g_i2c_mutex);
    }
    if (out_err) *out_err = r;
    return ((uint16_t)hi << 8) | lo;
}

// Enhanced WHOAMI: bus, address, WHO_AM_I readback vs expected, plus SD/mic
// operational status. Attempts a live I2C probe of each chip's ID register.
static void rtc_cli_dump_whoami(void) {
    printf("[WHOAMI]\n");
    printf("  bus  addr  chip         who@reg  read     expect  hw_alive  status\n");

    // Table row helper for 8-bit reads.
    #define ROW8(bus, addr, name, who_reg, expect, hw_alive, note)               \
    do {                                                                          \
        esp_err_t err = ESP_OK;                                                   \
        uint8_t got = whoami_read_reg8((bus), (addr), (who_reg), &err);           \
        bool match = (err == ESP_OK && got == (expect));                          \
        printf("  I2C%d 0x%02X  %-11s  0x%02X     0x%02X     0x%02X    %d         %s%s%s\n", \
               (bus) == I2C_NUM_0 ? 0 : 1, (addr), (name),                        \
               (who_reg), got, (expect), (hw_alive) ? 1 : 0,                      \
               (err != ESP_OK) ? "I2C_ERR" :                                      \
               (match ? "OK" : "MISMATCH"),                                       \
               (note[0]) ? " (" : "", (note[0]) ? (note) : (""));                 \
        if (note[0]) printf(")\n"); /* close paren if note given */               \
    } while (0)

    // PCF85063A -- no WHO_AM_I register. Report via broker_rtc_hw_alive only.
    printf("  I2C0 0x51  %-11s  -        -        -       %d         %s\n",
           "PCF85063A", broker_rtc_hw_alive() ? 1 : 0,
           broker_rtc_hw_alive() ? "OK (no chip-ID reg, hw_alive from I2C probe)" : "FAILED");

    // BME688 -- chip_id at 0xD0, expected 0x61.
    {
        esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_0, 0x76, 0xD0, &e);
        printf("  I2C0 0x76  %-11s  0xD0     0x%02X     0x61    %d         %s\n",
               "BME688", got, broker_env_hw_alive() ? 1 : 0,
               (e != ESP_OK) ? "I2C_ERR" : (got == 0x61 ? "OK" : "MISMATCH"));
    }
    // LSM6DSV16X -- WHO_AM_I at 0x0F, expected 0x70.
    {
        esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_0, 0x6B, 0x0F, &e);
        printf("  I2C0 0x6B  %-11s  0x0F     0x%02X     0x70    %d         %s\n",
               "LSM6DSV16X", got, broker_imu_hw_alive() ? 1 : 0,
               (e != ESP_OK) ? "I2C_ERR" : (got == 0x70 ? "OK" : "MISMATCH"));
    }
    // LIS3MDL -- WHO_AM_I at 0x0F, expected 0x3D.
    {
        esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_0, 0x1C, 0x0F, &e);
        printf("  I2C0 0x1C  %-11s  0x0F     0x%02X     0x3D    %d         %s\n",
               "LIS3MDL", got, broker_mag_hw_alive() ? 1 : 0,
               (e != ESP_OK) ? "I2C_ERR" : (got == 0x3D ? "OK" : "MISMATCH"));
    }
    // MAX30101 -- PART_ID at 0xFF, expected 0x15.
    {
        esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_0, 0x57, 0xFF, &e);
        printf("  I2C0 0x57  %-11s  0xFF     0x%02X     0x15    %d         %s\n",
               "MAX30101", got, broker_hr_hw_alive() ? 1 : 0,
               (e != ESP_OK) ? "I2C_ERR" : (got == 0x15 ? "OK" : "MISMATCH"));
    }
    // TMP117 -- DEVICE_ID at 0x0F (16-bit), low 12 bits = 0x117.
    {
        esp_err_t e; uint16_t got = whoami_read_reg16be(I2C_NUM_0, 0x48, 0x0F, &e);
        uint16_t low12 = got & 0x0FFF;
        printf("  I2C0 0x48  %-11s  0x0F     0x%04X   0x0117  %d         %s\n",
               "TMP117", got, broker_skin_hw_alive() ? 1 : 0,
               (e != ESP_OK) ? "I2C_ERR" : (low12 == 0x117 ? "OK" : "MISMATCH"));
    }
    // VEML6030 -- no WHO_AM_I. Broker only.
    printf("  I2C0 0x10  %-11s  -        -        -       %d         %s\n",
           "VEML6030", broker_light_hw_alive() ? 1 : 0,
           broker_light_hw_alive() ? "OK (no chip-ID reg)" : "FAILED");
    // BQ25619 -- REG_PART at 0x0A, expected 0x80 (per boot log).
    {
        esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_1, 0x6A, 0x0A, &e);
        printf("  I2C1 0x6A  %-11s  0x0A     0x%02X     0x80    %d         %s\n",
               "BQ25619", got, broker_battery_hw_alive() ? 1 : 0,
               (e != ESP_OK) ? "I2C_ERR" : (got == 0x80 ? "OK" : "MISMATCH"));
    }
    // DRV2605 -- STATUS at 0x00. Only DEV_ID bits [7:5] are stable; bits
    // [4:0] carry diagnostic state that varies over the chip's lifecycle.
    // Compare only the top nibble mask (0xE0 = 0b111_00000) -- that's what
    // uniquely identifies our clone / TI silicon variant.
    {
        esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_1, 0x5A, 0x00, &e);
        uint8_t dev_id = got & 0xE0;
        printf("  I2C1 0x5A  %-11s  0x00     0x%02X     0xE0    %d         %s\n",
               "DRV2605", got, broker_haptic_get_status() != SENSOR_OFFLINE,
               (e != ESP_OK) ? "I2C_ERR" :
               (dev_id == 0xE0 ? "OK (DEV_ID match; low bits = diagnostic)" : "MISMATCH"));
    }

    #undef ROW8

    // Non-I2C peripherals.
    printf("  --\n");
    if (sdcard_is_mounted()) {
        printf("  SD card               mounted=1  cap=%ld MiB  free=%ld MiB\n",
               (long)sdcard_get_capacity_mib(), (long)sdcard_get_free_mib());
    } else {
        printf("  SD card               mounted=0\n");
    }
    printf("  Mic (PDM)             running=%d\n", mic_pdm_is_running() ? 1 : 0);
}

// Dump 18 PCF85063A registers (0x00..0x11 covers control, RAM_byte, time,
// alarm, timer, and CLKOUT).
static void rtc_cli_dump_pcf_regs(void) {
    uint8_t regs[18] = {0};
    esp_err_t r = pcf85063_read_regs_raw(I2C_NUM_0, 0x00, regs, sizeof(regs));
    if (r != ESP_OK) {
        printf("[RTC_DUMP] i2c read failed: %s\n", esp_err_to_name(r));
        return;
    }
    printf("[RTC_DUMP] PCF85063A registers 0x00..0x11:\n");
    static const char *names[18] = {
        "Control_1",   "Control_2",   "Offset",     "RAM_byte",
        "Seconds",     "Minutes",     "Hours",      "Days",
        "Weekdays",    "Months",      "Years",
        "Sec_alarm",   "Min_alarm",   "Hour_alarm", "Day_alarm", "Wday_alarm",
        "Timer_val",   "Timer_mode",
    };
    for (int i = 0; i < 18; i++) {
        printf("  0x%02X  %-11s = 0x%02X  (%3u)\n", i, names[i], regs[i], regs[i]);
    }
    if (regs[4] & 0x80) {
        printf("  ⚠ Seconds bit 7 (OS) = 1 -- oscillator was stopped, time INVALID\n");
    }
}

// FS_LS -- list SD directory.
static void rtc_cli_fs_ls(const char *path) {
    if (!path || !*path) path = "/sd";
    DIR *d = opendir(path);
    if (!d) {
        printf("[FS_LS] opendir(%s) failed: %s\n", path, strerror(errno));
        return;
    }
    printf("[FS_LS] %s\n", path);
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d))) {
        char full[400];   // path (up to 128) + '/' + d_name (up to NAME_MAX=260) + NUL
        (void)snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            char kind = S_ISDIR(st.st_mode) ? 'D' : 'F';
            printf("  [%c]  %-30s  %10lld B\n",
                   kind, ent->d_name, (long long)st.st_size);
        } else {
            printf("  [?]  %-30s  (stat failed)\n", ent->d_name);
        }
        count++;
    }
    closedir(d);
    printf("[FS_LS] %d entries\n", count);
}

// FS_CAT -- dump file to console. Caps at 32 KB to avoid runaway.
static void rtc_cli_fs_cat(const char *path) {
    if (!path || !*path) {
        printf("[FS_CAT] usage: FS_CAT /sd/path/to/file\n");
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[FS_CAT] fopen(%s) failed: %s\n", path, strerror(errno));
        return;
    }
    printf("[FS_CAT] %s\n", path);
    char buf[256];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
        total += n;
        if (total >= 32 * 1024) {
            printf("\n[FS_CAT] ...truncated at 32 KB\n");
            break;
        }
    }
    fclose(f);
    printf("\n[FS_CAT] %zu bytes\n", total);
}

static void rtc_cli_handle_line(char *line) {
    // Strip trailing whitespace / control chars.
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t' ||
                     line[n-1] == '\r' || line[n-1] == '\n')) {
        line[--n] = 0;
    }
    if (n == 0) return;

    if (startswith_ci(line, "GET_TIME")) {
        rtc_cli_print_now();
        // Best-effort: also record the read in the redundancy channel.
        pcf85063_ram_byte_write(I2C_NUM_0, PCF85063_RAM_CMD_GET_TIME);
        // If arg is "-v" or "verbose", dump NVS + RAM_byte too.
        const char *arg = line + 8;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "-V") || startswith_ci(arg, "VERBOSE")) {
            rtc_cli_dump_nvs();
        }
        return;
    }
    if (startswith_ci(line, "SET_TIME")) {
        const char *arg = line + 8;
        while (*arg == ' ' || *arg == '\t') arg++;
        int yr, mo, da, hr, mi, se;
        if (!try_parse_iso(arg, &yr, &mo, &da, &hr, &mi, &se)) {
            printf("[RTC] SET_TIME parse fail. Format: SET_TIME YYYY-MM-DDTHH:MM:SS\n");
            return;
        }
        if (yr < 2000 || yr > 2099 || mo < 1 || mo > 12 || da < 1 || da > 31 ||
            hr > 23 || mi > 59 || se > 59) {
            printf("[RTC] SET_TIME out-of-range\n");
            return;
        }
        if (!broker_rtc_hw_alive()) {
            printf("[RTC] PCF85063A not alive -- cannot set\n");
            return;
        }
        esp_err_t r = pcf85063_sync_utc(I2C_NUM_0,
                                        (uint8_t)hr, (uint8_t)mi, (uint8_t)se,
                                        (uint8_t)da, (uint8_t)mo, (uint16_t)yr);
        if (r == ESP_OK) {
            printf("[RTC] SET_TIME OK -> ");
            // Redundancy log: RAM_byte, then NVS record. Failures are logged
            // but do not fail the command -- the RTC chip has already been set.
            (void)pcf85063_ram_byte_write(I2C_NUM_0, PCF85063_RAM_CMD_SET_TIME);
            uint64_t wall_ts = civil_to_unix(yr, mo, da, hr, mi, se);
            uint64_t wr_ms   = (uint64_t)(esp_timer_get_time() / 1000LL);
            esp_err_t save   = nvs_cfg_rtc_save(wall_ts, wr_ms,
                                                 field_capture_get_boot_seq(),
                                                 line);
            if (save != ESP_OK) {
                printf("[NVS] SET_TIME save failed: %s\n", esp_err_to_name(save));
            }
            // Give the RTC task ~1 s to publish the new value to broker.
            vTaskDelay(pdMS_TO_TICKS(1100));
            rtc_cli_print_now();
            printf("[NVS] persisted: wall_ts=%llu wr_ms=%llu boot_seq=%lu\n",
                   (unsigned long long)wall_ts, (unsigned long long)wr_ms,
                   (unsigned long)field_capture_get_boot_seq());
        } else {
            printf("[RTC] SET_TIME write failed: %s\n", esp_err_to_name(r));
        }
        return;
    }
    if (startswith_ci(line, "REBOOT")) {
        printf("[REBOOT] esp_restart() in 200 ms\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));   // let the log line make it to the wire
        esp_restart();
        // unreachable
        return;
    }
    if (startswith_ci(line, "HELP")) {
        rtc_cli_print_help();
        return;
    }
    if (startswith_ci(line, "STATUS")) {
        rtc_cli_dump_status();
        return;
    }
    if (startswith_ci(line, "WHOAMI")) {
        rtc_cli_dump_whoami();
        return;
    }
    if (startswith_ci(line, "PM_DUMP")) {
        boot_pm_dump_locks();
        return;
    }
    if (startswith_ci(line, "RTC_DUMP")) {
        rtc_cli_dump_pcf_regs();
        return;
    }
    if (startswith_ci(line, "FS_LS")) {
        const char *arg = line + 5;
        while (*arg == ' ' || *arg == '\t') arg++;
        rtc_cli_fs_ls(*arg ? arg : NULL);
        return;
    }
    if (startswith_ci(line, "FS_CAT")) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t') arg++;
        rtc_cli_fs_cat(arg);
        return;
    }
    if (startswith_ci(line, "RGB")) {
        const char *arg = line + 3;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "AUTO") || startswith_ci(arg, "RESET")) {
            // Drop manual override by re-entering the current state. This
            // sets s_manual_override=false and lets the animation timer
            // resume driving whatever the firmware wants (standby pulse,
            // charging animation, etc.).
            ws2812_set_state(ws2812_get_state());
            printf("[RGB] manual override cleared -- firmware animations resume\n");
            return;
        }
        int r=-1, g=-1, b=-1;
        if (sscanf(arg, "%d %d %d", &r, &g, &b) == 3 &&
            r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
            ws2812_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
            printf("[RGB] set to (%d, %d, %d) -- manual override latched; RGB AUTO to release\n",
                   r, g, b);
        } else {
            printf("[RGB] usage: RGB <r> <g> <b>   or   RGB AUTO   (each 0..255)\n");
        }
        return;
    }
    if (startswith_ci(line, "SHIPMODE")) {
        // Manual escape when the physical button is stuck or unreachable
        // (e.g. mid-battery-test with a wedged encoder / cover). Same effect
        // as a 4 s button hold: BQ25619 BATFET_DIS + red LED cue.
        printf("[SHIP] triggering ship mode -- BATFET drops in ~10 s. "
               "Unplug USB (if attached) to complete shutdown.\n");
        // Force a stdout flush in case USB-Serial-JTAG buffers the last line.
        fflush(stdout);
        watcher_ship_mode();
        // If we return, USB is holding power. Report and continue.
        printf("[SHIP] BATFET dropped but device still powered by USB. "
               "Unplug USB to finish shutdown.\n");
        return;
    }
    if (startswith_ci(line, "BLACKBOX_CADENCE")) {
        const char *arg = line + 16;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == 0) {
            printf("[BB] cadence = %u s\n", (unsigned)nvs_cfg_sys_get_bb_cadence_s());
        } else {
            int s = atoi(arg);
            if (s < 1 || s > 3600) {
                printf("[BB] usage: BLACKBOX_CADENCE <1..3600>\n");
            } else {
                esp_err_t r = nvs_cfg_sys_set_bb_cadence_s((uint16_t)s);
                printf("[BB] cadence = %d s (%s). Takes effect on next sample tick.\n",
                       s, esp_err_to_name(r));
            }
        }
        return;
    }
    if (startswith_ci(line, "BLACKBOX")) {
        const char *arg = line + 8;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "ON")) {
            esp_err_t r = nvs_cfg_sys_set_blackbox(true);
            printf("[BB] blackbox=1 (%s). Reboot to start task.\n", esp_err_to_name(r));
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = nvs_cfg_sys_set_blackbox(false);
            printf("[BB] blackbox=0 (%s). Reboot to stop task.\n", esp_err_to_name(r));
        } else if (*arg == 0) {
            printf("[BB] blackbox = %d  cadence=%u s\n",
                   nvs_cfg_sys_get_blackbox() ? 1 : 0,
                   (unsigned)nvs_cfg_sys_get_bb_cadence_s());
        } else {
            printf("[BB] usage: BLACKBOX [ON|OFF]  (no arg = show state)\n");
        }
        return;
    }
    if (startswith_ci(line, "BATT_TEST")) {
        const char *arg = line + 9;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "ON")) {
            esp_err_t r = nvs_cfg_sys_set_batt_test(true);
            printf("[BATT] batt_test=1 (%s). Reboot to enter mode.\n",
                   esp_err_to_name(r));
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = nvs_cfg_sys_set_batt_test(false);
            printf("[BATT] batt_test=0 (%s). Reboot to return to normal FCM.\n",
                   esp_err_to_name(r));
        } else if (*arg == 0) {
            printf("[BATT] batt_test = %d\n",
                   nvs_cfg_sys_get_batt_test() ? 1 : 0);
        } else {
            printf("[BATT] usage: BATT_TEST [ON|OFF]  (no arg = show state)\n");
        }
        return;
    }
    if (startswith_ci(line, "NVS_PRINT")) {
        const char *arg = line + 9;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "ON")) {
            esp_err_t r = nvs_cfg_sys_set_print_on_boot(true);
            printf("[NVS] print_on_boot=1 (%s)\n", esp_err_to_name(r));
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = nvs_cfg_sys_set_print_on_boot(false);
            printf("[NVS] print_on_boot=0 (%s)\n", esp_err_to_name(r));
        } else if (*arg == 0) {
            // No arg: dump current NVS state.
            rtc_cli_dump_nvs();
            printf("[SYS] print_on_boot = %d\n",
                   nvs_cfg_sys_get_print_on_boot() ? 1 : 0);
        } else {
            printf("[NVS] usage: NVS_PRINT [ON|OFF]  (no arg = dump state)\n");
        }
        return;
    }
    // Unknown line -- ignore silently so log lines from the app don't get
    // echoed as errors. Only respond to the recognised commands.
}

void task_rtc_cli_fn(void *arg) {
    (void)arg;

    // Install the USB-Serial-JTAG driver so we can actually READ bytes back.
    // ESP-IDF's default USB-JTAG console only wires up TX (printf); RX
    // requires either this driver install or CONFIG_ESP_CONSOLE_USB_SERIAL_
    // JTAG_USES_DRIVER=y in menuconfig. Doing it here in the task keeps
    // the fix self-contained. TX (printf, ESP_LOG) continues to use the
    // ROM-based path unchanged.
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 256;
    cfg.tx_buffer_size = 256;
    esp_err_t r = usb_serial_jtag_driver_install(&cfg);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(r));
        vTaskDelete(NULL);
        return;
    }

    // Print the current RTC state on boot -- once the RTC task has had a
    // second to run its first read cycle. Satisfies "RTC should always
    // report the time on boot".
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("\n");
    printf("[RTC] Boot-time state:\n  ");
    rtc_cli_print_now();
    rtc_cli_print_help();

    // Line-assembly loop reading directly from USB-Serial-JTAG driver.
    static char  buf[80];
    static size_t len = 0;
    for (;;) {
        uint8_t c;
        int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        if (c == '\r' || c == '\n') {
            if (len > 0) {
                buf[len] = 0;
                rtc_cli_handle_line(buf);
                len = 0;
            }
            continue;
        }
        // Basic backspace so the operator can correct typos.
        if (c == 0x08 || c == 0x7F) {
            if (len > 0) len--;
            continue;
        }
        if (len < sizeof(buf) - 1) buf[len++] = (char)c;
        else                       len = 0;   // overflow -- drop and reset
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Priority shutdown watcher -- owns double-click, force-shuts regardless of
// what the field_capture task is doing. Runs on its own high-priority task
// so no recording / mic capture / ECG loop can starve it. Uses a completely
// independent button state machine (does not share statics with the
// button_poll() used by field_capture).
// ═════════════════════════════════════════════════════════════════════════════

static volatile bool s_ship_mode_latched = false;

// Set by the shutdown watcher while the button is held. Field_capture's
// standby LED animation checks this and skips its WS2812 writes during
// the hold so the watcher's red-LED countdown isn't flickered by concurrent
// field_capture writes. Cleared on release or on ship-mode fire.
volatile bool g_shutdown_hold_active = false;

// -- Stage 10 MLC_COLLECT support flags --------------------------------------
// When TRUE, the shutdown watcher continuously pushes its 15-minute uptime
// cap deadline forward (so recording is never interrupted by the cap), and
// resumes normal 15-min countdown from the moment this returns to FALSE.
// See run_mlc_collect_mode() for the setter; task_shutdown_watcher_fn for
// the observer.
volatile bool g_recording_active = false;

// One-shot: when TRUE and the button is still held, the shutdown watcher
// resets its press_start timestamp to now (as if the button had just been
// pressed). Used by MLC_COLLECT so that hold-to-stop-recording (2 s) doesn't
// eat into the shutdown watcher's own 4 s countdown -- Ivan's spec is
// 2 s stop + 4 s ship = 6 s total continuous hold, not just 4 s.
volatile bool g_watcher_press_reset_pending = false;

// When TRUE, task_shutdown_watcher_fn suppresses the 15-minute uptime-cap
// auto-shutdown. Set by run_battery_test_mode() on entry. Manual double-click
// / 4 s hold still ship the device (important as the operator's escape). The
// flag is boot-scoped -- next boot's state is decided by the NVS flag alone.
volatile bool g_batt_test_active = false;

// Best-effort "shut down NOW". Does NOT try to flush open files -- if a
// recording is in progress and the user wants it out, that is their call
// (single-click can early-end a recording; double-click drops power).
// The 300 ms red LED gives immediate visual confirmation before BATFET
// drops; if USB is attached, BATFET drops but the app keeps running --
// unplug USB to finish shutdown.
static void watcher_ship_mode(void) {
    if (s_ship_mode_latched) return;   // idempotent
    s_ship_mode_latched = true;

    ESP_LOGW(TAG, "PRIORITY SHIP MODE -- double-click intercepted");

    // Instant red LED so the operator sees it registered even if the
    // field_capture task was mid-print.
    ws2812_set_color(WS_MAX, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(300));

    if (broker_battery_hw_alive()) {
        bq25619_enter_ship_mode(I2C_NUM_1);
        // On battery only: BATFET drops within milliseconds and this delay
        // never returns because power is gone. On USB: BATFET drops but the
        // device stays alive via USB power, so we come back through -- give
        // the ship attempt half a second to actually kill us if it can.
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGE(TAG, "BQ not alive -- power stays on");
    }
    // If we reach here, USB is holding us up (or BQ is dead). Return to the
    // watcher loop; the caller is responsible for clearing g_shutdown_hold_active
    // so LED writes from field_capture resume. Without this, the previous
    // "while(1)" left the LED gate permanently latched and every downstream
    // LED write silently no-op'd (Ivan bench bug 2026-07-23).
    ESP_LOGW(TAG, "ship mode returned (USB present?) -- device stays running");
}

// Own state machine so it doesn't fight the field_capture button_poll().
// Hold-to-shutdown replaces the old double-click (Stage 7 § 3): pocket taps
// were false-triggering ship mode. Sequence:
//   0 - 2 s pressed  : solid red LED (visual confirmation)
//   2 - 4 s pressed  : DRV click every 500 ms (haptic countdown warning)
//   >= 4 s pressed   : long DRV buzz + fire watcher_ship_mode()
//   any release < 4 s: reset state, no fire, LED released back to field_capture
#define SHDN_WARN_MS     2000
#define SHDN_FIRE_MS     4000
#define SHDN_BUZZ_MS      500

// Damage-control auto-shutoff: if the firmware has been running for this long
// regardless of what the user has been doing, drop BATFET. Protects the
// battery in the case where the button dies mechanically while the device is
// left running, or the operator forgets to shut down. Currently 15 minutes;
// bump generously once we trust battery + hardware reliability.
#define SHDN_MAX_UPTIME_MS  (15u * 60u * 1000u)

void task_shutdown_watcher_fn(void *arg) {
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    bool     hold_active   = false;
    uint32_t press_start   = 0;
    uint32_t last_buzz_ms  = 0;
    bool     warn_started  = false;

    // Uptime-cap deadline. Reset to now + SHDN_MAX_UPTIME_MS whenever
    // g_recording_active is TRUE (or transitions FALSE) so that recording
    // never itself trips the cap, and the 15-min "idle shutoff" resets each
    // time a recording ends. See MLC_COLLECT for the setter side.
    uint32_t uptime_cap_deadline = SHDN_MAX_UPTIME_MS;

    ESP_LOGI(TAG, "shutdown watcher armed: hold %d ms to ship (buzz warn at %d ms). "
                  "hard uptime cap = %u s.",
             SHDN_FIRE_MS, SHDN_WARN_MS, (unsigned)(SHDN_MAX_UPTIME_MS / 1000));

    for (;;) {
        uint32_t now = millis_u32();
        bool     low = (gpio_get_level(PIN_BUTTON) == 0);

        // ── Damage-control uptime cap ────────────────────────────────────
        // Paused while recording (g_recording_active); on each recording
        // tick we push the deadline forward so it never fires. When
        // recording ends, deadline stays at ~(recording_end + 15 min),
        // giving the operator a fresh 15-minute idle window.
        if (g_recording_active || g_batt_test_active) {
            uptime_cap_deadline = now + SHDN_MAX_UPTIME_MS;
        }
        if (!g_recording_active && !g_batt_test_active && now >= uptime_cap_deadline) {
            ESP_LOGW(TAG, "shutdown FIRE (uptime cap reached, now=%u ms)", (unsigned)now);
            g_shutdown_hold_active = true;
            ws2812_set_color(WS_MAX, 0, 0);
            haptic_play_forced(DRV_LONG_BUZZ);
            watcher_ship_mode();   // returns if USB present
            // Release the LED gate and push the deadline out so we don't
            // re-fire every tick if ship mode failed silently (USB plugged).
            g_shutdown_hold_active = false;
            uptime_cap_deadline    = now + SHDN_MAX_UPTIME_MS;
        }

        // ── Press-reset request (from MLC_COLLECT after its 2 s stop) ────
        // When the button is still held and we've been asked to reset,
        // pin press_start to now so the fresh 4 s ship-mode countdown
        // starts from this instant, not from the original press.
        if (g_watcher_press_reset_pending && hold_active && low) {
            press_start   = now;
            last_buzz_ms  = 0;
            warn_started  = false;
            g_watcher_press_reset_pending = false;
        } else if (g_watcher_press_reset_pending && !low) {
            // No point resetting if button's already up.
            g_watcher_press_reset_pending = false;
        }

        if (low && !hold_active) {
            // -- Fresh press begins --
            hold_active   = true;
            press_start   = now;
            last_buzz_ms  = 0;
            warn_started  = false;
            g_shutdown_hold_active = true;
            // LED starts dark and ramps to full red over 4 s -- distinct from
            // mic-mode's solid red so the operator can't confuse them.
            ws2812_set_color(0, 0, 0);
        } else if (!low && hold_active) {
            // -- Released before fire threshold --
            uint32_t held = now - press_start;
            hold_active   = false;
            g_shutdown_hold_active = false;
            if (held >= SHDN_WARN_MS) {
                // The operator got a buzz warning but backed off. Give a
                // single confirmation click so they know we noticed.
                haptic_play(DRV_MEDIUM_CLICK);
                ESP_LOGI(TAG, "shutdown aborted (released after %u ms)",
                         (unsigned)held);
            }
            // Field_capture's own STANDBY LED tick will refresh the color
            // within 5 ms.
        } else if (low && hold_active) {
            uint32_t held = now - press_start;

            // Linear red-intensity ramp: 0 at press start -> WS_MAX at fire
            // threshold (4 s). Overrides any field_capture LED writes because
            // g_shutdown_hold_active is set.
            uint32_t intensity = (held * WS_MAX) / SHDN_FIRE_MS;
            if (intensity > WS_MAX) intensity = WS_MAX;
            ws2812_set_color((uint8_t)intensity, 0, 0);

            // Enter the warn-buzz phase at 2 s. First buzz fires immediately,
            // then every 500 ms until fire threshold.
            if (held >= SHDN_WARN_MS && held < SHDN_FIRE_MS) {
                if (!warn_started) {
                    warn_started = true;
                    ESP_LOGW(TAG, "shutdown warn phase started -- release to abort");
                    haptic_play_forced(DRV_STRONG_CLICK);
                    last_buzz_ms = now;
                } else if ((now - last_buzz_ms) >= SHDN_BUZZ_MS) {
                    last_buzz_ms = now;
                    haptic_play_forced(DRV_STRONG_CLICK);
                }
            }

            // Fire at threshold.
            if (held >= SHDN_FIRE_MS) {
                ESP_LOGW(TAG, "shutdown FIRE at %u ms hold", (unsigned)held);
                haptic_play_forced(DRV_LONG_BUZZ);
                watcher_ship_mode();   // returns if USB present
                // Release the LED gate + reset the hold state so the user can
                // continue interacting when the ship attempt failed silently
                // (USB plugged, no battery cutoff).
                g_shutdown_hold_active = false;
                hold_active            = false;
                warn_started           = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ── Public init ──────────────────────────────────────────────────────────────
void field_capture_init(void) {
    static bool inited = false;
    if (inited) return;
    inited = true;

    // Button + encoder pins: input, internal pull-up, no ISR (all polled).
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BUTTON) |
                        (1ULL << PIN_ENC_A)  |
                        (1ULL << PIN_ENC_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    s_btn_prev_low = (gpio_get_level(PIN_BUTTON) == 0);

    nvs_load();
    s_last_activity_ms = millis_u32();
    ensure_sd();
    ESP_LOGI(TAG, "field_capture_init OK (mode=%s boot_seq=%lu)",
             MODE_INFO[s_mode].name, (unsigned long)s_boot_seq);
}

// ═════════════════════════════════════════════════════════════════════════════
// Battery-test mode (Stage 11 preparation for Item D power baseline).
//
// Entered on boot when cfg_sys.batt_test == 1. Purpose: run untouched from a
// full charge until BQ25619 cuts power at UVLO, then compute battery life
// from timestamps in the CSV.
//
//  - All I2C sensors stay parked (their brokers default disabled).
//  - RTC + BQ25619 tasks stay running so we get wall-clock + voltage samples.
//  - 15-min shutdown-watcher uptime cap is suppressed via g_batt_test_active.
//  - CSV row every 10 s: /sd/data/battery/batt_<boot_seq>.csv.
//  - LED blips cyan for 50 ms per sample (< 1 mA average) so the operator
//    can visually confirm the device is still alive during the run.
//
// To exit: reboot with USB, then run "BATT_TEST OFF" over serial and reboot
// again. Ship-mode gesture (4 s button hold) also still works as a hard exit.
// ═════════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════════
// Shared telemetry helpers (BATT_TEST + BLACKBOX use these).
//
// vbat_adc  -- reads the 5k1-5k1 Vbat divider on GPIO9 (ADC1_CH8). Returns
//              mV or 0 on failure. TEMPORARY hack until screen returns or
//              mk2's BQ25896 ADC lands. See top-of-file include-block for
//              wiring notes.
// esp_ts    -- reads ESP32-S3 die junction temperature. Returns °C or
//              -273.15 on failure.
// idle_pct  -- returns percentage of time both cores spent in the Idle task
//              since the last call. Direct light-sleep engagement metric.
//              Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y.
// ═════════════════════════════════════════════════════════════════════════════

static adc_oneshot_unit_handle_t s_vbat_adc  = NULL;
static adc_cali_handle_t         s_vbat_cali = NULL;
static bool                      s_vbat_init_tried = false;

static void vbat_adc_ensure_init(void)
{
    if (s_vbat_init_tried) return;
    s_vbat_init_tried = true;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_vbat_adc) != ESP_OK) {
        ESP_LOGW(TAG, "Vbat ADC: unit init failed");
        s_vbat_adc = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(s_vbat_adc, ADC_CHANNEL_8, &chan_cfg);

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = ADC_CHANNEL_8,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_vbat_cali) != ESP_OK) {
        ESP_LOGW(TAG, "Vbat ADC: curve-fit cali unavailable, using raw counts");
        s_vbat_cali = NULL;
    }
    ESP_LOGI(TAG, "Vbat ADC armed on GPIO9 (ADC1_CH8), 5k1-5k1 divider");
}

// Returns Vbat in mV (after divider correction) or 0 on failure.
static uint32_t vbat_adc_read_mv(void)
{
    if (!s_vbat_adc) return 0;
    int raw_acc = 0;
    int ok_n    = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        if (adc_oneshot_read(s_vbat_adc, ADC_CHANNEL_8, &raw) == ESP_OK) {
            raw_acc += raw;
            ok_n++;
        }
    }
    if (ok_n == 0) return 0;
    int raw_avg = raw_acc / ok_n;
    int mv_at_pin = 0;
    if (s_vbat_cali &&
        adc_cali_raw_to_voltage(s_vbat_cali, raw_avg, &mv_at_pin) == ESP_OK) {
        return (uint32_t)(mv_at_pin * 2);
    }
    return (uint32_t)((raw_avg * 3300 / 4095) * 2);
}

static temperature_sensor_handle_t s_esp_ts = NULL;
static bool                        s_esp_ts_tried = false;

static void esp_ts_ensure_init(void)
{
    if (s_esp_ts_tried) return;
    s_esp_ts_tried = true;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_esp_ts) != ESP_OK ||
        temperature_sensor_enable(s_esp_ts) != ESP_OK) {
        ESP_LOGW(TAG, "ESP32-S3 SoC temp install failed -- reads return -273");
        s_esp_ts = NULL;
    }
}

// Returns SoC temp in °C or -273.15f if sensor not available.
static float esp_ts_read_c(void)
{
    if (!s_esp_ts) return -273.15f;
    float t = -273.15f;
    (void)temperature_sensor_get_celsius(s_esp_ts, &t);
    return t;
}

// idle_pct helper. Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y.
// Call at each sample -- returns % of wall-time the CPU spent in Idle since
// the previous call (both cores summed and normalised). First call primes
// counters and returns 0.
typedef struct {
    uint32_t last_idle_c0;
    uint32_t last_idle_c1;
    uint32_t last_wall_us;
    bool     primed;
} idle_pct_state_t;

// ESP-IDF 5.5 exposes ulTaskGetIdleRunTimeCounter() with no argument -- it
// returns the idle-task counter for the CURRENT core. We rely on the caller
// being pinned to a specific core (BATT_TEST is on Core 1 via task_field;
// BLACKBOX is on Core 0). Per-core reading is sufficient for the PM signal.
// Getting BOTH cores would require enumerating via uxTaskGetSystemState which
// isn't worth the complexity at the current level of investigation.
static uint8_t idle_pct_sample(idle_pct_state_t *st)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    uint32_t c   = ulTaskGetIdleRunTimeCounter();
    uint32_t now = (uint32_t)esp_timer_get_time();

    if (!st->primed) {
        st->last_idle_c0 = c;
        st->last_wall_us = now;
        st->primed = true;
        return 0;
    }

    uint32_t d_idle = c - st->last_idle_c0;
    uint32_t d_wall = now - st->last_wall_us;
    st->last_idle_c0 = c;
    st->last_wall_us = now;

    if (d_wall == 0) return 0;
    uint32_t pct = (uint32_t)((uint64_t)d_idle * 100U / (uint64_t)d_wall);
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
#else
    (void)st;
    return 0;
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// BLACKBOX telemetry task (Stage 11).
//
// Always-on background CSV logger. NVS-flagged. Snapshots every broker + all
// system stats every cfg_sys.bb_cadence_s seconds to
//   /sd/data/blackbox/bb_<boot_seq>.csv
// Uses Pattern A durability (open-append-close per row). One file per boot.
//
// Runs alongside normal FCM operation. Exits at startup if batt_test is
// active (BATT_TEST already logs a compatible superset for that use case).
//
// Column format documented in the CSV header itself.
// ═════════════════════════════════════════════════════════════════════════════

void task_blackbox_fn(void *arg);   // forward decl (registered via boot_tasks.c)

void task_blackbox_fn(void *arg)
{
    (void)arg;

    // Check flag first. If off, exit immediately -- boot_tasks.c always
    // creates this task, we self-terminate here when unused.
    if (!nvs_cfg_sys_get_blackbox()) {
        ESP_LOGI(TAG, "BLACKBOX: disabled by NVS flag -- task exiting");
        vTaskDelete(NULL);
        return;
    }
    if (nvs_cfg_sys_get_batt_test()) {
        ESP_LOGI(TAG, "BLACKBOX: batt_test mode is active -- task exiting to avoid double-CSV");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "BLACKBOX: task started on Core %d", xPortGetCoreID());

    // Wait for SD to mount (field_capture_init calls ensure_sd during startup).
    for (int i = 0; i < 50 && !s_sd_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!s_sd_ready) {
        ESP_LOGE(TAG, "BLACKBOX: SD never mounted -- task exiting");
        vTaskDelete(NULL);
        return;
    }
    try_mkdir("/sd/data/blackbox");

    // Shared telemetry helpers.
    esp_ts_ensure_init();
    vbat_adc_ensure_init();
    idle_pct_state_t idle_st = { 0 };

    // Open file, write header, close (Pattern A).
    char bb_path[80];
    snprintf(bb_path, sizeof(bb_path),
             "/sd/data/blackbox/bb_%04lu.csv",
             (unsigned long)s_boot_seq);
    {
        FILE *f = fopen(bb_path, "w");
        if (!f) {
            ESP_LOGE(TAG, "BLACKBOX: fopen %s failed: %s", bb_path, strerror(errno));
            vTaskDelete(NULL);
            return;
        }
        char now[32]; rtc_iso_now(now, sizeof(now));
        fprintf(f, "# kompic mk1 %s fw=%s hw=%s mode=blackbox boot=%lu rtc_start=%s\n",
                KOMPIC_HW_VERSION, KOMPIC_FW_VERSION, KOMPIC_HW_VERSION,
                (unsigned long)s_boot_seq, now);
        fprintf(f, "# sensors_on bitmap: IMU|MAG|ENV|LIGHT|HR|SKIN|BAT|RTC (LSB=IMU).\n");
        fprintf(f, "# sensors_stat: packed nibbles per sensor, same order. Values are sensor_status_t:\n");
        fprintf(f, "#   0=DISABLED 1=OFFLINE 2=ACQUIRING 3=STALE 4=ONLINE 5=NOTIF\n");
        fprintf(f, "# fcm_mode is the current top-level FCM enum name; fcm_state = STANDBY/FL_ON/RECORDING/ALARM.\n");
        fprintf(f, "# idle_pct: %% of wall-time both cores spent in Idle since previous row.\n");
        fprintf(f, "# batt_mv is the BQ25619 threshold register -- IGNORE; use vbat_adc_mv.\n");
        fprintf(f, "t_ms,iso_utc,uptime_min,boot_seq,");
        fprintf(f, "fcm_mode,fcm_state,");
        fprintf(f, "heap_free_kb,min_heap_kb,cpu_mhz,idle_pct,sensors_on,sensors_stat,");
        fprintf(f, "bq_v,bq_pct,bq_chg,bq_pg,bq_fault,vbat_adc_mv,soc_temp_c,");
        fprintf(f, "imu_ax,imu_ay,imu_az,imu_gx,imu_gy,imu_gz,imu_temp,");
        fprintf(f, "mag_x,mag_y,mag_z,");
        fprintf(f, "env_t,env_h,env_p,env_gas,env_alt,");
        fprintf(f, "light_lux,");
        fprintf(f, "hr_bpm,hr_spo2,hr_finger,");
        fprintf(f, "skin_t\n");
        fclose(f);
        ESP_LOGI(TAG, "BLACKBOX: logging to %s (open-append-close per row)", bb_path);
    }

    // FCM state name lookup (static enum name).
    static const char *st_names[] = {"STANDBY", "FL_ON", "RECORDING", "ALARM"};

    // Sample loop -- drift-free schedule.
    uint32_t next_ms  = millis_u32();
    uint32_t idx      = 0;

    for (;;) {
        uint32_t cadence_ms = (uint32_t)nvs_cfg_sys_get_bb_cadence_s() * 1000U;
        uint32_t now = millis_u32();

        if ((int32_t)(now - next_ms) < 0) {
            // Wake often enough that cadence changes are honoured quickly.
            uint32_t wait = next_ms - now;
            if (wait > 1000) wait = 1000;
            vTaskDelay(pdMS_TO_TICKS(wait));
            continue;
        }

        // ---- Snapshot ----
        char iso[32]; rtc_iso_now(iso, sizeof(iso));
        uint32_t uptime_min = now / 60000U;

        uint32_t heap_free_kb = esp_get_free_heap_size() / 1024;
        uint32_t min_heap_kb  = esp_get_minimum_free_heap_size() / 1024;
        rtc_cpu_freq_config_t cpu_cfg;
        rtc_clk_cpu_freq_get_config(&cpu_cfg);
        uint32_t cpu_mhz = cpu_cfg.freq_mhz;
        uint8_t  idle_pct = idle_pct_sample(&idle_st);

        uint32_t sensors_on = 0;
        if (broker_imu_get_enabled())     sensors_on |= (1 << 0);
        if (broker_mag_get_enabled())     sensors_on |= (1 << 1);
        if (broker_env_get_enabled())     sensors_on |= (1 << 2);
        if (broker_light_get_enabled())   sensors_on |= (1 << 3);
        if (broker_hr_get_enabled())      sensors_on |= (1 << 4);
        if (broker_skin_get_enabled())    sensors_on |= (1 << 5);
        if (broker_battery_get_enabled()) sensors_on |= (1 << 6);
        if (broker_rtc_get_enabled())     sensors_on |= (1 << 7);

        // Pack per-sensor status nibbles (order matches sensors_on).
        uint32_t stat = 0;
        stat |= ((uint32_t)(broker_imu_get_status()     & 0x0F) << (0  * 4));
        stat |= ((uint32_t)(broker_mag_get_status()     & 0x0F) << (1  * 4));
        stat |= ((uint32_t)(broker_env_get_status()     & 0x0F) << (2  * 4));
        stat |= ((uint32_t)(broker_light_get_status()   & 0x0F) << (3  * 4));
        stat |= ((uint32_t)(broker_hr_get_status()      & 0x0F) << (4  * 4));
        stat |= ((uint32_t)(broker_skin_get_status()    & 0x0F) << (5  * 4));
        stat |= ((uint32_t)(broker_battery_get_status() & 0x0F) << (6  * 4));
        stat |= ((uint32_t)(broker_rtc_get_status()     & 0x0F) << (7  * 4));

        broker_battery_data_t bat; broker_battery_read(&bat);
        broker_imu_data_t     im;  broker_imu_read(&im);
        broker_mag_data_t     mg;  broker_mag_read(&mg);
        broker_env_data_t     en;  broker_env_read(&en);
        broker_light_data_t   li;  broker_light_read(&li);
        broker_hr_data_t      hr;  broker_hr_read(&hr);
        broker_skin_data_t    sk;  broker_skin_read(&sk);

        float t_soc = esp_ts_read_c();
        uint32_t vbat_adc_mv = vbat_adc_read_mv();

        const char *mode_name = MODE_INFO[s_mode].name;
        const char *state_name = (s_state < (int)(sizeof(st_names)/sizeof(*st_names)))
                                  ? st_names[s_state] : "?";

        // ---- Append row ----
        FILE *f = fopen(bb_path, "a");
        if (f) {
            // Time + FCM state
            fprintf(f, "%lu,%s,%lu,%lu,%s,%s,",
                    (unsigned long)now, iso,
                    (unsigned long)uptime_min, (unsigned long)s_boot_seq,
                    mode_name, state_name);
            // System
            fprintf(f, "%lu,%lu,%lu,%u,0x%02lX,0x%08lX,",
                    (unsigned long)heap_free_kb, (unsigned long)min_heap_kb,
                    (unsigned long)cpu_mhz, (unsigned)idle_pct,
                    (unsigned long)sensors_on, (unsigned long)stat);
            // Power
            fprintf(f, "%.3f,%u,%u,%u,0x%02X,%lu,%.2f,",
                    bat.voltage, (unsigned)bat.percentage,
                    (unsigned)(bat.charging ? 1 : 0),
                    (unsigned)(bat.power_good ? 1 : 0),
                    (unsigned)bat.fault,
                    (unsigned long)vbat_adc_mv, t_soc);
            // IMU
            fprintf(f, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,",
                    im.accel_x, im.accel_y, im.accel_z,
                    im.gyro_x, im.gyro_y, im.gyro_z, im.temperature);
            // MAG
            fprintf(f, "%.2f,%.2f,%.2f,",
                    mg.x_ut, mg.y_ut, mg.z_ut);
            // ENV
            fprintf(f, "%.2f,%.2f,%.2f,%.0f,%.1f,",
                    en.temperature_c, en.humidity_pct, en.pressure_hpa,
                    en.gas_resistance_ohm, en.altitude_m);
            // LIGHT
            fprintf(f, "%.1f,", li.lux);
            // HR
            fprintf(f, "%u,%.1f,%u,",
                    (unsigned)hr.bpm, hr.spo2_pct,
                    (unsigned)(hr.finger_detected ? 1 : 0));
            // SKIN
            fprintf(f, "%.2f\n", sk.skin_temp_c);
            fclose(f);
        } else {
            ESP_LOGW(TAG, "BLACKBOX: append fopen failed: %s (SD unmounted?)",
                     strerror(errno));
        }

        // Debug print every 6 samples (once per minute at 10 s cadence).
        if ((idx % 6) == 0) {
            ESP_LOGI(TAG, "BLACKBOX #%lu  %s  mode=%s  heap=%luKB  idle=%u%%  sens=0x%02lX",
                     (unsigned long)idx, iso, mode_name,
                     (unsigned long)heap_free_kb, (unsigned)idle_pct,
                     (unsigned long)sensors_on);
        }

        idx++;
        next_ms += cadence_ms;
    }
}

static void run_battery_test_mode(void) {
    g_batt_test_active = true;

    ESP_LOGW(TAG, "BATTERY TEST MODE active. 15-min uptime cap suppressed. "
                  "Sample cadence: 10 s. Sensors parked. To exit: reboot with "
                  "USB attached and run BATT_TEST OFF.");

    ensure_sd();
    if (!s_sd_ready) {
        ESP_LOGE(TAG, "BATT_TEST: SD not mounted -- CSV logging disabled. "
                      "Battery run continues but no data will be recorded.");
    } else {
        try_mkdir("/sd/data/battery");
    }

    esp_ts_ensure_init();
    vbat_adc_ensure_init();
    idle_pct_state_t idle_st = { 0 };

    // VBUS-aware PM lock (0.4.12). Held while USB is plugged in so that
    // USB-Serial-JTAG stays clocked and the host doesn't drop the port
    // (Errno 71 softlock). Released when unplugged so light-sleep can
    // engage normally during the battery discharge. Toggled at each
    // sample tick based on broker_battery.power_good.
    //
    // Effect: user can plug USB in mid-batt_test to recover serial access
    // (~10 s to next tick), do a "BATT_TEST OFF" if desired, then unplug
    // and let discharge continue. Also lets us log the charging curve in
    // the same CSV as the discharge, if wanted.
    esp_pm_lock_handle_t vbus_lock = NULL;
    bool                 vbus_lock_held = false;
#ifdef CONFIG_PM_ENABLE
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0,
                            "batt_test_vbus", &vbus_lock) != ESP_OK) {
        ESP_LOGW(TAG, "BATT_TEST: PM lock create failed -- serial may drop on VBUS");
        vbus_lock = NULL;
    }
#endif

    // PATTERN A (see SD durability rule above): write the header, close, then
    // open-append-close per row. If the watch dies between samples, every row
    // that made it to the loop's fclose() is durable on disk.
    //
    // Stage 11 PM extension (0.4.8): additionally UNMOUNT the SD card
    // between samples. sdcard_unmount() releases the ESP_PM_APB_FREQ_MAX
    // lock, letting light-sleep engage during the 9.95 s idle window. Mount
    // adds ~200 ms latency per row -- tolerable at 10 s cadence. Header is
    // written under the initial mount then unmount is called before entering
    // the sample loop.
    char batt_path[80];
    bool batt_path_ok = false;
    if (s_sd_ready) {
        snprintf(batt_path, sizeof(batt_path),
                 "/sd/data/battery/batt_%04lu.csv",
                 (unsigned long)s_boot_seq);
        FILE *f = fopen(batt_path, "w");
        if (f) {
            char now[32]; rtc_iso_now(now, sizeof(now));
            fprintf(f, "# kompic mk1 %s fw=%s hw=%s mode=battery_test "
                       "boot=%lu rtc_start=%s\n",
                    KOMPIC_HW_VERSION, KOMPIC_FW_VERSION, KOMPIC_HW_VERSION,
                    (unsigned long)s_boot_seq, now);
            fprintf(f, "# sample every 10 s until BQ25619 UVLO cutoff\n");
            fprintf(f, "# batt_mv is a BQ25619 threshold register (not a real ADC), ignore it.\n");
            fprintf(f, "# vbat_adc_mv is a temporary 5k1-5k1 divider on GPIO9 (screen D2 pad) -- ~+/-1%% accuracy.\n");
            fprintf(f, "# cpu_mhz is snapshot at sample time (DFS makes it vary; sample is during work window -> usually 240).\n");
            fprintf(f, "# sensors_on: bitmap IMU|MAG|ENV|LIGHT|HR|SKIN|BAT|RTC (LSB=IMU). All 0 during batt_test except BAT+RTC which are always-on.\n");
            fprintf(f, "# idle_pct: %% of wall-time both cores spent in Idle since previous row. High = PM light-sleep engaging.\n");
            fprintf(f, "t_ms,iso_utc,soc_temp_c,batt_mv,batt_pct,charging,vbat_adc_mv,heap_free_kb,cpu_mhz,sensors_on,idle_pct\n");
            fclose(f);   // commit header to disk before the sampling loop begins
            batt_path_ok = true;
            ESP_LOGI(TAG, "BATT_TEST: logging to %s (open-append-close per row, SD unmounted between samples)", batt_path);
        } else {
            ESP_LOGE(TAG, "BATT_TEST: fopen %s failed: %s", batt_path, strerror(errno));
        }
        // Unmount now so the SDMMC APB_FREQ_MAX PM lock is released. Each
        // sample tick re-mounts before writing, unmounts after.
        (void)sdcard_unmount();
        s_sd_ready = false;
    }

    // Sampling loop. 10 s cadence exact; drift-free by aligning to millis_u32.
    const uint32_t PERIOD_MS  = 10000;
    const uint32_t LED_BLIP_MS = 50;
    uint32_t next_sample_ms = millis_u32();
    uint32_t sample_idx     = 0;

    for (;;) {
        uint32_t now = millis_u32();
        if ((int32_t)(now - next_sample_ms) < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // LED blip (short cyan) at each tick for a "still alive" visual.
        ws2812_set_color(0, WS_MAX, WS_MAX);

        float t_soc = esp_ts_read_c();
        uint8_t idle_pct = idle_pct_sample(&idle_st);

        broker_battery_data_t bd; broker_battery_read(&bd);

        // VBUS-aware PM lock toggle -- decouple serial survival from
        // PM engagement based on live VBUS state.
#ifdef CONFIG_PM_ENABLE
        if (vbus_lock) {
            if (bd.power_good && !vbus_lock_held) {
                esp_pm_lock_acquire(vbus_lock);
                vbus_lock_held = true;
                ESP_LOGI(TAG, "BATT_TEST: VBUS present -- PM light-sleep SUSPENDED (serial alive)");
            } else if (!bd.power_good && vbus_lock_held) {
                esp_pm_lock_release(vbus_lock);
                vbus_lock_held = false;
                ESP_LOGI(TAG, "BATT_TEST: VBUS removed -- PM light-sleep RE-ENGAGED (serial may drop)");
            }
        }
#endif

        char iso[32]; rtc_iso_now(iso, sizeof(iso));

        // Enhanced telemetry -- see CSV header comment.
        uint32_t heap_free_kb = esp_get_free_heap_size() / 1024;
        rtc_cpu_freq_config_t cpu_cfg;
        rtc_clk_cpu_freq_get_config(&cpu_cfg);
        uint32_t cpu_mhz = cpu_cfg.freq_mhz;   // snapshot under DFS
        uint32_t sensors_on = 0;
        if (broker_imu_get_enabled())     sensors_on |= (1 << 0);
        if (broker_mag_get_enabled())     sensors_on |= (1 << 1);
        if (broker_env_get_enabled())     sensors_on |= (1 << 2);
        if (broker_light_get_enabled())   sensors_on |= (1 << 3);
        if (broker_hr_get_enabled())      sensors_on |= (1 << 4);
        if (broker_skin_get_enabled())    sensors_on |= (1 << 5);
        if (broker_battery_get_enabled()) sensors_on |= (1 << 6);
        if (broker_rtc_get_enabled())     sensors_on |= (1 << 7);

        uint32_t vbat_adc_mv = vbat_adc_read_mv();

        if (batt_path_ok) {
            // Mount SD just for this write, then unmount so the PM lock
            // releases and light-sleep can engage during the 9.95 s idle.
            if (sdcard_mount() == ESP_OK) {
                FILE *f = fopen(batt_path, "a");
                if (f) {
                    fprintf(f, "%lu,%s,%.2f,%.0f,%u,%u,%lu,%lu,%lu,0x%02lX,%u\n",
                            (unsigned long)now, iso, t_soc,
                            bd.voltage * 1000.0f, (unsigned)bd.percentage,
                            (unsigned)(bd.charging ? 1 : 0),
                            (unsigned long)vbat_adc_mv,
                            (unsigned long)heap_free_kb,
                            (unsigned long)cpu_mhz,
                            (unsigned long)sensors_on,
                            (unsigned)idle_pct);
                    fclose(f);   // commit -- survives power loss between samples
                } else {
                    ESP_LOGW(TAG, "BATT_TEST: append fopen failed: %s", strerror(errno));
                }
                (void)sdcard_unmount();   // release SDMMC APB_FREQ_MAX lock
            } else {
                ESP_LOGW(TAG, "BATT_TEST: sdcard_mount failed -- row #%lu lost",
                         (unsigned long)sample_idx);
            }
        }

        ESP_LOGI(TAG, "BATT_TEST #%lu  %s  soc=%.1fC  vbat=%lumV%s  heap=%luKB  cpu=%luMHz  idle=%u%%  sens=0x%02lX",
                 (unsigned long)sample_idx, iso, t_soc,
                 (unsigned long)vbat_adc_mv,
                 bd.charging ? " CHG" : "",
                 (unsigned long)heap_free_kb,
                 (unsigned long)cpu_mhz,
                 (unsigned)idle_pct,
                 (unsigned long)sensors_on);

        vTaskDelay(pdMS_TO_TICKS(LED_BLIP_MS));
        ws2812_set_color(0, 0, 0);

        sample_idx++;
        next_sample_ms += PERIOD_MS;
    }
}

// ── Task loop ────────────────────────────────────────────────────────────────
void task_field_capture_fn(void *arg) {
    (void)arg;
    field_capture_init();

    // Battery-test mode: NVS-flagged one-way boot into a 10 s cadence logger.
    // Never returns from run_battery_test_mode() -- device runs until BQ25619
    // cuts power at UVLO. Ship-mode gesture (4 s button hold) is the manual
    // escape via task_shutdown_watcher_fn.
    //
    // 0.4.12 change: batt_test ALWAYS runs when the NVS flag is set. Serial
    // survivability while USB is plugged in is handled INSIDE the loop via
    // a VBUS-aware PM lock (see run_battery_test_mode). This lets us log
    // charging behavior alongside discharge, and recovery from softlock
    // becomes "plug USB in, unmount SD auto-releases, PM lock kicks in on
    // next sample tick, serial responsive again".
    if (nvs_cfg_sys_get_batt_test()) {
        run_battery_test_mode();
        // unreachable
    }

    for (;;) {
        int btn = button_poll();
        int enc = encoder_delta();

        // Ship mode: task_shutdown_watcher_fn owns it as a 4-second hold
        // (Stage 7 §3). Double-click is now bound as a submenu-exit backup
        // for when tap-Z is unreliable / not yet tuned (Stage 10 §12).

        // ── Stage 10 "go back" gesture: LSM tap double or button double ──
        // Only acts while ST_STANDBY -- if a recording is live we don't
        // want a stray gesture to abort the session. Tap counter is polled
        // + deduped on strict increase (published by task_imu_fn); button
        // double-click is btn==2 from button_poll(). Both do the same thing
        // so the user has a fallback while tap thresholds are being tuned.
        uint32_t tap_dbl = lsm6dsv16x_tap_z_double_count();
        bool exit_submenu = false;
        const char *exit_reason = NULL;
        if (tap_dbl != s_last_tap_dbl_count) {
            s_last_tap_dbl_count = tap_dbl;
            exit_submenu = true;
            exit_reason  = "tap-double";
        }
        if (btn == 2 && s_state == ST_STANDBY && s_in_submenu) {
            exit_submenu = true;
            exit_reason  = "button-double (tap backup)";
            btn = 0;  // consume so the ST_STANDBY switch doesn't also see it
        }
        if (exit_submenu && s_state == ST_STANDBY && s_in_submenu) {
            s_in_submenu = false;
            s_mode = FCM_LSM;
            nvs_save_mode();
            haptic_play(DRV_MEDIUM_CLICK);
            s_last_activity_ms = millis_u32();
            ESP_LOGI(TAG, "LSM submenu EXIT (%s)", exit_reason);
        }

        switch (s_state) {

        case ST_STANDBY: {
            if (enc != 0) {
                if (s_in_submenu) {
                    s_mode = lsm_submenu_step(s_mode, enc);
                } else {
                    s_mode = top_mode_step(s_mode, enc);
                }
                nvs_save_mode();
                haptic_play(DRV_MEDIUM_CLICK);
                s_last_activity_ms = millis_u32();
                ESP_LOGI(TAG, "mode -> %s (%s)", MODE_INFO[s_mode].name,
                         s_in_submenu ? "LSM sub" : "top");
            }
            if (btn == 1) {
                if (!s_in_submenu && s_mode == FCM_LSM) {
                    // Enter the LSM submenu. Land on the first submenu entry.
                    s_in_submenu = true;
                    s_mode = FC_LSM_SUBMENU[0];
                    nvs_save_mode();
                    haptic_play(DRV_STRONG_CLICK);
                    s_last_activity_ms = millis_u32();
                    ESP_LOGI(TAG, "LSM submenu ENTER (mode -> %s)",
                             MODE_INFO[s_mode].name);
                } else if (s_mode == FCM_FLASHLIGHT) {
                    flashlight_set_brightness(fl_pct_from_level(s_fl_level));
                    s_state = ST_FL_ON;
                } else if (s_mode == FCM_ALARM) {
                    s_state = ST_ALARM_FIRING;
                    run_alarm_firing();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_COMPASS) {
                    run_compass_or_cal();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_ECG) {
                    run_ecg_session();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_TEMP) {
                    run_temp_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_BCG) {
                    run_bcg_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_STEPS) {
                    run_steps_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_MLC_COLLECT) {
                    run_mlc_collect_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_TAP_DBG) {
                    run_tap_dbg_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_USB_MSC) {
                    // Close any recording sessions cleanly before handing the
                    // SD host to USB. usb_msc_run_until_exit() reboots on
                    // click / unplug, so this call typically does not return.
                    (void)sdcard_close_session();
                    s_sd_ready = false;
                    ESP_LOGW(TAG, "Entering FCM_USB_MSC -- device will reboot "
                                  "on exit (button click or USB unplug).");
                    (void)usb_msc_run_until_exit(usb_msc_button_click_cb);
                    // Only reached on setup failure; fall through to STANDBY.
                    s_state = ST_STANDBY;
                } else {
                    s_state = ST_RECORDING;
                    run_recording_for_current_mode();
                    s_state = ST_STANDBY;
                }
                s_last_activity_ms = millis_u32();
            }
            // LED animation. Skip entirely while the shutdown watcher is
            // running its hold-countdown -- watcher owns the LED then.
            if (!g_shutdown_hold_active) {
                if (s_in_submenu) {
                    rgb_lsm_submenu_indicator(s_mode);
                } else if (s_mode == FCM_COMPASS) {
                    rgb_compass_alt_red_blue();
                } else if (s_mode == FCM_ECG) {
                    rgb_qvar_alt_yellow_purple();
                } else if (s_mode == FCM_TEMP) {
                    rgb_temp_warm_cycle();
                } else {
                    uint32_t solid_end = s_last_activity_ms + SOLID_ON_ACTIVITY_MS;
                    if (millis_u32() < solid_end) rgb_set_max(s_mode);
                    else                          rgb_pulse(s_mode, solid_end, PULSE_STANDBY_MS);
                }
            }
            break;
        }

        case ST_FL_ON: {
            if (enc != 0) {
                int nl = (int)s_fl_level + enc;
                if (nl < 0)          nl = 0;
                if (nl > FL_LEVELS)  nl = FL_LEVELS;
                s_fl_level = (uint8_t)nl;
                flashlight_set_brightness(fl_pct_from_level(s_fl_level));
                haptic_play(DRV_STRONG_CLICK);
            }
            if (btn == 1) {
                flashlight_off();
                s_state = ST_STANDBY;
                s_last_activity_ms = millis_u32();
            }
            rgb_set_max(FCM_FLASHLIGHT);
            break;
        }

        case ST_RECORDING:
        case ST_ALARM_FIRING:
            // Handled by their blocking helpers above; state is restored to
            // STANDBY on their return. This branch shouldn't be reached in
            // normal flow.
            s_state = ST_STANDBY;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_TICK_MS));
    }
}
