/**
 * @file ui_navigation.c
 * @brief Screen-state machine — one gesture cb, driven by s_current.
 *
 * GESTURE MODEL:
 *   state=MAIN     + LV_DIR_TOP   → Settings (drawer slides UP from below)
 *   state=MAIN     + LV_DIR_RIGHT → Alarm    (horizontal slide left)
 *   state=SETTINGS + LV_DIR_BOTTOM→ Main     (drawer slides DOWN off-screen)
 *   state=ALARM    + LV_DIR_LEFT  → Main     (horizontal slide right)
 *
 * Stage 31.3: settings is a sibling PANE (child of main screen), animated
 * in/out via translate_y instead of lv_scr_load_anim. Main stays visible
 * under the drawer during the slide. Alarm stays on the old
 * lv_scr_load_anim path (Stage 33.1 converts it to a sibling pane).
 *
 * Stage 31.3c: gestures all dispatch through cb_main_gesture. LVGL 9's
 * GESTURE_BUBBLE flag is a DEFAULT on every non-screen object
 * (lv_obj.c:593), so gestures on the drawer pane / tileview / tile
 * children all bubble UP to main_scr (the only object WITHOUT the
 * flag). Registering a cb on the drawer pane was dead code -- events
 * never reached it. Callback branches on s_current.
 *
 * Sub-tile guard: when the drawer's tileview is on a sub-tile row
 * (GPS sub-tile at (col, 1)), a swipe-down is meant to navigate back
 * to the main tile row (0), NOT to close the drawer. cb_main_gesture
 * compares the active tile against every subtile_handle in the
 * registry; a match skips the close.
 *
 * Core 1 only. All functions called inside lvgl_port_lock().
 */

#include "ui_navigation.h"
#include "ui_lock_screen.h"
#include "ui_animations.h"           // Stage 31.3: drawer slide helpers
#include "ui_settings_screen.h"      // settings_screen_get_tileview
#include "tile_registry.h"           // 31.3b: sub-tile handle comparison
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

// Drawer slide duration (ms). Tuned for perceptible slide without feeling
// laggy on the CO5300 partial-refresh path.
#define DRAWER_ANIM_MS  300

// Sub-tile guard: return true when the active tile is a sub-tile
// (row 1) -- swipe-down should let tileview navigate back to row 0,
// NOT close the drawer.
static bool active_is_subtile(void)
{
    lv_obj_t *tv = settings_screen_get_tileview();
    if (!tv) return false;

    lv_obj_t     *active = lv_tileview_get_tile_active(tv);
    tile_entry_t *tiles  = tile_registry_get();
    uint8_t       count  = tile_registry_count();
    for (uint8_t i = 0; i < count; i++) {
        if (tiles[i].subtile_handle && active == tiles[i].subtile_handle) {
            ESP_LOGD(TAG, "swipe-down on sub-tile col=%u -- tileview navs, drawer stays", i);
            return true;
        }
    }
    return false;
}

static void cb_main_gesture(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());

    // Stage 31.3c: unconditional entry log so bench can trace every
    // dispatch (including the previously-invisible drawer close path).
    ESP_LOGD(TAG, "cb_main_gesture entry dir=%d state=%d asleep=%d",
             (int)dir, (int)s_current, display_is_asleep() ? 1 : 0);

    if (display_is_asleep()) return;

    switch (s_current) {
        case UI_SCREEN_MAIN:
            if (dir == LV_DIR_TOP && s_settings_screen) {
                s_current = UI_SCREEN_SETTINGS;
                kw_ui_animate_slide_in(s_settings_screen, LV_DIR_BOTTOM,
                                       DRAWER_ANIM_MS, 0);
                haptic_play(haptic_get_ui_effect());
                ESP_LOGI(TAG, "→ Settings (drawer opens)");
            }
            else if (dir == LV_DIR_RIGHT && s_alarm_screen) {
                s_current = UI_SCREEN_ALARM;
                lv_scr_load_anim(s_alarm_screen,
                                 LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
                haptic_play(haptic_get_ui_effect());
                ESP_LOGI(TAG, "→ Alarm (swipe right)");
            }
            break;

        case UI_SCREEN_SETTINGS:
            if (dir != LV_DIR_BOTTOM || !s_settings_screen) break;
            if (active_is_subtile()) break;
            s_current = UI_SCREEN_MAIN;
            kw_ui_animate_slide_out(s_settings_screen, LV_DIR_BOTTOM,
                                    DRAWER_ANIM_MS, 0);
            haptic_play(haptic_get_ui_effect());
            ESP_LOGI(TAG, "→ Main (drawer closes)");
            break;

        case UI_SCREEN_ALARM:
            // Alarm gestures are handled by cb_alarm_gesture on the
            // alarm screen itself (still a top-level screen).
            break;
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

    // Stage 31.3c: gesture cb NOT registered on the drawer pane -- all
    // events bubble up to main_scr's cb_main_gesture via the default
    // LV_OBJ_FLAG_GESTURE_BUBBLE on every non-screen obj (lv_obj.c:593).
    // Explicit BUBBLE add on tileview keeps intent visible even though
    // it's redundant with the default flag.
    lv_obj_add_flag(tileview, LV_OBJ_FLAG_GESTURE_BUBBLE);

    ESP_LOGI(TAG, "Settings drawer registered (gestures dispatch via main cb)");
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

bool ui_navigation_is_on_main(void)
{
    return s_current == UI_SCREEN_MAIN;
}