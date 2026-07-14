/**
 * @file encoder.h
 * @brief Crown rotary encoder -- ESP32-S3 PCNT quadrature decoder.
 *
 * Hardware (v7.2 §GPIO ASSIGNMENT):
 *   GPIO21 = EC_SigA  (channel A, rising/falling edges drive PCNT)
 *   GPIO43 = EC_SigB  (channel B, direction reference). v7.2 notes:
 *           "boot-log TX edge ~3 ms spurious activity, FW ignores pre-init".
 *           We discard events for the first ENCODER_BOOT_GUARD_MS after
 *           init to absorb the boot-log noise.
 *
 * Mechanical: rotary encoder with detents. One detent = one quadrature
 * cycle (4 edge transitions). PCNT in "x1" mode counts +/- 1 per cycle.
 *
 * Output: each debounced detent crosses fires one UI event into
 *   g_ui_event_q (Core 0 -> Core 1 navigation queue):
 *     UI_EVENT_CROWN_CW   -- clockwise
 *     UI_EVENT_CROWN_CCW  -- counter-clockwise
 *
 * The queue + ui_event_t enum live in `ui_event.h` (Core 1 / lvgl_ui owns
 * that header). It does not exist in this tree yet -- the driver references
 * the symbols via forward extern; see DEFECT-002.
 *
 * Core 0 only. No LVGL includes here.
 *
 * Architecture: Blueprint 1 §3, Blueprint 5 §2, Blueprint 11 (input)
 */

#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

// ── Pin / PCNT config ────────────────────────────────────────────────────────
#define ENCODER_GPIO_A           GPIO_NUM_21
#define ENCODER_GPIO_B           GPIO_NUM_43

// PCNT high/low limits. We treat the chip counter as a delta accumulator
// drained by the read task; bounds are conservative.
#define ENCODER_PCNT_HIGH_LIMIT  100
#define ENCODER_PCNT_LOW_LIMIT  (-100)

// Glitch filter (PCNT hardware): ignore pulses shorter than this many APB
// clock cycles. APB on the S3 runs at 80 MHz; 1000 cycles = 12.5 us.
#define ENCODER_GLITCH_NS        12500

// Software debounce window after each detected detent. Anything that arrives
// within this window is ignored. Mechanical encoders bounce for 1-2 ms.
#define ENCODER_DEBOUNCE_MS      2

// Boot guard: discard events for this long after encoder_init() returns,
// to swallow the GPIO43 boot-log TX spurious edges (v7.2 line 100-ish).
#define ENCODER_BOOT_GUARD_MS    200

// Steps per detent: a quadrature cycle = 4 edges; PCNT in default x4 mode
// reports +/- 4 per detent. Adjust if mechanical SKU differs.
#define ENCODER_STEPS_PER_DETENT 4

// ── Identity ─────────────────────────────────────────────────────────────────
const char *encoder_get_chip_name(void);   // "CrownEnc"
const char *encoder_get_chip_desc(void);   // "Rotary encoder (quadrature)"

// ── Lifecycle ────────────────────────────────────────────────────────────────

/**
 * @brief Configure PCNT unit on GPIO21/43, install glitch filter, start
 *        counting. Also installs a 5 ms periodic esp_timer that drains the
 *        PCNT counter and fires CW/CCW events into g_ui_event_q.
 *
 *        Must be called AFTER the boot-log TX is finished (the boot guard
 *        absorbs the worst of it, but a few ms of post-init drain is safer).
 */
esp_err_t encoder_init(void);

/** @brief Stop the periodic drain timer and tear down PCNT. */
void encoder_deinit(void);

// ── Counters / diagnostics ───────────────────────────────────────────────────

/**
 * @brief Cumulative detent count since boot (signed: positive = CW total).
 *        Useful for the test harness; production code should consume CW/CCW
 *        events from g_ui_event_q instead of polling this.
 */
int32_t encoder_get_total_detents(void);

/** @brief Number of CW events fired since boot. */
uint32_t encoder_get_cw_count(void);

/** @brief Number of CCW events fired since boot. */
uint32_t encoder_get_ccw_count(void);

/** @brief Number of glitch / debounce-suppressed events since boot. */
uint32_t encoder_get_glitch_count(void);

#endif // ENCODER_H
