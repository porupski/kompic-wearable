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
#include "data_broker.h"     // broker_env_get_status/set_enabled/get_enabled
#include "app_nvs.h"         // app_nvs_save_height_reference
#include "ui_theme_colors.h"
#include "ui_subjects.h"     // Stage 31.2: subj_env_* string bindings
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

    // Stage 31.2: piggyback the just-updated sample into the UI drain
    // queue so the Δ Height label refreshes on the next 200 ms drain
    // tick instead of waiting for bme688's 2 s cycle.
    if (g_env_q) {
        (void)xQueueOverwrite(g_env_q, &bd);
    }

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
// ---------------------------------------------------------------------------
// Corner-safe layout constants (Stage 31.2 batch b, 2026-09-18).
// AMOLED corners cut ~15% each side per project_display_corner_cutoff.
// Interactive elements must stay INSIDE these pads; text can be a touch
// closer to the edge but never past 40 px.
// Display is 410x502 portrait.
// ---------------------------------------------------------------------------
#define ENV_PAD_H_TEXT   40    // px from left for text-only rows
#define ENV_PAD_H_UI     60    // px from edge for interactive (touch)
#define ENV_PAD_V_UI     40    // px from top/bottom for interactive
#define ENV_ROW_STEP     42    // vertical stride between value labels
#define ENV_ROW_Y0      130    // first data-row y (below divider)

void env_tile_init(lv_obj_t *parent)
{
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ── Status LED (top-left, inside safe zone) ────────────────────────────
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, 16, 16);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, ENV_PAD_H_UI, ENV_PAD_V_UI + 12);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    // ── Chip header label (title font, right of LED) ──────────────────────
    s_lbl_header = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_header, "%s  %s",
        bme688_get_chip_name(), bme688_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, ENV_PAD_H_UI + 26, ENV_PAD_V_UI);

    // ── Power toggle switch (top-right, larger, inside safe zone) ─────────
    // Was 46x22 at (-8, 6) -- 100% inside the corner arc. Bumped ~2x and
    // pulled INSIDE the corner-safe zone so the touch surface is reachable.
    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 90, 46);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, -ENV_PAD_H_UI, ENV_PAD_V_UI);
    lv_obj_add_event_cb(s_sw_power, cb_power_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    // ── Divider ───────────────────────────────────────────────────────────
    s_divider = lv_obj_create(parent);
    lv_obj_set_size(s_divider, 260, 2);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, ENV_PAD_V_UI + 60);
    lv_obj_set_style_bg_color(s_divider, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    // ── Data rows (28 px font, 42 px stride, subject-bound) ───────────────
    // Stage 31.2: labels bind to subj_env_* strings; drain timer pushes.
    // Font bumped 20 -> 28 px (UI_FONT_TITLE). A literal 2x (40 px)
    // needs a new lv_font_conv output; ping Ivan if 28 isn't big enough.
    const lv_font_t *fnt = UI_FONT_TITLE;
    lv_color_t       col = theme_subtext();

    s_lbl_temp = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_temp, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_temp, col, 0);
    lv_obj_align(s_lbl_temp, LV_ALIGN_TOP_LEFT, ENV_PAD_H_TEXT, ENV_ROW_Y0);
    lv_label_bind_text(s_lbl_temp, &subj_env_temp_str, NULL);

    s_lbl_hum = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_hum, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_hum, col, 0);
    lv_obj_align(s_lbl_hum, LV_ALIGN_TOP_LEFT, ENV_PAD_H_TEXT, ENV_ROW_Y0 + ENV_ROW_STEP);
    lv_label_bind_text(s_lbl_hum, &subj_env_hum_str, NULL);

    s_lbl_press = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_press, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_press, col, 0);
    lv_obj_align(s_lbl_press, LV_ALIGN_TOP_LEFT, ENV_PAD_H_TEXT, ENV_ROW_Y0 + 2 * ENV_ROW_STEP);
    lv_label_bind_text(s_lbl_press, &subj_env_press_str, NULL);

    s_lbl_alt = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_alt, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_alt, col, 0);
    lv_obj_align(s_lbl_alt, LV_ALIGN_TOP_LEFT, ENV_PAD_H_TEXT, ENV_ROW_Y0 + 3 * ENV_ROW_STEP);
    lv_label_bind_text(s_lbl_alt, &subj_env_alt_str, NULL);

    s_lbl_delta = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_delta, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_delta, col, 0);
    lv_obj_align(s_lbl_delta, LV_ALIGN_TOP_LEFT, ENV_PAD_H_TEXT, ENV_ROW_Y0 + 4 * ENV_ROW_STEP);
    lv_label_bind_text(s_lbl_delta, &subj_env_delta_str, NULL);

    // ── ZERO HEIGHT button (bottom, larger, inside safe zone) ─────────────
    // Was 200x34 at (0, -8). Bumped size + pulled inside the bottom arc.
    s_btn_zero = lv_btn_create(parent);
    lv_obj_set_size(s_btn_zero, 280, 60);
    lv_obj_align(s_btn_zero, LV_ALIGN_BOTTOM_MID, 0, -ENV_PAD_V_UI);
    lv_obj_set_style_bg_color(s_btn_zero, COL_ACCENT, 0);

    s_lbl_btn_zero = lv_label_create(s_btn_zero);
    lv_label_set_text(s_lbl_btn_zero, "ZERO HEIGHT");
    lv_obj_set_style_text_font(s_lbl_btn_zero, UI_FONT_TITLE, 0);
    lv_obj_center(s_lbl_btn_zero);

    lv_obj_add_event_cb(s_btn_zero, cb_zero_height, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "%s tile init OK", bme688_get_chip_name());
}

// ---------------------------------------------------------------------------
// env_tile_update  — called every 200 ms by task_ui_refresh_fn()
//
// Stage 31.2: value labels (temp / hum / press / alt / delta) are now
// subject-bound and driven by the ui_subjects drain timer (see
// ui_subjects.c). This function retains only the pieces that aren't
// value-tracked observables yet:
//   - Status LED colour (transitions rare; poll fine for now)
//   - Power toggle sync (two-way widget<->broker; Stage 32/33 scope)
//   - ZERO HEIGHT button enable + feedback timeout
// Fully-no-op status ships in Stage 33.3 with the update-cb removal
// from the registry contract.
// ---------------------------------------------------------------------------
void env_tile_update(void)
{
    sensor_status_t st = broker_env_get_status();
    bool data_valid    = (st == SENSOR_ONLINE || st == SENSOR_STALE);

    // ── Status LED ──────────────────────────────────────────────────────────
    update_led(st);

    // ── Power toggle sync ────────────────────────────────────────────────────
    s_syncing = true;
    if (broker_env_get_enabled()) lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
    else                          lv_obj_clear_state(s_sw_power, LV_STATE_CHECKED);
    s_syncing = false;

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