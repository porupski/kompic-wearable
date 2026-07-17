/**
 * @file field_capture.c
 * @brief Port of 7_demo_field_capture. See field_capture.h.
 */

#include "field_capture.h"
#include "firmware_version.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#include "driver/usb_serial_jtag.h"       // reliable stdin over USB-CDC
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
    { "mot",     14, 26,  0 },
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
    // TEMP -- rendered as a slow warm-color cycle (red -> orange -> yellow),
    // handled specially in the standby LED path.
    { "temp",    26,  8,  0 },
    // BCG (Stage 8) -- rendered as a 2 Hz yellow<->red alternation, handled
    // specially in the standby LED path.
    { "bcg",     26, 12,  0 },
};

typedef enum {
    ST_STANDBY = 0,
    ST_FL_ON,
    ST_RECORDING,
    ST_ALARM_FIRING,
} app_state_t;

// ── Globals ──────────────────────────────────────────────────────────────────
static fc_mode_t   s_mode              = FCM_ENV;
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

// ── NVS (field/mode + field/boot_seq) ────────────────────────────────────────
static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open("field", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t m = FCM_ENV;
    nvs_get_u8(h, "mode", &m);
    if (m >= FCM_COUNT) m = FCM_ENV;
    s_mode = (fc_mode_t)m;
    nvs_get_u32(h, "boot_seq", &s_boot_seq);
    s_boot_seq++;
    nvs_set_u32(h, "boot_seq", s_boot_seq);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "boot_seq=%lu restored mode=%s",
             (unsigned long)s_boot_seq, MODE_INFO[s_mode].name);
}
static void nvs_save_mode(void) {
    nvs_handle_t h;
    if (nvs_open("field", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "mode", (uint8_t)s_mode);
    nvs_commit(h);
    nvs_close(h);
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

// TEMP mode signature: warm-color cycle red -> orange -> yellow at ~0.5 Hz
// (2000 ms period, 666 ms per step). Distinct from every other mode's
// signature at a glance.
static void rgb_temp_warm_cycle(void) {
    uint32_t t = millis_u32();
    uint32_t phase = (t / 666) % 3;
    switch (phase) {
    case 0: ws2812_set_color(26,  0, 0); break;   // red
    case 1: ws2812_set_color(26,  8, 0); break;   // orange
    case 2: ws2812_set_color(26, 20, 0); break;   // yellow
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

static void run_temp_session(void) {
    ESP_LOGI(TAG, "[TEMP] active. single-click to exit.");

    // ESP32-S3 internal temperature sensor: one-time install on first entry.
    // Range -10..80C default. Kept installed after exit -- cheap to leave on.
    static temperature_sensor_handle_t s_esp_ts = NULL;
    static bool s_esp_ts_installed = false;
    if (!s_esp_ts_installed) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_esp_ts) == ESP_OK &&
            temperature_sensor_enable(s_esp_ts) == ESP_OK) {
            s_esp_ts_installed = true;
        } else {
            ESP_LOGW(TAG, "[TEMP] ESP32-S3 temp sensor install failed -- SoC reading disabled");
        }
    }

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
            float t_soc  = -273.15f;
            if (s_esp_ts_installed) {
                temperature_sensor_get_celsius(s_esp_ts, &t_soc);
            }

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

        if (csv) {
            fprintf(csv, "%lu,%.4f,%.2f,%d\n",
                    (unsigned long)(now - session_start),
                    im.accel_z / 9.81f, (double)filt_mg, beat_marker);
            rows_written++;
        }

#if BCG_PLOTTER_MODE
        printf("%d\t%d\n", (int)filt_mg, beat_marker);
#else
        // Monitor mode: one status line per second at INFO.
        if ((now - last_status_ms) >= 1000) {
            last_status_ms = now;
            uint32_t elapsed = (now - session_start) / 1000;
            ESP_LOGI(TAG, "[BCG] t=%us  BPM=%3.0f  beats=%u  rows=%u",
                     (unsigned)elapsed,
                     (double)bpm_last,
                     (unsigned)total_beats,
                     (unsigned)rows_written);
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
            // Give the RTC task ~1 s to publish the new value to broker.
            vTaskDelay(pdMS_TO_TICKS(1100));
            rtc_cli_print_now();
        } else {
            printf("[RTC] SET_TIME write failed: %s\n", esp_err_to_name(r));
        }
        return;
    }
    // Unknown line -- ignore silently so log lines from the app don't get
    // echoed as errors. Only respond to SET_TIME / GET_TIME.
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
    printf("  Commands: GET_TIME | SET_TIME YYYY-MM-DDTHH:MM:SS\n");

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
    } else {
        ESP_LOGE(TAG, "BQ not alive -- power stays on");
    }
    // Spin forever. Either power dies here or (USB plugged) the user
    // unplugs and power dies then.
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
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

    ESP_LOGI(TAG, "shutdown watcher armed: hold %d ms to ship (buzz warn at %d ms). "
                  "hard uptime cap = %u s.",
             SHDN_FIRE_MS, SHDN_WARN_MS, (unsigned)(SHDN_MAX_UPTIME_MS / 1000));

    for (;;) {
        uint32_t now = millis_u32();
        bool     low = (gpio_get_level(PIN_BUTTON) == 0);

        // ── Damage-control uptime cap ────────────────────────────────────
        // Independent of button state. Fires unconditionally at the cap so
        // a dead / stuck button cannot leave the device running until the
        // battery is dead.
        if (now >= SHDN_MAX_UPTIME_MS) {
            ESP_LOGW(TAG, "shutdown FIRE (uptime cap %u ms reached)", (unsigned)now);
            g_shutdown_hold_active = true;
            ws2812_set_color(WS_MAX, 0, 0);
            haptic_play_forced(DRV_LONG_BUZZ);
            watcher_ship_mode();   // never returns
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
                watcher_ship_mode();   // never returns
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

// ── Task loop ────────────────────────────────────────────────────────────────
void task_field_capture_fn(void *arg) {
    (void)arg;
    field_capture_init();

    for (;;) {
        int btn = button_poll();
        int enc = encoder_delta();

        // NOTE: double-click was the previous ship-mode trigger; now
        // handled by task_shutdown_watcher_fn as a 4-second hold (Stage 7
        // § 3). btn == 2 events from button_poll() are ignored below.

        switch (s_state) {

        case ST_STANDBY: {
            if (enc != 0) {
                int m = (int)s_mode + enc;
                while (m < 0)          m += FCM_COUNT;
                while (m >= FCM_COUNT) m -= FCM_COUNT;
                s_mode = (fc_mode_t)m;
                nvs_save_mode();
                haptic_play(DRV_MEDIUM_CLICK);
                s_last_activity_ms = millis_u32();
                ESP_LOGI(TAG, "mode -> %s", MODE_INFO[s_mode].name);
            }
            if (btn == 1) {
                if (s_mode == FCM_FLASHLIGHT) {
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
                if (s_mode == FCM_COMPASS) {
                    rgb_compass_alt_red_blue();
                } else if (s_mode == FCM_ECG) {
                    rgb_qvar_alt_yellow_purple();
                } else if (s_mode == FCM_TEMP) {
                    rgb_temp_warm_cycle();
                } else if (s_mode == FCM_BCG) {
                    rgb_bcg_alt_yellow_red();
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
