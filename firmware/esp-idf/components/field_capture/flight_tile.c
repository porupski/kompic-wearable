/**
 * @file flight_tile.c
 * @brief FCM_FLIGHT recorder tile (Stage 38, revised 2026-10-04).
 *
 * The RECORD / STOP button on this tile is the primary entry point for
 * flight recording. Click toggles `fc_flight_cmd_start()` /
 * `fc_flight_cmd_stop()`. Recording runs in a background task so the
 * user can swipe to other tiles or let the display sleep (double-tap
 * body / button) while CSV rows keep landing on SD.
 *
 * No RGB animation during flight. The tile IS the status indicator;
 * the LED stays owned by rgb_policy for discretion (Ivan 2026-10-04).
 *
 * Portrait layout (410x502). Interactive elements stay inside the 15%
 * corner-safe zone per [[project_display_corner_cutoff]].
 */

#include "flight_tile.h"
#include "fc_flight.h"
#include "ui_theme_colors.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "FLIGHT_TILE";

#define F_PAD_H    40
#define F_PAD_V    40
#define F_PAD_UI   60   // interactive elements inside the arc
#define F_ROW_Y0  130
#define F_ROW_H    42

static lv_obj_t *s_parent      = NULL;
static lv_obj_t *s_lbl_header  = NULL;
static lv_obj_t *s_lbl_state   = NULL;   // "READY" / "REC HH:MM:SS"
static lv_obj_t *s_lbl_samples = NULL;
static lv_obj_t *s_lbl_sd      = NULL;
static lv_obj_t *s_lbl_gps     = NULL;
static lv_obj_t *s_btn_record  = NULL;
static lv_obj_t *s_lbl_btn     = NULL;

static void cb_record_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const fc_flight_state_t *fs = fc_flight_get_state();
    if (fs->active) {
        int r = fc_flight_cmd_stop();
        ESP_LOGI(TAG, "stop requested (r=%d)", r);
    } else {
        int r = fc_flight_cmd_start();
        ESP_LOGI(TAG, "start requested (r=%d)", r);
    }
}

static void flight_tile_init(lv_obj_t *parent)
{
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_header = lv_label_create(parent);
    lv_label_set_text(s_lbl_header, "FLIGHT");
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, F_PAD_H, F_PAD_V);

    s_lbl_state = lv_label_create(parent);
    lv_label_set_text(s_lbl_state, "READY");
    lv_obj_set_style_text_font(s_lbl_state, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_state, theme_text(), 0);
    lv_obj_align(s_lbl_state, LV_ALIGN_TOP_LEFT, F_PAD_H, F_ROW_Y0);

    s_lbl_samples = lv_label_create(parent);
    lv_label_set_text(s_lbl_samples, "Samples:  --");
    lv_obj_set_style_text_font(s_lbl_samples, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_samples, theme_subtext(), 0);
    lv_obj_align(s_lbl_samples, LV_ALIGN_TOP_LEFT, F_PAD_H, F_ROW_Y0 + F_ROW_H);

    s_lbl_sd = lv_label_create(parent);
    lv_label_set_text(s_lbl_sd, "SD:       --");
    lv_obj_set_style_text_font(s_lbl_sd, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_sd, theme_subtext(), 0);
    lv_obj_align(s_lbl_sd, LV_ALIGN_TOP_LEFT, F_PAD_H, F_ROW_Y0 + 2 * F_ROW_H);

    s_lbl_gps = lv_label_create(parent);
    lv_label_set_text(s_lbl_gps, "GPS:      no fix");
    lv_obj_set_style_text_font(s_lbl_gps, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_gps, theme_subtext(), 0);
    lv_obj_align(s_lbl_gps, LV_ALIGN_TOP_LEFT, F_PAD_H, F_ROW_Y0 + 3 * F_ROW_H);

    // Big RECORD / STOP button at the bottom, inside the corner-safe zone.
    s_btn_record = lv_btn_create(parent);
    lv_obj_set_size(s_btn_record, 280, 70);
    lv_obj_align(s_btn_record, LV_ALIGN_BOTTOM_MID, 0, -F_PAD_V);
    lv_obj_set_style_bg_color(s_btn_record, COL_STATUS_OFFLINE, 0);
    lv_obj_add_event_cb(s_btn_record, cb_record_btn, LV_EVENT_CLICKED, NULL);

    s_lbl_btn = lv_label_create(s_btn_record);
    lv_label_set_text(s_lbl_btn, "RECORD");
    lv_obj_set_style_text_font(s_lbl_btn, UI_FONT_TITLE, 0);
    lv_obj_center(s_lbl_btn);

    ESP_LOGI(TAG, "flight tile init OK");
}

static void flight_tile_update(void)
{
    const fc_flight_state_t *fs = fc_flight_get_state();
    if (!s_lbl_state) return;

    if (fs->active) {
        uint32_t s = fs->elapsed_s;
        lv_label_set_text_fmt(s_lbl_state, "REC %02u:%02u:%02u",
                              (unsigned)(s / 3600),
                              (unsigned)((s / 60) % 60),
                              (unsigned)(s % 60));
        lv_obj_set_style_text_color(s_lbl_state,
            fs->sd_ok ? COL_STATUS_ONLINE : COL_STATUS_OFFLINE, 0);
        lv_label_set_text(s_lbl_btn, "STOP");
        lv_obj_set_style_bg_color(s_btn_record, COL_STATUS_ONLINE, 0);
    } else {
        lv_label_set_text(s_lbl_state, "READY");
        lv_obj_set_style_text_color(s_lbl_state, theme_text(), 0);
        lv_label_set_text(s_lbl_btn, "RECORD");
        lv_obj_set_style_bg_color(s_btn_record, COL_STATUS_OFFLINE, 0);
    }

    lv_label_set_text_fmt(s_lbl_samples, "Samples:  %lu",
                          (unsigned long)fs->sample_count);
    lv_label_set_text_fmt(s_lbl_sd, "SD:       %s  %luMiB  err %lu",
                          fs->sd_ok ? "OK" : "FAIL",
                          (unsigned long)fs->sd_free_mib,
                          (unsigned long)fs->write_errors);
    lv_label_set_text_fmt(s_lbl_gps, "GPS:      %s",
                          fs->gps_fix == 3 ? "3D fix" :
                          fs->gps_fix == 2 ? "2D fix" : "no fix");
}

static void flight_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_parent) return;
    lv_obj_set_style_bg_color(s_parent, theme_bg(), 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_set_style_text_color(s_lbl_samples, theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_sd, theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_gps, theme_subtext(), 0);
    // s_lbl_state colour + button bg are set per-update based on recording state.
}

const tile_desc_t flight_tile_desc = {
    .init           = flight_tile_init,
    .update         = flight_tile_update,
    .apply_theme    = flight_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};
