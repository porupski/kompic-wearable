/**
 * @file haptic_tile.c
 * @brief Haptic motor tile -- Type B (status + controls).
 *
 * Blueprint 5 §7 / Blueprint 13 conformant. Reads broker_haptic_read() only.
 *
 * Header: lv_led 12x12 TOP_LEFT(12,12) + single merged label at TOP_LEFT(30,8).
 *
 * Effect roller: 13 named effects. Preview fires on scroll. SET button saves
 * to NVS via haptic_set_ui_effect(). s_syncing_roller guard prevents update()
 * from fighting user selection (same pattern as power switch re-entrancy guard).
 * Roller styled explicitly for both MAIN and SELECTED parts — required in
 * LVGL 9 to avoid white-box artefact and invisible text in dark theme.
 *
 * IMU sweep UI: IMU CAL button → 5s countdown → sweep with live amplitude bar
 * and per-step info. SET FREQ button visible during sweep for manual override.
 *
 * Core 1 only. All functions called inside lvgl_port_lock().
 */

#include "haptic_tile.h"
#include "data_broker.h"
#include "haptic.h"
#include "ui_theme_colors.h"
#include "drv2605.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "HAPTIC_TILE";

// ---------------------------------------------------------------------------
// Effect roller definitions
// ---------------------------------------------------------------------------

static const char *EFFECT_ROLLER_OPTS =
    "Click\n"
    "Click Soft\n"
    "Double Click\n"
    "Triple Click\n"
    "Soft Bump\n"
    "Strong Buzz\n"
    "Soft Buzz\n"
    "Alert Pulse\n"
    "Transition Click\n"
    "Pulsing Sharp\n"
    "Long Buzz\n"
    "Short Double\n"
    "Sharp Tick";

static const uint8_t EFFECT_IDS[] = {
    HAPTIC_EFFECT_CLICK,
    HAPTIC_EFFECT_CLICK_SOFT,
    HAPTIC_EFFECT_DOUBLE_CLICK,
    HAPTIC_EFFECT_TRIPLE_CLICK,
    HAPTIC_EFFECT_SOFT_BUMP,
    HAPTIC_EFFECT_STRONG_BUZZ,
    HAPTIC_EFFECT_SOFT_BUZZ,
    HAPTIC_EFFECT_ALERT_750MS,
    HAPTIC_EFFECT_TRANSITION_CLICK,
    HAPTIC_EFFECT_PULSING_SHARP,
    HAPTIC_EFFECT_LONG_BUZZ,
    HAPTIC_EFFECT_SHORT_DOUBLE,
    HAPTIC_EFFECT_SHARP_TICK,
};
#define EFFECT_COUNT  (sizeof(EFFECT_IDS) / sizeof(EFFECT_IDS[0]))

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------

static lv_obj_t *s_parent         = NULL;

static lv_obj_t *s_led_status     = NULL;
static lv_obj_t *s_lbl_header     = NULL;
static lv_obj_t *s_sw_power       = NULL;

static lv_obj_t *s_lbl_status     = NULL;
static lv_obj_t *s_lbl_freq       = NULL;

static lv_obj_t *s_lbl_sweep_info = NULL;
static lv_obj_t *s_bar_amp        = NULL;

static lv_obj_t *s_btn_imu_cal    = NULL;
static lv_obj_t *s_lbl_btn_imu   = NULL;
static lv_obj_t *s_btn_set        = NULL;
static lv_obj_t *s_lbl_btn_set    = NULL;

static lv_obj_t *s_roller_effect   = NULL;
static lv_obj_t *s_btn_confirm     = NULL;
static lv_obj_t *s_lbl_btn_confirm = NULL;

// ---------------------------------------------------------------------------
// Re-entrancy guards
// ---------------------------------------------------------------------------

static bool s_syncing_power  = false;
static bool s_syncing_roller = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint16_t effect_id_to_roller_idx(uint8_t id)
{
    for (uint16_t i = 0; i < EFFECT_COUNT; i++) {
        if (EFFECT_IDS[i] == id) return i;
    }
    return 0;
}

