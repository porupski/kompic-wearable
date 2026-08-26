/**
 * @file fc_modes.c
 * @brief Interactive modes: Compass, ECG/QVAR, Temperature aggregate.
 *
 * Split out of field_capture.c in the Stage 12 refactor. LSM-submenu modes
 * (BCG, Steps, MLC_COLLECT, TAP_DBG) live in fc_modes_lsm.c to keep this
 * file under the 1k-line cap.
 *
 * Owns the four mode-signature LED animations that field_capture.c's main
 * loop renders in ST_STANDBY:
 *   rgb_compass_alt_red_blue    -- for FCM_COMPASS
 *   rgb_qvar_alt_yellow_purple  -- for FCM_ECG
 *   rgb_temp_warm_cycle         -- for FCM_TEMP
 *   rgb_lsm_submenu_indicator   -- for the LSM-submenu latch
 *
 * Owns the inline QVAR block (worked around a link-order bug in the
 * qvar_ecg component -- see notes below).
 */

#include "fc_internal.h"

#include <math.h>
#include <string.h>

#include "driver/i2c.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "data_broker.h"
#include "ws2812.h"
#include "haptic.h"
#include "qvar_ecg.h"

static const char *TAG = "FC_MODES";

// ── Inline QVAR block (bypass qvar_ecg component due to link-order bug) ─────
// Bit-layout fixes verified on-bench with firmware/arduino/9_test_ecg:
//   * AH_QVAR_EN is CTRL7 bit 7 (0x80), NOT bit 0 (0x01) as the qvar_ecg.h
//     macro claims. The old value was actually enabling LPF1_G_EN (a
//     gyro filter), so the analog hub never turned on.
//   * CTRL1 layout is OP_MODE_XL[6:4] + ODR_XL[3:0]. Do not use the
//     lsm6dsv16x.h CTRL1_ODR_* macros -- they have the fields reversed
//     and would drop the accel into low-power mode.
#define ECG_LSM_ADDR            0x6B
#define ECG_REG_CTRL1           0x10
#define ECG_REG_CTRL2           0x11
#define ECG_REG_CTRL7           0x16
#define ECG_REG_OUT_AH_L        0x3A
#define ECG_CTRL7_AH_QVAR_EN    (1 << 7)
#define ECG_CTRL7_ZIN_2G4       (0x00 << 4)
#define ECG_CTRL1_HP_240HZ      0x07

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
    r = qvar_reg_write(ECG_REG_CTRL1, 0x00);
    if (r != ESP_OK) return r;
    r = qvar_reg_write(ECG_REG_CTRL2, 0x00);
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
    return qvar_reg_write(ECG_REG_CTRL7, 0x00);
}

// ── Compass mode ────────────────────────────────────────────────────────────
//
// Coordinate assumption: with the watch flat and the LIS3MDL "dot" corner
// at top-right, the chip's +X axis points AWAY from the wearer (forward).
// heading is computed with atan2f(-y, x) so that CW rotation of the watch
// (looking down) increases the reported bearing 0-360. If the compass spins
// the wrong way for you on the bench, flip the -y to +y in run_compass().

void rgb_compass_alt_red_blue(void) {
    // 2 Hz alternation, 250 ms per colour. Same red/blue palette as the
    // live compass gradient -- the alternation just distinguishes "waiting
    // to enter compass" from "in compass mode".
    uint32_t t = millis_u32();
    bool red = ((t / 250) % 2) == 0;
    if (red) ws2812_set_color(26, 0,  0);
    else     ws2812_set_color( 0, 0, 26);
}
static void rgb_flash_purple(void) {
    uint32_t t = millis_u32();
    bool on = ((t / 100) % 2) == 0;
    if (on) ws2812_set_color(12, 0, 26);
    else    ws2812_set_color( 0, 0,  0);
}
static void rgb_compass_gradient(float heading_deg) {
    float rad = heading_deg * (float)M_PI / 180.0f;
    float c   = cosf(rad);
    uint8_t r = c > 0.0f ? (uint8_t)( c * (float)WS_MAX) : 0;
    uint8_t b = c < 0.0f ? (uint8_t)(-c * (float)WS_MAX) : 0;
    ws2812_set_color(r, 0, b);
}
static bool heading_on_ns(float heading_deg) {
    float d_n = fminf(heading_deg, 360.0f - heading_deg);
    float d_s = fabsf(heading_deg - 180.0f);
    return d_n < 5.0f || d_s < 5.0f;
}

