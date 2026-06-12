/**
 * @file encoder.c
 * @brief Crown rotary encoder driver -- PCNT-based, Core 0.
 *
 * Why PCNT (Pulse Counter)?
 *   - Quadrature decoding in hardware (no per-edge ISR overhead).
 *   - Built-in glitch filter (programmable threshold in APB cycles).
 *   - Watchpoint events for over/underflow handled by the IDF.
 *   - Free CPU: a quadrature detent at 200 RPM is one read every 5 ms;
 *     decoding it in software via GPIO ISRs would burn an ISR + a couple
 *     of memory accesses per edge. PCNT is one read per drain tick.
 *
 * Drain strategy:
 *   A periodic esp_timer (5 ms) reads the PCNT counter, computes the delta
 *   since the previous read, divides by ENCODER_STEPS_PER_DETENT (default 4
 *   for x4 quadrature decoding), and fires CW/CCW events into g_ui_event_q
 *   for each detent crossed. Sub-detent motion is preserved in a residue
 *   accumulator so partial rotations don't lose information.
 *
 * Debounce + boot guard:
 *   The PCNT hardware glitch filter eats sub-12.5-us pulses (default 1000
 *   APB cycles @ 80 MHz). A software debounce window (default 2 ms) gates
 *   subsequent detent events; mechanical bounce within 2 ms is suppressed
 *   and counted in s_glitch_count for diagnostics.
 *
 *   For the first ENCODER_BOOT_GUARD_MS after init, all drained detents
 *   are silently discarded -- this absorbs the v7.2-documented "GPIO43
 *   boot-log TX ~3 ms spurious activity".
 *
 * Architecture: Blueprint 1 §3, Blueprint 5 §2, Blueprint 11
 */

#include "encoder.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "ENCODER";

const char *encoder_get_chip_name(void) { return "CrownEnc";                    }
const char *encoder_get_chip_desc(void) { return "Rotary encoder (quadrature)"; }

// ────────────────────────────────────────────────────────────────────────────
// Forward extern: the UI event queue + ui_event_t enum live in lvgl_ui's
// ui_event.h which has not landed yet (DEFECT-002). We declare what we need
// here so the chip-layer driver compiles in isolation; the queue identity
// must match what lvgl_ui creates at boot.
//
// Once ui_event.h exists, swap this block for `#include "ui_event.h"`.
// ────────────────────────────────────────────────────────────────────────────

typedef enum {
    UI_EVENT_CROWN_CW  = 1,
    UI_EVENT_CROWN_CCW = 2,
    // Additional UI events (touch, button) belong in ui_event.h, not here.
} ui_event_t;

extern QueueHandle_t g_ui_event_q;   // depth/payload owned by lvgl_ui

// ────────────────────────────────────────────────────────────────────────────
// Internal state
// ────────────────────────────────────────────────────────────────────────────

static pcnt_unit_handle_t    s_pcnt_unit    = NULL;
static pcnt_channel_handle_t s_pcnt_chan    = NULL;
static esp_timer_handle_t    s_drain_timer  = NULL;

static int  s_residue = 0;   // sub-detent accumulator (signed)
static int32_t  s_total_detents = 0;
static uint32_t s_cw_count      = 0;
static uint32_t s_ccw_count     = 0;
static uint32_t s_glitch_count  = 0;

static int64_t s_last_event_us  = 0;
static int64_t s_boot_guard_us  = 0;   // events suppressed until esp_timer_get_time() > this

// ────────────────────────────────────────────────────────────────────────────
// Drain callback: read PCNT, compute delta, emit events
// ────────────────────────────────────────────────────────────────────────────

static void encoder_drain_cb(void *arg)
{
    (void)arg;

    int raw = 0;
    if (pcnt_unit_get_count(s_pcnt_unit, &raw) != ESP_OK) return;
    if (raw == 0) return;

    // Snapshot + zero the hardware counter atomically so we don't lose any
    // edges that arrive between the read and the clear.
    pcnt_unit_clear_count(s_pcnt_unit);

    s_residue += raw;

    // Convert residue (in PCNT edges) into detent crossings. Integer-division
    // toward zero is fine here: residue keeps the leftover for the next tick.
    int detents = s_residue / ENCODER_STEPS_PER_DETENT;
    if (detents == 0) return;

    s_residue -= detents * ENCODER_STEPS_PER_DETENT;

    int64_t now_us = esp_timer_get_time();

    // Boot guard: chew through the v7.2-documented GPIO43 boot-log noise.
    if (now_us < s_boot_guard_us) {
        s_glitch_count += (uint32_t)abs(detents);
        return;
    }

    // Software debounce: gate against rapid-fire events.
    if ((now_us - s_last_event_us) < (int64_t)ENCODER_DEBOUNCE_MS * 1000LL) {
        s_glitch_count += (uint32_t)abs(detents);
        return;
    }
    s_last_event_us = now_us;

    // Fire one event per detent (loop because a fast turn may have crossed
    // multiple detents within a 5 ms drain tick).
    ui_event_t evt = (detents > 0) ? UI_EVENT_CROWN_CW : UI_EVENT_CROWN_CCW;
    int n = abs(detents);
    for (int i = 0; i < n; i++) {
        if (g_ui_event_q != NULL) {
            // Non-blocking send. If the queue is full, drop -- the UI is busy.
            (void)xQueueSend(g_ui_event_q, &evt, 0);
        }
    }

    if (detents > 0) { s_cw_count  += n; s_total_detents += n; }
    else             { s_ccw_count += n; s_total_detents -= n; }
}

