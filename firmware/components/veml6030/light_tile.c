/**
 * @file light_tile.c
 * @brief Ambient light sensor tile -- Type B (controls + live data).
 *
 * Blueprint 10 §7 conformant. Reads broker_light_read() only -- no I2C, no NVS direct.
 * Settings persistence via ui_settings_save_async() (non-blocking queue).
 *
 * Re-entrancy guards (s_syncing_*) prevent feedback loops when update() writes
 * back to switch state from broker data.
 *
 * Header style: lv_led_create() 12x12 TOP_LEFT(12,12) + single merged header
 * label "BH1750  Ambient light (lux)" at TOP_LEFT(30,8) — matches GPS/MAG/IMU
 * tile visual standard (template_module_tile.c §header).
 *
 * Blue-light overlay: parented to lv_layer_top() so it covers BOTH the main
 * screen and the settings screen. light_tile_create_overlay() takes no
 * argument — call it once from lvgl_ui_init() after both screens are built.
 *
 * Haptic feedback: haptic_play(HAPTIC_EFFECT_CLICK) fires on power toggle.
 *
 * Core 1 only. All functions called inside lvgl_port_lock().
 */

#include "light_tile.h"
#include "data_broker.h"
#include "ui_broker.h"          // ui_settings_save_async(), g_saved_brightness, g_blue_light_on
#include "ui_theme_colors.h"
#include "boot_display.h"       // backlight_set_brightness()
#include "haptic.h"             // haptic_play(), HAPTIC_EFFECT_CLICK
#include "veml6030.h"           // veml6030_get_chip_name(), veml6030_get_chip_desc()
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "LIGHT_TILE";

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------

static lv_obj_t *s_parent        = NULL;

static lv_obj_t *s_led_status    = NULL;   // lv_led, 12x12, TOP_LEFT(12,12)
static lv_obj_t *s_lbl_header    = NULL;   // "BH1750  Ambient light (lux)"
static lv_obj_t *s_sw_power      = NULL;
static lv_obj_t *s_lbl_lux       = NULL;
static lv_obj_t *s_lbl_status    = NULL;
static lv_obj_t *s_lbl_auto_row  = NULL;
static lv_obj_t *s_sw_auto       = NULL;
static lv_obj_t *s_lbl_br_row    = NULL;
static lv_obj_t *s_slider_br     = NULL;
static lv_obj_t *s_lbl_br_pct    = NULL;
static lv_obj_t *s_lbl_theme_row = NULL;
static lv_obj_t *s_btn_dark      = NULL;
static lv_obj_t *s_btn_light     = NULL;
static lv_obj_t *s_lbl_bl_row    = NULL;
static lv_obj_t *s_sw_bluelight  = NULL;

// Blue-light amber overlay — parented to lv_layer_top(), covers all screens.
static lv_obj_t *s_bl_overlay    = NULL;

// Re-entrancy guards
static bool s_syncing_power     = false;
static bool s_syncing_auto_br   = false;
static bool s_syncing_theme     = false;
static bool s_syncing_bluelight = false;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void apply_auto_br_state(bool auto_on)
{
    if (s_slider_br) {
        if (auto_on) lv_obj_add_state(s_slider_br, LV_STATE_DISABLED);
        else         lv_obj_clear_state(s_slider_br, LV_STATE_DISABLED);
    }
}

static void apply_bluelight(bool on)
{
    if (!s_bl_overlay) return;
    if (on) lv_obj_clear_flag(s_bl_overlay, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_bl_overlay,   LV_OBJ_FLAG_HIDDEN);
}

static void apply_theme_buttons(ui_theme_t theme)
{
    if (!s_btn_dark || !s_btn_light) return;
    lv_color_t active   = lv_palette_main(LV_PALETTE_BLUE);
    lv_color_t inactive = lv_palette_darken(LV_PALETTE_GREY, 2);
    if (theme == UI_THEME_DARK) {
        lv_obj_set_style_bg_color(s_btn_dark,  active,   0);
        lv_obj_set_style_bg_color(s_btn_light, inactive, 0);
    } else {
        lv_obj_set_style_bg_color(s_btn_dark,  inactive, 0);
        lv_obj_set_style_bg_color(s_btn_light, active,   0);
    }
}

