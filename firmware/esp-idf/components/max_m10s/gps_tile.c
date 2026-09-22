/**
 * @file gps_tile.c
 * @brief GPS (MAX-M10S) settings tile -- LVGL 9 UI, Core 1 only.
 *
 * Stage 31.4 rewrite -- subject-bound, corner-safe, status-first.
 *
 * Layout (portrait 410x502, 60H/40V corner-safe pads per
 * project_display_corner_cutoff):
 *
 *   Row 0 (y=40):
 *     LED(16x16) at x=60
 *     Header "MAX-M10S" (28 px) at x=90
 *     Power switch (90x46, two-way bound) at TOP_RIGHT(-60, 40)
 *   Divider at y=100
 *   STATUS line (28 px, primary user-facing) at y=115
 *   Time+date line at y=160
 *   LAT line at y=200
 *   LON line at y=240
 *   ALT+SPD line at y=280
 *   Photo button (small) at BOTTOM_LEFT(60, -100)
 *   ATOMIC SYNC button (280x60) at BOTTOM_MID(0, -40)
 *
 * Bindings (Stage 31.4):
 *   All value labels use lv_label_bind_text against subj_gps_*_str.
 *   Power switch uses lv_obj_bind_checked against subj_gps_enabled
 *     (two-way -- observer in ui_subjects.c writes broker on change).
 *   Photo container + normal-view widgets use lv_obj_bind_flag_if_(not_)eq
 *     against subj_gps_photo_view.
 *   Sub-tile (raw NMEA) stays poll-based -- debug view, low priority.
 *
 * gps_tile_update() is minimal: only LED colour, sync-button enable
 * gating, and the sync-feedback timeout remain. All value / switch
 * updates are subject-driven.
 *
 * Core 1 only. No I2C/UART/NVS calls.
 */

#include "gps_tile.h"
#include "max_m10s.h"      // broker_gps_data_t, max_m10s_get_chip_name/desc,
                           // max_m10s_get_debug_sentences, gps_fix_type_t
#include "data_broker.h"   // broker_gps_get_status
#include "ui_theme_colors.h"
#include "ui_subjects.h"   // Stage 31.4: subj_gps_*, kw_gps_view_*
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "GPS_TILE";

// ---------------------------------------------------------------------------
// Layout constants (corner-safe zone per project_display_corner_cutoff)
// ---------------------------------------------------------------------------
#define GPS_PAD_H_UI       60
#define GPS_PAD_H_TEXT     40
#define GPS_PAD_V_UI       40
#define GPS_STATUS_Y      115
#define GPS_ROW_Y0        160
#define GPS_ROW_STEP       40
#define SYNC_FEEDBACK_MS  2000U

// ---------------------------------------------------------------------------
// Atomic flag posted by this tile, consumed by Core 0.
// ---------------------------------------------------------------------------
extern volatile bool g_gps_sync_requested;

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

// Data labels (all subject-bound)
static lv_obj_t *s_lbl_status   = NULL;   // primary status line
static lv_obj_t *s_lbl_time     = NULL;
static lv_obj_t *s_lbl_lat      = NULL;
static lv_obj_t *s_lbl_lon      = NULL;
static lv_obj_t *s_lbl_altspd   = NULL;

// Sync button
static lv_obj_t *s_btn_sync     = NULL;
static lv_obj_t *s_lbl_btn_sync = NULL;
static bool     s_showing_feedback  = false;
static int64_t  s_feedback_start_us = 0;

// MODE cycle button (Stage 31.4b: on-tile dynmodel cycle)
static lv_obj_t *s_btn_mode     = NULL;
static lv_obj_t *s_lbl_mode     = NULL;

// Photo button + photo container + photo view labels
static lv_obj_t *s_btn_photo    = NULL;
static lv_obj_t *s_photo        = NULL;
static lv_obj_t *s_photo_time   = NULL;
static lv_obj_t *s_photo_lat    = NULL;
static lv_obj_t *s_photo_lon    = NULL;
static lv_obj_t *s_photo_alt    = NULL;
static lv_obj_t *s_photo_hint   = NULL;

