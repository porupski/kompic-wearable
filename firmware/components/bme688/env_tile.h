/**
 * @file env_tile.h
 * @brief Environment sensor tile — Core 1 / LVGL 9 only.
 *
 * Type A tile: status LED + chip header, data rows, Zero Height button.
 * No sub-tile in Phase 14.
 *
 * Reads broker_env_data_t. Never calls I2C or NVS directly except for the
 * Zero Height button which calls app_nvs_save_height_reference() — safe from
 * Core 1 because NVS has its own internal mutex.
 *
 * Architecture: Blueprint 3 §6, Blueprint 5 §4–§6, Blueprint 14a §6
 */

#ifndef ENV_TILE_H
#define ENV_TILE_H

#include "lvgl.h"
#include "ui_theme_colors.h"
#include "system_tile.h"   // tile_desc_t

// ── Mandatory tile API ────────────────────────────────────────────────────────

/**
 * @brief Build all LVGL widgets for the ENV tile.
 *        Called once during settings screen construction.
 *        Must be called inside lvgl_port_lock().
 */
void env_tile_init(lv_obj_t *parent);

/**
 * @brief Refresh all labels from broker_env_read().
 *        Called every 200 ms by task_ui_refresh_fn() when tile is active.
 *        Must be called inside lvgl_port_lock().
 *        Never reads hardware. Never writes broker (except Zero Height button).
 */
void env_tile_update(void);

/**
 * @brief Restyle all widgets for the current theme.
 *        Called by apply_ui_theme() fan-out.
 *        Must be called inside lvgl_port_lock().
 */
void env_tile_apply_theme(ui_theme_t theme);

// ── Tile descriptor (registered in tile_registry.c) ──────────────────────────
extern const tile_desc_t env_tile_desc;

#endif // ENV_TILE_H