static void roller_apply_theme(void)
{
    if (!s_roller_effect) return;
    // Main part: match tile bg and theme text
    lv_obj_set_style_bg_color(s_roller_effect, theme_bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_roller_effect, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_roller_effect, theme_text(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_roller_effect, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_roller_effect, theme_divider(), LV_PART_MAIN);
    // Selected row: blue accent, always white text
    lv_obj_set_style_bg_color(s_roller_effect,
        lv_palette_main(LV_PALETTE_BLUE), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_roller_effect, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller_effect,
        lv_color_white(), LV_PART_SELECTED);
}

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

static void cb_power(lv_event_t *e)
{
    if (s_syncing_power) return;
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    broker_haptic_set_enabled(on);
    haptic_play(haptic_get_ui_effect());
    ESP_LOGI(TAG, "Haptic %s", on ? "ON" : "OFF");
}

static void cb_roller_changed(lv_event_t *e)
{
    if (s_syncing_roller) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t idx = lv_roller_get_selected(lv_event_get_target(e));
    if (idx >= EFFECT_COUNT) return;
    haptic_play(EFFECT_IDS[idx]);   // preview only — does NOT save
}

static void cb_confirm_effect(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_roller_effect) return;
    uint16_t idx = lv_roller_get_selected(s_roller_effect);
    if (idx >= EFFECT_COUNT) return;
    haptic_set_ui_effect(EFFECT_IDS[idx]);   // saves NVS + plays confirmation
    ESP_LOGI(TAG, "UI effect set: id=%d", EFFECT_IDS[idx]);
}

static void cb_imu_cal(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    haptic_sweep_set_imu_mode(true);
    haptic_sweep_start();
    ESP_LOGI(TAG, "IMU sweep started");
}

static void cb_set_freq(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    haptic_sweep_set();
    ESP_LOGI(TAG, "Sweep: SET FREQ pressed");
}

// ---------------------------------------------------------------------------
// haptic_tile_init
// ---------------------------------------------------------------------------

