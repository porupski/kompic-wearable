/**
 * @file alarm_tile.c
 * @brief Alarm screen — Core 1 / LVGL 9 only.
 *
 * Overlay: built lazily on ADD/edit tap, destroyed on SAVE/DELETE/CANCEL.
 * Two time rollers (HH, MM), one pattern roller. LED strobe toggle is
 * persisted via alarm_slot_t.led_strobe but currently not exposed in the
 * overlay — defaults to OFF for newly-created slots. (Phase 6 v7.2.)
 *
 * Core 1 only. No I2C. Broker writes use read-before-write exclusively.
 * Architecture: Blueprint 16, Blueprint 3 §6
 */

#include "alarm_tile.h"
#include "alarm.h"
#include "data_broker.h"
#include "haptic.h"
#include "ui_theme_colors.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ALARM_TILE";

// ---------------------------------------------------------------------------
// Widget handles — screen
// ---------------------------------------------------------------------------

static lv_obj_t *s_screen       = NULL;
static lv_obj_t *s_lbl_title    = NULL;
static lv_obj_t *s_lbl_next     = NULL;
static lv_obj_t *s_lbl_clock    = NULL;
static lv_obj_t *s_divider_top  = NULL;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *lbl_time;
    lv_obj_t *lbl_pattern;
    lv_obj_t *sw_armed;
} slot_row_t;

static slot_row_t s_slots[ALARM_MAX_SLOTS];
static bool       s_syncing_armed[ALARM_MAX_SLOTS] = {0};

static lv_obj_t *s_divider_bot  = NULL;
static lv_obj_t *s_btn_add      = NULL;
static lv_obj_t *s_lbl_btn_add  = NULL;

// ---------------------------------------------------------------------------
// Widget handles — overlay (NULL when not visible)
// ---------------------------------------------------------------------------

static lv_obj_t *s_overlay      = NULL;
static lv_obj_t *s_roller_hour  = NULL;
static lv_obj_t *s_roller_min   = NULL;
static lv_obj_t *s_roller_pat   = NULL;
static lv_obj_t *s_btn_save     = NULL;
static lv_obj_t *s_btn_delete   = NULL;
static lv_obj_t *s_btn_cancel   = NULL;

static int8_t    s_editing_slot = -1;

// ---------------------------------------------------------------------------
// Roller option strings (built once, reused across overlay lifetimes)
// ---------------------------------------------------------------------------

static const char *s_hour_opts = NULL;
static const char *s_min_opts  = NULL;
static char        s_pat_opts[128];

