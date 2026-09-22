/**
 * @file compass_tile.c
 * @brief Compass (LIS3MDLTR) settings tile -- LVGL 9 UI, Core 1 only.
 *
 * Stage 32.1 rewrite -- subject-bound. XYZ / heading / cardinal / cal-button
 * labels bound to subj_mag_*_str via lv_label_bind_text. Power switch
 * bound two-way via lv_obj_bind_checked against subj_mag_enabled (observer
 * in ui_subjects.c pushes to broker).
 *
 * compass_tile_update() keeps ONLY the numeric / style ops that don't
 * cleanly reduce to text bindings:
 *   - LED colour, heading + cardinal colour (semantic).
 *   - Needle rotation via transform_angle (numeric).
 *   - Calibrate-button colour + enable state.
 * All text updates are drain-driven from ui_subjects.
 *
 * Layout (portrait 410x502, 60H/40V corner-safe pads):
 *   Row 0 (y=40):  LED + header + power switch
 *   Divider y=100
 *   y=115  status? (skipped, LED conveys)
 *   y=120  XYZ row
 *   y=155  heading (large)
 *   y=225  cardinal
 *   y=100..280 right column: compass rose 140x140
 *   Bottom: CALIBRATE button
 */

#include "compass_tile.h"
#include "lis3mdl.h"           // broker_mag_data_t, get_chip_name/desc
#include "data_broker.h"       // broker_mag_read/get_status/set_enabled
#include "ui_theme_colors.h"
#include "ui_subjects.h"       // subj_mag_*, subj_mag_enabled
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "COMPASS_TILE";

// ---------------------------------------------------------------------------
// Corner-safe layout constants
// ---------------------------------------------------------------------------
#define MAG_PAD_H_UI      60
#define MAG_PAD_H_TEXT    40
#define MAG_PAD_V_UI      40
#define MAG_DIVIDER_Y    100
#define MAG_XYZ_Y        120
#define MAG_HEAD_Y       160
#define MAG_CARD_Y       230
#define MAG_ROSE_SIZE    140
#define MAG_ROSE_Y       110
#define MAG_BTN_W        280
#define MAG_BTN_H         60

// ---------------------------------------------------------------------------
// Static widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_parent       = NULL;

static lv_obj_t *s_led_status   = NULL;
static lv_obj_t *s_lbl_header   = NULL;
static lv_obj_t *s_sw_power     = NULL;
static lv_obj_t *s_divider      = NULL;

static lv_obj_t *s_lbl_xyz      = NULL;
static lv_obj_t *s_lbl_heading  = NULL;
static lv_obj_t *s_lbl_cardinal = NULL;

static lv_obj_t *s_compass_bg   = NULL;
static lv_obj_t *s_needle       = NULL;

static lv_obj_t *s_btn_cal      = NULL;
static lv_obj_t *s_lbl_btn_cal  = NULL;

// Calibration callback (registered at boot from bring-up code)
static void (*s_cal_cb)(void) = NULL;

