/**
 * @file ui_navigation.c
 * @brief Screen-state machine — native LVGL gesture callbacks.
 *
 * GESTURE MODEL:
 *   Main screen   LV_DIR_TOP   → Settings (vertical slide up)
 *   Main screen   LV_DIR_RIGHT → Alarm    (horizontal slide left)
 *   Settings scr  LV_DIR_BOTTOM→ Main     (vertical slide down)
 *   Alarm screen  LV_DIR_LEFT  → Main     (horizontal slide right)
 *
 * Core 1 only. All functions called inside lvgl_port_lock().
 */

#include "ui_navigation.h"
#include "ui_lock_screen.h"
#include "haptic.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "UI_NAV";

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static ui_screen_state_t s_current         = UI_SCREEN_MAIN;
static lv_obj_t         *s_main_screen     = NULL;
static lv_obj_t         *s_settings_screen = NULL;
static lv_obj_t         *s_alarm_screen    = NULL;

// ---------------------------------------------------------------------------
// Gesture callbacks
// ---------------------------------------------------------------------------

static void cb_main_gesture(lv_event_t *e)
{
    if (display_is_asleep()) return;
    if (s_current != UI_SCREEN_MAIN) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());

    if (dir == LV_DIR_TOP && s_settings_screen) {
        s_current = UI_SCREEN_SETTINGS;
        lv_scr_load_anim(s_settings_screen,
                         LV_SCR_LOAD_ANIM_MOVE_TOP, 200, 0, false);
        haptic_play(haptic_get_ui_effect());
        ESP_LOGI(TAG, "→ Settings (swipe up)");
    }
    else if (dir == LV_DIR_RIGHT && s_alarm_screen) {
        s_current = UI_SCREEN_ALARM;
        lv_scr_load_anim(s_alarm_screen,
                         LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        haptic_play(haptic_get_ui_effect());
        ESP_LOGI(TAG, "→ Alarm (swipe right)");
    }
}

static void cb_settings_gesture(lv_event_t *e)
{
    if (display_is_asleep()) return;
    if (s_current != UI_SCREEN_SETTINGS) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());

    if (dir == LV_DIR_BOTTOM && s_main_screen) {
        s_current = UI_SCREEN_MAIN;
        lv_scr_load_anim(s_main_screen,
                         LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 200, 0, false);
        haptic_play(haptic_get_ui_effect());
        ESP_LOGI(TAG, "→ Main (swipe down)");
    }
}

static void cb_alarm_gesture(lv_event_t *e)
{
    if (display_is_asleep()) return;
    if (s_current != UI_SCREEN_ALARM) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());

    if (dir == LV_DIR_LEFT && s_main_screen) {
        s_current = UI_SCREEN_MAIN;
        lv_scr_load_anim(s_main_screen,
                         LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
        haptic_play(haptic_get_ui_effect());
        ESP_LOGI(TAG, "→ Main (swipe left from alarm)");
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void ui_navigation_register_main(lv_obj_t *main_scr, lv_obj_t *settings_scr)
{
    if (!main_scr || !settings_scr) {
        ESP_LOGE(TAG, "register_main: null argument");
        return;
    }
    s_main_screen     = main_scr;
    s_settings_screen = settings_scr;

    lv_obj_add_event_cb(main_scr, cb_main_gesture, LV_EVENT_GESTURE, NULL);

    ESP_LOGI(TAG, "Main screen registered");
}

void ui_navigation_register_settings(lv_obj_t *settings_scr,
                                     lv_obj_t *main_scr,
                                     lv_obj_t *tileview)
{
    if (!settings_scr || !main_scr || !tileview) {
        ESP_LOGE(TAG, "register_settings: null argument");
        return;
    }
    s_settings_screen = settings_scr;
    s_main_screen     = main_scr;

    lv_obj_add_event_cb(settings_scr, cb_settings_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(tileview, LV_OBJ_FLAG_GESTURE_BUBBLE);

    ESP_LOGI(TAG, "Settings screen registered, gesture + bubble set");
}

void ui_navigation_register_alarm(lv_obj_t *alarm_scr)
{
    if (!alarm_scr) {
        ESP_LOGE(TAG, "register_alarm: null argument");
        return;
    }
    s_alarm_screen = alarm_scr;

    lv_obj_add_event_cb(alarm_scr, cb_alarm_gesture, LV_EVENT_GESTURE, NULL);

    ESP_LOGI(TAG, "Alarm screen registered");
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

ui_screen_state_t ui_navigation_current(void)
{
    return s_current;
}