// Signal-strength bar (7 discrete blocks, driven from GSV-derived SNR)
#define SIG_BAR_BLOCKS   7
static lv_obj_t *s_sig_bar_blocks[SIG_BAR_BLOCKS] = {0};
static lv_obj_t *s_sig_bar_lbl   = NULL;   // "12/15 sats  CN0 34 dBHz" tag

// Sub-tile handles
static lv_obj_t *s_sub_parent   = NULL;
static lv_obj_t *s_lbl_sub_hdr  = NULL;
static lv_obj_t *s_lbl_gga      = NULL;
static lv_obj_t *s_lbl_rmc      = NULL;

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
        case SENSOR_NOTIF:     col = COL_STATUS_NOTIF;     break;
        case SENSOR_DISABLED:
        default:               col = COL_STATUS_DISABLED;  break;
    }
    lv_led_set_color(s_led_status, col);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

// ATOMIC SYNC: fire-and-forget command (not observable), stays as event cb.
static void cb_sync_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_add_state(s_btn_sync, LV_STATE_DISABLED);
    g_gps_sync_requested = true;
    ESP_LOGI(TAG, "Atomic sync requested");
}

// Photo entry button -> subject flip via view API.
static void cb_photo_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    kw_ui_gps_view_set(KW_GPS_VIEW_PHOTO);
}

// Tap anywhere on the photo container -> back to normal.
static void cb_photo_container_tap(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    kw_ui_gps_view_set(KW_GPS_VIEW_NORMAL);
}

// Stage 31.4b: MODE cycle. Same command surface as GPS_DYNMODEL CLI.
// Cycles the four modes Ivan asked for: PED -> WRIST -> AUTO -> AIR1G.
// WRIST may NAK on M10 (chip refuses, prior mode stays) -- that's OK,
// bench sees UBX-ACK-NAK at DEBUG and knows to skip it.
static void cb_mode_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    max_m10s_dynmodel_t cur = max_m10s_get_dynmodel();
    max_m10s_dynmodel_t next;
    switch (cur) {
        case UBX_DYNMODEL_PEDESTRIAN: next = UBX_DYNMODEL_WRIST;      break;
        case UBX_DYNMODEL_WRIST:      next = UBX_DYNMODEL_AUTOMOTIVE; break;
        case UBX_DYNMODEL_AUTOMOTIVE: next = UBX_DYNMODEL_AIR1G;      break;
        case UBX_DYNMODEL_AIR1G:
        default:                      next = UBX_DYNMODEL_PEDESTRIAN; break;
    }
    (void)max_m10s_set_dynmodel(next);
    // Label refresh: update() will pick it up on the next tick, but do
    // an immediate write so tap feels instant.
    if (s_lbl_mode) {
        lv_label_set_text_fmt(s_lbl_mode, "MODE\n%s",
                              max_m10s_dynmodel_name(next));
    }
}