static void save_settings(void)
{
    ui_settings_t cfg = {
        .theme           = g_ui_theme,
        .brightness      = g_saved_brightness,
        .blue_light_on   = g_blue_light_on,
        .auto_brightness = g_auto_brightness,
    };
    ui_settings_save_async(&cfg);
}

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

static void cb_power(lv_event_t *e)
{
    if (s_syncing_power) return;
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    broker_light_set_enabled(on);
    haptic_play(HAPTIC_EFFECT_CLICK);
    ESP_LOGI(TAG, "Light sensor %s", on ? "ON" : "OFF");
}

static void cb_auto_br(lv_event_t *e)
{
    if (s_syncing_auto_br) return;
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    g_auto_brightness = on;
    apply_auto_br_state(on);
    haptic_play(HAPTIC_EFFECT_CLICK);
    save_settings();
    ESP_LOGI(TAG, "Auto-brightness %s", on ? "ON" : "OFF");
}

static void cb_slider(lv_event_t *e)
{
    if (g_auto_brightness) return;
    int val = lv_slider_get_value(lv_event_get_target(e));
    g_saved_brightness = (uint8_t)val;
    backlight_set_brightness((uint8_t)val);
    if (s_lbl_br_pct) lv_label_set_text_fmt(s_lbl_br_pct, "%d%%", val);
    save_settings();
}