static void build_roller_opts(void)
{
    if (!s_hour_opts) {
        static char buf[24 * 3 + 1];
        char *p = buf;
        for (int i = 0; i < 24; i++) {
            if (i > 0) *p++ = '\n';
            p += snprintf(p, 4, "%02d", i);
        }
        s_hour_opts = buf;
    }
    if (!s_min_opts) {
        static char buf[60 * 3 + 1];
        char *p = buf;
        for (int i = 0; i < 60; i++) {
            if (i > 0) *p++ = '\n';
            p += snprintf(p, 4, "%02d", i);
        }
        s_min_opts = buf;
    }
    {
        const alarm_haptic_pattern_t *pats = alarm_get_patterns();
        uint8_t n = alarm_get_pattern_count();
        char *p = s_pat_opts;
        size_t rem = sizeof(s_pat_opts);
        for (uint8_t i = 0; i < n; i++) {
            if (i > 0) { *p++ = '\n'; rem--; }
            int w = snprintf(p, rem, "%s", pats[i].name);
            p += w; rem -= (size_t)w;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char *pattern_name(uint8_t id)
{
    const alarm_haptic_pattern_t *pats = alarm_get_patterns();
    if (id < alarm_get_pattern_count()) return pats[id].name;
    return "Unknown";
}

static int compute_next_delta(const broker_alarm_data_t *ad,
                              uint8_t cur_h, uint8_t cur_m)
{
    int cur_mod = cur_h * 60 + cur_m;
    int best    = -1;
    for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
        if (ad->slots[i].hour == ALARM_SLOT_EMPTY) continue;
        if (!ad->slots[i].armed) continue;
        int slot_mod = ad->slots[i].hour * 60 + ad->slots[i].minute;
        int delta = slot_mod - cur_mod;
        if (delta <= 0) delta += 1440;
        if (best < 0 || delta < best) best = delta;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void show_overlay(int8_t slot_idx);
static void hide_overlay(void);

// ---------------------------------------------------------------------------
// Callbacks — armed switches
// ---------------------------------------------------------------------------

static void cb_armed_toggle(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= ALARM_MAX_SLOTS) return;
    if (s_syncing_armed[idx]) return;

    bool val = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    ad.slots[idx].armed = val;
    broker_alarm_write(&ad);

    alarm_nvs_save_slot((uint8_t)idx, &ad.slots[idx]);
    haptic_play(haptic_get_ui_effect());
}

// ---------------------------------------------------------------------------
// Callbacks — slot row tap (SHORT_CLICKED avoids swipe triggers)
// ---------------------------------------------------------------------------

static void cb_slot_tap(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= ALARM_MAX_SLOTS) return;

    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    if (ad.slots[idx].hour == ALARM_SLOT_EMPTY) return;

    show_overlay((int8_t)idx);
}

// ---------------------------------------------------------------------------
// Callbacks — ADD button
// ---------------------------------------------------------------------------

static void cb_add_pressed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;

    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    int8_t free_idx = -1;
    for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
        if (ad.slots[i].hour == ALARM_SLOT_EMPTY) { free_idx = (int8_t)i; break; }
    }
    if (free_idx < 0) return;
    show_overlay(-1);
}

// ---------------------------------------------------------------------------
// Callbacks — overlay buttons
// ---------------------------------------------------------------------------

static void cb_ovl_save(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_overlay) return;

    uint8_t h   = (uint8_t)lv_roller_get_selected(s_roller_hour);
    uint8_t m   = (uint8_t)lv_roller_get_selected(s_roller_min);
    uint8_t pat = (uint8_t)lv_roller_get_selected(s_roller_pat);

    int8_t idx = s_editing_slot;
    if (idx < 0) {
        broker_alarm_data_t ad = {0};
        broker_alarm_read(&ad);
        for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
            if (ad.slots[i].hour == ALARM_SLOT_EMPTY) { idx = (int8_t)i; break; }
        }
        if (idx < 0) { hide_overlay(); return; }
    }

    alarm_slot_t slot = {
        .hour = h, .minute = m, .pattern_id = pat,
        .armed = true, .led_strobe = false,
    };

    if (s_editing_slot >= 0) {
        broker_alarm_data_t ad = {0};
        broker_alarm_read(&ad);
        slot.armed = ad.slots[idx].armed;
    }

    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    ad.slots[idx] = slot;
    broker_alarm_write(&ad);

    alarm_nvs_save_slot((uint8_t)idx, &slot);
    haptic_play(haptic_get_ui_effect());
    hide_overlay();
}

static void cb_ovl_delete(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_editing_slot < 0) return;

    uint8_t idx = (uint8_t)s_editing_slot;
    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    ad.slots[idx].hour       = ALARM_SLOT_EMPTY;
    ad.slots[idx].minute     = 0;
    ad.slots[idx].pattern_id = 0;
    ad.slots[idx].armed      = false;
    ad.slots[idx].led_strobe = false;
    broker_alarm_write(&ad);

    alarm_nvs_delete_slot(idx);
    haptic_play(haptic_get_ui_effect());
    hide_overlay();
}

static void cb_ovl_cancel(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_overlay();
}

// ---------------------------------------------------------------------------
// Overlay — lazy build / destroy
// ---------------------------------------------------------------------------

