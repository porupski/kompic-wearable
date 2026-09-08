/**
 * @file rgb_policy.h
 * @brief Background LED policy on Core 0. Owns the RGB when no mode is active.
 *
 * Priority ladder (high -> low):
 *   1. Battery alert (< 10 %% and not charging)   -- red blink 2 Hz
 *   2. Preview window (5 s after mode select)     -- mode colour, solid
 *   3. Charging                                    -- blue pulse
 *   4. Charged (charging=0, pg=1)                  -- green steady dim
 *   5. Wrist down (broker_imu.gesture=WRIST_DOWN)  -- OFF
 *   6. Idle wrist up                                -- dim white
 *
 * Modes that want the LED for their own animation call rgb_policy_pause()
 * at entry and rgb_policy_resume() on exit. While paused the policy makes
 * no ws2812 writes; the mode owns the pixel via ws2812_set_color().
 *
 * The 5 s preview window is triggered explicitly by field_capture when the
 * encoder lands on a new top-level mode (rgb_policy_preview_start).
 *
 * Runs on esp_timer, 50 ms cadence. Zero FreeRTOS tasks of its own.
 *
 * Stage 17 §3.3 -- skeleton. Wrist-down gate is a placeholder until §3.4
 * wires the LSM 6D orientation reader into broker_imu_data_t.gesture.
 */

#ifndef RGB_POLICY_H
#define RGB_POLICY_H

#define RGB_POLICY_DRIVER_VERSION "0.1.0"

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rgb_policy_init(void);

// Modes call these when they want to drive the LED directly.
void rgb_policy_pause(void);
void rgb_policy_resume(void);

// Publish the current mode colour without touching the activity timer.
// Called once from field_capture after NVS mode load.
void rgb_policy_set_mode_color(uint8_t r, uint8_t g, uint8_t b);

// Encoder mode-select or notification: update the mode colour AND reset
// the activity timer. The tick cb then paints the 0..25 s timeline:
//   0..5 s   solid mode colour (preview)
//   5..15 s  full-amplitude mode-colour pulse
//   15..25 s pulse with amplitude fading linearly to zero
//   25 s+    OFF
void rgb_policy_preview_start(uint8_t r, uint8_t g, uint8_t b);

// Same as preview_start with no colour change (future notification path).
void rgb_policy_notify(void);

#ifdef __cplusplus
}
#endif

#endif  // RGB_POLICY_H
