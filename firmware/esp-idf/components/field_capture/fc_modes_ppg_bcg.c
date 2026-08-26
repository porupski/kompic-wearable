/**
 * @file fc_modes_ppg_bcg.c
 * @brief FCM_PPG_BCG -- combined raw PPG + BCG recording (Stage 17).
 *
 * Repurposes the old FCM_SKIN enum slot (see field_capture.h). Implements the
 * data-collection target from Stage 15 §CSV: interleaved rows at a 200 Hz
 * tick, `src=ppg` for MAX30101 multi-LED FIFO samples, `src=bcg` for LSM
 * accel-Z filtered ballistocardiography, both timestamped in Kompic-local µs
 * plus PC-aligned µs (t_local_us + fc_sync offset).
 *
 * Sync stickiness surfaces two ways:
 *   1. Header block via fc_sync_write_csv_headers() -- carries pc_sync_applied=0|1.
 *   2. Mode-start ESP_LOGI banner -- so bench operator sees YES/NO at flash time.
 *
 * The BCG detector state matches run_bcg() in fc_modes_lsm.c (HPF+LPF+envelope
 * + refractory + median-of-5 BPM); it is inlined here rather than shared to
 * avoid coupling run_bcg's mode wrapper into this file.
 *
 * PPG:
 *   MAX30101 in MULTI_LED (Red=0, IR=0, Green PA=0x2A) at SR=100 Hz, SMP_AVE=8
 *   -> ~12.5 Hz effective FIFO rate. HR task is parked while we drive the chip
 *   directly to avoid FIFO drain contention. Green PA is a first-pass value --
 *   tune per bench feedback.
 *
 * BCG:
 *   Reads broker_imu_data_t on every tick. Runs the same HPF/LPF/envelope
 *   detector as run_bcg().
 *
 * Recording ends on button click. No REC_DURATION timer yet.
 */

#include "fc_internal.h"
#include "firmware_version.h"

#include <math.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "data_broker.h"
#include "ws2812.h"
#include "max30101.h"

static const char *TAG = "FC_PPG_BCG";

// ── BCG detector constants (mirror run_bcg() in fc_modes_lsm.c) ─────────────
#define BCG_HPF_ALPHA        0.9745f
#define BCG_LPF_ALPHA        0.2822f
#define BCG_REFRACTORY_MS     400u
#define BCG_THRESH_FLOOR     0.003f
#define BCG_BPM_MEDIAN_N        5
#define BCG_STALE_MS         2000u

// ── PPG / MAX30101 config ───────────────────────────────────────────────────
#define PPG_GREEN_PA         0x2Au   // ~8.4 mA per LED (first-pass; tune)
#define PPG_RED_PA           0x00u
#define PPG_IR_PA            0x00u
#define PPG_MAX_DRAIN        8u      // cap FIFO drain per tick to bound I2C

#define STATUS_LOG_MS        1000u
#define FSYNC_PERIOD_MS      1000u

