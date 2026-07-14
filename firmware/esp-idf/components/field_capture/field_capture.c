/**
 * @file field_capture.c
 * @brief Port of 7_demo_field_capture. See field_capture.h.
 */

#include "field_capture.h"

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
#define RECORDING_MS           30000
#define VOICE_ANNOT_MS         5000
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
    // as a 2 Hz yellow<->purple alternation to distinguish it visually.
    // Kept here for logging + parity with the enum.
    { "compass", 14, 26,  0 },
    // ECG same story -- rendered as a 2 Hz pink<->red alternation.
    { "ecg",     26,  0,  8 },
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
    while ((millis_u32() - start) < duration_ms && !s_recording_early_end) {
        size_t got = 0;
        if (mic_pdm_read(frame, sizeof(frame), &got, 40) == ESP_OK && got) {
            fwrite(frame, 1, got, f);
            written += got;
        }
        // Pulse red at 1 Hz while capturing (annot + MIC mode).
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
    fprintf(f, "# rtc_start=%s boot=%lu seq=%lu mode=%s\n",
            now, (unsigned long)s_boot_seq, (unsigned long)seq, dir);
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

// ── Ship mode ────────────────────────────────────────────────────────────────
static void abort_recording_and_flush(void) {
    if (s_csv_file) { fflush(s_csv_file); fclose(s_csv_file); s_csv_file = NULL; }
    mic_pdm_stop();
}
static void enter_ship_mode(void) {
    ESP_LOGW(TAG, "SHIP MODE requested (double-click)");
    abort_recording_and_flush();
    if (!broker_battery_hw_alive()) {
        ESP_LOGE(TAG, "BQ not alive -- cannot ship mode");
        return;
    }
    // 2 s red LED countdown, hands off the button.
    for (int i = 0; i < 40; i++) {
        ws2812_set_color(WS_MAX, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (gpio_get_level(PIN_BUTTON) == 0) {
        ESP_LOGI(TAG, "button held at end of countdown -- ship mode aborted");
        rgb_off();
        return;
    }
    bq25619_enter_ship_mode(I2C_NUM_1);
    ESP_LOGW(TAG, "BATFET cmd issued -- power down or USB unplug now");
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

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
static void rgb_compass_alt_yellow_purple(void) {
    // 2 Hz alternation, 250 ms per colour. Yellow (motion) + purple
    // (alarm) merged into one hint that this is the compass/cal mode.
    uint32_t t = millis_u32();
    bool yellow = ((t / 250) % 2) == 0;
    if (yellow) ws2812_set_color(14, 26, 0);
    else        ws2812_set_color( 8, 0, 26);
}
static void rgb_flash_purple(void) {
    // 5 Hz on/off flash during the 10 s figure-8 cal.
    uint32_t t = millis_u32();
    bool on = ((t / 100) % 2) == 0;
    if (on) ws2812_set_color(12, 0, 26);
    else    ws2812_set_color( 0, 0,  0);
}
static void rgb_compass_gradient(float heading_deg) {
    float rad = heading_deg * (float)M_PI / 180.0f;
    float c = cosf(rad);
    uint8_t r = c < 0.0f ? (uint8_t)(-c * (float)WS_MAX) : 0;
    uint8_t b = c > 0.0f ? (uint8_t)( c * (float)WS_MAX) : 0;
    ws2812_set_color(r, 0, b);
}

// True when heading is within +/-5 deg of N (0) or S (180).
static bool heading_on_ns(float heading_deg) {
    float d_n = fminf(heading_deg, 360.0f - heading_deg);
    float d_s = fabsf(heading_deg - 180.0f);
    return d_n < 5.0f || d_s < 5.0f;
}

// 10 s figure-8 hard-iron calibration. Tracks per-axis min/max on X and Y
// (Z is not needed for a flat-worn 2D compass). Aborts early on button.
static void run_mag_cal(void) {
    ESP_LOGI(TAG, "[COMPASS] calibration START -- figure-8 for 10 seconds");
    haptic_play_forced(DRV_MEDIUM_CLICK);   // "get moving" tick
    float min_x =  1e9f, max_x = -1e9f;
    float min_y =  1e9f, max_y = -1e9f;
    uint32_t start = millis_u32();
    bool aborted = false;
    while ((millis_u32() - start) < 10000) {
        rgb_flash_purple();
        broker_mag_data_t m; broker_mag_read(&m);
        if (m.x_ut < min_x) min_x = m.x_ut;
        if (m.x_ut > max_x) max_x = m.x_ut;
        if (m.y_ut < min_y) min_y = m.y_ut;
        if (m.y_ut > max_y) max_y = m.y_ut;
        if (button_poll() == 1) { aborted = true; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (aborted) {
        ESP_LOGW(TAG, "[COMPASS] cal aborted -- press again in compass mode to retry");
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
    ESP_LOGI(TAG, "[COMPASS] cal DONE  off=(%.1f, %.1f) uT  span=(%.1f, %.1f) uT",
             s_mag_offset_x, s_mag_offset_y, rx * 2.0f, ry * 2.0f);
    haptic_play_forced(DRV_LONG_BUZZ);
}

// Live compass -- runs until single button press. LED red<->blue on the
// N-S axis, DRV pulses at 1 Hz whenever heading is within +/-5 deg of
// N or S.
static void run_compass(void) {
    ESP_LOGI(TAG, "[COMPASS] compass ACTIVE (press to exit)");
    uint32_t last_ns_pulse = 0;
    uint32_t last_print    = 0;
    while (1) {
        if (button_poll() == 1) break;
        broker_mag_data_t m; broker_mag_read(&m);
        float nx = (m.x_ut - s_mag_offset_x) / s_mag_scale_x;
        float ny = (m.y_ut - s_mag_offset_y) / s_mag_scale_y;
        // Bearing 0..360 CW-from-N. Flip -ny to +ny if the compass spins
        // the wrong way on the bench (see coordinate note above).
        float heading = atan2f(+ny, nx) * 180.0f / (float)M_PI;
        if (heading < 0.0f) heading += 360.0f;

        rgb_compass_gradient(heading);

        uint32_t now = millis_u32();
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

// LED indicator: 2 Hz alternation between pink (skin palette) and red
// (mic palette). Visually distinct from both individually.
static void rgb_ecg_alt_pink_red(void) {
    uint32_t t = millis_u32();
    bool pink = ((t / 250) % 2) == 0;
    if (pink) ws2812_set_color(26, 0, 12);
    else      ws2812_set_color(26, 0,  0);
}

// 50 Hz IIR biquad notch @ fs=240 Hz, Q=5. Same coefficients as the
// arduino sketch 9_test_ecg. If you're in a 60 Hz-mains country,
// recompute with w0 = 2*pi*60/240.
#define ECG_NOTCH_B0   0.9119f
#define ECG_NOTCH_B1  -0.4721f
#define ECG_NOTCH_B2   0.9119f
#define ECG_NOTCH_A1  -0.4721f
#define ECG_NOTCH_A2   0.8238f

static void run_ecg(void) {
    if (qvar_local_enable() != ESP_OK) {
        ESP_LOGE(TAG, "[ECG] QVAR enable failed -- skipping");
        return;
    }
    ESP_LOGI(TAG, "[ECG] active (50 Hz notch on). Open Arduino Serial Plotter.");
    // Silence our own INFO chatter during the plot -- keeps the plotter
    // input as clean as possible without touching global log level.
    esp_log_level_set(TAG, ESP_LOG_WARN);

    // Baseline + AC envelope for the beat detector.
    float    baseline       = 0.0f;
    bool     baseline_ready = false;
    float    ac_peak        = 0.0f;
    float    ac_prev        = 0.0f;
    uint32_t last_beat_ms   = 0;
    uint32_t start_ms       = millis_u32();
    uint32_t last_led_tick  = 0;
    // Notch filter state
    float    nx1 = 0.0f, nx2 = 0.0f, ny1 = 0.0f, ny2 = 0.0f;

    while (1) {
        if (button_poll() == 1) break;

        int16_t raw = 0;
        if (qvar_reg_read_word(ECG_REG_OUT_AH_L, &raw) == ESP_OK) {
            // ── Apply 50 Hz notch first ─────────────────────────────────
            float xin = (float)raw;
            float notched = ECG_NOTCH_B0 * xin + ECG_NOTCH_B1 * nx1 + ECG_NOTCH_B2 * nx2
                          - ECG_NOTCH_A1 * ny1 - ECG_NOTCH_A2 * ny2;
            nx2 = nx1; nx1 = xin;
            ny2 = ny1; ny1 = notched;

            // ── 1) Serial-Plotter feed: emit notched value ─────────────
            printf("%d\n", (int)notched);

            // ── 2) Beat detector runs on notched signal ────────────────
            uint32_t now     = millis_u32();
            uint32_t elapsed = now - start_ms;
            float    x       = notched;
            if (baseline == 0.0f) baseline = x;
            float alpha = (elapsed < 3000) ? 0.05f : 0.005f;
            baseline = baseline * (1.0f - alpha) + x * alpha;
            if (elapsed >= 3000) baseline_ready = true;
            float ac     = x - baseline;
            float ac_abs = fabsf(ac);
            if (ac_abs > ac_peak) ac_peak = ac_abs;
            else                  ac_peak *= 0.9995f;

            if (baseline_ready) {
                float thresh = ac_peak * 0.5f;
                if (thresh < 50.0f) thresh = 50.0f;
                if (ac_prev > ac && ac_prev > thresh &&
                    (now - last_beat_ms) > 300) {
                    last_beat_ms = now;
                    haptic_play_forced(DRV_STRONG_CLICK);
                }
            }
            ac_prev = ac;
        }

        // Refresh the LED alternation ~20 Hz so the pink/red flash stays
        // smooth without spamming ws2812_set_color() every 4 ms.
        uint32_t now_ms = millis_u32();
        if ((now_ms - last_led_tick) >= 50) {
            last_led_tick = now_ms;
            rgb_ecg_alt_pink_red();
        }

        vTaskDelay(pdMS_TO_TICKS(4));   // ~250 Hz poll (driver ODR 240 Hz)
    }

    esp_log_level_set(TAG, ESP_LOG_INFO);
    qvar_local_disable();
    ESP_LOGI(TAG, "[ECG] exit -> STANDBY");
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
    }

    switch (s_mode) {
    case FCM_MIC: {
        char path[96];
        snprintf(path, sizeof(path), "/sd/data/mic/s%04lu_r%04lu.wav",
                 (unsigned long)s_boot_seq, (unsigned long)s_rec_seq);
        rgb_set_max(FCM_MIC);
        wav_record_to(path, RECORDING_MS);
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
        while ((millis_u32() - start) < RECORDING_MS && !s_recording_early_end) {
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
        ESP_LOGI(TAG, "CSV closed (%s)", s_recording_early_end ? "early" : "full");
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
void task_shutdown_watcher_fn(void *arg) {
    (void)arg;

    // Redundantly configure the pin (field_capture_init also does this;
    // gpio_config is idempotent when values match).
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    typedef enum { W_IDLE, W_PRESSED, W_WAIT_DBL, W_PRESSED_2 } w_state_t;
    w_state_t state       = W_IDLE;
    uint32_t  last_change = 0;
    uint32_t  release_ms  = 0;
    bool      prev_low    = (gpio_get_level(PIN_BUTTON) == 0);

    ESP_LOGI(TAG, "shutdown watcher armed (double-click = priority ship mode)");

    for (;;) {
        uint32_t now = millis_u32();
        bool     low = (gpio_get_level(PIN_BUTTON) == 0);

        if (low != prev_low && (now - last_change) >= BTN_DEBOUNCE_MS) {
            last_change = now;
            prev_low    = low;
            if (low) {
                state = (state == W_WAIT_DBL) ? W_PRESSED_2 : W_PRESSED;
            } else {
                if (state == W_PRESSED) {
                    release_ms = now;
                    state = W_WAIT_DBL;
                } else if (state == W_PRESSED_2) {
                    state = W_IDLE;
                    watcher_ship_mode();   // never returns
                }
            }
        }
        if (state == W_WAIT_DBL && (now - release_ms) > BTN_DOUBLE_GAP_MS) {
            // Single click detected -- ignored here; field_capture's own
            // button_poll() picks it up and dispatches per mode.
            state = W_IDLE;
        }
        vTaskDelay(pdMS_TO_TICKS(5));   // fast poll, ~200 Hz
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

        if (btn == 2) {
            enter_ship_mode();       // does not return
        }

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
                } else {
                    s_state = ST_RECORDING;
                    run_recording_for_current_mode();
                    s_state = ST_STANDBY;
                }
                s_last_activity_ms = millis_u32();
            }
            // LED animation. FCM_COMPASS and FCM_ECG each use a distinct
            // 2 Hz alternation so the operator can tell them apart from the
            // standard solid-then-pulse modes at a glance.
            if (s_mode == FCM_COMPASS) {
                rgb_compass_alt_yellow_purple();
            } else if (s_mode == FCM_ECG) {
                rgb_ecg_alt_pink_red();
            } else {
                uint32_t solid_end = s_last_activity_ms + SOLID_ON_ACTIVITY_MS;
                if (millis_u32() < solid_end) rgb_set_max(s_mode);
                else                          rgb_pulse(s_mode, solid_end, PULSE_STANDBY_MS);
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
