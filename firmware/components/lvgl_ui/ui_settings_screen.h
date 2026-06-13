/**
 * @file ui_settings_screen.h
 * @brief Settings screen — horizontal tileview driven by tile_registry.
 *
 * Owns construction of the settings screen and its tileview.
 * All tile slot creation is registry-driven — this file has zero per-module
 * code and never changes when new module tiles are added.
 *
 * GESTURE CONTRACT:
 *   This module does NOT register gesture callbacks.  The caller must pass
 *   the returned screen and tileview pointers to:
 *     ui_navigation_register_settings(settings_scr, main_scr, tileview)
 *   Navigation owns all gesture registrations and the GESTURE_BUBBLE flag.
 *
 * THEME:
 *   The settings screen and tileview background are themed (unlike main screen).
 *   settings_screen_apply_theme() must be called from apply_ui_theme() in
 *   lvgl_ui.c whenever g_ui_theme changes.  Tile-level theme fan-out is still
 *   done by lvgl_ui.c via tile_registry — this module only owns the screen
 *   and tileview container backgrounds.
 *
 * DEFAULT TILE:
 *   Registry order: [Light(0), System(1), GPS(2), RTC(3), Compass(4), ...]
 *   lv_obj_set_tile_id() is called after build to start on col 1 (System).
 *   Light (col 0) is reachable by swiping left from System.
 *
 * Core 1 only.  Must always be called inside lvgl_port_lock().
 */

#ifndef UI_SETTINGS_SCREEN_H
#define UI_SETTINGS_SCREEN_H

#include "lvgl.h"

/**
 * @brief Create and return the settings screen lv_obj_t*.
 *
 * Iterates tile_registry, creates tileview columns (row 0) and optional
 * sub-tile rows (row 1) for each registered descriptor.  Calls each
 * descriptor's init() and subtile_init() functions.
 *
 * Does NOT register gestures — caller must pass the results to
 * ui_navigation_register_settings().
 *
 * Call once from lvgl_ui_init(), inside lvgl_port_lock().
 *
 * @return lv_obj_t*  Screen object, or NULL on allocation failure.
 */
lv_obj_t *settings_screen_build(void);

/**
 * @brief Return the tileview child of the settings screen.
 *
 * Valid only after settings_screen_build() has been called.
 * Used by lvgl_ui_init() to pass to ui_navigation_register_settings().
 *
 * @return lv_obj_t*  Tileview object, or NULL if not yet built.
 */
lv_obj_t *settings_screen_get_tileview(void);

/**
 * @brief Apply current theme to the settings screen and tileview backgrounds.
 *
 * Call from apply_ui_theme() in lvgl_ui.c whenever g_ui_theme changes.
 * Does NOT fan out to individual tiles — that remains lvgl_ui.c's job via
 * the tile_registry loop.
 *
 * Must be called inside lvgl_port_lock().
 */
void settings_screen_apply_theme(void);

/**
 * @brief Dispatch update() or subtile_update() to the currently active tile.
 *
 * Queries lv_tileview_get_tile_active() and walks the registry to find the
 * matching entry.  Calls only one tile's update function per invocation.
 *
 * Call from task_ui_refresh_fn() when the settings screen is active,
 * inside lvgl_port_lock().
 */
void settings_screen_update_active(void);

#endif // UI_SETTINGS_SCREEN_H
