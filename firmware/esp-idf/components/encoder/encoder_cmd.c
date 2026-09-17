/**
 * @file encoder_cmd.c
 * @brief Crown encoder command surface -- see encoder_cmd.h.
 */

#include "encoder_cmd.h"
#include "encoder.h"

#include "esp_timer.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ENC_CMD";

// The driver's file-static counters live in encoder.c and are only
// mutable via `encoder_note_detent()`. For "reset", we accumulate a
// user-visible baseline here that the summary subtracts out. Keeps
// encoder.c free of extra API surface for a bench-only reset.
static int32_t  s_baseline_total = 0;
static uint32_t s_baseline_cw    = 0;
static uint32_t s_baseline_ccw   = 0;

void encoder_cmd_reset(void)
{
    s_baseline_total = encoder_get_total_detents();
    s_baseline_cw    = encoder_get_cw_count();
    s_baseline_ccw   = encoder_get_ccw_count();
    ESP_LOGI(TAG, "counters zeroed (baseline captured)");
}

// ── dump ────────────────────────────────────────────────────────────────────

void encoder_cmd_dump(void)
{
    int32_t  total = encoder_get_total_detents() - s_baseline_total;
    uint32_t cw    = encoder_get_cw_count()      - s_baseline_cw;
    uint32_t ccw   = encoder_get_ccw_count()     - s_baseline_ccw;
    uint32_t last  = encoder_get_last_event_ms();
    float    rate  = encoder_get_rate_dps();

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint32_t age    = (last == 0) ? 0 : (now_ms - last);

    printf("[ENC] %s @ GPIO A=21 B=43 (polled detent-rest)\n",
           encoder_get_chip_name());
    printf("[ENC]   CW           = %u\n",  (unsigned)cw);
    printf("[ENC]   CCW          = %u\n",  (unsigned)ccw);
    printf("[ENC]   total (net)  = %ld\n", (long)total);
    printf("[ENC]   rate         = %.2f detents/s\n", (double)rate);
    printf("[ENC]   last event   = %u ms ago\n", (unsigned)age);
    printf("[ENC]   glitches     = %u (PCNT path -- dormant)\n",
           (unsigned)encoder_get_glitch_count());
}

// ── status summary ──────────────────────────────────────────────────────────

void encoder_cmd_status_summary(char *out, size_t max)
{
    if (!out || max == 0) return;
    int32_t  total = encoder_get_total_detents() - s_baseline_total;
    uint32_t cw    = encoder_get_cw_count()      - s_baseline_cw;
    uint32_t ccw   = encoder_get_ccw_count()     - s_baseline_ccw;
    float    rate  = encoder_get_rate_dps();
    snprintf(out, max,
             "cw=%u ccw=%u net=%ld rate=%.1fdps",
             (unsigned)cw, (unsigned)ccw, (long)total, (double)rate);
}