// ────────────────────────────────────────────────────────────────────────────
// Init / deinit
// ────────────────────────────────────────────────────────────────────────────

esp_err_t encoder_init(void)
{
    ESP_LOGI(TAG, "Init PCNT on GPIO%d (A) / GPIO%d (B)",
             ENCODER_GPIO_A, ENCODER_GPIO_B);

    pcnt_unit_config_t unit_cfg = {
        .high_limit = ENCODER_PCNT_HIGH_LIMIT,
        .low_limit  = ENCODER_PCNT_LOW_LIMIT,
    };
    esp_err_t ret = pcnt_new_unit(&unit_cfg, &s_pcnt_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_unit: %s", esp_err_to_name(ret));
        return ret;
    }

    pcnt_glitch_filter_config_t filter_cfg = {
        .max_glitch_ns = ENCODER_GLITCH_NS,
    };
    ret = pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_glitch_filter: %s", esp_err_to_name(ret));
        goto fail;
    }

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num  = ENCODER_GPIO_A,
        .level_gpio_num = ENCODER_GPIO_B,
    };
    ret = pcnt_new_channel(s_pcnt_unit, &chan_cfg, &s_pcnt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_channel: %s", esp_err_to_name(ret));
        goto fail;
    }

    // Quadrature x4 decoding: count on both edges of A, direction from B.
    ret = pcnt_channel_set_edge_action(s_pcnt_chan,
                                       PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    if (ret != ESP_OK) goto fail;
    ret = pcnt_channel_set_level_action(s_pcnt_chan,
                                        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    if (ret != ESP_OK) goto fail;

    ret = pcnt_unit_enable(s_pcnt_unit); if (ret != ESP_OK) goto fail;
    ret = pcnt_unit_clear_count(s_pcnt_unit); if (ret != ESP_OK) goto fail;
    ret = pcnt_unit_start(s_pcnt_unit); if (ret != ESP_OK) goto fail;

    // 5 ms drain timer -- finer than the human can rotate, coarser than the
    // PCNT can over/underflow at any plausible RPM.
    const esp_timer_create_args_t timer_args = {
        .callback = encoder_drain_cb,
        .arg      = NULL,
        .name     = "encoder_drain",
    };
    ret = esp_timer_create(&timer_args, &s_drain_timer);
    if (ret != ESP_OK) goto fail;
    ret = esp_timer_start_periodic(s_drain_timer, 5 * 1000ULL);
    if (ret != ESP_OK) goto fail;

    s_boot_guard_us = esp_timer_get_time() + ((int64_t)ENCODER_BOOT_GUARD_MS * 1000LL);
    s_last_event_us = 0;
    s_residue       = 0;

    ESP_LOGI(TAG, "Encoder init OK -- glitch=%d ns, debounce=%d ms, boot guard=%d ms",
             ENCODER_GLITCH_NS, ENCODER_DEBOUNCE_MS, ENCODER_BOOT_GUARD_MS);
    return ESP_OK;

fail:
    encoder_deinit();
    return ret;
}

void encoder_deinit(void)
{
    if (s_drain_timer) {
        esp_timer_stop(s_drain_timer);
        esp_timer_delete(s_drain_timer);
        s_drain_timer = NULL;
    }
    if (s_pcnt_unit) {
        pcnt_unit_stop(s_pcnt_unit);
        pcnt_unit_disable(s_pcnt_unit);
        if (s_pcnt_chan) { pcnt_del_channel(s_pcnt_chan); s_pcnt_chan = NULL; }
        pcnt_del_unit(s_pcnt_unit);
        s_pcnt_unit = NULL;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ────────────────────────────────────────────────────────────────────────────

int32_t  encoder_get_total_detents(void) { return s_total_detents; }
uint32_t encoder_get_cw_count(void)      { return s_cw_count;      }
uint32_t encoder_get_ccw_count(void)     { return s_ccw_count;     }
uint32_t encoder_get_glitch_count(void)  { return s_glitch_count;  }
