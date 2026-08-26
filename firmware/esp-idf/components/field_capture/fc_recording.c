/**
 * @file fc_recording.c
 * @brief WAV + CSV writers, recording orchestrator, alarm firing.
 *
 * Split out of field_capture.c in the Stage 12 refactor. Runs on the main
 * field_capture task -- blocking helpers only, no task creation here.
 *
 * SD durability rule (see field_capture.c top-of-file): CSV writers used
 * here follow Pattern A (open-append-close) or Pattern B (fflush + fsync
 * per row). See comments per writer.
 */

#include "fc_internal.h"
#include "firmware_version.h"

#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "data_broker.h"
#include "ws2812.h"
#include "mic_pdm.h"
#include "haptic.h"
#include "nvs_cfg.h"

static const char *TAG = "FC_REC";

// ── WAV header (44 B canonical PCM RIFF) ────────────────────────────────────
void wav_write_header(FILE *f, uint32_t sample_rate, uint16_t bits) {
    uint8_t hdr[44] = {0};
    memcpy(hdr,    "RIFF", 4);
    memcpy(hdr+8,  "WAVEfmt ", 8);
    hdr[16] = 16;
    hdr[20] = 1;
    hdr[22] = 1;
    hdr[24] = sample_rate         & 0xFF;
    hdr[25] = (sample_rate >>  8) & 0xFF;
    hdr[26] = (sample_rate >> 16) & 0xFF;
    hdr[27] = (sample_rate >> 24) & 0xFF;
    uint32_t byte_rate = sample_rate * (bits / 8);
    hdr[28] = byte_rate         & 0xFF;
    hdr[29] = (byte_rate >>  8) & 0xFF;
    hdr[30] = (byte_rate >> 16) & 0xFF;
    hdr[31] = (byte_rate >> 24) & 0xFF;
    hdr[32] = (bits / 8);
    hdr[34] = (uint8_t)bits;
    memcpy(hdr+36, "data", 4);
    fwrite(hdr, 1, sizeof(hdr), f);
}
void wav_patch_header(FILE *f, uint32_t data_bytes) {
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

void wav_write_meta_sidecar(const char *wav_path, const char *tag) {
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
uint32_t wav_record_to(const char *path, uint32_t duration_ms) {
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

// ── CSV open + per-mode row writer ──────────────────────────────────────────
FILE *csv_open(const char *dir, uint32_t seq, const char *header) {
    ensure_sd();
    if (!s_sd_ready) return NULL;
    // Stage 17: guarantee the mode's data folder exists so new modes never
    // need to remember try_mkdir themselves. ensure_sd() pre-creates the
    // classic set (mic/env/mot/...); any new tag (e.g. "ppgbcg") gets its
    // folder on-demand here. try_mkdir treats EEXIST as OK, so this is safe
    // to call on every csv_open.
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "/sd/data/%s", dir);
    try_mkdir(dir_path);
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
    fprintf(f, "# rtc_start=%s hw=%s fw=%s boot=%lu seq=%lu mode=%s\n",
            now, KOMPIC_HW_VERSION, KOMPIC_FW_VERSION,
            (unsigned long)s_boot_seq, (unsigned long)seq, dir);
    fprintf(f, "%s\n", header);
    ESP_LOGI(TAG, "CSV %s opened", path);
    return f;
}
void csv_row_env(FILE *f) {
    broker_env_data_t e;   broker_env_read(&e);
    broker_light_data_t l; broker_light_read(&l);
    fprintf(f, "%lu,%.2f,%.2f,%.2f,%.0f,%.1f\n",
            (unsigned long)millis_u32(),
            e.temperature_c, e.humidity_pct, e.pressure_hpa,
            e.gas_resistance_ohm, l.lux);
}
void csv_row_motion(FILE *f) {
    broker_imu_data_t im; broker_imu_read(&im);
    broker_mag_data_t mg; broker_mag_read(&mg);
    fprintf(f, "%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f\n",
            (unsigned long)millis_u32(),
            im.accel_x, im.accel_y, im.accel_z,
            im.gyro_x,  im.gyro_y,  im.gyro_z,
            mg.x_ut,    mg.y_ut,    mg.z_ut);
}
#if DEBUG_PRINT_SENSORS
void debug_dump_mode(fc_mode_t m) {
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
    default: break;
    }
}
#endif

// ── Recording orchestrator (blocking) ───────────────────────────────────────
void voice_annot_lead_in(void) {
    for (int i = 0; i < BEEP_COUNT; i++) {
        ws2812_set_color(WS_MAX, 0, 0);
        haptic_play_forced(DRV_STRONG_CLICK);
        vTaskDelay(pdMS_TO_TICKS(BEEP_ON_MS));
        rgb_off();
        vTaskDelay(pdMS_TO_TICKS(BEEP_OFF_MS));
    }
}
void run_recording_for_current_mode(void) {
    s_recording_early_end = false;
    s_rec_seq++;

    wake_sensors_for_mode(s_mode);
    vTaskDelay(pdMS_TO_TICKS(200));

    bool needs_annot = (s_mode == FCM_ENV || s_mode == FCM_MOTION);
    if (needs_annot && nvs_cfg_sys_get_rec_audio()) {
        voice_annot_lead_in();
        char path[96];
        snprintf(path, sizeof(path), "/sd/data/mic/s%04lu_r%04lu_annot.wav",
                 (unsigned long)s_boot_seq, (unsigned long)s_rec_seq);
        wav_record_to(path, VOICE_ANNOT_MS);
        wav_write_meta_sidecar(path, "annot");
    } else if (needs_annot) {
        ESP_LOGI(TAG, "REC_AUDIO=off -- skipping 5 s voice annotation");
    }

    switch (s_mode) {
    case FCM_MIC: {
        char path[96];
        snprintf(path, sizeof(path), "/sd/data/mic/s%04lu_r%04lu.wav",
                 (unsigned long)s_boot_seq, (unsigned long)s_rec_seq);
        rgb_set_max(FCM_MIC);
        wav_record_to(path, 0);
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
    default: break;
    }

    if (s_csv_file) {
        uint32_t start = millis_u32();
        const uint32_t row_period_ms = (s_mode == FCM_ENV) ? 500 : 100;
        while (!s_recording_early_end) {
            rgb_pulse(s_mode, start, PULSE_RECORD_MS);
            switch (s_mode) {
            case FCM_ENV:    csv_row_env(s_csv_file);    break;
            case FCM_MOTION: csv_row_motion(s_csv_file); break;
            default: break;
            }
#if DEBUG_PRINT_SENSORS
            debug_dump_mode(s_mode);
#endif
            if (button_poll() == 1) s_recording_early_end = true;
            vTaskDelay(pdMS_TO_TICKS(row_period_ms));
        }
        fflush(s_csv_file);
        fclose(s_csv_file);
        s_csv_file = NULL;
        ESP_LOGI(TAG, "CSV closed (click-exit)");
    }

    haptic_play_forced(DRV_LONG_BUZZ);
    park_all_modal_sensors();
}

// ── Alarm firing (simplified vs sketch: 15 s buzz + click pattern) ──────────
void run_alarm_firing(void) {
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
