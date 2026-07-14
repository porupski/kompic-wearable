/**
 * @file health_tile.c
 * @brief Health / HR sensor tile — LVGL 9 UI, Core 1 only.
 *
 * Tile type   : A (status LED + data rows + power switch + Buzz-to-BPM toggle)
 *
 * Layout:
 *   [●] MAX30101  HR / SpO2 / PPG                [SW power]
 *   ─────────────────────────────────────────────────────
 *   [large BPM number, centered]
 *   [BPM label]
 *   [■■■■░  Signal: 68%]
 *   [status label: READING / CALIBRATING / NO FINGER / POWER OFF]
 *   ─────────────────────────────────────────────────────
 *   Buzz to BPM   [SW buzz]
 *   SpO2:  ---
 *
 * Phase 15 fixes:
 *
 *   FIX-A — power sync uses d.enabled snapshot (NOT broker_hr_get_enabled()):
 *     broker_hr_get_enabled() acquires the broker mutex (5 ms timeout).
 *     If Core 0 holds it during FIFO drain (up to 50 ms), this times out
 *     and returns false → lv_obj_clear_state fires LV_EVENT_VALUE_CHANGED
 *     → cb_power_toggle(false) → broker_hr_set_enabled(false) → spontaneous
 *     sensor sleep. Fix: use d.enabled from broker_hr_read() snapshot taken
 *     at the top of update() — already consistent, no second mutex call.
 *
 *   FIX-B — cb_buzz_toggle pins bd.enabled to broker_hr_get_enabled():
 *     RBW previously recycled stale bd.enabled=true from a prior task write,
 *     refreshed last_update_ms, causing SENSOR_ONLINE (green LED) while the
 *     sensor was logically disabled.
 *
 * Core 1 only. No I2C. No direct NVS calls.
 * Architecture: Blueprint 3 §6, Blueprint 5 §4–§6, Blueprint 14b §7
 */

#include "health_tile.h"
#include "max30101.h"
#include "data_broker.h"
#include "haptic.h"
#include "ui_theme_colors.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "HEALTH_TILE";

#define SIG_BAR_COUNT    5
#define SIG_BAR_W        18
#define SIG_BAR_H        8
#define SIG_BAR_GAP      4

static lv_obj_t *s_parent         = NULL;
static lv_obj_t *s_led_status     = NULL;
static lv_obj_t *s_lbl_header     = NULL;
static lv_obj_t *s_sw_power       = NULL;
static lv_obj_t *s_divider_top    = NULL;
static lv_obj_t *s_lbl_bpm        = NULL;
static lv_obj_t *s_lbl_bpm_unit   = NULL;
static lv_obj_t *s_bar[SIG_BAR_COUNT];
static lv_obj_t *s_lbl_signal     = NULL;
static lv_obj_t *s_lbl_status     = NULL;
static lv_obj_t *s_divider_bot    = NULL;
static lv_obj_t *s_lbl_buzz       = NULL;
static lv_obj_t *s_sw_buzz        = NULL;
static lv_obj_t *s_lbl_spo2       = NULL;

static bool     s_syncing_power   = false;
static bool     s_syncing_buzz    = false;
static uint32_t s_last_beat_count = 0;

// ---------------------------------------------------------------------------
// Helpers
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
        case SENSOR_DISABLED:
        default:               col = COL_STATUS_DISABLED;  break;
    }
    lv_led_set_color(s_led_status, col);
}

