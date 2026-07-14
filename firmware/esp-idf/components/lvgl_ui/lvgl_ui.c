/**
 * @file lvgl_ui.c
 * @brief LVGL UI manager - thin orchestrator. Wires sub-modules, drives refresh task.
 *
 * ARCHITECTURE (Blueprint 3 REVISEDv3):
 *   This file is the orchestrator only. It contains zero widget code, zero gesture
 *   callbacks, and zero overlay logic. All of that lives in focused sub-modules:
 *
 *     ui_main_screen.c      - clock face, date, battery label
 *     ui_settings_screen.c  - tileview + tile_registry build loop
 *     ui_navigation.c       - swipe gesture callbacks + screen-state machine
 *     ui_status_bar.c       - FA icon + LED dot strip (main screen bottom)
 *     ui_lock_screen.c      - lock overlay, slider, backlight timer state machine
 *     ui_shutdown_overlay.c - shutdown overlay (lv_layer_sys)
 *     ui_notif_overlay.c    - notification / alarm overlay (lv_layer_top)
 *     tile_registry.c       - ordered tile descriptor table (add modules here)
 *     alarm_tile.c          - standalone alarm screen (swipe-right from main)
 *
 * INIT CALL ORDER (all inside lvgl_port_lock):
 *   1. Apply globals from cfg (theme, brightness, blue_light_on)
 *   2. main_screen_build()
 *   3. settings_screen_build()
 *   4. alarm_screen_build()
 *   5. ui_navigation_register_main(main, settings)
 *   6. ui_navigation_register_settings(settings, main, tileview)
 *   7. ui_navigation_register_alarm(alarm)
 *   8. lock_screen_init()
 *   9. shutdown_overlay_init()
 *      light_tile_create_overlay();
 *  10. Initial tile status update
 *  11. alarm_tile_apply_theme() + apply_ui_theme()
 *  12. lv_scr_load(main_screen)
 *
 * REFRESH TASK (200 ms, Core 1):
 *   1. broker_rtc_read + broker_battery_read -> main_screen_update()
 *   2. Theme change check -> apply_ui_theme()
 *   3. lock_screen_poll()
 *   4. shutdown_overlay_poll()
 *   5. UI event queue drain (Core 0 -> Core 1 commands)
 *   6. If settings active -> settings_screen_update_active()
 *   7. If alarm active -> alarm_tile_update()
 */

#include "lvgl_ui.h"
#include "ui_main_screen.h"
#include "ui_settings_screen.h"
#include "ui_navigation.h"
#include "ui_lock_screen.h"
#include "ui_shutdown_overlay.h"
#include "ui_notif_overlay.h"
#include "alarm_tile.h"
#include "tile_registry.h"
#include "ui_theme_colors.h"
#include "data_broker.h"
#include "ui_event.h"
#include "power_flags.h"        // g_wake_display, g_display_sleep, g_show_shutdown_overlay
#include "light_tile.h"         // resolves to components/veml6030/light_tile.h
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LVGL_UI";

// Theme-change sentinel - forces apply_ui_theme() on first frame.
static uint8_t s_last_theme = 0xFF;

// ---------------------------------------------------------------------------
// Theme fan-out (registry-driven, zero per-module code)
// ---------------------------------------------------------------------------

void apply_ui_theme(void)
{
    settings_screen_apply_theme();

    tile_entry_t *tiles = tile_registry_get();
    uint8_t       count = tile_registry_count();

    for (uint8_t i = 0; i < count; i++) {
        if (tiles[i].desc && tiles[i].desc->apply_theme) {
            tiles[i].desc->apply_theme((ui_theme_t)g_ui_theme);
        }
    }

    // Alarm screen theme (not in tile_registry — standalone screen)
    alarm_tile_apply_theme((ui_theme_t)g_ui_theme);

    s_last_theme = g_ui_theme;
    ESP_LOGD(TAG, "Theme applied: %s",
             g_ui_theme == UI_THEME_LIGHT ? "LIGHT" : "DARK");
}

// ---------------------------------------------------------------------------
// UI event queue dispatch (called inside lvgl_port_lock)
// ---------------------------------------------------------------------------

