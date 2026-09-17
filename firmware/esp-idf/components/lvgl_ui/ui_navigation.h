/**
 * @file ui_navigation.h
 * @brief Screen-state machine and native LVGL gesture callback registration.
 *
 * GESTURE MODEL:
 *   Main screen  ──[swipe UP]──────► Settings screen
 *   Main screen  ──[swipe RIGHT]───► Alarm screen
 *   Settings     ──[swipe DOWN]────► Main screen
 *   Alarm        ──[swipe LEFT]────► Main screen
 *
 * Core 1 only. All functions called inside lvgl_port_lock().
 */

#ifndef UI_NAVIGATION_H
#define UI_NAVIGATION_H

#include "lvgl.h"
#include "lvgl_ui.h"   // ui_screen_state_t

/**
 * @brief Register the main screen and attach its LV_EVENT_GESTURE callback.
 *        Handles swipe-up → settings AND swipe-right → alarm.
 */
void ui_navigation_register_main(lv_obj_t *main_scr, lv_obj_t *settings_scr);

/**
 * @brief Register the settings screen + tileview.
 *        Handles swipe-down → main.
 */
void ui_navigation_register_settings(lv_obj_t *settings_scr,
                                     lv_obj_t *main_scr,
                                     lv_obj_t *tileview);

/**
 * @brief Register the alarm screen.
 *        Handles swipe-left → main.
 *        Call after alarm_screen_build() in lvgl_ui_init().
 */
void ui_navigation_register_alarm(lv_obj_t *alarm_scr);

/**
 * @brief Return the currently active screen state.
 */
ui_screen_state_t ui_navigation_current(void);

/**
 * @brief Convenience predicate: is the main screen (clock face) currently
 *        the active LVGL screen? Cross-core safe -- reads a single aligned
 *        int written only by the LVGL task on Core 1. Used by field_capture
 *        to narrow the double-tap-to-lock gesture so accidental double taps
 *        while browsing tiles / settings do NOT sleep the panel (Stage 28
 *        §4.5).
 */
bool ui_navigation_is_on_main(void);

#endif // UI_NAVIGATION_H