// ---------------------------------------------------------------------------
// gps_tile_init
// ---------------------------------------------------------------------------
void gps_tile_init(lv_obj_t *parent)
{
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ── Row 0: LED + header + power switch ────────────────────────────────
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, 16, 16);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, GPS_PAD_H_UI, GPS_PAD_V_UI + 12);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    s_lbl_header = lv_label_create(parent);
    lv_label_set_text(s_lbl_header, max_m10s_get_chip_name());   // chip-name only
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, GPS_PAD_H_UI + 26, GPS_PAD_V_UI);

    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 90, 46);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, -GPS_PAD_H_UI, GPS_PAD_V_UI);
    // Stage 31.4: two-way binding. User tap updates subject; observer in
    // ui_subjects.c writes broker. Drain mirrors broker back into subject.
    // No cb_power_toggle event cb needed here.
    lv_obj_bind_checked(s_sw_power, &subj_gps_enabled);

    // ── Divider ───────────────────────────────────────────────────────────
    s_divider = lv_obj_create(parent);
    lv_obj_set_size(s_divider, 260, 2);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_color(s_divider, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    // ── STATUS line (primary user-facing text) ────────────────────────────
    // Larger, brighter, always shows a coherent message regardless of chip
    // state. Bound to subj_gps_status_str which the drain populates from
    // the producer sample OR from broker status when no producer data.
    s_lbl_status = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_status, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_status, theme_text(), 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, GPS_STATUS_Y);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_status, 290);
    lv_obj_set_style_text_align(s_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_bind_text(s_lbl_status, &subj_gps_status_str, NULL);

    // ── Value rows ────────────────────────────────────────────────────────
    const lv_font_t *fnt = UI_FONT_LABEL;   // smaller than status for hierarchy
    lv_color_t       col = theme_subtext();

    s_lbl_time = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_time, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_time, col, 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_LEFT, GPS_PAD_H_TEXT, GPS_ROW_Y0);
    lv_label_bind_text(s_lbl_time, &subj_gps_time_str, NULL);

    s_lbl_lat = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_lat, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_lat, col, 0);
    lv_obj_align(s_lbl_lat, LV_ALIGN_TOP_LEFT, GPS_PAD_H_TEXT, GPS_ROW_Y0 + GPS_ROW_STEP);
    lv_label_bind_text(s_lbl_lat, &subj_gps_lat_str, NULL);

    s_lbl_lon = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_lon, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_lon, col, 0);
    lv_obj_align(s_lbl_lon, LV_ALIGN_TOP_LEFT, GPS_PAD_H_TEXT, GPS_ROW_Y0 + 2 * GPS_ROW_STEP);
    lv_label_bind_text(s_lbl_lon, &subj_gps_lon_str, NULL);

    s_lbl_altspd = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_altspd, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_altspd, col, 0);
    lv_obj_align(s_lbl_altspd, LV_ALIGN_TOP_LEFT, GPS_PAD_H_TEXT, GPS_ROW_Y0 + 3 * GPS_ROW_STEP);
    lv_label_bind_text(s_lbl_altspd, &subj_gps_altspd_str, NULL);

    // ── Signal-strength bar (7 discrete blocks, driven by GSV-derived SNR)
    // Buckets (top-4 avg CN0 in dBHz):
    //   [0] sliver: alive-no-sats
    //   [1] <15 red        [2] 15-24 red-orange     [3] 25-29 orange
    //   [4] 30-34 yellow   [5] 35-44 light green    [6] 45+   deep green
    // A block is "on" (its fixed colour) if bar_level > block_idx; else dim.
    static const lv_color_t block_colors[SIG_BAR_BLOCKS] = {
        LV_COLOR_MAKE(0x60, 0x60, 0x60),  // 0: alive marker (grey when off)
        LV_COLOR_MAKE(0xE0, 0x20, 0x20),  // 1: deep red
        LV_COLOR_MAKE(0xE0, 0x60, 0x10),  // 2: red-orange
        LV_COLOR_MAKE(0xE0, 0xA0, 0x00),  // 3: orange
        LV_COLOR_MAKE(0xE0, 0xE0, 0x00),  // 4: yellow
        LV_COLOR_MAKE(0x60, 0xE0, 0x30),  // 5: light green
        LV_COLOR_MAKE(0x00, 0xC0, 0x30),  // 6: deep green
    };
    const int bar_w      = 280;
    const int bar_y      = GPS_ROW_Y0 + 4 * GPS_ROW_STEP + 8;   // y ≈ 328
    const int block_h    = 22;
    const int block_gap  = 4;
    const int block_w    = (bar_w - (SIG_BAR_BLOCKS - 1) * block_gap) / SIG_BAR_BLOCKS;
    const int bar_x0     = -bar_w / 2;
    for (int i = 0; i < SIG_BAR_BLOCKS; i++) {
        s_sig_bar_blocks[i] = lv_obj_create(parent);
        lv_obj_set_size(s_sig_bar_blocks[i], block_w, block_h);
        lv_obj_align(s_sig_bar_blocks[i], LV_ALIGN_TOP_MID,
                     bar_x0 + i * (block_w + block_gap) + block_w / 2, bar_y);
        lv_obj_set_style_bg_color(s_sig_bar_blocks[i], block_colors[i], 0);
        lv_obj_set_style_bg_opa(s_sig_bar_blocks[i], LV_OPA_20, 0);  // start dim
        lv_obj_set_style_border_width(s_sig_bar_blocks[i], 0, 0);
        lv_obj_set_style_radius(s_sig_bar_blocks[i], 4, 0);
        lv_obj_clear_flag(s_sig_bar_blocks[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_sig_bar_blocks[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_sig_bar_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(s_sig_bar_lbl, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_sig_bar_lbl, theme_subtext(), 0);
    lv_obj_align(s_sig_bar_lbl, LV_ALIGN_TOP_MID, 0, bar_y + block_h + 4);
    lv_label_set_text(s_sig_bar_lbl, "no data");

    // ── PHOTO button (small, bottom-left, inside safe zone) ───────────────
    s_btn_photo = lv_btn_create(parent);
    lv_obj_set_size(s_btn_photo, 90, 40);
    lv_obj_align(s_btn_photo, LV_ALIGN_BOTTOM_LEFT, GPS_PAD_H_UI, -110);
    lv_obj_set_style_bg_color(s_btn_photo, theme_divider(), 0);
    {
        lv_obj_t *lbl = lv_label_create(s_btn_photo);
        lv_label_set_text(lbl, "PHOTO");
        lv_obj_set_style_text_font(lbl, UI_FONT_LABEL, 0);
        lv_obj_center(lbl);
    }
    lv_obj_add_event_cb(s_btn_photo, cb_photo_btn, LV_EVENT_CLICKED, NULL);

    // ── MODE cycle button (bottom-right, mirrors PHOTO) ───────────────────
    // Two-line label: "MODE\n<current>". Tap cycles PED->WRIST->AUTO->AIR1G
    // via the same max_m10s_set_dynmodel() the CLI uses.
    s_btn_mode = lv_btn_create(parent);
    lv_obj_set_size(s_btn_mode, 100, 60);
    lv_obj_align(s_btn_mode, LV_ALIGN_BOTTOM_RIGHT, -GPS_PAD_H_UI, -110);
    lv_obj_set_style_bg_color(s_btn_mode, theme_divider(), 0);
    s_lbl_mode = lv_label_create(s_btn_mode);
    lv_label_set_text_fmt(s_lbl_mode, "MODE\n%s",
                          max_m10s_dynmodel_name(max_m10s_get_dynmodel()));
    lv_obj_set_style_text_font(s_lbl_mode, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_align(s_lbl_mode, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_lbl_mode);
    lv_obj_add_event_cb(s_btn_mode, cb_mode_btn, LV_EVENT_CLICKED, NULL);

    // ── ATOMIC SYNC button (bottom, big) ──────────────────────────────────
    s_btn_sync = lv_btn_create(parent);
    lv_obj_set_size(s_btn_sync, 280, 60);
    lv_obj_align(s_btn_sync, LV_ALIGN_BOTTOM_MID, 0, -GPS_PAD_V_UI);
    lv_obj_set_style_bg_color(s_btn_sync, COL_ACCENT, 0);
    lv_obj_add_state(s_btn_sync, LV_STATE_DISABLED);
    s_lbl_btn_sync = lv_label_create(s_btn_sync);
    lv_label_set_text(s_lbl_btn_sync, "ATOMIC SYNC");
    lv_obj_set_style_text_font(s_lbl_btn_sync, UI_FONT_TITLE, 0);
    lv_obj_center(s_lbl_btn_sync);
    lv_obj_add_event_cb(s_btn_sync, cb_sync_btn, LV_EVENT_CLICKED, NULL);

    // ── PHOTO view container (subject-bound HIDDEN flag) ──────────────────
    // Container: hidden when subj_photo_view != 1 (i.e., not in photo mode).
    s_photo = lv_obj_create(parent);
    lv_obj_set_size(s_photo, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_photo, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_photo, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_photo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_photo, 0, 0);
    lv_obj_set_style_pad_all(s_photo, 0, 0);
    lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_photo, cb_photo_container_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_bind_flag_if_not_eq(s_photo, &subj_gps_photo_view,
                               LV_OBJ_FLAG_HIDDEN, 1);

    s_photo_time = lv_label_create(s_photo);
    lv_obj_set_style_text_font(s_photo_time, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_photo_time, lv_color_white(), 0);
    lv_obj_align(s_photo_time, LV_ALIGN_TOP_MID, 0, 60);
    lv_label_bind_text(s_photo_time, &subj_gps_photo_time_str, NULL);

    s_photo_lat = lv_label_create(s_photo);
    lv_obj_set_style_text_font(s_photo_lat, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(s_photo_lat, lv_color_white(), 0);
    lv_obj_align(s_photo_lat, LV_ALIGN_CENTER, 0, -20);
    lv_label_bind_text(s_photo_lat, &subj_gps_photo_lat_str, NULL);

    s_photo_lon = lv_label_create(s_photo);
    lv_obj_set_style_text_font(s_photo_lon, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(s_photo_lon, lv_color_white(), 0);
    lv_obj_align(s_photo_lon, LV_ALIGN_CENTER, 0, 24);
    lv_label_bind_text(s_photo_lon, &subj_gps_photo_lon_str, NULL);

    s_photo_alt = lv_label_create(s_photo);
    lv_obj_set_style_text_font(s_photo_alt, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_photo_alt, lv_color_white(), 0);
    lv_obj_align(s_photo_alt, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_label_bind_text(s_photo_alt, &subj_gps_photo_alt_str, NULL);

    s_photo_hint = lv_label_create(s_photo);
    lv_label_set_text(s_photo_hint, "tap to exit");
    lv_obj_set_style_text_font(s_photo_hint, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_photo_hint,
                                lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(s_photo_hint, LV_ALIGN_BOTTOM_MID, 0, -30);

    // Normal-view widgets: HIDDEN when subj_photo_view == 1.
    lv_obj_t *normal_widgets[] = {
        s_led_status, s_lbl_header, s_sw_power, s_divider,
        s_lbl_status, s_lbl_time, s_lbl_lat, s_lbl_lon, s_lbl_altspd,
        s_btn_photo, s_btn_mode, s_btn_sync,
        s_sig_bar_blocks[0], s_sig_bar_blocks[1], s_sig_bar_blocks[2],
        s_sig_bar_blocks[3], s_sig_bar_blocks[4], s_sig_bar_blocks[5],
        s_sig_bar_blocks[6], s_sig_bar_lbl,
    };
    for (size_t i = 0; i < sizeof normal_widgets / sizeof normal_widgets[0]; i++) {
        lv_obj_bind_flag_if_eq(normal_widgets[i], &subj_gps_photo_view,
                               LV_OBJ_FLAG_HIDDEN, 1);
    }

    ESP_LOGI(TAG, "%s tile init OK (subject-bound, normal + photo views)",
             max_m10s_get_chip_name());
}

// ---------------------------------------------------------------------------
// gps_tile_update -- minimal: LED + sync button gating + feedback timeout
// ---------------------------------------------------------------------------
void gps_tile_update(void)
{
    sensor_status_t st = broker_gps_get_status();
    update_led(st);

    // ── Signal-strength bar ──────────────────────────────────────────────
    // Level 0: chip silent (no bar lit, label = "silent")
    // Level 1: chip alive but zero sats (only the leftmost grey block lit)
    // Levels 2..7: fill up-to-and-including the bucket for top-4 CN0.
    // We consider the chip "alive" if we've seen any NMEA within ~2s AND
    // the driver isn't OFFLINE.
    {
        max_m10s_snr_summary_t snr = {0};
        max_m10s_get_snr_summary(&snr);
        bool alive = (st != SENSOR_DISABLED && st != SENSOR_OFFLINE);
        uint8_t level;
        if (!alive)                          level = 0;
        else if (snr.sats_with_snr == 0)     level = 1;
        else if (snr.top4_avg_cn0 < 15)      level = 2;
        else if (snr.top4_avg_cn0 < 25)      level = 3;
        else if (snr.top4_avg_cn0 < 30)      level = 4;
        else if (snr.top4_avg_cn0 < 35)      level = 5;
        else if (snr.top4_avg_cn0 < 45)      level = 6;
        else                                  level = 7;
        for (int i = 0; i < SIG_BAR_BLOCKS; i++) {
            if (!s_sig_bar_blocks[i]) continue;
            lv_obj_set_style_bg_opa(s_sig_bar_blocks[i],
                                    (i < level) ? LV_OPA_COVER : LV_OPA_20, 0);
        }
        if (s_sig_bar_lbl) {
            if (!alive) {
                lv_label_set_text(s_sig_bar_lbl, "silent");
            } else if (snr.sats_with_snr == 0) {
                lv_label_set_text(s_sig_bar_lbl, "alive, 0 sats");
            } else {
                lv_label_set_text_fmt(s_sig_bar_lbl,
                                      "%u sats  CN0 %u dBHz  (max %u)",
                                      snr.sats_with_snr, snr.top4_avg_cn0,
                                      snr.max_cn0);
            }
        }
    }

    // Sync button enable: needs valid time from GPS + not showing feedback.
    if (!s_showing_feedback) {
        broker_gps_data_t d = {0};
        broker_gps_read(&d);
        if (d.time_valid && st != SENSOR_DISABLED && st != SENSOR_OFFLINE) {
            lv_obj_clear_state(s_btn_sync, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_btn_sync, LV_STATE_DISABLED);
        }
    }

    // Sync feedback timeout.
    if (s_showing_feedback) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_feedback_start_us) / 1000LL;
        if (elapsed_ms >= SYNC_FEEDBACK_MS) {
            lv_label_set_text(s_lbl_btn_sync, "ATOMIC SYNC");
            s_showing_feedback = false;
        }
    }

    // MODE label kept in sync with max_m10s_get_dynmodel() so CLI-driven
    // changes (GPS_DYNMODEL AUTO) also update the on-tile label.
    if (s_lbl_mode) {
        static max_m10s_dynmodel_t last_mode = (max_m10s_dynmodel_t)-1;
        max_m10s_dynmodel_t cur = max_m10s_get_dynmodel();
        if (cur != last_mode) {
            lv_label_set_text_fmt(s_lbl_mode, "MODE\n%s",
                                  max_m10s_dynmodel_name(cur));
            last_mode = cur;
        }
    }
}

// ---------------------------------------------------------------------------
// gps_tile_show_sync_result -- called from Core 0 after sync attempt
// ---------------------------------------------------------------------------
void gps_tile_show_sync_result(bool success)
{
    if (!s_lbl_btn_sync) return;
    lv_label_set_text(s_lbl_btn_sync,
                      success ? "SYNC OK \xe2\x9c\x93"
                              : "SYNC FAIL \xe2\x9c\x97");
    lv_obj_clear_state(s_btn_sync, LV_STATE_DISABLED);
    s_showing_feedback   = true;
    s_feedback_start_us  = esp_timer_get_time();
    ESP_LOGI(TAG, "Sync result: %s", success ? "OK" : "FAIL");
}

// ---------------------------------------------------------------------------
// gps_tile_apply_theme
// ---------------------------------------------------------------------------
void gps_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_parent) return;
    lv_obj_set_style_bg_color(s_parent,     theme_bg(),      0);
    lv_obj_set_style_bg_color(s_divider,    theme_divider(), 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_status, theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_time,   theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_lat,    theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_lon,    theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_altspd, theme_subtext(), 0);
}

// ---------------------------------------------------------------------------
// Sub-tile (raw NMEA debug view) -- kept poll-based; low-priority debug.
// ---------------------------------------------------------------------------
void gps_subtile_init(lv_obj_t *parent)
{
    s_sub_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_sub_hdr = lv_label_create(parent);
    lv_label_set_text(s_lbl_sub_hdr, "RAW NMEA DEBUG");
    lv_obj_set_style_text_font(s_lbl_sub_hdr, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_sub_hdr, theme_text(), 0);
    lv_obj_align(s_lbl_sub_hdr, LV_ALIGN_TOP_MID, 0, GPS_PAD_V_UI);

    s_lbl_gga = lv_label_create(parent);
    lv_label_set_text(s_lbl_gga, "$GPGGA: --");
    lv_obj_set_style_text_font(s_lbl_gga, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_gga, theme_subtext(), 0);
    lv_label_set_long_mode(s_lbl_gga, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_gga, 290);
    lv_obj_align(s_lbl_gga, LV_ALIGN_TOP_LEFT, GPS_PAD_H_TEXT, GPS_PAD_V_UI + 60);

    s_lbl_rmc = lv_label_create(parent);
    lv_label_set_text(s_lbl_rmc, "$GPRMC: --");
    lv_obj_set_style_text_font(s_lbl_rmc, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_rmc, theme_subtext(), 0);
    lv_label_set_long_mode(s_lbl_rmc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_rmc, 290);
    lv_obj_align(s_lbl_rmc, LV_ALIGN_TOP_LEFT, GPS_PAD_H_TEXT, GPS_PAD_V_UI + 200);

    ESP_LOGI(TAG, "GPS sub-tile init OK");
}

void gps_subtile_update(void)
{
    char gga[96] = {0};
    char rmc[96] = {0};
    max_m10s_get_debug_sentences(gga, sizeof(gga), rmc, sizeof(rmc));
    lv_label_set_text(s_lbl_gga, gga[0] ? gga : "-- no signal --");
    lv_label_set_text(s_lbl_rmc, rmc[0] ? rmc : "-- no signal --");
}

// ---------------------------------------------------------------------------
// Command surface -- delegates to ui_subjects view API.
// Kept in the old namespace so existing CLI verb (GPS_VIEW) still resolves.
// ---------------------------------------------------------------------------
gps_tile_view_t gps_tile_cmd_view_get(void)
{
    return kw_ui_gps_view_get() ? GPS_TILE_VIEW_PHOTO : GPS_TILE_VIEW_NORMAL;
}

void gps_tile_cmd_view_set(gps_tile_view_t v)
{
    bool photo = (v == GPS_TILE_VIEW_PHOTO);
    kw_ui_gps_view_set(photo ? KW_GPS_VIEW_PHOTO : KW_GPS_VIEW_NORMAL);
    ESP_LOGI(TAG, "view -> %s", photo ? "PHOTO" : "NORMAL");
}

void gps_tile_cmd_view_toggle(void)
{
    kw_ui_gps_view_toggle();
}

// ---------------------------------------------------------------------------
// Tile descriptor
// ---------------------------------------------------------------------------
const tile_desc_t gps_tile_desc = {
    .init           = gps_tile_init,
    .update         = gps_tile_update,
    .apply_theme    = gps_tile_apply_theme,
    .has_subtile    = true,
    .subtile_init   = gps_subtile_init,
    .subtile_update = gps_subtile_update,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT | LV_DIR_BOTTOM,
};
