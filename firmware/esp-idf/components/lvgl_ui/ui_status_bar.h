/**
 * @file ui_status_bar.h
 * @brief Status LED dot strip + FA icon labels for the main watch face.
 *
 * Owns the row of 10 px coloured circles at the bottom of the main screen,
 * each with a Font Awesome icon above it.  One dot per broker module.
 *
 * Order (left → right): GPS | MAG | RTC | BAT | LIGHT
 * Colours map directly to sensor_status_t via COL_STATUS_* macros.
 *
 * SET IN STONE once implemented — no changes needed when new module tiles are
 * added to the settings screen.  To add a new dot: increment LED_COUNT here
 * and add one line to ui_status_bar_update() in the .c file.
 *
 * Core 1 only.  All functions must be called inside lvgl_port_lock().
 */

#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include "lvgl.h"

// Number of status dots.  Update together with ui_status_bar_update() body.
#define UI_STATUS_BAR_LED_COUNT  5

/**
 * @brief Create all LED dot and icon label widgets as children of @p parent.
 *
 * Call once from ui_main_screen_build(), inside lvgl_port_lock().
 *
 * @param parent  The main screen lv_obj_t* — dots align to its BOTTOM_MID.
 */
void ui_status_bar_init(lv_obj_t *parent);

/**
 * @brief Read all broker module statuses and update dot colours.
 *
 * Call from task_ui_refresh_fn() every 200 ms, inside lvgl_port_lock().
 * Safe to call even if the main screen is not active — no-op if not yet init.
 */
void ui_status_bar_update(void);

#endif // UI_STATUS_BAR_H
