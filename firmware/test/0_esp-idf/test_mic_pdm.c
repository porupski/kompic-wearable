/**
 * @file test_mic_pdm.c
 * @brief Standalone diagnostic for the PDM mic driver.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §GPIO ASSIGNMENT):
 *   GPIO47 -> PDM CLK (host out)
 *   GPIO48 <- PDM DATA (mic out)
 *   JP9 = GND (left channel) per v7.2 §SOLDER JUMPERS.
 *
 * Phases:
 *   1. mic_pdm_init  -> channel allocated.
 *   2. mic_pdm_start -> DMA running.
 *   3. Capture 50 frames (= 1 s of audio at 16 kHz / 20 ms frames).
 *      Compute per-frame DC offset, peak |sample|, RMS. Log each frame.
 *   4. mic_pdm_capture() convenience -- 25 frames (500 ms) into a
 *      stack-allocated buffer (auto-stop after).
 *   5. mic_pdm_stop / deinit, stack high-water + heap report.
 *
 * Bench notes:
 *   - In a quiet room expect RMS around 50-200 (16-bit signed).
 *   - Tap the watch near the mic port; peak should jump to thousands.
 *   - If RMS is exactly 0 across all frames, suspect that the L/R slot
 *     mask is wrong (i.e. JP9 is on VDD instead of GND).
 *   - If RMS is saturated (~32000), suspect that PDM DC blocker is
 *     disabled or the front-end is overdriven.
 */

#include "mic_pdm.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_mic_pdm";

#define CAPTURE_FRAMES_PHASE3   50   // 1.0 s
#define CAPTURE_FRAMES_PHASE4   25   // 0.5 s

static void analyse_frame(const int16_t *samples, size_t n,
                          int32_t *out_dc, uint16_t *out_peak, uint32_t *out_rms)
{
    int64_t sum = 0;
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = samples[i];
        sum += v;
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    int32_t dc = (int32_t)(sum / (int64_t)n);
    int64_t sq = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t d = (int32_t)samples[i] - dc;
        sq += (int64_t)d * d;
    }
    uint32_t rms = (uint32_t)sqrtf((float)(sq / (int64_t)n));
    *out_dc   = dc;
    *out_peak = (uint16_t)peak;
    *out_rms  = rms;
}

static void test_mic_pdm_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             mic_pdm_get_chip_name(), mic_pdm_get_chip_desc());

    int64_t t_i0 = esp_timer_get_time();
    esp_err_t err = mic_pdm_init();
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "mic_pdm_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    int64_t t_s0 = esp_timer_get_time();
    err = mic_pdm_start();
    int64_t t_s1 = esp_timer_get_time();
    ESP_LOGI(TAG, "mic_pdm_start: %s in %lld us",
             esp_err_to_name(err), (long long)(t_s1 - t_s0));
    if (err != ESP_OK) { mic_pdm_deinit(); return; }

    // ── Phase 3: 50 frames via mic_pdm_read ─────────────────────────────────
    int16_t frame[MIC_PDM_FRAME_SAMPLES];
    int32_t dc_total = 0;
    uint32_t rms_total = 0;
    uint16_t peak_total = 0;
    int64_t  total_us = 0;
    for (int f = 0; f < CAPTURE_FRAMES_PHASE3; f++) {
        size_t got = 0;
        int64_t tr0 = esp_timer_get_time();
        err = mic_pdm_read(frame, sizeof(frame), &got, 200);
        int64_t tr1 = esp_timer_get_time();
        total_us += (tr1 - tr0);
        if (err != ESP_OK || got != sizeof(frame)) {
            ESP_LOGE(TAG, "read frame %d failed: %s, got %u",
                     f, esp_err_to_name(err), (unsigned)got);
            break;
        }
        int32_t dc; uint16_t pk; uint32_t rms;
        analyse_frame(frame, MIC_PDM_FRAME_SAMPLES, &dc, &pk, &rms);
        dc_total   += dc;
        rms_total  += rms;
        if (pk > peak_total) peak_total = pk;
        if ((f % 10) == 0) {
            ESP_LOGI(TAG, "frame %02d: dc=%5ld peak=%5u rms=%5lu  read_us=%lld",
                     f, (long)dc, (unsigned)pk, (unsigned long)rms,
                     (long long)(tr1 - tr0));
        }
    }
    ESP_LOGI(TAG, "[Phase 3] avg dc=%ld avg rms=%lu peak across=%u  total read time=%lld us",
             (long)(dc_total / CAPTURE_FRAMES_PHASE3),
             (unsigned long)(rms_total / CAPTURE_FRAMES_PHASE3),
             (unsigned)peak_total, (long long)total_us);

    // ── Phase 4: convenience mic_pdm_capture ────────────────────────────────
    static int16_t buf[CAPTURE_FRAMES_PHASE4 * MIC_PDM_FRAME_SAMPLES];
    int64_t tc0 = esp_timer_get_time();
    err = mic_pdm_capture(buf, CAPTURE_FRAMES_PHASE4, 200);
    int64_t tc1 = esp_timer_get_time();
    ESP_LOGI(TAG, "mic_pdm_capture(%u frames): %s in %lld us",
             (unsigned)CAPTURE_FRAMES_PHASE4,
             esp_err_to_name(err), (long long)(tc1 - tc0));

    // ── Phase 5: stop + deinit + memory ─────────────────────────────────────
    mic_pdm_stop();
    mic_pdm_deinit();

    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_mic_pdm_run();
}