// ---------------------------------------------------------------------------
// Helper: map sensor_status_t -> LED colour
// ---------------------------------------------------------------------------
static void update_led(sensor_status_t st)
{
    lv_color_t col;
    switch (st) {
        case SENSOR_ONLINE:    col = COL_STATUS_ONLINE;    break;
        case SENSOR_OFFLINE:   col = COL_STATUS_OFFLINE;   break;
        case SENSOR_ACQUIRING: col = COL_STATUS_ACQUIRING; break;
        case SENSOR_STALE:     col = COL_STATUS_STALE;     break;
        case SENSOR_DISABLED:
        default:               col = COL_STATUS_DISABLED;  break;
    }
    lv_led_set_color(s_led_status, col);
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

    // ── Row 0: LED + header + power switch ────────────────────────────────
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, 16, 16);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, MAG_PAD_H_UI, MAG_PAD_V_UI + 12);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    s_lbl_header = lv_label_create(parent);
    lv_label_set_text(s_lbl_header, lis3mdl_get_chip_name());   // chip-name only
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, MAG_PAD_H_UI + 26, MAG_PAD_V_UI);

    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 90, 46);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, -MAG_PAD_H_UI, MAG_PAD_V_UI);
    // Two-way bind (user tap -> subject -> observer -> broker; drain
    // mirrors broker back). Old cb_power_toggle event cb deleted.
    lv_obj_bind_checked(s_sw_power, &subj_mag_enabled);

    // ── Divider ───────────────────────────────────────────────────────────
    s_divider = lv_obj_create(parent);
    lv_obj_set_size(s_divider, 260, 2);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, MAG_DIVIDER_Y);
    lv_obj_set_style_bg_color(s_divider, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    // ── XYZ row (subject-bound) ───────────────────────────────────────────
    s_lbl_xyz = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_xyz, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_xyz, theme_subtext(), 0);
    lv_obj_align(s_lbl_xyz, LV_ALIGN_TOP_LEFT, MAG_PAD_H_TEXT, MAG_XYZ_Y);
    lv_label_bind_text(s_lbl_xyz, &subj_mag_xyz_str, NULL);

    // ── Heading (large, subject-bound; colour owned by update()) ──────────
    s_lbl_heading = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_heading, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_heading, theme_subtext(), 0);
    lv_obj_align(s_lbl_heading, LV_ALIGN_TOP_LEFT, MAG_PAD_H_TEXT, MAG_HEAD_Y);
    lv_label_bind_text(s_lbl_heading, &subj_mag_heading_str, NULL);

    // ── Cardinal (subject-bound) ──────────────────────────────────────────
    s_lbl_cardinal = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_cardinal, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_cardinal, theme_subtext(), 0);
    lv_obj_align(s_lbl_cardinal, LV_ALIGN_TOP_LEFT, MAG_PAD_H_TEXT, MAG_CARD_Y);
    lv_label_bind_text(s_lbl_cardinal, &subj_mag_cardinal_str, NULL);

    // ── Compass rose (right column) ───────────────────────────────────────
    s_compass_bg = lv_obj_create(parent);
    lv_obj_set_size(s_compass_bg, MAG_ROSE_SIZE, MAG_ROSE_SIZE);
    lv_obj_align(s_compass_bg, LV_ALIGN_TOP_RIGHT, -MAG_PAD_H_UI, MAG_ROSE_Y);
    lv_obj_set_style_radius(s_compass_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_compass_bg, lv_color_hex(0x2A2A2E), 0);
    lv_obj_set_style_border_color(s_compass_bg, lv_color_hex(0x48484A), 0);
    lv_obj_set_style_border_width(s_compass_bg, 2, 0);
    lv_obj_set_style_pad_all(s_compass_bg, 0, 0);
    lv_obj_clear_flag(s_compass_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_compass_bg, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

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

    s_needle = lv_label_create(s_compass_bg);
    lv_label_set_text(s_needle, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(s_needle, COL_STATUS_OFFLINE, 0);
    lv_obj_set_style_text_font(s_needle, UI_FONT_LABEL, 0);
    lv_obj_center(s_needle);
    lv_obj_set_style_transform_pivot_x(s_needle, 7, 0);
    lv_obj_set_style_transform_pivot_y(s_needle, 9, 0);

    // ── Calibrate button (subject-bound label; colour + state in update) ──
    s_btn_cal = lv_btn_create(parent);
    lv_obj_set_size(s_btn_cal, MAG_BTN_W, MAG_BTN_H);
    lv_obj_align(s_btn_cal, LV_ALIGN_BOTTOM_MID, 0, -MAG_PAD_V_UI);
    lv_obj_set_style_bg_color(s_btn_cal, COL_STATUS_DISABLED, 0);
    lv_obj_add_state(s_btn_cal, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_btn_cal, cb_calibrate, LV_EVENT_CLICKED, NULL);

    s_lbl_btn_cal = lv_label_create(s_btn_cal);
    lv_obj_set_style_text_font(s_lbl_btn_cal, UI_FONT_TITLE, 0);
    lv_obj_center(s_lbl_btn_cal);
    lv_label_bind_text(s_lbl_btn_cal, &subj_mag_cal_btn_str, NULL);

    ESP_LOGI(TAG, "%s tile init OK (subject-bound)", lis3mdl_get_chip_name());
}

// ---------------------------------------------------------------------------
// compass_tile_update -- LED + heading colour + needle rotation +
//                        cal-button colour/state. Text is drain-driven.
// ---------------------------------------------------------------------------
void compass_tile_update(void)
{
    if (!s_led_status) return;

    broker_mag_data_t d = {0};
    broker_mag_read(&d);
    sensor_status_t st = broker_mag_get_status();

    update_led(st);

    bool data_ok  = (st == SENSOR_ONLINE || st == SENSOR_STALE || st == SENSOR_ACQUIRING);
    bool has_fix  = (st == SENSOR_ONLINE || st == SENSOR_STALE) && d.enabled;

    // -- Heading + cardinal colour tint ----------------------------------------
    lv_color_t hcol;
    if (has_fix)                      hcol = d.calibrated ? COL_STATUS_ONLINE : COL_STATUS_STALE;
    else if (data_ok && d.enabled)    hcol = COL_STATUS_ACQUIRING;
    else                              hcol = theme_subtext();
    lv_obj_set_style_text_color(s_lbl_heading,  hcol, 0);
    lv_obj_set_style_text_color(s_lbl_cardinal, hcol, 0);

    // -- Needle rotation -------------------------------------------------------
    if (has_fix) {
        int16_t angle_lv = (int16_t)(d.heading_deg * 10.0f);
        lv_obj_set_style_transform_angle(s_needle, angle_lv, 0);
        lv_obj_set_style_opa(s_needle, LV_OPA_COVER, 0);
    } else if (data_ok && d.enabled) {
        lv_obj_set_style_transform_angle(s_needle, 0, 0);
        lv_obj_set_style_opa(s_needle, LV_OPA_50, 0);
    } else {
        lv_obj_set_style_transform_angle(s_needle, 0, 0);
        lv_obj_set_style_opa(s_needle, LV_OPA_30, 0);
    }

    // -- Calibrate button colour + enable state --------------------------------
    if (d.calibrating) {
        lv_obj_set_style_bg_color(s_btn_cal, COL_STATUS_ACQUIRING, 0);
        lv_obj_add_state(s_btn_cal, LV_STATE_DISABLED);
    } else if (data_ok && d.enabled) {
        lv_obj_set_style_bg_color(s_btn_cal, COL_ACCENT, 0);
        lv_obj_clear_state(s_btn_cal, LV_STATE_DISABLED);
    } else {
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
    // heading + cardinal colour semantic; owned by update()
    // Rose bg + button accent stay constant across themes.
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