static void build_overlay(int8_t slot_idx)
{
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 240, 280);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // ── CANCEL (left) + SAVE (right) ────────────────────────────────────────
    s_btn_cancel = lv_btn_create(s_overlay);
    lv_obj_set_size(s_btn_cancel, 90, 32);
    lv_obj_align(s_btn_cancel, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_obj_set_style_bg_color(s_btn_cancel, lv_color_hex(0x404060), 0);
    lv_obj_add_event_cb(s_btn_cancel, cb_ovl_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(s_btn_cancel);
    lv_label_set_text(lc, "CANCEL");
    lv_obj_set_style_text_font(lc, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lc, lv_color_white(), 0);
    lv_obj_center(lc);

    s_btn_save = lv_btn_create(s_overlay);
    lv_obj_set_size(s_btn_save, 90, 32);
    lv_obj_align(s_btn_save, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_bg_color(s_btn_save, lv_color_hex(0x206020), 0);
    lv_obj_add_event_cb(s_btn_save, cb_ovl_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(s_btn_save);
    lv_label_set_text(ls, "SAVE");
    lv_obj_set_style_text_font(ls, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(ls, lv_color_white(), 0);
    lv_obj_center(ls);

    // ── Divider ─────────────────────────────────────────────────────────────
    lv_obj_t *div = lv_obj_create(s_overlay);
    lv_obj_set_size(div, 220, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x404060), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    // ── Title ───────────────────────────────────────────────────────────────
    lv_obj_t *lt = lv_label_create(s_overlay);
    lv_label_set_text(lt, slot_idx >= 0 ? "Edit Alarm" : "New Alarm");
    lv_obj_set_style_text_font(lt, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lt, lv_color_hex(0xaaaacc), 0);
    lv_obj_align(lt, LV_ALIGN_TOP_MID, 0, 50);

    // ── HH : MM rollers ─────────────────────────────────────────────────────
    s_roller_hour = lv_roller_create(s_overlay);
    lv_roller_set_options(s_roller_hour, s_hour_opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(s_roller_hour, 3);
    lv_obj_set_width(s_roller_hour, 80);
    lv_obj_align(s_roller_hour, LV_ALIGN_TOP_MID, -50, 72);
    lv_obj_set_style_text_font(s_roller_hour, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_roller_hour, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_roller_hour, lv_color_hex(0x2a2a4a), 0);
    lv_obj_set_style_bg_color(s_roller_hour, lv_color_hex(0x4060c0), LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller_hour, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_border_width(s_roller_hour, 0, 0);

    lv_obj_t *colon = lv_label_create(s_overlay);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(colon, lv_color_white(), 0);
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 88);

    s_roller_min = lv_roller_create(s_overlay);
    lv_roller_set_options(s_roller_min, s_min_opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(s_roller_min, 3);
    lv_obj_set_width(s_roller_min, 80);
    lv_obj_align(s_roller_min, LV_ALIGN_TOP_MID, 50, 72);
    lv_obj_set_style_text_font(s_roller_min, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_roller_min, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_roller_min, lv_color_hex(0x2a2a4a), 0);
    lv_obj_set_style_bg_color(s_roller_min, lv_color_hex(0x4060c0), LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller_min, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_border_width(s_roller_min, 0, 0);

    // ── Pattern (above DELETE) ──────────────────────────────────────────────
    lv_obj_t *lp = lv_label_create(s_overlay);
    lv_label_set_text(lp, "Pattern:");
    lv_obj_set_style_text_font(lp, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lp, lv_color_hex(0xaaaacc), 0);
    lv_obj_align(lp, LV_ALIGN_TOP_LEFT, 12, 192);

    s_roller_pat = lv_roller_create(s_overlay);
    lv_roller_set_options(s_roller_pat, s_pat_opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_pat, 1);
    lv_obj_set_width(s_roller_pat, 138);
    lv_obj_align(s_roller_pat, LV_ALIGN_TOP_RIGHT, -10, 186);
    lv_obj_set_style_text_font(s_roller_pat, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_roller_pat, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_roller_pat, lv_color_hex(0x2a2a4a), 0);
    lv_obj_set_style_bg_color(s_roller_pat, lv_color_hex(0x4060c0), LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller_pat, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_border_width(s_roller_pat, 0, 0);

    // ── DELETE (bottom, hidden when adding) ──────────────────────────────────
    s_btn_delete = lv_btn_create(s_overlay);
    lv_obj_set_size(s_btn_delete, 160, 34);
    lv_obj_align(s_btn_delete, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(s_btn_delete, lv_color_hex(0x802020), 0);
    lv_obj_add_event_cb(s_btn_delete, cb_ovl_delete, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ld = lv_label_create(s_btn_delete);
    lv_label_set_text(ld, "DELETE");
    lv_obj_set_style_text_font(ld, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(ld, lv_color_white(), 0);
    lv_obj_center(ld);

    // ── Pre-fill ─────────────────────────────────────────────────────────────
    if (slot_idx >= 0 && slot_idx < ALARM_MAX_SLOTS) {
        broker_alarm_data_t ad = {0};
        broker_alarm_read(&ad);
        lv_roller_set_selected(s_roller_hour, ad.slots[slot_idx].hour, LV_ANIM_OFF);
        lv_roller_set_selected(s_roller_min,  ad.slots[slot_idx].minute, LV_ANIM_OFF);
        lv_roller_set_selected(s_roller_pat,  ad.slots[slot_idx].pattern_id, LV_ANIM_OFF);
        lv_obj_clear_flag(s_btn_delete, LV_OBJ_FLAG_HIDDEN);
    } else {
        broker_rtc_data_t rtc = {0};
        broker_rtc_read(&rtc);
        uint8_t next_h = rtc.valid ? (rtc.hour + 1) % 24 : 7;
        lv_roller_set_selected(s_roller_hour, next_h, LV_ANIM_OFF);
        lv_roller_set_selected(s_roller_min,  0, LV_ANIM_OFF);
        lv_roller_set_selected(s_roller_pat,  0, LV_ANIM_OFF);
        lv_obj_add_flag(s_btn_delete, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_overlay(int8_t slot_idx)
{
    if (s_overlay) return;
    s_editing_slot = slot_idx;
    build_overlay(slot_idx);
    ESP_LOGI(TAG, "Overlay opened (slot=%d)", (int)slot_idx);
}

static void hide_overlay(void)
{
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay     = NULL;
        s_roller_hour = NULL;
        s_roller_min  = NULL;
        s_roller_pat  = NULL;
        s_btn_save    = NULL;
        s_btn_delete  = NULL;
        s_btn_cancel  = NULL;
    }
    s_editing_slot = -1;
    ESP_LOGI(TAG, "Overlay closed");
}

// ---------------------------------------------------------------------------
// alarm_screen_build
// ---------------------------------------------------------------------------

lv_obj_t *alarm_screen_build(void)
{
    build_roller_opts();

    s_screen = lv_obj_create(NULL);
    if (!s_screen) {
        ESP_LOGE(TAG, "Failed to create alarm screen");
        return NULL;
    }

    lv_obj_set_style_bg_color(s_screen, theme_bg(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_title = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_title, "Alarm");
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_title, theme_text(), 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_LEFT, 12, 8);

    s_lbl_next = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_next, "Next: None");
    lv_obj_set_style_text_font(s_lbl_next, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_next, theme_subtext(), 0);
    lv_obj_align(s_lbl_next, LV_ALIGN_TOP_LEFT, 12, 34);

    s_lbl_clock = lv_label_create(s_screen);
    lv_label_set_text(s_lbl_clock, "Clock: ---");
    lv_obj_set_style_text_font(s_lbl_clock, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_clock, theme_subtext(), 0);
    lv_obj_align(s_lbl_clock, LV_ALIGN_TOP_LEFT, 12, 52);

    s_divider_top = lv_obj_create(s_screen);
    lv_obj_set_size(s_divider_top, 216, 1);
    lv_obj_align(s_divider_top, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_color(s_divider_top, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider_top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider_top, 0, 0);
    lv_obj_clear_flag(s_divider_top, LV_OBJ_FLAG_SCROLLABLE);

    int y_base = 76;
    int row_h  = 36;

    for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
        int y = y_base + i * row_h;

        s_slots[i].row = lv_obj_create(s_screen);
        lv_obj_set_size(s_slots[i].row, 180, row_h - 2);
        lv_obj_align(s_slots[i].row, LV_ALIGN_TOP_LEFT, 4, y);
        lv_obj_set_style_bg_opa(s_slots[i].row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_slots[i].row, 0, 0);
        lv_obj_set_style_pad_all(s_slots[i].row, 0, 0);
        lv_obj_clear_flag(s_slots[i].row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_slots[i].row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_slots[i].row, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(s_slots[i].row, cb_slot_tap,
                            LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

        s_slots[i].lbl_time = lv_label_create(s_slots[i].row);
        lv_label_set_text(s_slots[i].lbl_time, "--:--");
        lv_obj_set_style_text_font(s_slots[i].lbl_time, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(s_slots[i].lbl_time, theme_text(), 0);
        lv_obj_align(s_slots[i].lbl_time, LV_ALIGN_LEFT_MID, 8, 0);

        s_slots[i].lbl_pattern = lv_label_create(s_slots[i].row);
        lv_label_set_text(s_slots[i].lbl_pattern, "");
        lv_obj_set_style_text_font(s_slots[i].lbl_pattern, UI_FONT_CHIP, 0);
        lv_obj_set_style_text_color(s_slots[i].lbl_pattern, theme_subtext(), 0);
        lv_obj_align(s_slots[i].lbl_pattern, LV_ALIGN_LEFT_MID, 68, 0);

        s_slots[i].sw_armed = lv_switch_create(s_screen);
        lv_obj_set_size(s_slots[i].sw_armed, 42, 20);
        lv_obj_align(s_slots[i].sw_armed, LV_ALIGN_TOP_RIGHT, -8, y + (row_h - 20) / 2);
        lv_obj_add_event_cb(s_slots[i].sw_armed, cb_armed_toggle,
                            LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }

    s_divider_bot = lv_obj_create(s_screen);
    lv_obj_set_size(s_divider_bot, 216, 1);
    lv_obj_align(s_divider_bot, LV_ALIGN_TOP_MID, 0, y_base + ALARM_MAX_SLOTS * row_h + 4);
    lv_obj_set_style_bg_color(s_divider_bot, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider_bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider_bot, 0, 0);
    lv_obj_clear_flag(s_divider_bot, LV_OBJ_FLAG_SCROLLABLE);

    s_btn_add = lv_btn_create(s_screen);
    lv_obj_set_size(s_btn_add, 200, 38);
    lv_obj_align(s_btn_add, LV_ALIGN_TOP_MID, 0,
                 y_base + ALARM_MAX_SLOTS * row_h + 12);
    lv_obj_set_style_bg_color(s_btn_add, lv_color_hex(0x304060), 0);
    lv_obj_add_event_cb(s_btn_add, cb_add_pressed, LV_EVENT_SHORT_CLICKED, NULL);

    s_lbl_btn_add = lv_label_create(s_btn_add);
    lv_label_set_text(s_lbl_btn_add, "+ ADD ALARM");
    lv_obj_set_style_text_font(s_lbl_btn_add, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_btn_add, lv_color_white(), 0);
    lv_obj_center(s_lbl_btn_add);

    ESP_LOGI(TAG, "Alarm screen built (%d slots, lazy overlay)", ALARM_MAX_SLOTS);
    return s_screen;
}

// ---------------------------------------------------------------------------
// alarm_tile_update
// ---------------------------------------------------------------------------

void alarm_tile_update(void)
{
    if (!s_screen) return;

    broker_alarm_data_t ad  = {0};
    broker_rtc_data_t   rtc = {0};
    broker_alarm_read(&ad);
    broker_rtc_read(&rtc);

    bool rtc_alive = broker_rtc_hw_alive();
    if (rtc_alive) {
        lv_label_set_text(s_lbl_clock, "Clock: RTC (PCF85063)");
        lv_obj_set_style_text_color(s_lbl_clock, COL_STATUS_ONLINE, 0);
    } else {
        lv_label_set_text(s_lbl_clock, "Clock: RTC OFFLINE");
        lv_obj_set_style_text_color(s_lbl_clock, COL_STATUS_OFFLINE, 0);
    }

    if (rtc_alive && rtc.valid) {
        int delta = compute_next_delta(&ad, rtc.hour, rtc.minute);
        if (delta > 0) {
            int cur_mod = rtc.hour * 60 + rtc.minute;
            for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
                if (ad.slots[i].hour == ALARM_SLOT_EMPTY || !ad.slots[i].armed)
                    continue;
                int sm = ad.slots[i].hour * 60 + ad.slots[i].minute;
                int d  = sm - cur_mod;
                if (d <= 0) d += 1440;
                if (d == delta) {
                    char buf[40];
                    snprintf(buf, sizeof(buf), "Next: %02u:%02u  (in %dh %02dm)",
                             (unsigned)ad.slots[i].hour,
                             (unsigned)ad.slots[i].minute,
                             delta / 60, delta % 60);
                    lv_label_set_text(s_lbl_next, buf);
                    break;
                }
            }
        } else {
            lv_label_set_text(s_lbl_next, "Next: None");
        }
    } else {
        lv_label_set_text(s_lbl_next, "Next: ---");
    }

    for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
        if (ad.slots[i].hour == ALARM_SLOT_EMPTY) {
            lv_label_set_text(s_slots[i].lbl_time, "--:--");
            lv_obj_set_style_text_color(s_slots[i].lbl_time, theme_subtext(), 0);
            lv_label_set_text(s_slots[i].lbl_pattern, "(empty)");
            lv_obj_add_flag(s_slots[i].sw_armed, LV_OBJ_FLAG_HIDDEN);
        } else {
            char tbuf[8];
            snprintf(tbuf, sizeof(tbuf), "%02u:%02u",
                     (unsigned)ad.slots[i].hour, (unsigned)ad.slots[i].minute);
            lv_label_set_text(s_slots[i].lbl_time, tbuf);
            lv_obj_set_style_text_color(s_slots[i].lbl_time, theme_text(), 0);
            lv_label_set_text(s_slots[i].lbl_pattern, pattern_name(ad.slots[i].pattern_id));
            lv_obj_clear_flag(s_slots[i].sw_armed, LV_OBJ_FLAG_HIDDEN);

            s_syncing_armed[i] = true;
            if (ad.slots[i].armed)
                lv_obj_add_state(s_slots[i].sw_armed, LV_STATE_CHECKED);
            else
                lv_obj_clear_state(s_slots[i].sw_armed, LV_STATE_CHECKED);
            s_syncing_armed[i] = false;

            if (!rtc_alive)
                lv_obj_add_state(s_slots[i].sw_armed, LV_STATE_DISABLED);
            else
                lv_obj_clear_state(s_slots[i].sw_armed, LV_STATE_DISABLED);
        }
    }

    bool has_free = false;
    for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
        if (ad.slots[i].hour == ALARM_SLOT_EMPTY) { has_free = true; break; }
    }
    if (!has_free || !rtc_alive)
        lv_obj_add_state(s_btn_add, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(s_btn_add, LV_STATE_DISABLED);
}

// ---------------------------------------------------------------------------
// alarm_tile_apply_theme
// ---------------------------------------------------------------------------

void alarm_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_screen) return;

    lv_obj_set_style_bg_color(s_screen, theme_bg(), 0);
    lv_obj_set_style_text_color(s_lbl_title, theme_text(), 0);
    lv_obj_set_style_text_color(s_lbl_next,  theme_subtext(), 0);
    lv_obj_set_style_bg_color(s_divider_top, theme_divider(), 0);
    lv_obj_set_style_bg_color(s_divider_bot, theme_divider(), 0);

    for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
        lv_obj_set_style_text_color(s_slots[i].lbl_time,    theme_text(), 0);
        lv_obj_set_style_text_color(s_slots[i].lbl_pattern, theme_subtext(), 0);
    }
}