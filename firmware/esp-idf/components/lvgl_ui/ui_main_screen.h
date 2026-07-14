/**
 * @file ui_main_screen.h
 * @brief Main watch face screen — clock, date, battery label, status bar.
 *
 * Owns the construction and data refresh of the MAIN screen.
 * The main screen is ALWAYS DARK — exempt from apply_ui_theme().
 * No theme helpers are called here; all colours are hardcoded to the dark palette.
 *
 * Widget layout:
 *   lbl_time     UI_FONT_TITLE,  COL_TEXT_DARK,    LV_ALIGN_CENTER  (0, -30)
 *   lbl_date     UI_FONT_LABEL,  COL_SUBTEXT_DARK,  LV_ALIGN_CENTER  (0,   0)
 *   lbl_battery  UI_FONT_CHIP,   COL_TEXT_DARK,    LV_ALIGN_TOP_RIGHT (-pad, +pad)
 *   status_bar   → delegated to ui_status_bar_init() / ui_status_bar_update()
 *
 * Gesture:
 *   Swipe-up callback is NOT registered here.  The screen object is passed to
 *   ui_navigation_register_main() in lvgl_ui_init() and navigation owns the
 *   gesture contract.
 *
 * Core 1 only.  Must always be called inside lvgl_port_lock().
 */

#ifndef UI_MAIN_SCREEN_H
#define UI_MAIN_SCREEN_H

#include "lvgl.h"
#include "data_broker.h"   // broker_rtc_data_t, broker_battery_data_t

/**
 * @brief Create and return the main screen lv_obj_t*.
 *
 * Creates all widgets (time, date, battery labels + status bar dots).
 * Does NOT register gestures — caller must pass the returned pointer to
 * ui_navigation_register_main().
 *
 * Call once from lvgl_ui_init(), inside lvgl_port_lock().
 *
 * @return lv_obj_t* screen object, or NULL on allocation failure.
 */
lv_obj_t *main_screen_build(void);

/**
 * @brief Update all main screen widgets from fresh broker snapshots.
 *
 * Updates time (with timezone offset), date, battery percentage.
 * Also calls ui_status_bar_update() for LED dot colours.
 *
 * Call from task_ui_refresh_fn() every 200 ms, inside lvgl_port_lock().
 * Safe to call when settings screen is active — widgets update in background.
 *
 * @param rtc  Fresh RTC snapshot (may have valid=false → shows "--:--" / "---").
 * @param bat  Fresh battery snapshot.
 */
void main_screen_update(const broker_rtc_data_t *rtc,
                        const broker_battery_data_t *bat);

#endif // UI_MAIN_SCREEN_H
