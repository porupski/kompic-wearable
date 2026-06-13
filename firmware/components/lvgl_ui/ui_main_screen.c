/**
 * @file ui_main_screen.c
 * @brief Main watch face screen — clock, date, battery label, status bar.
 *
 * This file owns the main screen object and its three dynamic label widgets.
 * All colours are hardcoded to the DARK palette — the main screen is exempt
 * from the theme system and never changes appearance regardless of g_ui_theme.
 *
 * Timezone offset (g_tz_offset_hours) is declared extern here and defined in
 * data_broker.c.  It is applied to rtc.hour at update time so the displayed
 * time reflects the local timezone without any modification to stored RTC data.
 *
 * The status bar (LED dots + FA icons) is fully delegated to ui_status_bar.c.
 * main_screen_build() calls ui_status_bar_init(screen) and main_screen_update()
 * calls ui_status_bar_update().  No LED logic lives in this file.
 *
 * Gesture registration is NOT done here.  The returned screen pointer is passed
 * by lvgl_ui_init() to ui_navigation_register_main() — navigation owns all
 * gesture callbacks.
 *
 * Core 1 only.  No I2C.  No NVS.  No broker writes.
 * All functions must be called inside lvgl_port_lock() / lvgl_port_unlock().
 */

#include "ui_main_screen.h"
#include "ui_status_bar.h"
#include "ui_theme_colors.h"
#include "data_broker.h"
#include "boot_display.h"      // LCD_H_RES, LCD_V_RES
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "MAIN_SCR";

// Timezone offset — defined in data_broker.c, read here for clock display.
extern volatile int8_t g_tz_offset_hours;

// ---------------------------------------------------------------------------
// Module-static widget handles
// ---------------------------------------------------------------------------

static lv_obj_t *s_screen      = NULL;
static lv_obj_t *s_lbl_time    = NULL;
static lv_obj_t *s_lbl_date    = NULL;
static lv_obj_t *s_lbl_battery = NULL;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

lv_obj_t *main_screen_build(void)
{
    s_screen = lv_obj_create(NULL);
    if (!s_screen) {
        ESP_LOGE(TAG, "Failed to create main screen");
        return NULL;
    }

    // Always DARK — never themed.
    lv_obj_set_style_bg_color(s_screen, COL_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Time label ────────────────────────────────────────────────────────
    // Large centred display, shifted 30 px above centre to make room for date.
    s_lbl_time = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_time, "--:--");
    lv_obj_set_style_text_font(s_lbl_time, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_time, COL_TEXT_DARK, 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_CENTER, 0, -30);

    // ── Date label ────────────────────────────────────────────────────────
    // Subtext colour, sits directly below the time label.
    s_lbl_date = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_date, "---");
    lv_obj_set_style_text_font(s_lbl_date, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_date, COL_SUBTEXT_DARK, 0);
    lv_obj_align(s_lbl_date, LV_ALIGN_CENTER, 0, 0);

    // ── Battery label ─────────────────────────────────────────────────────
    // Small chip-style label in the top-right corner.
    s_lbl_battery = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_battery, "--%");
    lv_obj_set_style_text_font(s_lbl_battery, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_battery, COL_TEXT_DARK, 0);
    lv_obj_align(s_lbl_battery, LV_ALIGN_TOP_RIGHT, -UI_TILE_PAD_H, UI_TILE_PAD_V);

    // ── Status bar ────────────────────────────────────────────────────────
    // Delegates entirely to ui_status_bar.c — dot creation, spacing, icons.
    ui_status_bar_init(s_screen);

    ESP_LOGI(TAG, "Main screen built");
    return s_screen;
}

void main_screen_update(const broker_rtc_data_t *rtc,
                        const broker_battery_data_t *bat)
{
    // Guard: not yet built
    if (!s_screen) return;

    // ── Time and date ─────────────────────────────────────────────────────
    if (rtc && rtc->valid) {
        // Apply timezone offset with 24 h wrap.  Addition before modulo
        // handles negative offsets without invoking UB.
        int local_h = ((int)rtc->hour + (int)g_tz_offset_hours + 24) % 24;

        char tbuf[8];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02u", local_h, rtc->minute);
        lv_label_set_text(s_lbl_time, tbuf);

        static const char * const k_weekdays[] = {
            "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
        };
        const char *wstr = (rtc->weekday <= 6) ? k_weekdays[rtc->weekday] : "---";

        char dbuf[20];
        snprintf(dbuf, sizeof(dbuf), "%s %04u-%02u-%02u",
                 wstr, rtc->year, rtc->month, rtc->day);
        lv_label_set_text(s_lbl_date, dbuf);

    } else {
        lv_label_set_text(s_lbl_time, "--:--");
        lv_label_set_text(s_lbl_date, "---");
    }

    // ── Battery ───────────────────────────────────────────────────────────
    if (bat) {
        char bbuf[16];
        if (bat->charging) {
            // Show lightning bolt when charging
            snprintf(bbuf, sizeof(bbuf), "%u%% \xe2\x9a\xa1", bat->percentage);
        } else {
            snprintf(bbuf, sizeof(bbuf), "%u%%", bat->percentage);
        }
        lv_label_set_text(s_lbl_battery, bbuf);
    }

    // ── Status bar LED dots ───────────────────────────────────────────────
    ui_status_bar_update();
}