static void cb_theme_dark(lv_event_t *e)
{
    if (s_syncing_theme || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_ui_theme = UI_THEME_DARK;
    apply_theme_buttons(UI_THEME_DARK);
    haptic_play(HAPTIC_EFFECT_CLICK);
    save_settings();
}

static void cb_theme_light(lv_event_t *e)
{
    if (s_syncing_theme || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_ui_theme = UI_THEME_LIGHT;
    apply_theme_buttons(UI_THEME_LIGHT);
    haptic_play(HAPTIC_EFFECT_CLICK);
    save_settings();
}

static void cb_bluelight(lv_event_t *e)
{
    if (s_syncing_bluelight) return;
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    g_blue_light_on = on;
    apply_bluelight(on);
    haptic_play(HAPTIC_EFFECT_CLICK);
    save_settings();
}

// ---------------------------------------------------------------------------
// light_tile_init
// ---------------------------------------------------------------------------

void light_tile_init(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "light_tile_init");

    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // 410x502 layout constants (old 240x280 values in /* */).
    static const int LED_SIZE   = 16;  /* 12 */
    static const int LED_X      = 20;  /* 12 */
    static const int LED_Y      = 20;  /* 12 */
    static const int HEADER_X   = 44;  /* 30 */
    static const int HEADER_Y   = 14;  /*  8 */
    static const int SWITCH_X   = -14; /* -8 */
    static const int SWITCH_Y   = 13;  /*  7 */
    static const int DIVIDER_W  = 360; /* 216 */
    static const int DIVIDER_Y  = 50;  /* 32 */
    static const int STATUS_Y   = 79;  /* 44 */
    static const int LUX_Y      = 111; /* 62 */
    static const int ROW_AUTO_Y = 160; /* 90 */
    static const int BR_LBL_Y   = 230; /* 128 */
    static const int BR_SLIDER_Y= 260; /* 146 */
    static const int ROW_THEME_Y= 300; /* 168 */
    static const int ROW_BL_Y   = 370; /* 206 */
    static const int ROW_H      = 40;  /* 30 */

    // -- Status LED (top-left) — matches template_module_tile.c standard -----
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, LED_SIZE, LED_SIZE);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, LED_X, LED_Y);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    // -- Header: chip name + description, merged single label ----------------
    s_lbl_header = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_header, "%s  %s",
        veml6030_get_chip_name(),
        veml6030_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, HEADER_X, HEADER_Y);

    // -- Power switch (top-right) --------------------------------------------
    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 60, 30);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, SWITCH_X, SWITCH_Y);
    lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_sw_power, cb_power, LV_EVENT_VALUE_CHANGED, NULL);

    // -- Divider -------------------------------------------------------------
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_set_size(div, DIVIDER_W, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, DIVIDER_Y);
    lv_obj_set_style_bg_color(div, theme_divider(), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    // -- Status + lux --------------------------------------------------------
    s_lbl_status = lv_label_create(parent);
    lv_label_set_text(s_lbl_status, "Acquiring...");
    lv_obj_set_style_text_color(s_lbl_status, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_text_font(s_lbl_status, UI_FONT_CHIP, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, STATUS_Y);

    s_lbl_lux = lv_label_create(parent);
    lv_label_set_text(s_lbl_lux, "--- lux");
    lv_obj_set_style_text_font(s_lbl_lux, UI_FONT_VALUE, 0);
    lv_obj_set_style_text_color(s_lbl_lux, theme_text(), 0);
    lv_obj_align(s_lbl_lux, LV_ALIGN_TOP_MID, 0, LUX_Y);

    // -- Auto-brightness row -------------------------------------------------
    lv_obj_t *row_auto = lv_obj_create(parent);
    lv_obj_set_size(row_auto, lv_pct(90), ROW_H);
    lv_obj_align(row_auto, LV_ALIGN_TOP_MID, 0, ROW_AUTO_Y);
    lv_obj_set_style_bg_opa(row_auto, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row_auto, 0, 0);
    lv_obj_set_style_pad_all(row_auto, 0, 0);
    lv_obj_clear_flag(row_auto, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_auto_row = lv_label_create(row_auto);
    lv_label_set_text(s_lbl_auto_row, "Auto Brightness");
    lv_obj_set_style_text_font(s_lbl_auto_row, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_auto_row, theme_subtext(), 0);
    lv_obj_align(s_lbl_auto_row, LV_ALIGN_LEFT_MID, 0, 0);

    s_sw_auto = lv_switch_create(row_auto);
    lv_obj_set_size(s_sw_auto, 60, 30);
    lv_obj_align(s_sw_auto, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_sw_auto, cb_auto_br, LV_EVENT_VALUE_CHANGED, NULL);

    // -- Brightness row label + slider ---------------------------------------
    s_lbl_br_row = lv_label_create(parent);
    lv_label_set_text(s_lbl_br_row, "Brightness");
    lv_obj_set_style_text_font(s_lbl_br_row, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_br_row, theme_subtext(), 0);
    lv_obj_align(s_lbl_br_row, LV_ALIGN_TOP_LEFT, 18, BR_LBL_Y);

    s_slider_br = lv_slider_create(parent);
    lv_slider_set_range(s_slider_br, 10, 100);
    lv_slider_set_value(s_slider_br, (int)g_saved_brightness, LV_ANIM_OFF);
    lv_obj_set_size(s_slider_br, lv_pct(65), 18);
    lv_obj_align(s_slider_br, LV_ALIGN_TOP_LEFT, 18, BR_SLIDER_Y);
    lv_obj_add_event_cb(s_slider_br, cb_slider, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_br_pct = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_br_pct, "%d%%", (int)g_saved_brightness);
    lv_obj_set_style_text_font(s_lbl_br_pct, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_br_pct, theme_text(), 0);
    lv_obj_align_to(s_lbl_br_pct, s_slider_br, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    apply_auto_br_state(g_auto_brightness);

    // -- Theme buttons row ---------------------------------------------------
    lv_obj_t *row_theme = lv_obj_create(parent);
    lv_obj_set_size(row_theme, lv_pct(90), ROW_H);
    lv_obj_align(row_theme, LV_ALIGN_TOP_MID, 0, ROW_THEME_Y);
    lv_obj_set_style_bg_opa(row_theme, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row_theme, 0, 0);
    lv_obj_set_style_pad_all(row_theme, 0, 0);
    lv_obj_clear_flag(row_theme, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_theme_row = lv_label_create(row_theme);
    lv_label_set_text(s_lbl_theme_row, "Theme");
    lv_obj_set_style_text_font(s_lbl_theme_row, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_theme_row, theme_subtext(), 0);
    lv_obj_align(s_lbl_theme_row, LV_ALIGN_LEFT_MID, 0, 0);

    s_btn_dark = lv_btn_create(row_theme);
    lv_obj_set_size(s_btn_dark, 52, 24);
    lv_obj_align(s_btn_dark, LV_ALIGN_RIGHT_MID, -58, 0);
    lv_obj_add_event_cb(s_btn_dark, cb_theme_dark, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_d = lv_label_create(s_btn_dark);
    lv_label_set_text(lbl_d, "Dark");
    lv_obj_set_style_text_font(lbl_d, UI_FONT_CHIP, 0);
    lv_obj_center(lbl_d);

    s_btn_light = lv_btn_create(row_theme);
    lv_obj_set_size(s_btn_light, 52, 24);
    lv_obj_align(s_btn_light, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_btn_light, cb_theme_light, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_l = lv_label_create(s_btn_light);
    lv_label_set_text(lbl_l, "Light");
    lv_obj_set_style_text_font(lbl_l, UI_FONT_CHIP, 0);
    lv_obj_center(lbl_l);

    apply_theme_buttons(g_ui_theme);

    // -- Blue-light filter row -----------------------------------------------
    lv_obj_t *row_bl = lv_obj_create(parent);
    lv_obj_set_size(row_bl, lv_pct(90), ROW_H);
    lv_obj_align(row_bl, LV_ALIGN_TOP_MID, 0, ROW_BL_Y);
    lv_obj_set_style_bg_opa(row_bl, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row_bl, 0, 0);
    lv_obj_set_style_pad_all(row_bl, 0, 0);
    lv_obj_clear_flag(row_bl, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_bl_row = lv_label_create(row_bl);
    lv_label_set_text(s_lbl_bl_row, "Blue-light filter");
    lv_obj_set_style_text_font(s_lbl_bl_row, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_bl_row, theme_subtext(), 0);
    lv_obj_align(s_lbl_bl_row, LV_ALIGN_LEFT_MID, 0, 0);

    s_sw_bluelight = lv_switch_create(row_bl);
    lv_obj_set_size(s_sw_bluelight, 44, 22);
    lv_obj_align(s_sw_bluelight, LV_ALIGN_RIGHT_MID, 0, 0);
    if (g_blue_light_on) lv_obj_add_state(s_sw_bluelight, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_sw_bluelight, cb_bluelight, LV_EVENT_VALUE_CHANGED, NULL);

    ESP_LOGI(TAG, "%s tile init OK", veml6030_get_chip_name());
}

// ---------------------------------------------------------------------------
// light_tile_update
// ---------------------------------------------------------------------------

void light_tile_update(void)
{
    if (!s_lbl_lux) return;

    sensor_status_t status   = broker_light_get_status();
    bool            hw_alive = broker_light_hw_alive();
    bool            enabled  = broker_light_get_enabled();

    // -- Status LED ----------------------------------------------------------
    if (s_led_status) {
        lv_color_t col;
        if (!hw_alive)                    col = COL_STATUS_OFFLINE;
        else if (!enabled)                col = COL_STATUS_DISABLED;
        else if (status == SENSOR_ONLINE) col = COL_STATUS_ONLINE;
        else                              col = COL_STATUS_STALE;
        lv_led_set_color(s_led_status, col);
    }

    // -- Power switch sync ---------------------------------------------------
    if (s_sw_power) {
        bool sw_on = lv_obj_has_state(s_sw_power, LV_STATE_CHECKED);
        if (sw_on != enabled) {
            s_syncing_power = true;
            if (enabled) lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
            else         lv_obj_clear_state(s_sw_power, LV_STATE_CHECKED);
            s_syncing_power = false;
        }
    }

    // -- Offline / disabled early-out ----------------------------------------
    if (!hw_alive) {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, "OFFLINE");
            lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_OFFLINE, 0);
        }
        return;
    }
    if (!enabled) {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, "Disabled");
            lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_DISABLED, 0);
        }
        lv_label_set_text(s_lbl_lux, "--- lux");
        return;
    }

    // -- Live sensor data ----------------------------------------------------
    broker_light_data_t ld = {0};
    broker_light_read(&ld);

    if (status == SENSOR_ONLINE) {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, "Online");
            lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_ONLINE, 0);
        }
        char buf[20];
        float lux = ld.lux;
        if (lux < 1.0f) {
            lv_label_set_text(s_lbl_lux, "< 1 lux");
        } else if (lux > 10000.0f) {
            snprintf(buf, sizeof(buf), "%.1fk lux", (double)(lux / 1000.0f));
            lv_label_set_text(s_lbl_lux, buf);
        } else {
            snprintf(buf, sizeof(buf), "%.0f lux", (double)lux);
            lv_label_set_text(s_lbl_lux, buf);
        }
    } else {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, "Acquiring...");
            lv_obj_set_style_text_color(s_lbl_status, lv_palette_main(LV_PALETTE_ORANGE), 0);
        }
    }

    // -- Auto-brightness switch sync -----------------------------------------
    if (s_sw_auto) {
        bool sw_on = lv_obj_has_state(s_sw_auto, LV_STATE_CHECKED);
        if (sw_on != g_auto_brightness) {
            s_syncing_auto_br = true;
            if (g_auto_brightness) lv_obj_add_state(s_sw_auto, LV_STATE_CHECKED);
            else                   lv_obj_clear_state(s_sw_auto, LV_STATE_CHECKED);
            s_syncing_auto_br = false;
            apply_auto_br_state(g_auto_brightness);
        }
    }

    // -- Auto-brightness backlight application --------------------------------
    if (g_auto_brightness && status == SENSOR_ONLINE) {
        backlight_set_brightness(ld.auto_brightness);
        g_saved_brightness = ld.auto_brightness;
        if (s_slider_br)
            lv_slider_set_value(s_slider_br, (int)ld.auto_brightness, LV_ANIM_OFF);
        if (s_lbl_br_pct)
            lv_label_set_text_fmt(s_lbl_br_pct, "%d%%", (int)ld.auto_brightness);
    }

    // -- Blue-light switch sync ----------------------------------------------
    if (s_sw_bluelight) {
        bool sw_on = lv_obj_has_state(s_sw_bluelight, LV_STATE_CHECKED);
        if (sw_on != g_blue_light_on) {
            s_syncing_bluelight = true;
            if (g_blue_light_on) lv_obj_add_state(s_sw_bluelight, LV_STATE_CHECKED);
            else                 lv_obj_clear_state(s_sw_bluelight, LV_STATE_CHECKED);
            s_syncing_bluelight = false;
            apply_bluelight(g_blue_light_on);
        }
    }

    // -- Theme button sync ---------------------------------------------------
    s_syncing_theme = true;
    apply_theme_buttons(g_ui_theme);
    s_syncing_theme = false;
}

