/**
 * @file env_tile.c
 * @brief Environment sensor tile — LVGL 9 UI, Core 1 only.
 *
 * Tile type   : A (status LED + data rows + action button)
 * Layout      : Header, divider, 5 data rows, ZERO HEIGHT button + Δ height label.
 * No sub-tile in Phase 14.
 *
 * Data rows:
 *   Temp:      23.4 °C
 *   Hum:       48.2 %
 *   Press:     1012.3 hPa
 *   Alt:       34 m
 *   Δ Height:  +1.2 m  (or "-- not zeroed --" until button pressed)
 *
 * Zero Height button:
 *   - Reads current altitude_m from broker (no I2C)
 *   - Writes home_ref_altitude_m + home_ref_valid back to broker (read-before-write)
 *   - Saves to NVS via app_nvs_save_height_reference() — safe from Core 1 (NVS mutex)
 *   - Shows "ZEROED ✓" feedback for ZERO_FEEDBACK_MS then reverts
 *
 * Power toggle:
 *   - s_syncing guard prevents LV_EVENT_VALUE_CHANGED re-entrancy
 *
 * Core 1 only. No I2C. No uart. No direct nvs_* calls (app_nvs wrapper only).
 *
 * Architecture: Blueprint 3 §6, Blueprint 5 §4–§6, Blueprint 14a §6
 */

#include "env_tile.h"
#include "bme688_drv.h"      // bme688_get_chip_name/desc, broker_env_data_t
#include "data_broker.h"     // broker_env_read/get_status/set_enabled/get_enabled
#include "app_nvs.h"         // app_nvs_save_height_reference
#include "ui_theme_colors.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"       // esp_timer_get_time() for button feedback timing

static const char *TAG = "ENV_TILE";

// ---------------------------------------------------------------------------
// Zero Height feedback timing
// ---------------------------------------------------------------------------
#define ZERO_FEEDBACK_MS  2000U

// ---------------------------------------------------------------------------
// Static widget handles
// ---------------------------------------------------------------------------

static lv_obj_t *s_parent       = NULL;

// Header row
static lv_obj_t *s_led_status   = NULL;
static lv_obj_t *s_lbl_header   = NULL;
static lv_obj_t *s_sw_power     = NULL;

// Divider
static lv_obj_t *s_divider      = NULL;

// Data rows
static lv_obj_t *s_lbl_temp     = NULL;  // "Temp:      23.4 °C"
static lv_obj_t *s_lbl_hum      = NULL;  // "Hum:       48.2 %"
static lv_obj_t *s_lbl_press    = NULL;  // "Press:     1012.3 hPa"
static lv_obj_t *s_lbl_alt      = NULL;  // "Alt:       34 m"
static lv_obj_t *s_lbl_delta    = NULL;  // "Δ Height:  +1.2 m"

// Zero Height button + its label
static lv_obj_t *s_btn_zero     = NULL;
static lv_obj_t *s_lbl_btn_zero = NULL;

// Button feedback state
static bool     s_showing_feedback  = false;
static int64_t  s_feedback_start_us = 0;

// Power toggle re-entrancy guard
static bool s_syncing = false;

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
        case SENSOR_NOTIF:     col = COL_STATUS_NOTIF;     break;
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
    bool new_val = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    broker_env_set_enabled(new_val);
}

// ---------------------------------------------------------------------------
// Callback: ZERO HEIGHT button
// ---------------------------------------------------------------------------
static void cb_zero_height(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    // Read current altitude from broker — no I2C from Core 1
    broker_env_data_t d = {0};
    broker_env_read(&d);

    float ref = d.altitude_m;

    // Read-before-write: preserve all other fields, update only home_ref fields
    broker_env_data_t bd = {0};
    broker_env_read(&bd);
    bd.home_ref_altitude_m = ref;
    bd.home_ref_valid      = true;
    broker_env_write(&bd);

    // Persist to NVS — safe from Core 1 (app_nvs uses NVS internal mutex)
    esp_err_t ret = app_nvs_save_height_reference(ref);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Height zeroed at %.1f m", (double)ref);
    } else {
        ESP_LOGW(TAG, "NVS save failed: %s", esp_err_to_name(ret));
    }

    // Show feedback on button
    lv_obj_add_state(s_btn_zero, LV_STATE_DISABLED);
    lv_label_set_text(s_lbl_btn_zero, "ZEROED \xe2\x9c\x93");
    s_showing_feedback   = true;
    s_feedback_start_us  = esp_timer_get_time();
}