void haptic_tile_init(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "haptic_tile_init");

    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // -- Status LED ----------------------------------------------------------
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, 12, 12);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    // -- Header --------------------------------------------------------------
    s_lbl_header = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_header, "%s  %s",
        haptic_get_chip_name(),
        haptic_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, 30, 8);

    // -- Power switch --------------------------------------------------------
    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 44, 22);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, -8, 7);
    lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_sw_power, cb_power, LV_EVENT_VALUE_CHANGED, NULL);

    // -- Divider -------------------------------------------------------------
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_set_size(div, 216, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_color(div, theme_divider(), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    // -- Cal status + freq ---------------------------------------------------
    s_lbl_status = lv_label_create(parent);
    lv_label_set_text(s_lbl_status, "Not Calibrated");
    lv_obj_set_style_text_color(s_lbl_status,
        lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_text_font(s_lbl_status, UI_FONT_CHIP, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_LEFT, 12, 40);

    s_lbl_freq = lv_label_create(parent);
    lv_label_set_text(s_lbl_freq, "Freq: --- Hz");
    lv_obj_set_style_text_font(s_lbl_freq, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_freq, theme_subtext(), 0);
    lv_obj_align(s_lbl_freq, LV_ALIGN_TOP_LEFT, 12, 56);

    // -- Sweep info label ----------------------------------------------------
    s_lbl_sweep_info = lv_label_create(parent);
    lv_label_set_text(s_lbl_sweep_info, "");
    lv_obj_set_style_text_font(s_lbl_sweep_info, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_sweep_info,
        lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_align(s_lbl_sweep_info, LV_ALIGN_TOP_LEFT, 12, 72);

    // -- Amplitude bar (hidden until sweep active) ---------------------------
    s_bar_amp = lv_bar_create(parent);
    lv_bar_set_range(s_bar_amp, 0, 100);
    lv_bar_set_value(s_bar_amp, 0, LV_ANIM_OFF);
    lv_obj_set_size(s_bar_amp, 190, 8);
    lv_obj_align(s_bar_amp, LV_ALIGN_TOP_LEFT, 12, 88);
    lv_obj_set_style_bg_color(s_bar_amp,
        lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
    lv_obj_add_flag(s_bar_amp, LV_OBJ_FLAG_HIDDEN);

    // -- Effect roller -------------------------------------------------------
    s_roller_effect = lv_roller_create(parent);
    lv_roller_set_options(s_roller_effect, EFFECT_ROLLER_OPTS, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_effect, 3);
    lv_obj_set_width(s_roller_effect, 140);
    lv_obj_align(s_roller_effect, LV_ALIGN_TOP_LEFT, 12, 102);
    lv_obj_set_style_text_font(s_roller_effect, UI_FONT_CHIP, 0);
    roller_apply_theme();
    // Set initial selection BEFORE registering callback — avoids boot haptic
    lv_roller_set_selected(s_roller_effect,
        effect_id_to_roller_idx(haptic_get_ui_effect()), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_roller_effect, cb_roller_changed,
        LV_EVENT_VALUE_CHANGED, NULL);

    // -- SET (confirm) button ------------------------------------------------
    s_btn_confirm = lv_btn_create(parent);
    lv_obj_set_size(s_btn_confirm, 54, 36);
    lv_obj_align_to(s_btn_confirm, s_roller_effect, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_set_style_bg_color(s_btn_confirm,
        lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_add_event_cb(s_btn_confirm, cb_confirm_effect, LV_EVENT_CLICKED, NULL);
    s_lbl_btn_confirm = lv_label_create(s_btn_confirm);
    lv_label_set_text(s_lbl_btn_confirm, "SET");
    lv_obj_set_style_text_font(s_lbl_btn_confirm, UI_FONT_CHIP, 0);
    lv_obj_center(s_lbl_btn_confirm);

    // -- IMU CAL button (bottom-left) ----------------------------------------
    s_btn_imu_cal = lv_btn_create(parent);
    lv_obj_set_size(s_btn_imu_cal, 110, 32);
    lv_obj_align(s_btn_imu_cal, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_add_event_cb(s_btn_imu_cal, cb_imu_cal, LV_EVENT_CLICKED, NULL);
    s_lbl_btn_imu = lv_label_create(s_btn_imu_cal);
    lv_label_set_text(s_lbl_btn_imu, "IMU CAL");
    lv_obj_set_style_text_font(s_lbl_btn_imu, UI_FONT_CHIP, 0);
    lv_obj_center(s_lbl_btn_imu);

    // -- SET FREQ button (bottom-right, hidden until sweep active) -----------
    s_btn_set = lv_btn_create(parent);
    lv_obj_set_size(s_btn_set, 84, 32);
    lv_obj_align(s_btn_set, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_bg_color(s_btn_set,
        lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_add_event_cb(s_btn_set, cb_set_freq, LV_EVENT_CLICKED, NULL);
    s_lbl_btn_set = lv_label_create(s_btn_set);
    lv_label_set_text(s_lbl_btn_set, "SET FREQ");
    lv_obj_set_style_text_font(s_lbl_btn_set, UI_FONT_CHIP, 0);
    lv_obj_center(s_lbl_btn_set);
    lv_obj_add_flag(s_btn_set, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "%s tile init OK", haptic_get_chip_name());
}

// ---------------------------------------------------------------------------
// haptic_tile_update
// ---------------------------------------------------------------------------

void haptic_tile_update(void)
{
    if (!s_lbl_status) return;

    bool hw_alive = broker_haptic_hw_alive();
    bool enabled  = broker_haptic_get_enabled();

    // -- Status LED ----------------------------------------------------------
    if (s_led_status) {
        lv_color_t col;
        if (!hw_alive)     col = COL_STATUS_OFFLINE;
        else if (!enabled) col = COL_STATUS_DISABLED;
        else               col = COL_STATUS_ONLINE;
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
        lv_label_set_text(s_lbl_status, "OFFLINE");
        lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_OFFLINE, 0);
        if (s_btn_imu_cal) lv_obj_add_state(s_btn_imu_cal, LV_STATE_DISABLED);
        if (s_btn_set)     lv_obj_add_flag(s_btn_set, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!enabled) {
        lv_label_set_text(s_lbl_status, "Disabled");
        lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_DISABLED, 0);
        if (s_btn_imu_cal) lv_obj_add_state(s_btn_imu_cal, LV_STATE_DISABLED);
        if (s_btn_set)     lv_obj_add_flag(s_btn_set, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (s_btn_imu_cal) lv_obj_clear_state(s_btn_imu_cal, LV_STATE_DISABLED);

    broker_haptic_data_t hd = {0};
    broker_haptic_read(&hd);

    // -- Countdown phase -----------------------------------------------------
    if (hd.sweep_countdown) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Place flat... %ds",
                 (int)hd.sweep_countdown_sec);
        lv_label_set_text(s_lbl_status, "IMU Cal: hold still");
        lv_obj_set_style_text_color(s_lbl_status,
            lv_palette_main(LV_PALETTE_YELLOW), 0);
        lv_label_set_text(s_lbl_sweep_info, buf);
        lv_obj_set_style_text_color(s_lbl_sweep_info,
            lv_palette_main(LV_PALETTE_YELLOW), 0);
        if (s_bar_amp) lv_obj_add_flag(s_bar_amp, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_set) lv_obj_add_flag(s_btn_set, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // -- Active sweep phase --------------------------------------------------
    if (hd.sweep_active) {
        lv_label_set_text(s_lbl_status, "Sweeping...");
        lv_obj_set_style_text_color(s_lbl_status,
            lv_palette_main(LV_PALETTE_BLUE), 0);

        char buf[48];
        snprintf(buf, sizeof(buf), "Step %d/%d  %.0f Hz  amp:%.3f",
                 (int)(hd.sweep_step + 1), DRV2605_SWEEP_STEPS,
                 (double)hd.sweep_current_hz,
                 (double)hd.sweep_last_amp);
        lv_label_set_text(s_lbl_sweep_info, buf);
        lv_obj_set_style_text_color(s_lbl_sweep_info,
            lv_palette_main(LV_PALETTE_BLUE), 0);

        if (s_bar_amp) {
            lv_obj_clear_flag(s_bar_amp, LV_OBJ_FLAG_HIDDEN);
            int32_t pct = (int32_t)(hd.sweep_last_amp * 100.0f);
            if (pct > 100) pct = 100;
            lv_bar_set_value(s_bar_amp, pct, LV_ANIM_OFF);
        }
        if (s_btn_set) lv_obj_clear_flag(s_btn_set, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // -- Idle phase ----------------------------------------------------------
    if (s_bar_amp) lv_obj_add_flag(s_bar_amp, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_set) lv_obj_add_flag(s_btn_set, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_lbl_sweep_info, "");

    // Calibration status
    if (hd.calibrating) {
        lv_label_set_text(s_lbl_status, "Auto-cal running...");
        lv_obj_set_style_text_color(s_lbl_status,
            lv_palette_main(LV_PALETTE_BLUE), 0);
    } else if (hd.calibrated) {
        lv_label_set_text(s_lbl_status, "Calibrated " LV_SYMBOL_OK);
        lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_ONLINE, 0);
    } else {
        lv_label_set_text(s_lbl_status, "Not Calibrated");
        lv_obj_set_style_text_color(s_lbl_status,
            lv_palette_main(LV_PALETTE_ORANGE), 0);
    }

    // Freq label
    if (s_lbl_freq) {
        char buf[32];
        if (hd.calibrated && hd.resonant_freq_hz > 0.0f)
            snprintf(buf, sizeof(buf), "Freq: %.1f Hz",
                     (double)hd.resonant_freq_hz);
        else
            snprintf(buf, sizeof(buf), "Freq: --- Hz");
        lv_label_set_text(s_lbl_freq, buf);
    }

    // // -- Roller sync — only correct when saved value differs from display ----
    // // Guard prevents lv_roller_set_selected() from firing VALUE_CHANGED
    // // which would re-trigger cb_roller_changed and fight the user.
    // if (s_roller_effect) {
    //     uint16_t saved_idx = effect_id_to_roller_idx(haptic_get_ui_effect());
    //     uint16_t cur_idx   = lv_roller_get_selected(s_roller_effect);
    //     if (cur_idx != saved_idx) {
    //         s_syncing_roller = true;
    //         lv_roller_set_selected(s_roller_effect, saved_idx, LV_ANIM_OFF);
    //         s_syncing_roller = false;
    //     }
    // }
}

// ---------------------------------------------------------------------------
// haptic_tile_apply_theme
// ---------------------------------------------------------------------------

void haptic_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent, theme_bg(), 0);

    if (s_lbl_header)
        lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    if (s_lbl_freq)
        lv_obj_set_style_text_color(s_lbl_freq, theme_subtext(), 0);

    // Re-apply full part-aware roller styling on theme change
    roller_apply_theme();
}

// ---------------------------------------------------------------------------
// Tile descriptor
// ---------------------------------------------------------------------------

const tile_desc_t haptic_tile_desc = {
    .init           = haptic_tile_init,
    .update         = haptic_tile_update,
    .apply_theme    = haptic_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};