// 10 s figure-8 hard-iron calibration.
#define MAG_CAL_MAX_ABS_UT   500.0f
void run_mag_cal(void) {
    ESP_LOGI(TAG, "[COMPASS] calibration START -- figure-8 for 10 seconds");
    haptic_play_forced(DRV_MEDIUM_CLICK);
    float min_x =  1e9f, max_x = -1e9f;
    float min_y =  1e9f, max_y = -1e9f;
    uint32_t rejected = 0;
    uint32_t accepted = 0;
    uint32_t start = millis_u32();
    bool aborted = false;
    while ((millis_u32() - start) < 10000) {
        rgb_flash_purple();
        broker_mag_data_t m; broker_mag_read(&m);

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
    s_mag_scale_x = (rx > 1.0f) ? rx : 1.0f;
    s_mag_scale_y = (ry > 1.0f) ? ry : 1.0f;
    s_mag_cal_done = true;
    ESP_LOGI(TAG, "[COMPASS] cal DONE  off=(%.1f, %.1f) uT  span=(%.1f, %.1f) uT  (%u kept / %u rejected)",
             s_mag_offset_x, s_mag_offset_y, rx * 2.0f, ry * 2.0f,
             (unsigned)accepted, (unsigned)rejected);
    haptic_play_forced(DRV_LONG_BUZZ);
}

void run_compass(void) {
    ESP_LOGI(TAG, "[COMPASS] compass ACTIVE  (single-click: exit)");
    uint32_t last_ns_pulse = 0;
    uint32_t last_print    = 0;
    while (1) {
        if (button_poll() == 1) break;
        uint32_t now = millis_u32();

        broker_mag_data_t m; broker_mag_read(&m);
        float nx = (m.x_ut - s_mag_offset_x) / s_mag_scale_x;
        float ny = (m.y_ut - s_mag_offset_y) / s_mag_scale_y;
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

void run_compass_or_cal(void) {
    wake_sensors_for_mode(FCM_COMPASS);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!s_mag_cal_done) run_mag_cal();
    if (s_mag_cal_done)  run_compass();
    park_all_modal_sensors();
}

// ── ECG mode (QVAR touch demo) ──────────────────────────────────────────────
void rgb_qvar_alt_yellow_purple(void) {
    uint32_t t = millis_u32();
    bool yellow = ((t / 250) % 2) == 0;
    if (yellow) ws2812_set_color(26, 26,  0);
    else        ws2812_set_color(14,  0, 26);
}

void rgb_temp_warm_cycle(void) {
    // Fire strobing -- red-orange-yellow palette cycle with per-frame
    // pseudorandom brightness jitter (~5 Hz palette, ~30 Hz jitter).
    uint32_t t = millis_u32();
    uint32_t h = (t / 32) * 2654435761U;
    uint8_t  jitter = (uint8_t)((h >> 26) & 0x0F);
    uint8_t  amp    = 18 + jitter;
    if (amp > WS_MAX) amp = WS_MAX;
    uint32_t phase = (t / 200) % 3;
    uint8_t  g_full;
    switch (phase) {
    case 0:  g_full = 0;         break;
    case 1:  g_full = amp / 4;   break;
    default: g_full = amp / 2;   break;
    }
    ws2812_set_color(amp, g_full, 0);
}

void rgb_lsm_submenu_indicator(fc_mode_t sub) {
    // Half-second alternation between LSM yellow and the currently-selected
    // submode's color.
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

// QVAR button demo (see full physics comment in the original file / porting
// notes). Three observable states: idle sawtooth, one-electrode swings,
// both-electrode railed. 1st-order high-pass filter at ~2 Hz kills the
// idle sawtooth so only the AC content of a real touch reaches the envelope.
#define QVAR_HPF_ALPHA          0.952f
#define QVAR_ONSET_THRESH_RAW   800.0f
#define QVAR_TOUCH_THRESH_RAW   400.0f
#define QVAR_ENV_DECAY          0.985f
#define QVAR_RAIL_ABS_MIN       15000.0f
#define QVAR_QUIET_ENV_MAX      150.0f
#define QVAR_RAW_MEAN_ALPHA     0.02f
#define QVAR_PLOTTER_MODE   0

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

void run_ecg(void) {
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
    float    raw_mean    = 0.0f;
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

            raw_mean = raw_mean + QVAR_RAW_MEAN_ALPHA * (xin - raw_mean);

            if (abs_y > QVAR_ONSET_THRESH_RAW) {
                last_sign = (y > 0.0f) ? +1 : -1;
            }

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

        vTaskDelay(pdMS_TO_TICKS(4));
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

// FCM_ECG single click: park the IMU (so its task doesn't stomp CTRL1/CTRL7),
// enable QVAR, stream+detect beats, park on exit.
void run_ecg_session(void) {
    broker_imu_set_enabled(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    run_ecg();
    // Leave IMU parked -- matches boot policy.
}

// ═════════════════════════════════════════════════════════════════════════════
// FCM_TEMP -- aggregate thermal map
// ═════════════════════════════════════════════════════════════════════════════

// LSM6DSV16X OUT_TEMP: signed 16-bit at 256 LSB/degC with +25 degC zero offset.
#define LSM_ADDR              0x6B
#define LSM_REG_OUT_TEMP_L    0x20

// MAX30101 die-temp: write 1 to TEMP_CONFIG (0x21), wait ~30 ms, read
// TINT + TFRAC. Chip must be awake (not SHDN=1) or the ADC won't update.
#define MAX_ADDR              0x57
#define MAX_REG_MODE_CONFIG   0x09
#define MAX_REG_LED1_PA       0x0C
#define MAX_REG_LED2_PA       0x0D
#define MAX_REG_LED3_PA       0x0E
#define MAX_REG_TEMP_INT      0x1F
#define MAX_REG_TEMP_FRAC     0x20
#define MAX_REG_TEMP_CONFIG   0x21
#define MAX_MODE_HR           0x02
#define MAX_MODE_SHDN         0x80

float read_lsm_die_temp(void) {
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

float read_max_die_temp(void) {
    // Wake with LED currents zeroed, trigger one-shot, read TINT/TFRAC,
    // return to shutdown.
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

    vTaskDelay(pdMS_TO_TICKS(5));

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return -273.15f;
    uint8_t buf_trig[] = { MAX_REG_TEMP_CONFIG, 0x01 };
    i2c_master_write_to_device(I2C_NUM_0, MAX_ADDR, buf_trig, 2, pdMS_TO_TICKS(20));
    xSemaphoreGive(g_i2c_mutex);

    vTaskDelay(pdMS_TO_TICKS(35));

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

void run_temp_session(void) {
    ESP_LOGI(TAG, "[TEMP] active. single-click to exit.");

    esp_ts_ensure_init();

    uint32_t last_print = 0;
    while (1) {
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

            (void)im;   // broker imu_data.temperature is also available; direct read is fresher.
            printf("[TEMP] skin(TMP117)=%.2fC  air(BME688)=%.2fC  imu(LSM)=%.1fC  ppg(MAX)=%.1fC  soc(ESP32)=%.1fC\n",
                   s.skin_temp_c, e.temperature_c, t_lsm, t_max, t_soc);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "[TEMP] exit -> STANDBY");
}

void run_temp_mode(void) {
    wake_sensors_for_mode(FCM_TEMP);
    vTaskDelay(pdMS_TO_TICKS(200));
    run_temp_session();
    park_all_modal_sensors();
}
