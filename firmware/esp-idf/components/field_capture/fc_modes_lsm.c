/**
 * @file fc_modes_lsm.c
 * @brief LSM6DSV16X-driven interactive modes: BCG, Steps, MLC_COLLECT, TAP_DBG.
 *
 * Split out of field_capture.c in the Stage 12 refactor. All four modes share
 * the IMU + broker_imu pipeline; keeping them together makes the coupling
 * visible without swelling fc_modes.c past the 1k-line cap.
 *
 * SD durability:
 *   Steps  -- PATTERN A (small file, fflush per row, fclose on exit).
 *   MLC    -- PATTERN B (fflush + fsync per row, high rate).
 *   BCG    -- fflush + fclose on exit (short bench sessions).
 */

#include "fc_internal.h"
#include "firmware_version.h"

#include <math.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "data_broker.h"
#include "ws2812.h"
#include "haptic.h"
#include "lsm6dsv16x.h"

static const char *TAG = "FC_MODES_LSM";

// ═════════════════════════════════════════════════════════════════════════════
// FCM_BCG -- ballistocardiography on LSM6DSV16X accelerometer
// ═════════════════════════════════════════════════════════════════════════════
#define BCG_HPF_ALPHA        0.9745f
#define BCG_LPF_ALPHA        0.2822f
#define BCG_REFRACTORY_MS     400
#define BCG_THRESH_FLOOR     0.003f
#define BCG_PLOTTER_MODE      0
#define BCG_BPM_MEDIAN_N      5
#define BCG_STALE_MS       2000

static void rgb_bcg_alt_yellow_red(void) {
    uint32_t t = millis_u32();
    bool yellow = ((t / 250) % 2) == 0;
    if (yellow) ws2812_set_color(26, 26, 0);
    else        ws2812_set_color(26,  0, 0);
}

