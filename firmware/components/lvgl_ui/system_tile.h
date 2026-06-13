/**
 * @file system_tile.h
 * @brief System info settings tile — public API.
 *
 * Tile type : info-only (no power switch, no control buttons).
 * Component : components/lvgl_ui/ — settings-side tile, no dedicated driver.
 *
 * Layout (Blueprint 3 §5 "System Info"):
 *   Header   : "System" title (no LED, no power toggle — always available)
 *   List rows: RTC clock, Uptime, Battery, CPU freq, Internal temp, Flash size
 *
 * Static data  (filled once at init) : Flash size.
 * Slow dynamic (lv_timer 1 s)        : CPU freq, internal temp, uptime.
 * Fast dynamic (200 ms broker poll)  : RTC time, battery voltage + %.
 *
 * Core 1 only. No I2C. Reads broker_rtc_data_t and broker_battery_data_t.
 *
 * Architecture: Blueprint 3 §5, Blueprint 5 §4
 */

#ifndef SYSTEM_TILE_H
#define SYSTEM_TILE_H

#include "lvgl.h"
#include "ui_theme_colors.h"

/**
 * @brief Build all system tile widgets as children of `parent`.
 *        Called once from lvgl_ui.c during settings screen construction.
 *        Installs the internal 1-second lv_timer for slow metrics.
 */
void system_tile_init(lv_obj_t *parent);

/**
 * @brief Refresh RTC time and battery rows from broker data.
 *        Called by task_ui_refresh_fn() when the system tile is active.
 *        Must be called inside lvgl_port_lock() / lvgl_port_unlock().
 *        Uptime / CPU / temp are handled by the internal lv_timer.
 */
void system_tile_update(void);

/**
 * @brief Reapply theme colours to all system tile widgets.
 *        Called by apply_ui_theme() fan-out in lvgl_ui.c.
 */
void system_tile_apply_theme(ui_theme_t theme);

#include "tile_registry.h"
extern const tile_desc_t system_tile_desc;

#endif // SYSTEM_TILE_H
