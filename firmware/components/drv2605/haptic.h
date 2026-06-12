/**
 * @file haptic.h
 * @brief Haptic module -- Core 0 driver wrapper + inter-core command queue.
 *
 * Three interfaces:
 *
 *   1. PLAY -- haptic_play(effect_id)
 *      Safe from any core or task. Enqueues HAPTIC_CMD_PLAY.
 *
 *   2. SWEEP CALIBRATION -- haptic_sweep_start() / haptic_sweep_set()
 *      IMU-assisted: 5s countdown, then auto-selects peak amplitude step.
 *      Manual fallback: haptic_sweep_set_imu_mode(false).
 *
 *   3. UI EFFECT -- haptic_get_ui_effect() / haptic_set_ui_effect()
 *      The effect ID used for all UI feedback (swipes, toggles).
 *      Persisted in NVS ("haptic_cfg" / "ui_effect"). Default: HAPTIC_EFFECT_CLICK.
 *      Call haptic_play(haptic_get_ui_effect()) at every UI interaction point.
 *
 * Architecture: Blueprint 1 §3, Blueprint 5 §2, Blueprint 13
 */

#ifndef HAPTIC_H
#define HAPTIC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "data_broker.h"
#include "drv2605.h"

// ---------------------------------------------------------------------------
// Queue configuration
// ---------------------------------------------------------------------------
#define HAPTIC_QUEUE_DEPTH  8

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

esp_err_t haptic_init(void);
void      task_haptic_fn(void *arg);

// ---------------------------------------------------------------------------
// Play API
// ---------------------------------------------------------------------------

void haptic_play(uint8_t effect_id);


/**
 * @brief Play effect unconditionally — ignores the enabled toggle.
 *        Used by alarm firing so vibration is never silenced by the
 *        UI haptic feedback setting. Enqueues HAPTIC_CMD_PLAY_FORCED.
 *        Safe from any core or task.
 * @param effect_id  DRV2605 effect 1-123.
 */
void haptic_play_forced(uint8_t effect_id);

// ---------------------------------------------------------------------------
// UI effect — stored in NVS, used for all UI feedback interactions
// ---------------------------------------------------------------------------

/**
 * @brief Set RTP amplitude for intensity-scaled playback (alarm escalation).
 *        Enqueues HAPTIC_CMD_RTP_AMP to the haptic command queue.
 *        Safe from any core. The haptic task writes DRV2605_REG_RTP.
 * @param amp_7bit  Amplitude 0-127 (0x00-0x7F). 0 = silent, 127 = max.
 */
void haptic_set_rtp_amp(uint8_t amp_7bit);

/**
 * @brief Get the current UI feedback effect ID.
 *        Safe to call from any core. Returns HAPTIC_UI_EFFECT_DEFAULT if
 *        not yet initialised.
 */
uint8_t haptic_get_ui_effect(void);

/**
 * @brief Set the UI feedback effect and save to NVS.
 *        Safe to call from Core 1 (tile roller callback).
 * @param effect_id  Any valid DRV2605 effect ID 1-123.
 */
void haptic_set_ui_effect(uint8_t effect_id);

// ---------------------------------------------------------------------------
// Sweep calibration API
// ---------------------------------------------------------------------------

void haptic_sweep_start(void);
void haptic_sweep_set(void);
void haptic_sweep_set_imu_mode(bool enable);

// ---------------------------------------------------------------------------
// Auto-calibration (diagnostic — always fails on Apple Taptic Engine)
// ---------------------------------------------------------------------------

void haptic_request_calibration(void);

#endif // HAPTIC_H