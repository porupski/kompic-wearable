/**
 * @file drv2605_cmd.h
 * @brief DRV2605 / haptic command surface -- Module_Blueprint.md §3.
 *
 * Thin veneer over the existing `haptic.h` / `drv2605.h` primitives so the
 * new `HAPTIC` CLI verb, the haptic tile, and any future outlet share one
 * surface. `haptic.h` already carries the effect-queue + sweep API; this
 * header adds the missing enable/dump/status entries called for by the
 * blueprint and wraps everything under a consistent `drv2605_cmd_*` name.
 */
#ifndef DRV2605_CMD_H
#define DRV2605_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// -- enable ------------------------------------------------------------------
// Master enable for UI-driven haptic feedback. Alarm firing uses
// haptic_play_forced() and is NOT gated by this flag (by design).
esp_err_t drv2605_cmd_enable_set(bool on);
bool      drv2605_cmd_enable_get(void);

// -- play --------------------------------------------------------------------
// Enqueue a DRV2605 effect (1..123). Respects the enable flag.
esp_err_t drv2605_cmd_play_effect(uint8_t effect_id);

// -- calibrate ---------------------------------------------------------------
// Non-blocking. Fires DRV2605 auto-calibration (known to fail on the
// Apple Taptic Engine -- diagnostic only; use sweep for real cal).
void drv2605_cmd_calibrate(void);

// -- sweep -------------------------------------------------------------------
// _start: begin a period-sweep across the LRA frequency range.
// _stop : latch the currently-playing step and end the sweep.
void drv2605_cmd_sweep_start(void);
void drv2605_cmd_sweep_stop(void);

// -- UI effect (roller default, persisted in NVS) ----------------------------
uint8_t drv2605_cmd_ui_effect_get(void);
void    drv2605_cmd_ui_effect_set(uint8_t effect_id);

// -- dump --------------------------------------------------------------------
void drv2605_cmd_dump(void);

// -- status summary ----------------------------------------------------------
void drv2605_cmd_status_summary(char *out, size_t max);

#endif // DRV2605_CMD_H
