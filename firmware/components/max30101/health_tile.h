/**
 * @file health_tile.h
 * @brief Health / HR sensor tile — Core 1 / LVGL 9 only.
 *
 * Type A tile: status LED + chip header, large BPM display, signal quality
 * bar, status label, Buzz-to-BPM toggle, power switch.
 *
 * No sub-tile in Phase 14.
 *
 * Reads broker_hr_data_t. Never calls I2C directly.
 * Buzz-to-BPM: when enabled and a new beat is detected, calls
 * haptic_play(haptic_get_ui_effect()) from Core 1 — non-blocking, safe.
 *
 * Architecture: Blueprint 3 §6, Blueprint 5 §4–§6, Blueprint 14b §7
 */

#ifndef HEALTH_TILE_H
#define HEALTH_TILE_H

#include "lvgl.h"
#include "ui_theme_colors.h"
#include "system_tile.h"   // tile_desc_t

// ── Mandatory tile API ────────────────────────────────────────────────────────

/**
 * @brief Build all LVGL widgets for the Health tile.
 *        Called once during settings screen construction.
 *        Must be called inside lvgl_port_lock().
 */
void health_tile_init(lv_obj_t *parent);

/**
 * @brief Refresh all widgets from broker_hr_read().
 *        Called every 200 ms by task_ui_refresh_fn() when tile is active.
 *        Must be called inside lvgl_port_lock().
 *        Never reads hardware. Calls haptic_play() on beat detection when
 *        Buzz-to-BPM is enabled.
 */
void health_tile_update(void);

/**
 * @brief Restyle all widgets for the current theme.
 *        Called by apply_ui_theme() fan-out.
 *        Must be called inside lvgl_port_lock().
 */
void health_tile_apply_theme(ui_theme_t theme);

// ── Tile descriptor (registered in tile_registry.c) ──────────────────────────
extern const tile_desc_t health_tile_desc;

#endif // HEALTH_TILE_H