void run_bcg(void) {
#if BCG_PLOTTER_MODE
    ESP_LOGI(TAG, "[BCG] active (PLOTTER mode). plot: filtered_mg  beat_marker. exit: click.");
    esp_log_level_set(TAG, ESP_LOG_WARN);
#else
    ESP_LOGI(TAG, "[BCG] active (MONITOR mode). recording -> SD until click.");
#endif

    s_rec_seq++;
    FILE *csv = csv_open("bcg", s_rec_seq, "time_ms,accel_z_g,filt_mg,beat");
    uint32_t rows_written = 0;

    float hp_prev_x = 0.0f, hp_prev_y = 0.0f, lp_prev_y = 0.0f;
    float envelope  = 0.0f, prev_af   = 0.0f;
    uint32_t last_beat_ms  = 0;
    uint32_t last_led_tick = 0;
    uint32_t last_status_ms = 0;
    uint32_t total_beats   = 0;

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

        vTaskDelay(pdMS_TO_TICKS(4));
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

void run_bcg_mode(void) {
    wake_sensors_for_mode(FCM_BCG);
    vTaskDelay(pdMS_TO_TICKS(200));
    run_bcg();
    park_all_modal_sensors();
}

// ═════════════════════════════════════════════════════════════════════════════
// FCM_STEPS -- pedometer session (live display + CSV)
// ═════════════════════════════════════════════════════════════════════════════
void run_steps_mode(void) {
    ESP_LOGI(TAG, "STEPS session start");
    ensure_sd();

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
            rgb_set_max(FCM_LSM);
        } else {
            rgb_set_max(FCM_STEPS);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (f) fclose(f);
    ESP_LOGI(TAG, "STEPS session end");
}

// ═════════════════════════════════════════════════════════════════════════════
// FCM_MLC_COLLECT -- raw accel+gyro training-data collector.
// Two-phase: LABEL_PICK (encoder cycles label, click confirms), then RECORDING
// (encoder locked, click = mark, 2 s hold = end + reset watcher press timer).
// ═════════════════════════════════════════════════════════════════════════════
#define MLC_LABEL_COUNT      4
#define MLC_HOLD_STOP_MS  2000
#define MLC_PICK_LED_DIVIDER  3

static const uint8_t MLC_LABEL_COLORS[MLC_LABEL_COUNT][3] = {
    { WS_MAX,   WS_MAX,   WS_MAX },   // 0 white  (still)
    { WS_MAX,   WS_MAX,   0      },   // 1 yellow (walking)
    { WS_MAX,   0,        0      },   // 2 red    (running)
    { WS_MAX/2, 0,        WS_MAX },   // 3 purple (other)
};

static void mlc_haptic_pulses(uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        haptic_play_forced(DRV_STRONG_CLICK);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}
static void mlc_led_for_label(uint8_t label, uint8_t divider) {
    if (g_shutdown_hold_active) return;
    ws2812_set_color(MLC_LABEL_COLORS[label][0] / divider,
                     MLC_LABEL_COLORS[label][1] / divider,
                     MLC_LABEL_COLORS[label][2] / divider);
}

void run_mlc_collect_mode(void) {
    ESP_LOGI(TAG, "MLC_COLLECT: LABEL_PICK phase -- encoder cycles label, "
                  "short-click confirms + starts recording, hold 2 s exits");
    ensure_sd();
    wake_sensors_for_mode(FCM_MLC_COLLECT);
    vTaskDelay(pdMS_TO_TICKS(120));

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

    mlc_led_for_label(label, MLC_PICK_LED_DIVIDER);
    ESP_LOGI(TAG, "MLC label -> 0 (pulses=1)  [pick]");

    while (!exit_mode) {
        uint32_t now = millis_u32();

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
                haptic_play(DRV_MEDIUM_CLICK);
                ESP_LOGD(TAG, "MLC encoder ignored (locked while recording)");
            }
        }

        bool now_low = (gpio_get_level(PIN_BUTTON) == 0);
        if (now_low && !btn_low) {
            press_start_ms   = now;
            press_hold_fired = false;
            btn_low = true;
        } else if (!now_low && btn_low) {
            uint32_t held = now - press_start_ms;
            if (!press_hold_fired && held < MLC_HOLD_STOP_MS) {
                if (phase == MLC_PICK) {
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
                    mlc_haptic_pulses(3);
                    mlc_led_for_label(label, 1);
                    ESP_LOGI(TAG, "MLC_COLLECT: RECORDING label=%u -- click=mark, "
                                  "hold 2 s to end", (unsigned)label);
                } else {
                    mark_next_row = true;
                    marks++;
                    haptic_play_forced(DRV_STRONG_CLICK);
                    ESP_LOGI(TAG, "MLC MARK #%u at t=%lu ms",
                             (unsigned)marks, (unsigned long)(now - start_ms));
                }
            }
            btn_low = false;
        } else if (now_low && btn_low) {
            if (!press_hold_fired && (now - press_start_ms) >= MLC_HOLD_STOP_MS) {
                press_hold_fired = true;
                exit_mode        = true;
                haptic_play_forced(DRV_LONG_BUZZ);
                if (phase == MLC_REC) {
                    g_watcher_press_reset_pending = true;
                    ESP_LOGI(TAG, "MLC_COLLECT: RECORDING end (%lu rows, %u marks, "
                                  "label=%u) -- watcher press reset",
                             (unsigned long)rows, (unsigned)marks, (unsigned)label);
                } else {
                    ESP_LOGI(TAG, "MLC_COLLECT: exit LABEL_PICK (no recording)");
                }
            }
        }

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
                // PATTERN B (durability rule) -- fflush + fsync every row.
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

    while (gpio_get_level(PIN_BUTTON) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    g_recording_active = false;

    if (f) fclose(f);
    park_all_modal_sensors();
    ESP_LOGI(TAG, "MLC_COLLECT: mode exit");
}

// ═════════════════════════════════════════════════════════════════════════════
// FCM_TAP_DBG -- tap debug + host-side magnitude detector
// ═════════════════════════════════════════════════════════════════════════════
void run_tap_dbg_mode(void) {
    // Wake the IMU explicitly -- prior modes (BCG/ECG) leave it parked.
    wake_sensors_for_mode(FCM_BCG);   // BCG wakes broker_imu; TAP_DBG needs same
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "TAP_DBG start -- magnitude-based tap detector + chip-tap echo. "
                  "single-click to exit.");

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
        if (button_poll() == 1) break;

        uint32_t now = millis_u32();

        broker_imu_data_t im;
        broker_imu_read(&im);

        float pitch = atan2f(-im.accel_x,
                             sqrtf(im.accel_y * im.accel_y + im.accel_z * im.accel_z))
                      * (180.0f / (float)M_PI);
        float roll  = atan2f(im.accel_y, im.accel_z) * (180.0f / (float)M_PI);
        bool in_zone = (pitch >= pitch_min && pitch <= pitch_max &&
                        roll  >= -roll_lim && roll <= roll_lim);

        float mag = sqrtf(im.accel_x * im.accel_x +
                          im.accel_y * im.accel_y +
                          im.accel_z * im.accel_z);
        baseline_mag = baseline_mag * (1.0f - mag_alpha) + mag * mag_alpha;
        float hp = mag - baseline_mag;

        float absx = fabsf(im.accel_x), absy = fabsf(im.accel_y), absz = fabsf(im.accel_z);
        if (absx > peak_ax) peak_ax = absx; else peak_ax *= 0.985f;
        if (absy > peak_ay) peak_ay = absy; else peak_ay *= 0.985f;
        if (absz > peak_az) peak_az = absz; else peak_az *= 0.985f;

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