void run_ppg_bcg_mode(void)
{
    ESP_LOGI(TAG, "PPG+BCG combined raw session start");

    // ── 1. Enable IMU broker + belt-and-suspenders park HR task ──────────────
    wake_sensors_for_mode(FCM_PPG_BCG);
    broker_hr_set_enabled(false);   // we drive MAX30101 directly
    vTaskDelay(pdMS_TO_TICKS(200));

    // ── 2. Configure MAX30101: MULTI_LED, green only, out of shutdown ────────
    esp_err_t cfg_ret = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        (void)max30101_set_shutdown(MAX30101_I2C_PORT, false);
        cfg_ret = max30101_setup_multi_led_mode(MAX30101_I2C_PORT,
                                                 PPG_RED_PA, PPG_IR_PA,
                                                 PPG_GREEN_PA);
        (void)max30101_clear_fifo(MAX30101_I2C_PORT);
        xSemaphoreGive(g_i2c_mutex);
    }
    if (cfg_ret != ESP_OK) {
        ESP_LOGW(TAG, "MAX30101 setup failed (%s) -- BCG-only recording",
                 esp_err_to_name(cfg_ret));
    }

    // ── 3. Open CSV + write sync headers ─────────────────────────────────────
    s_rec_seq++;
    FILE *csv = csv_open("ppgbcg", s_rec_seq,
                         "t_local_us,t_pc_us,src,raw,filt,beat,bpm");
    if (csv) {
        fc_sync_write_csv_headers(csv);
        fprintf(csv,
                "# ppg_led_pa_green=0x%02X ppg_sr_hz=100 ppg_smp_ave=8 "
                "bcg_hpf_alpha=%.4f bcg_lpf_alpha=%.4f\n",
                PPG_GREEN_PA, (double)BCG_HPF_ALPHA, (double)BCG_LPF_ALPHA);
        fflush(csv);
    } else {
        ESP_LOGW(TAG, "csv_open failed -- serial-only session");
    }

    // ── 4. Mode-start banner (Ivan's "did sync stick the landing" check) ─────
    bool    synced      = fc_sync_offset_valid();
    int64_t offset_us   = fc_sync_get_offset_us();
    int64_t step_us     = fc_sync_get_step_us();
    bool    step_host   = fc_sync_step_host_set();
    ESP_LOGI(TAG,
             "[PPG_BCG] start: pc_sync=%s offset_us=%" PRId64
             " step_us=%" PRId64 " step_host_set=%d pa_green=0x%02X",
             synced ? "YES" : "NO", offset_us, step_us,
             step_host ? 1 : 0, PPG_GREEN_PA);

    // ── 5. State: BCG detector ──────────────────────────────────────────────
    float    hp_prev_x = 0.0f, hp_prev_y = 0.0f, lp_prev_y = 0.0f;
    float    envelope  = 0.0f, prev_af   = 0.0f;
    uint32_t bcg_last_beat_ms = 0;
    uint32_t bcg_intervals[BCG_BPM_MEDIAN_N] = {0};
    uint8_t  bcg_int_idx    = 0;
    uint8_t  bcg_int_filled = 0;
    float    bcg_bpm_last   = 0.0f;
    uint32_t bcg_beats      = 0;

    // ── 6. State: PPG beat detector (existing max30101 CPU-only helper) ─────
    max30101_beat_detector_t ppg_det;
    max30101_beat_detector_init(&ppg_det);
    uint32_t ppg_beats = 0;

    // ── 7. Tick loop ────────────────────────────────────────────────────────
    int64_t   session_start_us = esp_timer_get_time();
    TickType_t next_tick       = xTaskGetTickCount();
    TickType_t tick_period     = pdMS_TO_TICKS((uint32_t)(step_us / 1000LL));
    if (tick_period == 0) tick_period = 1;   // guard 1 kHz+ config

    uint32_t rows_written    = 0;
    uint32_t last_status_ms  = 0;
    uint32_t last_fsync_ms   = 0;

    // Solid magenta at ~20 % (bench feedback 2026-08-26: blinking is
    // distracting during a rest recording). One-shot color set; no per-tick
    // LED work in the hot loop.
    ws2812_set_color(52, 0, 24);

    while (1) {
        vTaskDelayUntil(&next_tick, tick_period);
        if (button_poll() == 1) break;

        int64_t now_us    = esp_timer_get_time();
        int64_t t_local   = now_us - session_start_us;
        int64_t t_pc      = synced ? (t_local + offset_us) : t_local;
        uint32_t now_ms   = (uint32_t)(now_us / 1000LL);

        // ── BCG row ─────────────────────────────────────────────────────────
        broker_imu_data_t im; broker_imu_read(&im);
        float x = im.accel_z / 9.81f;

        float y_hp = BCG_HPF_ALPHA * (hp_prev_y + x - hp_prev_x);
        hp_prev_x = x; hp_prev_y = y_hp;
        float y_lp = BCG_LPF_ALPHA * y_hp + (1.0f - BCG_LPF_ALPHA) * lp_prev_y;
        lp_prev_y = y_lp;

        float filt = y_lp;
        float af   = fabsf(filt);
        envelope *= 0.998f;
        if (af > envelope) envelope = af;
        float thresh = envelope * 0.4f;
        if (thresh < BCG_THRESH_FLOOR) thresh = BCG_THRESH_FLOOR;

        int bcg_beat = 0;
        if (prev_af > af && prev_af > thresh &&
            (now_ms - bcg_last_beat_ms) > BCG_REFRACTORY_MS) {
            if (bcg_last_beat_ms != 0) {
                uint32_t interval = now_ms - bcg_last_beat_ms;
                bcg_intervals[bcg_int_idx] = interval;
                bcg_int_idx = (bcg_int_idx + 1) % BCG_BPM_MEDIAN_N;
                if (bcg_int_filled < BCG_BPM_MEDIAN_N) bcg_int_filled++;

                uint32_t sorted[BCG_BPM_MEDIAN_N];
                for (int i = 0; i < bcg_int_filled; i++) sorted[i] = bcg_intervals[i];
                for (int i = 1; i < bcg_int_filled; i++) {
                    uint32_t k = sorted[i]; int j = i - 1;
                    while (j >= 0 && sorted[j] > k) { sorted[j+1] = sorted[j]; j--; }
                    sorted[j+1] = k;
                }
                uint32_t med = sorted[bcg_int_filled / 2];
                bcg_bpm_last = (med > 0) ? (60000.0f / (float)med) : 0.0f;
            }
            bcg_last_beat_ms = now_ms;
            bcg_beats++;
            bcg_beat = 1;
        }
        prev_af = af;

        if (bcg_last_beat_ms != 0 && (now_ms - bcg_last_beat_ms) > BCG_STALE_MS) {
            bcg_bpm_last   = 0.0f;
            bcg_int_filled = 0;
            bcg_int_idx    = 0;
        }

        if (csv) {
            fprintf(csv, "%" PRId64 ",%" PRId64 ",bcg,%.4f,%.4f,%d,%.1f\n",
                    t_local, t_pc, (double)x, (double)filt,
                    bcg_beat, (double)bcg_bpm_last);
            rows_written++;
        }

        // ── PPG rows (drain FIFO opportunistically) ────────────────────────
        if (cfg_ret == ESP_OK) {
            if (xSemaphoreTake(g_i2c_mutex, 0) == pdTRUE) {
                uint8_t fifo_avail = 0;
                (void)max30101_get_fifo_available(MAX30101_I2C_PORT, &fifo_avail);
                for (uint8_t k = 0; k < fifo_avail && k < PPG_MAX_DRAIN; k++) {
                    max30101_sample_t samp;
                    if (max30101_read_fifo(MAX30101_I2C_PORT, &samp, true) != ESP_OK) break;
                    if (!samp.valid) break;
                    bool beat = max30101_check_for_beat(&ppg_det, samp.green, now_ms);
                    if (beat) ppg_beats++;
                    if (csv) {
                        fprintf(csv, "%" PRId64 ",%" PRId64 ",ppg,%" PRIu32 ",,%d,%u\n",
                                t_local, t_pc,
                                samp.green, beat ? 1 : 0,
                                (unsigned)ppg_det.bpm);
                        rows_written++;
                    }
                }
                xSemaphoreGive(g_i2c_mutex);
            }
        }

        // LED: solid 20 % magenta set once outside the loop -- see above.

        // ── Periodic fsync so a power-drop doesn't ghost the whole session ──
        if (csv && (now_ms - last_fsync_ms) >= FSYNC_PERIOD_MS) {
            last_fsync_ms = now_ms;
            fflush(csv);
            (void)fsync(fileno(csv));
        }

        // ── Periodic status ─────────────────────────────────────────────────
        if ((now_ms - last_status_ms) >= STATUS_LOG_MS) {
            last_status_ms = now_ms;
            uint32_t elapsed_s = (uint32_t)((now_us - session_start_us) / 1000000LL);
            ESP_LOGI(TAG,
                     "[PPG_BCG] t=%us bcg_bpm=%.0f ppg_bpm=%u "
                     "bcg_beats=%u ppg_beats=%u rows=%u",
                     (unsigned)elapsed_s, (double)bcg_bpm_last,
                     (unsigned)ppg_det.bpm,
                     (unsigned)bcg_beats, (unsigned)ppg_beats,
                     (unsigned)rows_written);
        }
    }

    // ── 8. Cleanup ──────────────────────────────────────────────────────────
    if (csv) {
        fflush(csv);
        (void)fsync(fileno(csv));
        fclose(csv);
        ESP_LOGI(TAG, "[PPG_BCG] CSV closed (%u rows)", (unsigned)rows_written);
    }
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        (void)max30101_set_shutdown(MAX30101_I2C_PORT, true);
        xSemaphoreGive(g_i2c_mutex);
    }
    park_all_modal_sensors();
    ESP_LOGI(TAG,
             "[PPG_BCG] exit: rows=%u bcg_beats=%u ppg_beats=%u",
             (unsigned)rows_written, (unsigned)bcg_beats, (unsigned)ppg_beats);
}
