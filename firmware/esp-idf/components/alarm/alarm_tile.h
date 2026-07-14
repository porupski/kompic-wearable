/**
 * @file alarm_tile.h
 * @brief Alarm screen — standalone LVGL screen (swipe-right from main clock).
 *
 * NOT part of the settings tileview. This is a separate screen object,
 * same architectural pattern as the main clock face.
 *
 * Navigation: main clock --[swipe right]--> alarm screen
 *             alarm screen --[swipe left]--> main clock
 *             Wired in ui_navigation.c.
 *
 * Core 1 only. All functions called inside lvgl_port_lock().
 * Architecture: Blueprint 16
 */

#ifndef ALARM_TILE_H
#define ALARM_TILE_H

#include "lvgl.h"
#include "data_broker.h"   // ui_theme_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build and return the alarm screen object.
 * Call once from lvgl_ui_init(), inside lvgl_port_lock().
 * @return lv_obj_t* screen, or NULL on failure.
 */
lv_obj_t *alarm_screen_build(void);

/**
 * @brief Update alarm screen widgets from broker data.
 * Called from task_ui_refresh_fn() when alarm screen is active,
 * inside lvgl_port_lock(). 200 ms period.
 */
void alarm_tile_update(void);

/**
 * @brief Apply current theme to alarm screen widgets.
 * Called from apply_ui_theme() in lvgl_ui.c.
 * Must be called inside lvgl_port_lock().
 */
void alarm_tile_apply_theme(ui_theme_t theme);

#ifdef __cplusplus
}
#endif

#endif // ALARM_TILE_H