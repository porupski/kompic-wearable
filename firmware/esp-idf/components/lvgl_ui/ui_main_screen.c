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

/* Stage 30.2 bring-up diagnostic pattern: 6 pure-colour squares (R G B W Y C)
 * plus a pure-white shear-test band. Set to 0 to hide once the port renders
 * cleanly. Ivan reads back the visible colours + geometry over serial so we
 * can decode which specific pipeline bug is firing (byte order / MADCTL.BGR
 * / window alignment / strip-boundary artefacts).
 */
#define UI_MAIN_DIAG_PATTERN  1

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
// Diagnostic pattern (temporary, Stage 30.2)
// ---------------------------------------------------------------------------
#if UI_MAIN_DIAG_PATTERN
static void main_screen_build_diag(lv_obj_t *parent)
{
    /* Left-to-right, expected order: R G B W Y C.
     * Decode key:
     *   R shows as B  and B shows as R  -> MADCTL.BGR bit wrong (R/B swap).
     *   Y shows as C  and C shows as Y  -> confirms R/B swap.
     *   W not pure white               -> one channel dropped or byte drift.
     *   G shifts hue                    -> byte alignment (not multiple of 3).
     *   Square edges slant             -> horizontal window off by N pixels
     *                                    (shear across each 25-row strip).
     *   Extra/missing squares          -> column-wrap wrong at panel side.
     */
    static const uint32_t k_diag_colours[6] = {
        0xFF0000,  /* R */
        0x00FF00,  /* G */
        0x0000FF,  /* B */
        0xFFFFFF,  /* W */
        0xFFFF00,  /* Y */
        0x00FFFF,  /* C */
    };

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 396, 46);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 90);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 3, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < 6; i++) {
        lv_obj_t *sq = lv_obj_create(row);
        lv_obj_set_size(sq, 40, 40);
        lv_obj_set_style_bg_color(sq, lv_color_hex(k_diag_colours[i]), 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(sq, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(sq, 1, 0);
        lv_obj_set_style_radius(sq, 0, 0);
        lv_obj_set_style_pad_all(sq, 0, 0);
        lv_obj_clear_flag(sq, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Full-width pure-white band -- best signal for shear. If any row inside
     * this band tints or shifts, the CASET/RASET window doesn't match the
     * pixel-count LVGL is sending, or the panel is wrapping columns wrong.
     */
    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_set_size(band, 380, 6);
    lv_obj_align(band, LV_ALIGN_CENTER, 0, 145);
    lv_obj_set_style_bg_color(band, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(band, 0, 0);
    lv_obj_set_style_radius(band, 0, 0);
    lv_obj_set_style_pad_all(band, 0, 0);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    ESP_LOGI(TAG, "DIAG pattern on: 6 squares R G B W Y C (L->R) + white band");
}
#endif  /* UI_MAIN_DIAG_PATTERN */

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
    // Large centred glanceable clock. Stage 28 §2.1: bumped Montserrat
    // 28 -> 48 (dedicated UI_FONT_TIME_XL alias) so time is readable at
    // arm's length. Centre offset bumped -30 -> -50 to keep the taller
    // glyph clear of the date label below.
    s_lbl_time = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_time, "--:--");
    lv_obj_set_style_text_font(s_lbl_time, UI_FONT_TIME_XL, 0);
    lv_obj_set_style_text_color(s_lbl_time, COL_TEXT_DARK, 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_CENTER, 0, -50);

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

#if UI_MAIN_DIAG_PATTERN
    main_screen_build_diag(s_screen);
#endif

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
