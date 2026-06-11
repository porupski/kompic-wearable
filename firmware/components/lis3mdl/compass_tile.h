/**
 * @file compass_tile.h
 * @brief Compass (QMC5883P / future LIS3MDLTR) settings tile — public API.
 *
 * Tile type : A (status LED + data rows) + compass rose + calibrate button.
 * Component : components/qmc5883p/ — co-located with the MAG driver.
 *
 * Layout (Blueprint 9 §8):
 *   Header   : status LED + chip name/desc + power toggle switch
 *   Data rows: X/Y/Z µT, Heading °, Cardinal direction
 *   Rose     : 70×70 circle, N/S/E/W labels, rotating needle (LV_SYMBOL_UP)
 *   Button   : [CALIBRATE] — disabled when offline/disabled or calibrating
 *
 * Core 1 only. No I2C. Data flows: broker_mag_read() → widgets.
 *
 * Architecture: Blueprint 3 §5, Blueprint 5 §4, Blueprint 9 §8
 */

#ifndef COMPASS_TILE_H
#define COMPASS_TILE_H

#include "lvgl.h"
#include "ui_theme_colors.h"

/**
 * @brief Build all compass tile widgets as children of `parent`.
 *        Called once from lvgl_ui.c during settings screen construction.
 */
void compass_tile_init(lv_obj_t *parent);

/**
 * @brief Refresh all compass tile widgets from broker data.
 *        Called by task_ui_refresh_fn() when the compass tile is active.
 *        Must be called inside lvgl_port_lock() / lvgl_port_unlock().
 */
void compass_tile_update(void);

/**
 * @brief Reapply theme colours to all compass tile widgets.
 *        Called by apply_ui_theme() fan-out in lvgl_ui.c.
 */
void compass_tile_apply_theme(ui_theme_t theme);

/**
 * @brief Register the calibration trigger callback.
 *        Called from boot_hw_init.c or boot_tasks.c after task creation.
 *        The callback runs on Core 1 (UI context) — it should only set a flag
 *        or post to a queue; the actual calibration runs on Core 0.
 *
 * @param cb  Function to call when the user taps CALIBRATE.
 */
void compass_tile_set_calibrate_callback(void (*cb)(void));

#include "tile_registry.h"
extern const tile_desc_t compass_tile_desc;

#endif // COMPASS_TILE_H
