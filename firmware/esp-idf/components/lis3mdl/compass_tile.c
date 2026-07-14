/**
 * @file compass_tile.c
 * @brief Compass (QMC5883P) settings tile — LVGL 9 UI, Core 1 only.
 *
 * Tile type   : A (status LED + data rows) + compass rose + calibrate button.
 * Component   : components/qmc5883p/ — co-located with the MAG driver.
 *
 * Layout:
 *   [LED] QMC5883P  3-axis Mag          [SW]
 *   ──────────────────────────────────────
 *   X: -12.4  Y: 45.2  Z: 8.1  µT   [rose]
 *   127°                             N/S/E/W
 *   SE                               needle
 *   ──────────────────────────────────────
 *              [ CALIBRATE ]
 *
 * Key patterns:
 *   - s_syncing guard on power toggle (prevents LV_EVENT_VALUE_CHANGED re-entry)
 *   - Needle rotation via lv_obj_set_style_transform_angle() × 10 (LVGL units)
 *   - Compass rose is a fixed 70×70 circle child; needle is a label child of it
 *   - Calibrate callback is a function pointer set at boot — Core 1 calls it,
 *     Core 0 task_mag_cal picks up the broker flag change
 *   - All float formatting via snprintf — never lv_label_set_text_fmt with %f
 *
 * Core 1 only. No I2C. No NVS.
 * Architecture: Blueprint 3 §6, Blueprint 5 §4–§6, Blueprint 9 §8
 */

#include "compass_tile.h"
#include "lis3mdl.h"           // broker_mag_data_t, get_chip_name/desc
#include "data_broker.h"       // broker_mag_read/get_status/set_enabled/get_enabled
#include "ui_theme_colors.h"
#include "lvgl.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "COMPASS_TILE";

// ---------------------------------------------------------------------------
// Static widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_parent       = NULL;

// Header row
static lv_obj_t *s_led_status   = NULL;
static lv_obj_t *s_lbl_header   = NULL;
static lv_obj_t *s_sw_power     = NULL;

// Divider (stored for theme recolour)
static lv_obj_t *s_divider      = NULL;

// Data rows
static lv_obj_t *s_lbl_xyz      = NULL;  // "X: -12.4  Y: 45.2  Z: 8.1  µT"
static lv_obj_t *s_lbl_heading  = NULL;  // "127°"
static lv_obj_t *s_lbl_cardinal = NULL;  // "SE"

// Compass rose
static lv_obj_t *s_compass_bg   = NULL;  // 70×70 circle container
static lv_obj_t *s_needle       = NULL;  // LV_SYMBOL_UP label, rotated

// Calibrate button
static lv_obj_t *s_btn_cal      = NULL;
static lv_obj_t *s_lbl_btn_cal  = NULL;

// Power toggle re-entrancy guard
static volatile bool s_syncing = false;

// Calibration callback (registered at boot)
static void (*s_cal_cb)(void) = NULL;

// ---------------------------------------------------------------------------
// Cardinal direction helper (identical to legacy + Blueprint 9 §5)
// ---------------------------------------------------------------------------
static const char *heading_to_cardinal(float h)
{
    if (h <  22.5f || h >= 337.5f) return "N";
    if (h <  67.5f)                return "NE";
    if (h < 112.5f)                return "E";
    if (h < 157.5f)                return "SE";
    if (h < 202.5f)                return "S";
    if (h < 247.5f)                return "SW";
    if (h < 292.5f)                return "W";
    return "NW";
}

// ---------------------------------------------------------------------------
// Helper: map sensor_status_t → LED colour
// ---------------------------------------------------------------------------
static void update_led(sensor_status_t st)
{
    lv_color_t col;
    switch (st) {
        case SENSOR_ONLINE:    col = COL_STATUS_ONLINE;    break;
        case SENSOR_OFFLINE:   col = COL_STATUS_OFFLINE;   break;
        case SENSOR_ACQUIRING: col = COL_STATUS_ACQUIRING; break;
        case SENSOR_STALE:     col = COL_STATUS_STALE;     break;
        case SENSOR_DISABLED:  /* fall-through */
        default:               col = COL_STATUS_DISABLED;  break;
    }
    lv_led_set_color(s_led_status, col);
}

// ---------------------------------------------------------------------------
// Callback: power toggle switch
// ---------------------------------------------------------------------------
static void cb_power_toggle(lv_event_t *e)
{
    if (s_syncing) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    broker_mag_set_enabled(on);
}

