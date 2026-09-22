/**
 * @file ui_animations.h
 * @brief Reusable LVGL animation helpers for drawer / overlay transitions.
 *
 * Stage 31.3 -- foundations rebuild. Wraps lv_anim_t boilerplate for the
 * two motion primitives we care about: slide (translate along an axis)
 * and opa fade. Consumers (settings drawer, alarm drawer in Stage 33,
 * overlays) call these instead of hand-rolling lv_anim each time.
 *
 * Convention:
 *   slide_in(obj, from_dir, ...) -- pane STARTS off-screen at from_dir
 *                                   edge and slides to on-screen (translate = 0).
 *   slide_out(obj, to_dir, ...)  -- pane STARTS at translate = 0 and
 *                                   slides off-screen toward to_dir edge.
 *
 * from_dir / to_dir accept LV_DIR_TOP / _BOTTOM / _LEFT / _RIGHT. Any
 * other value is silently ignored (no anim started).
 *
 * All functions must run inside lvgl_port_lock() -- they invoke
 * lv_anim_start which touches the LVGL animation refr chain.
 *
 * Log tiers (per feedback_esp_log_verbosity_practice):
 *   LOGI -- none (helpers are chatty by design; caller logs at event site)
 *   LOGD -- per-call summary (obj, dir, dur)
 *   LOGV -- per-tick anim value (would flood; disabled)
 */

#ifndef UI_ANIMATIONS_H
#define UI_ANIMATIONS_H

#include "lvgl.h"
#include <stdint.h>

/**
 * @brief Slide obj into view from an off-screen edge.
 *
 * At t=0 obj is translated to the from_dir edge (off-screen); over dur_ms
 * it translates back to (0, 0). Ease-out timing (fast start, gentle end).
 */
void kw_ui_animate_slide_in(lv_obj_t *obj, lv_dir_t from_dir,
                            uint32_t dur_ms, uint32_t delay_ms);

/**
 * @brief Slide obj out of view toward an off-screen edge.
 *
 * At t=0 obj is at translate (0, 0); over dur_ms it translates to the
 * to_dir edge (off-screen). Ease-in timing (gentle start, fast end).
 */
void kw_ui_animate_slide_out(lv_obj_t *obj, lv_dir_t to_dir,
                             uint32_t dur_ms, uint32_t delay_ms);

/**
 * @brief Fade obj's opacity from its current value to target_opa.
 *
 * target_opa is a standard LV_OPA_* value (0..255). Linear timing.
 */
void kw_ui_animate_opa(lv_obj_t *obj, lv_opa_t target_opa,
                       uint32_t dur_ms, uint32_t delay_ms);

#endif // UI_ANIMATIONS_H