static void drain_ui_event_queue(void)
{
    ui_event_t evt;
    while (xQueueReceive(g_ui_event_q, &evt, 0) == pdTRUE) {
        switch (evt.type) {
            case UI_EVENT_ALARM_FIRED:
                ESP_LOGI(TAG, "UI_EVENT_ALARM_FIRED id=%u snooze=%u",
                         (unsigned)evt.payload.alarm.alarm_id,
                         (unsigned)evt.payload.alarm.snooze_minutes);
                notif_overlay_show_alarm(evt.payload.alarm.alarm_id,
                                        evt.payload.alarm.snooze_minutes);
                break;

            case UI_EVENT_NOTIF_SHOW:
                ESP_LOGI(TAG, "UI_EVENT_NOTIF_SHOW \"%s\" (handler stub)",
                         evt.payload.notif.title);
                break;

            case UI_EVENT_NOTIF_DISMISS:
                ESP_LOGI(TAG, "UI_EVENT_NOTIF_DISMISS (handler stub)");
                break;

            default:
                ESP_LOGW(TAG, "Unknown ui_event type=%d", (int)evt.type);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void lvgl_ui_init(const ui_settings_t *cfg)
{
    g_ui_theme         = cfg->theme;
    g_blue_light_on    = cfg->blue_light_on;
    g_saved_brightness = cfg->brightness;
    s_last_theme       = cfg->theme;

    // -- Build screens --------------------------------------------------------
    lv_obj_t *main_scr     = main_screen_build();
    lv_obj_t *settings_scr = settings_screen_build();
    lv_obj_t *alarm_scr    = alarm_screen_build();

    if (!main_scr || !settings_scr) {
        ESP_LOGE(TAG, "Screen build failed - halting");
        return;
    }
    if (!alarm_scr) {
        ESP_LOGW(TAG, "Alarm screen build failed - alarm disabled");
    }

    // -- Register gestures ----------------------------------------------------
    ui_navigation_register_main(main_scr, settings_scr);
    ui_navigation_register_settings(settings_scr, main_scr,
                                    settings_screen_get_tileview());
    if (alarm_scr) {
        ui_navigation_register_alarm(alarm_scr);
    }

    // -- Overlays (pre-built, hidden) -----------------------------------------
    lock_screen_init();
    shutdown_overlay_init();
    light_tile_create_overlay();

    // -- Initial tile status update -------------------------------------------
    {
        tile_entry_t *tiles = tile_registry_get();
        uint8_t       count = tile_registry_count();

        for (uint8_t i = 0; i < count; i++) {
            if (tiles[i].desc && tiles[i].desc->update) {
                tiles[i].desc->update();
            }
        }
        ESP_LOGI(TAG, "Initial tile status update complete (%u tiles)", count);
    }

    // -- Theme + load ---------------------------------------------------------
    apply_ui_theme();
    lv_scr_load(main_scr);

    ESP_LOGI(TAG, "UI init complete - %u tiles + alarm screen, theme=%s, brightness=%u%%",
             tile_registry_count(),
             g_ui_theme == UI_THEME_LIGHT ? "LIGHT" : "DARK",
             (unsigned)g_saved_brightness);
}

// ---------------------------------------------------------------------------
// UI refresh task (Core 1, 200 ms period)
// ---------------------------------------------------------------------------

void task_ui_refresh_fn(void *arg)
{
    TickType_t last = xTaskGetTickCount();

    while (1) {
        if (lvgl_port_lock(pdMS_TO_TICKS(10))) {

            // -- 1. Broker reads ----------------------------------------------
            broker_rtc_data_t     rtc = {0};
            broker_battery_data_t bat = {0};
            broker_rtc_read(&rtc);
            broker_battery_read(&bat);

            // -- 2. Main screen update ----------------------------------------
            main_screen_update(&rtc, &bat);

            // -- 3. Theme change ----------------------------------------------
            if (g_ui_theme != s_last_theme) {
                apply_ui_theme();
            }

            // -- 4. Lock screen state machine ---------------------------------
            lock_screen_poll();

            // -- 5. Shutdown overlay ------------------------------------------
            shutdown_overlay_poll();

            // -- 6. UI event queue (Core 0 -> Core 1 commands) ----------------
            drain_ui_event_queue();

            // -- 7. Active screen dispatch ------------------------------------
            ui_screen_state_t cur = ui_navigation_current();
            if (cur == UI_SCREEN_SETTINGS) {
                settings_screen_update_active();
            } else if (cur == UI_SCREEN_ALARM) {
                alarm_tile_update();
            }

            lvgl_port_unlock();
        }

        vTaskDelayUntil(&last, pdMS_TO_TICKS(200));
    }
}