// ---------------------------------------------------------------------------
// Callback: calibrate button
// ---------------------------------------------------------------------------
static void cb_calibrate(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!broker_mag_get_enabled()) return;
    if (s_cal_cb) s_cal_cb();
}

// ---------------------------------------------------------------------------
// compass_tile_init
// ---------------------------------------------------------------------------
void compass_tile_init(lv_obj_t *parent)
{
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // 410x502 layout constants (old 240x280 values in /* */).
    static const int X_MARGIN   = 20;   /* 12 */
    static const int LED_SIZE   = 16;   /* 12 */
    static const int LED_X      = 20;   /* 12 */
    static const int LED_Y      = 20;   /* 12 */
    static const int HEADER_X   = 44;   /* 30 */
    static const int HEADER_Y   = 13;   /*  7 */
    static const int SWITCH_X   = -14;  /* -8 */
    static const int SWITCH_Y   = 12;   /*  6 */
    static const int DIVIDER_W  = 360;  /* 216 */
    static const int DIVIDER_Y  = 50;   /* 34 */
    static const int XYZ_Y      = 79;   /* 44 */
    static const int HEAD_Y     = 111;  /* 62 */
    static const int CARD_Y     = 179;  /* 100 */
    static const int ROSE_SIZE  = 120;  /* 70 */
    static const int ROSE_X     = -20;  /* -12 */
    static const int ROSE_Y     = 72;   /* 40 */
    static const int BTN_W      = 280;  /* 180 */
    static const int BTN_H      = 40;   /* 30 */
    static const int BTN_Y      = -16;  /* -8 */

    // ── Status LED (top-left) ──────────────────────────────────────────────
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, LED_SIZE, LED_SIZE);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, LED_X, LED_Y);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    // ── Header label ──────────────────────────────────────────────────────
    s_lbl_header = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_header, "%s  %s",
        lis3mdl_get_chip_name(), lis3mdl_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, HEADER_X, HEADER_Y);

    // ── Power toggle switch (top-right) ───────────────────────────────────
    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 60, 30);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, SWITCH_X, SWITCH_Y);
    lv_obj_add_event_cb(s_sw_power, cb_power_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    // ── Divider ───────────────────────────────────────────────────────────
    s_divider = lv_obj_create(parent);
    lv_obj_set_size(s_divider, DIVIDER_W, 1);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, DIVIDER_Y);
    lv_obj_set_style_bg_color(s_divider, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    // ── XYZ row (left column, below divider) ──────────────────────────────
    s_lbl_xyz = lv_label_create(parent);
    lv_label_set_text(s_lbl_xyz, "X:---  Y:---  Z:---  \xc2\xb5T");
    lv_obj_set_style_text_font(s_lbl_xyz, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_xyz, theme_subtext(), 0);
    lv_obj_align(s_lbl_xyz, LV_ALIGN_TOP_LEFT, X_MARGIN, XYZ_Y);

    // ── Heading (large, below XYZ) ────────────────────────────────────────
    s_lbl_heading = lv_label_create(parent);
    lv_label_set_text(s_lbl_heading, "---");
    lv_obj_set_style_text_font(s_lbl_heading, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_heading, theme_subtext(), 0);
    lv_obj_align(s_lbl_heading, LV_ALIGN_TOP_LEFT, X_MARGIN, HEAD_Y);

    // ── Cardinal direction ────────────────────────────────────────────────
    s_lbl_cardinal = lv_label_create(parent);
    lv_label_set_text(s_lbl_cardinal, "---");
    lv_obj_set_style_text_font(s_lbl_cardinal, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_cardinal, theme_subtext(), 0);
    lv_obj_align(s_lbl_cardinal, LV_ALIGN_TOP_LEFT, X_MARGIN, CARD_Y);

    // ── Compass rose (right column) ───────────────────────────────────────
    s_compass_bg = lv_obj_create(parent);
    lv_obj_set_size(s_compass_bg, ROSE_SIZE, ROSE_SIZE);
    lv_obj_align(s_compass_bg, LV_ALIGN_TOP_RIGHT, ROSE_X, ROSE_Y);
    lv_obj_set_style_radius(s_compass_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_compass_bg, lv_color_hex(0x2A2A2E), 0);
    lv_obj_set_style_border_color(s_compass_bg, lv_color_hex(0x48484A), 0);
    lv_obj_set_style_border_width(s_compass_bg, 2, 0);
    lv_obj_set_style_pad_all(s_compass_bg, 0, 0);
    lv_obj_clear_flag(s_compass_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_compass_bg, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Cardinal letters on the rose
    static const struct {
        const char   *t;
        lv_align_t    a;
        int8_t        ox, oy;
    } k_cards[] = {
        {"N", LV_ALIGN_TOP_MID,    0,  3},
        {"S", LV_ALIGN_BOTTOM_MID, 0, -3},
        {"E", LV_ALIGN_RIGHT_MID, -3,  0},
        {"W", LV_ALIGN_LEFT_MID,   3,  0},
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *cl = lv_label_create(s_compass_bg);
        lv_obj_set_style_text_font(cl, UI_FONT_CHIP, 0);
        lv_obj_set_style_text_color(cl, lv_color_hex(0x8E8E93), 0);
        lv_label_set_text(cl, k_cards[i].t);
        lv_obj_align(cl, k_cards[i].a, k_cards[i].ox, k_cards[i].oy);
    }

    // Needle — LV_SYMBOL_UP label, rotated by heading via transform_angle
    s_needle = lv_label_create(s_compass_bg);
    lv_label_set_text(s_needle, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(s_needle, COL_STATUS_OFFLINE, 0);
    lv_obj_set_style_text_font(s_needle, UI_FONT_LABEL, 0);
    lv_obj_center(s_needle);
    // Pivot at visual centre of the glyph — tune if font changes
    lv_obj_set_style_transform_pivot_x(s_needle, 7, 0);
    lv_obj_set_style_transform_pivot_y(s_needle, 9, 0);

    // ── Calibrate button (bottom, centred) ────────────────────────────────
    s_btn_cal = lv_btn_create(parent);
    lv_obj_set_size(s_btn_cal, BTN_W, BTN_H);
    lv_obj_align(s_btn_cal, LV_ALIGN_BOTTOM_MID, 0, BTN_Y);
    lv_obj_set_style_bg_color(s_btn_cal, COL_STATUS_DISABLED, 0);
    lv_obj_add_state(s_btn_cal, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_btn_cal, cb_calibrate, LV_EVENT_CLICKED, NULL);

    s_lbl_btn_cal = lv_label_create(s_btn_cal);
    lv_label_set_text(s_lbl_btn_cal, "CALIBRATE");
    lv_obj_set_style_text_font(s_lbl_btn_cal, UI_FONT_LABEL, 0);
    lv_obj_center(s_lbl_btn_cal);

    ESP_LOGI(TAG, "%s tile init OK", lis3mdl_get_chip_name());
}

// ---------------------------------------------------------------------------
// compass_tile_update — called every 200 ms by task_ui_refresh_fn()
// ---------------------------------------------------------------------------
void compass_tile_update(void)
{
    broker_mag_data_t d  = {0};
    broker_mag_read(&d);
    sensor_status_t   st = broker_mag_get_status();

    // ── Status LED ──────────────────────────────────────────────────────────
    update_led(st);

    // ── Power toggle sync (guard prevents re-entrancy) ─────────────────────
    s_syncing = true;
    if (broker_mag_get_enabled()) lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
    else                          lv_obj_clear_state(s_sw_power, LV_STATE_CHECKED);
    s_syncing = false;

    bool data_ok = (st == SENSOR_ONLINE || st == SENSOR_STALE || st == SENSOR_ACQUIRING);

    // ── XYZ row ──────────────────────────────────────────────────────────────
    if (data_ok) {
        char xs[10], ys[10], zs[10], buf[48];
        snprintf(xs,  sizeof(xs),  "%.0f", (double)d.x_ut);
        snprintf(ys,  sizeof(ys),  "%.0f", (double)d.y_ut);
        snprintf(zs,  sizeof(zs),  "%.0f", (double)d.z_ut);
        snprintf(buf, sizeof(buf), "X:%s  Y:%s  Z:%s  \xc2\xb5T", xs, ys, zs);
        lv_label_set_text(s_lbl_xyz, buf);
        lv_obj_set_style_text_color(s_lbl_xyz, theme_subtext(), 0);
    } else {
        lv_label_set_text(s_lbl_xyz, "X:---  Y:---  Z:---  \xc2\xb5T");
        lv_obj_set_style_text_color(s_lbl_xyz, theme_subtext(), 0);
    }

    // ── Heading + cardinal + needle ──────────────────────────────────────────
    if (data_ok && (st == SENSOR_ONLINE || st == SENSOR_STALE)) {
        char hdg_buf[12];
        snprintf(hdg_buf, sizeof(hdg_buf), "%.0f\xc2\xb0", (double)d.heading_deg);
        lv_label_set_text(s_lbl_heading, hdg_buf);
        lv_label_set_text(s_lbl_cardinal, heading_to_cardinal(d.heading_deg));

        // Colour: green if calibrated, orange if heading valid but uncalibrated
        lv_color_t hcol = d.calibrated ? COL_STATUS_ONLINE : COL_STATUS_STALE;
        lv_obj_set_style_text_color(s_lbl_heading,  hcol, 0);
        lv_obj_set_style_text_color(s_lbl_cardinal, hcol, 0);

        // Needle rotation: LVGL angle unit = 0.1°, so multiply by 10
        int16_t angle_lv = (int16_t)(d.heading_deg * 10.0f);
        lv_obj_set_style_transform_angle(s_needle, angle_lv, 0);
        lv_obj_set_style_opa(s_needle, LV_OPA_COVER, 0);
        lv_obj_invalidate(s_needle);
    } else if (data_ok && st == SENSOR_ACQUIRING) {
        // Calibrating: show dashes, reset needle
        lv_label_set_text(s_lbl_heading, "---");
        lv_label_set_text(s_lbl_cardinal, heading_to_cardinal(0.0f));
        lv_obj_set_style_text_color(s_lbl_heading,  COL_STATUS_ACQUIRING, 0);
        lv_obj_set_style_text_color(s_lbl_cardinal, COL_STATUS_ACQUIRING, 0);
        lv_obj_set_style_transform_angle(s_needle, 0, 0);
        lv_obj_set_style_opa(s_needle, LV_OPA_50, 0);
    } else {
        lv_label_set_text(s_lbl_heading,  "---");
        lv_label_set_text(s_lbl_cardinal, "---");
        lv_obj_set_style_text_color(s_lbl_heading,  theme_subtext(), 0);
        lv_obj_set_style_text_color(s_lbl_cardinal, theme_subtext(), 0);
        lv_obj_set_style_transform_angle(s_needle, 0, 0);
        lv_obj_set_style_opa(s_needle, LV_OPA_30, 0);
    }

    // ── Calibrate button ─────────────────────────────────────────────────────
    if (d.calibrating) {
        char cal_buf[24];
        snprintf(cal_buf, sizeof(cal_buf), "CAL %ds", (int)d.cal_countdown);
        lv_label_set_text(s_lbl_btn_cal, cal_buf);
        lv_obj_set_style_bg_color(s_btn_cal, COL_STATUS_ACQUIRING, 0);
        lv_obj_add_state(s_btn_cal, LV_STATE_DISABLED);
    } else if (st == SENSOR_ONLINE || st == SENSOR_STALE || st == SENSOR_ACQUIRING) {
        lv_label_set_text(s_lbl_btn_cal, "CALIBRATE");
        lv_obj_set_style_bg_color(s_btn_cal, COL_ACCENT, 0);
        lv_obj_clear_state(s_btn_cal, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(s_lbl_btn_cal, "CALIBRATE");
        lv_obj_set_style_bg_color(s_btn_cal, COL_STATUS_DISABLED, 0);
        lv_obj_add_state(s_btn_cal, LV_STATE_DISABLED);
    }
}

// ---------------------------------------------------------------------------
// compass_tile_set_calibrate_callback
// ---------------------------------------------------------------------------
void compass_tile_set_calibrate_callback(void (*cb)(void))
{
    s_cal_cb = cb;
}

// ---------------------------------------------------------------------------
// compass_tile_apply_theme
// ---------------------------------------------------------------------------
void compass_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent,      theme_bg(),      0);
    lv_obj_set_style_bg_color(s_divider,     theme_divider(), 0);
    lv_obj_set_style_text_color(s_lbl_header,   theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_xyz,      theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_heading,  theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_cardinal, theme_subtext(), 0);
    // Rose background stays dark regardless of theme — it's a display element
    // Button accent colour is constant
}

const tile_desc_t compass_tile_desc = {
    .init           = compass_tile_init,
    .update         = compass_tile_update,
    .apply_theme    = compass_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};