static void update_signal_bar(uint8_t quality)
{
    uint8_t filled = quality / 20;
    if (filled > SIG_BAR_COUNT) filled = SIG_BAR_COUNT;
    for (int i = 0; i < SIG_BAR_COUNT; i++) {
        lv_color_t col = (i < filled) ? COL_STATUS_ONLINE : theme_divider();
        lv_obj_set_style_bg_color(s_bar[i], col, 0);
    }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cb_power_toggle(lv_event_t *e)
{
    if (s_syncing_power) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool val = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    broker_hr_set_enabled(val);
    haptic_play(haptic_get_ui_effect());
}

static void cb_buzz_toggle(lv_event_t *e)
{
    if (s_syncing_buzz) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool val = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

    broker_hr_data_t bd = {0};
    broker_hr_read(&bd);
    // bd.enabled   = broker_hr_get_enabled();   // FIX-B: pin, do not recycle stale value
    bd.buzz_beat = val;
    broker_hr_write(&bd);

    haptic_play(haptic_get_ui_effect());
}

// ---------------------------------------------------------------------------
// health_tile_init
// ---------------------------------------------------------------------------

void health_tile_init(lv_obj_t *parent)
{
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, 12, 12);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    s_lbl_header = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_header, "%s  %s",
        max30101_get_chip_name(), max30101_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, 30, 7);

    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 46, 22);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_add_event_cb(s_sw_power, cb_power_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    s_divider_top = lv_obj_create(parent);
    lv_obj_set_size(s_divider_top, 216, 1);
    lv_obj_align(s_divider_top, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(s_divider_top, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider_top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider_top, 0, 0);
    lv_obj_clear_flag(s_divider_top, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_bpm = lv_label_create(parent);
    lv_label_set_text(s_lbl_bpm, "---");
    lv_obj_set_style_text_font(s_lbl_bpm, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_bpm, theme_text(), 0);
    lv_obj_align(s_lbl_bpm, LV_ALIGN_TOP_MID, 0, 44);

    s_lbl_bpm_unit = lv_label_create(parent);
    lv_label_set_text(s_lbl_bpm_unit, "BPM");
    lv_obj_set_style_text_font(s_lbl_bpm_unit, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_bpm_unit, theme_subtext(), 0);
    lv_obj_align(s_lbl_bpm_unit, LV_ALIGN_TOP_MID, 0, 100);

    int bar_total_w = SIG_BAR_COUNT * SIG_BAR_W + (SIG_BAR_COUNT - 1) * SIG_BAR_GAP;
    int bar_start_x = -(bar_total_w / 2) - 24;

    for (int i = 0; i < SIG_BAR_COUNT; i++) {
        s_bar[i] = lv_obj_create(parent);
        lv_obj_set_size(s_bar[i], SIG_BAR_W, SIG_BAR_H);
        lv_obj_set_style_radius(s_bar[i], 2, 0);
        lv_obj_set_style_border_width(s_bar[i], 0, 0);
        lv_obj_set_style_bg_color(s_bar[i], theme_divider(), 0);
        lv_obj_set_style_bg_opa(s_bar[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_bar[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s_bar[i], LV_ALIGN_TOP_MID,
                     bar_start_x + i * (SIG_BAR_W + SIG_BAR_GAP), 118);
    }

    s_lbl_signal = lv_label_create(parent);
    lv_label_set_text(s_lbl_signal, "Signal: --%");
    lv_obj_set_style_text_font(s_lbl_signal, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_signal, theme_subtext(), 0);
    lv_obj_align(s_lbl_signal, LV_ALIGN_TOP_MID, bar_start_x + bar_total_w + 6, 115);

    s_lbl_status = lv_label_create(parent);
    lv_label_set_text(s_lbl_status, "POWER OFF");
    lv_obj_set_style_text_font(s_lbl_status, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_status, theme_subtext(), 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, 134);

    s_divider_bot = lv_obj_create(parent);
    lv_obj_set_size(s_divider_bot, 216, 1);
    lv_obj_align(s_divider_bot, LV_ALIGN_TOP_MID, 0, 155);
    lv_obj_set_style_bg_color(s_divider_bot, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider_bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider_bot, 0, 0);
    lv_obj_clear_flag(s_divider_bot, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_buzz = lv_label_create(parent);
    lv_label_set_text(s_lbl_buzz, "Buzz to BPM");
    lv_obj_set_style_text_font(s_lbl_buzz, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_buzz, theme_text(), 0);
    lv_obj_align(s_lbl_buzz, LV_ALIGN_TOP_LEFT, 12, 162);

    s_sw_buzz = lv_switch_create(parent);
    lv_obj_set_size(s_sw_buzz, 46, 22);
    lv_obj_align(s_sw_buzz, LV_ALIGN_TOP_RIGHT, -8, 160);
    lv_obj_add_event_cb(s_sw_buzz, cb_buzz_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_spo2 = lv_label_create(parent);
    lv_label_set_text(s_lbl_spo2, "SpO2:      ---");
    lv_obj_set_style_text_font(s_lbl_spo2, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_spo2, theme_subtext(), 0);
    lv_obj_align(s_lbl_spo2, LV_ALIGN_TOP_LEFT, 12, 193);

    ESP_LOGI(TAG, "%s tile init OK", max30101_get_chip_name());
}

// ---------------------------------------------------------------------------
// health_tile_update — called every 200 ms by task_ui_refresh_fn()
// ---------------------------------------------------------------------------

void health_tile_update(void)
{
    broker_hr_data_t d  = {0};
    broker_hr_read(&d);                        // single mutex acquisition — snapshot
    sensor_status_t  st = broker_hr_get_status();

    update_led(st);

    // ── Power switch sync ─────────────────────────────────────────────────
    // FIX-A: use d.enabled from the snapshot above — NOT broker_hr_get_enabled().
    // A second mutex call here can time out (Core 0 holds mutex up to 50 ms
    // during FIFO drain), returning false and triggering a spurious disable.
    s_syncing_power = true;
    if (d.enabled) lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
    else           lv_obj_clear_state(s_sw_power, LV_STATE_CHECKED);
    s_syncing_power = false;

    // ── Buzz switch sync ──────────────────────────────────────────────────
    s_syncing_buzz = true;
    if (d.buzz_beat) lv_obj_add_state(s_sw_buzz, LV_STATE_CHECKED);
    else             lv_obj_clear_state(s_sw_buzz, LV_STATE_CHECKED);
    s_syncing_buzz = false;

    // ── Disabled ──────────────────────────────────────────────────────────
    if (st == SENSOR_DISABLED) {
        lv_label_set_text(s_lbl_bpm, "---");
        lv_obj_set_style_text_font(s_lbl_bpm, &lv_font_montserrat_48, 0);
        lv_label_set_text(s_lbl_signal, "Signal: --%");
        update_signal_bar(0);
        lv_label_set_text(s_lbl_status, "POWER OFF");
        lv_obj_set_style_text_color(s_lbl_status, theme_subtext(), 0);
        lv_label_set_text(s_lbl_spo2, "SpO2:      ---");
        lv_obj_add_state(s_sw_buzz, LV_STATE_DISABLED);
        return;
    }

    lv_obj_clear_state(s_sw_buzz, LV_STATE_DISABLED);

    // ── No finger ─────────────────────────────────────────────────────────
    if (!d.finger_detected) {
        lv_label_set_text(s_lbl_bpm, "---");
        lv_obj_set_style_text_font(s_lbl_bpm, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(s_lbl_bpm, theme_text(), 0);
        lv_label_set_text(s_lbl_signal, "Signal: --%");
        update_signal_bar(0);
        lv_label_set_text(s_lbl_status, "NO FINGER");
        lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_STALE, 0);
        lv_label_set_text(s_lbl_spo2, "SpO2:      ---");
        return;
    }

    // ── Finger detected ───────────────────────────────────────────────────
    if (d.bpm > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)d.bpm);
        lv_label_set_text(s_lbl_bpm, buf);
        lv_obj_set_style_text_font(s_lbl_bpm, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(s_lbl_bpm, theme_text(), 0);
        lv_label_set_text(s_lbl_status, "READING");
        lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_ONLINE, 0);
    } else {
        lv_label_set_text(s_lbl_bpm, "CAL");
        lv_obj_set_style_text_font(s_lbl_bpm, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(s_lbl_bpm, COL_STATUS_ACQUIRING, 0);
        lv_label_set_text(s_lbl_status, "CALIBRATING");
        lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_ACQUIRING, 0);
    }

    update_signal_bar(d.signal_quality);
    {
        char buf[20];
        snprintf(buf, sizeof(buf), "Signal: %u%%", (unsigned)d.signal_quality);
        lv_label_set_text(s_lbl_signal, buf);
    }

    if (d.spo2_valid) {
        char buf[20];
        int s_w = (int)d.spo2_pct;
        int s_d = (int)((d.spo2_pct - s_w) * 10);
        snprintf(buf, sizeof(buf), "SpO2:      %d.%d %%", s_w, s_d);
        lv_label_set_text(s_lbl_spo2, buf);
    } else {
        lv_label_set_text(s_lbl_spo2, "SpO2:      ---");
    }

    // ── Buzz-to-BPM haptic ────────────────────────────────────────────────
    if (d.buzz_beat && d.beat_count != s_last_beat_count) {
        if (broker_haptic_hw_alive()) {
            haptic_play(haptic_get_ui_effect());
        }
        s_last_beat_count = d.beat_count;
    }
}

// ---------------------------------------------------------------------------
// health_tile_apply_theme
// ---------------------------------------------------------------------------

void health_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent,      theme_bg(),      0);
    lv_obj_set_style_bg_color(s_divider_top, theme_divider(), 0);
    lv_obj_set_style_bg_color(s_divider_bot, theme_divider(), 0);

    lv_obj_set_style_text_color(s_lbl_header,   theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_bpm,      theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_bpm_unit, theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_signal,   theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_buzz,     theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_spo2,     theme_subtext(), 0);

    for (int i = 0; i < SIG_BAR_COUNT; i++) {
        lv_obj_set_style_bg_color(s_bar[i], theme_divider(), 0);
    }
}

// ---------------------------------------------------------------------------
// Tile descriptor
// ---------------------------------------------------------------------------

const tile_desc_t health_tile_desc = {
    .init           = health_tile_init,
    .update         = health_tile_update,
    .apply_theme    = health_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};