// ---------------------------------------------------------------------------
// light_tile_apply_theme
// ---------------------------------------------------------------------------

void light_tile_apply_theme(ui_theme_t theme)
{
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent, theme_bg(), 0);

    if (s_lbl_header)
        lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    if (s_lbl_lux)
        lv_obj_set_style_text_color(s_lbl_lux, theme_text(), 0);
    if (s_lbl_br_pct)
        lv_obj_set_style_text_color(s_lbl_br_pct, theme_text(), 0);
    if (s_lbl_auto_row)
        lv_obj_set_style_text_color(s_lbl_auto_row,  theme_subtext(), 0);
    if (s_lbl_br_row)
        lv_obj_set_style_text_color(s_lbl_br_row,    theme_subtext(), 0);
    if (s_lbl_theme_row)
        lv_obj_set_style_text_color(s_lbl_theme_row, theme_subtext(), 0);
    if (s_lbl_bl_row)
        lv_obj_set_style_text_color(s_lbl_bl_row,    theme_subtext(), 0);

    apply_theme_buttons(theme);
    // s_lbl_status: semantic colour owned by update() — not overridden here.
}

// ---------------------------------------------------------------------------
// light_tile_create_overlay
//
// Creates the amber blue-light filter overlay as a child of lv_layer_top().
// lv_layer_top() sits above all screens unconditionally — the overlay is
// visible whether the main screen or the settings screen is active.
//
// Call ONCE from lvgl_ui_init() after both screens are built, while still
// inside lvgl_port_lock(). No screen pointer argument needed.
// ---------------------------------------------------------------------------

void light_tile_create_overlay(void)
{
    if (s_bl_overlay) return;  // Guard: only create once.

    s_bl_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_bl_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_bl_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_bl_overlay, lv_color_hex(0xFF9500), 0);
    lv_obj_set_style_bg_opa(s_bl_overlay, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_bl_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_bl_overlay, 0, 0);
    lv_obj_clear_flag(s_bl_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_bl_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_bl_overlay, LV_OBJ_FLAG_HIDDEN);

    // Apply initial state from NVS-loaded global
    apply_bluelight(g_blue_light_on);

    ESP_LOGI(TAG, "Blue-light overlay created on lv_layer_top()");
}

// ---------------------------------------------------------------------------
// Tile descriptor
// ---------------------------------------------------------------------------

const tile_desc_t light_tile_desc = {
    .init           = light_tile_init,
    .update         = light_tile_update,
    .apply_theme    = light_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};