// ---------------------------------------------------------------------------
// env_tile_init
// ---------------------------------------------------------------------------
void env_tile_init(lv_obj_t *parent)
{
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ── Status LED (top-left) ──────────────────────────────────────────────
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, 12, 12);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    // ── Chip header label ─────────────────────────────────────────────────
    s_lbl_header = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_header, "%s  %s",
        bme688_get_chip_name(), bme688_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, 30, 7);

    // ── Power toggle switch (top-right) ───────────────────────────────────
    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 46, 22);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_add_event_cb(s_sw_power, cb_power_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    // ── Divider ───────────────────────────────────────────────────────────
    s_divider = lv_obj_create(parent);
    lv_obj_set_size(s_divider, 216, 1);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(s_divider, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    // ── Data rows (y starts at 44, step 22 px) ────────────────────────────
    const lv_font_t *fnt = UI_FONT_LABEL;
    lv_color_t       col = theme_subtext();

    s_lbl_temp = lv_label_create(parent);
    lv_label_set_text(s_lbl_temp, "Temp:      ---");
    lv_obj_set_style_text_font(s_lbl_temp, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_temp, col, 0);
    lv_obj_align(s_lbl_temp, LV_ALIGN_TOP_LEFT, 12, 44);

    s_lbl_hum = lv_label_create(parent);
    lv_label_set_text(s_lbl_hum, "Hum:       ---");
    lv_obj_set_style_text_font(s_lbl_hum, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_hum, col, 0);
    lv_obj_align(s_lbl_hum, LV_ALIGN_TOP_LEFT, 12, 66);

    s_lbl_press = lv_label_create(parent);
    lv_label_set_text(s_lbl_press, "Press:     ---");
    lv_obj_set_style_text_font(s_lbl_press, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_press, col, 0);
    lv_obj_align(s_lbl_press, LV_ALIGN_TOP_LEFT, 12, 88);

    s_lbl_alt = lv_label_create(parent);
    lv_label_set_text(s_lbl_alt, "Alt:       ---");
    lv_obj_set_style_text_font(s_lbl_alt, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_alt, col, 0);
    lv_obj_align(s_lbl_alt, LV_ALIGN_TOP_LEFT, 12, 110);

    s_lbl_delta = lv_label_create(parent);
    lv_label_set_text(s_lbl_delta, "\xce\x94 Height:  -- not zeroed --");
    lv_obj_set_style_text_font(s_lbl_delta, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_delta, col, 0);
    lv_obj_align(s_lbl_delta, LV_ALIGN_TOP_LEFT, 12, 132);

    // ── ZERO HEIGHT button (bottom) ───────────────────────────────────────
    s_btn_zero = lv_btn_create(parent);
    lv_obj_set_size(s_btn_zero, 200, 34);
    lv_obj_align(s_btn_zero, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_btn_zero, COL_ACCENT, 0);

    s_lbl_btn_zero = lv_label_create(s_btn_zero);
    lv_label_set_text(s_lbl_btn_zero, "ZERO HEIGHT");
    lv_obj_set_style_text_font(s_lbl_btn_zero, UI_FONT_LABEL, 0);
    lv_obj_center(s_lbl_btn_zero);

    lv_obj_add_event_cb(s_btn_zero, cb_zero_height, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "%s tile init OK", bme688_get_chip_name());
}

// ---------------------------------------------------------------------------
// env_tile_update  — called every 200 ms by task_ui_refresh_fn()
// ---------------------------------------------------------------------------
void env_tile_update(void)
{
    broker_env_data_t d  = {0};
    broker_env_read(&d);
    sensor_status_t   st = broker_env_get_status();

    // ── Status LED ──────────────────────────────────────────────────────────
    update_led(st);

    // ── Power toggle sync ────────────────────────────────────────────────────
    s_syncing = true;
    if (broker_env_get_enabled()) lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
    else                          lv_obj_clear_state(s_sw_power, LV_STATE_CHECKED);
    s_syncing = false;

    // ── Data validity gate ───────────────────────────────────────────────────
    bool data_valid = (st == SENSOR_ONLINE || st == SENSOR_STALE);

    // ── Temp row ──────────────────────────────────────────────────────────────
    if (data_valid) {
        char buf[48];
        int t_w = (int)d.temperature_c;
        int t_d = (int)((d.temperature_c - t_w) * 10);
        if (t_d < 0) t_d = -t_d;
        snprintf(buf, sizeof(buf), "Temp:      %d.%d \xc2\xb0""C", t_w, t_d);
        lv_label_set_text(s_lbl_temp, buf);
    } else {
        lv_label_set_text(s_lbl_temp, "Temp:      ---");
    }

    // ── Humidity row ─────────────────────────────────────────────────────────
    if (data_valid) {
        char buf[48];
        int h_w = (int)d.humidity_pct;
        int h_d = (int)((d.humidity_pct - h_w) * 10);
        if (h_d < 0) h_d = -h_d;
        snprintf(buf, sizeof(buf), "Hum:       %d.%d %%", h_w, h_d);
        lv_label_set_text(s_lbl_hum, buf);
    } else {
        lv_label_set_text(s_lbl_hum, "Hum:       ---");
    }

    // ── Pressure row ─────────────────────────────────────────────────────────
    if (data_valid) {
        char buf[48];
        int p_w = (int)d.pressure_hpa;
        int p_d = (int)((d.pressure_hpa - p_w) * 10);
        if (p_d < 0) p_d = -p_d;
        snprintf(buf, sizeof(buf), "Press:     %d.%d hPa", p_w, p_d);
        lv_label_set_text(s_lbl_press, buf);
    } else {
        lv_label_set_text(s_lbl_press, "Press:     ---");
    }

    // ── Altitude row ─────────────────────────────────────────────────────────
    if (data_valid) {
        char buf[48];
        int a_w = (int)d.altitude_m;
        snprintf(buf, sizeof(buf), "Alt:       %d m", a_w);
        lv_label_set_text(s_lbl_alt, buf);
    } else {
        lv_label_set_text(s_lbl_alt, "Alt:       ---");
    }

    // ── Δ Height row ─────────────────────────────────────────────────────────
    if (data_valid && d.home_ref_valid) {
        float delta = d.altitude_m - d.home_ref_altitude_m;
        char buf[48];
        // Format with explicit sign: +1.2 m or -0.5 m
        int d_w = (int)delta;
        int d_f = (int)((delta - (float)d_w) * 10);
        if (d_f < 0) d_f = -d_f;
        char sign = (delta >= 0.0f) ? '+' : '-';
        if (d_w < 0) d_w = -d_w;
        snprintf(buf, sizeof(buf), "\xce\x94 Height:  %c%d.%d m", sign, d_w, d_f);
        lv_label_set_text(s_lbl_delta, buf);
    } else if (!d.home_ref_valid) {
        lv_label_set_text(s_lbl_delta, "\xce\x94 Height:  -- not zeroed --");
    } else {
        lv_label_set_text(s_lbl_delta, "\xce\x94 Height:  ---");
    }

    // ── Zero Height button enable/disable ─────────────────────────────────────
    // Enable only when sensor is online and not showing feedback.
    if (!s_showing_feedback) {
        if (data_valid) {
            lv_obj_clear_state(s_btn_zero, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_btn_zero, LV_STATE_DISABLED);
        }
    }

    // ── Button feedback timeout ───────────────────────────────────────────────
    if (s_showing_feedback) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_feedback_start_us) / 1000LL;
        if (elapsed_ms >= ZERO_FEEDBACK_MS) {
            lv_label_set_text(s_lbl_btn_zero, "ZERO HEIGHT");
            lv_obj_clear_state(s_btn_zero, LV_STATE_DISABLED);
            s_showing_feedback = false;
        }
    }
}

// ---------------------------------------------------------------------------
// env_tile_apply_theme
// ---------------------------------------------------------------------------
void env_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;  // theme_xxx() helpers read g_ui_theme internally
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent,    theme_bg(),      0);
    lv_obj_set_style_bg_color(s_divider,   theme_divider(), 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_temp,   theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_hum,    theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_press,  theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_alt,    theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_delta,  theme_subtext(), 0);
    // Button accent colour is constant — no theme dependency.
}

// ---------------------------------------------------------------------------
// Tile descriptor
// ---------------------------------------------------------------------------
const tile_desc_t env_tile_desc = {
    .init           = env_tile_init,
    .update         = env_tile_update,
    .apply_theme    = env_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};