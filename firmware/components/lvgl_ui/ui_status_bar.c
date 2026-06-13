/**
 * @file ui_status_bar.c
 * @brief Status LED dot strip + FA icon labels — main watch face bottom strip.
 *
 * Layout (bottom of main screen):
 *   [GPS icon]  [MAG icon]  [RTC icon]  [BAT icon]  [LIGHT icon]
 *   [ dot  ]    [ dot  ]    [ dot  ]    [ dot  ]    [ dot  ]
 *
 * Dots are 10 px circles, spaced 30 px apart, centred at x=0 / BOTTOM_MID.
 * Icons sit 14 px above their dot using the fa5_select_14 custom font.
 * Colours come exclusively from COL_STATUS_* macros in ui_theme_colors.h.
 *
 * SET IN STONE — do not edit after initial integration test passes.
 * To add a new module dot: edit LED_COUNT in ui_status_bar.h and add one
 * broker call at the end of ui_status_bar_update().
 *
 * Core 1 only.  No I2C.  No NVS.  No broker writes.
 */

#include "ui_status_bar.h"
#include "ui_theme_colors.h"
#include "data_broker.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "STATUS_BAR";

// ---------------------------------------------------------------------------
// Font Awesome 5 glyphs (fa5_select_14 — custom subset font)
// ---------------------------------------------------------------------------

LV_FONT_DECLARE(fa5_select_14);

#define FA_GPS    "\xEF\x81\x81"   // fa-location-arrow  U+F041
#define FA_MAG    "\xEF\x85\x8E"   // fa-compass         U+F14E
#define FA_RTC    "\xEF\x80\x97"   // fa-clock-o         U+F017
#define FA_BAT    "\xEF\x89\x80"   // fa-battery-full    U+F240
#define FA_LIGHT  "\xEF\x86\x85"   // fa-sun-o           U+F185

// ---------------------------------------------------------------------------
// Module-static state
// ---------------------------------------------------------------------------

// Dot and icon widget handles — indexed to match LED order above.
static lv_obj_t *s_dots [UI_STATUS_BAR_LED_COUNT] = {0};
static lv_obj_t *s_icons[UI_STATUS_BAR_LED_COUNT] = {0};

// Spacing geometry: dots centred at BOTTOM_MID, spaced SPACING px apart.
#define DOT_SPACING   30
#define DOT_Y_OFFSET  (-8)    // px above bottom edge for dot centre
#define ICON_Y_OFFSET (-22)   // px above bottom edge for icon label

// FA icon string in broker index order
static const char * const k_icons[UI_STATUS_BAR_LED_COUNT] = {
    FA_GPS, FA_MAG, FA_RTC, FA_BAT, FA_LIGHT
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void set_dot_color(lv_obj_t *dot, sensor_status_t st)
{
    if (!dot) return;
    lv_color_t col;
    switch (st) {
        case SENSOR_DISABLED:  col = COL_STATUS_DISABLED;  break;
        case SENSOR_OFFLINE:   col = COL_STATUS_OFFLINE;   break;
        case SENSOR_ACQUIRING: col = COL_STATUS_ACQUIRING; break;
        case SENSOR_STALE:     col = COL_STATUS_STALE;     break;
        case SENSOR_ONLINE:    col = COL_STATUS_ONLINE;    break;
        case SENSOR_NOTIF:     col = COL_STATUS_NOTIF;     break;
        default:               col = COL_STATUS_DISABLED;  break;
    }
    lv_obj_set_style_bg_color(dot, col, 0);
}

static lv_obj_t *create_dot(lv_obj_t *parent, int x_off)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, COL_STATUS_DISABLED, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, x_off, DOT_Y_OFFSET);
    return dot;
}

static lv_obj_t *create_icon(lv_obj_t *parent, int x_off, const char *glyph)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, glyph);
    lv_obj_set_style_text_font(lbl, &fa5_select_14, 0);
    lv_obj_set_style_text_color(lbl, COL_SUBTEXT_DARK, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, x_off, ICON_Y_OFFSET);
    return lbl;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ui_status_bar_init(lv_obj_t *parent)
{
    if (!parent) {
        ESP_LOGE(TAG, "ui_status_bar_init: null parent");
        return;
    }

    // Centre the strip horizontally: compute left-most x offset.
    const int start_x = -(((int)UI_STATUS_BAR_LED_COUNT - 1) * DOT_SPACING) / 2;

    for (int i = 0; i < UI_STATUS_BAR_LED_COUNT; i++) {
        int x = start_x + i * DOT_SPACING;
        s_dots[i]  = create_dot(parent, x);
        s_icons[i] = create_icon(parent, x, k_icons[i]);
    }

    ESP_LOGI(TAG, "Status bar init OK — %d dots", UI_STATUS_BAR_LED_COUNT);
}

void ui_status_bar_update(void)
{
    // Guard: no-op if init hasn't run yet (s_dots[0] == NULL)
    if (!s_dots[0]) return;

    // Indexed to match k_icons order: GPS=0, MAG=1, RTC=2, BAT=3, LIGHT=4
    set_dot_color(s_dots[0], broker_gps_get_status());
    set_dot_color(s_dots[1], broker_mag_get_status());
    set_dot_color(s_dots[2], broker_rtc_get_status());
    set_dot_color(s_dots[3], broker_battery_get_status());
    set_dot_color(s_dots[4], broker_light_get_status());
}
