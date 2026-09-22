/**
 * @file imu_tile.c
 * @brief LSM6DSV16X IMU tile -- LVGL 9, Core 1 only.
 *
 * Stage 32.1 rewrite -- subject-bound, corner-safe. Every value label is
 * driven from ui_subjects; imu_tile_update() only owns LED colour +
 * subj_imu_status_str colour tinting. Power switch uses
 * lv_obj_bind_checked against subj_imu_enabled (two-way -- observer in
 * ui_subjects.c mirrors to broker).
 *
 * Layout (portrait 410x502, 60H/40V corner-safe pads per
 * project_display_corner_cutoff):
 *   Row 0 (y=40):  LED(16) + header + power switch
 *   y=100:         "Acquiring..." / "Online" / "Disabled" status
 *   y=140-200:     Accel XYZ (left) + Gyro XYZ (right)
 *   y=250:         Orientation header
 *   y=284:         Roll (left) + Pitch (right)
 *   y=-40 (bot):   Temperature
 */

#include "imu_tile.h"
#include "lsm6dsv16x.h"      // identity strings
#include "data_broker.h"
#include "ui_theme_colors.h"
#include "ui_subjects.h"     // subj_imu_*, subj_imu_enabled
#include "esp_log.h"

static const char *TAG = "IMU_TILE";

// ---------------------------------------------------------------------------
// Corner-safe layout constants (matches env_tile / gps_tile convention)
// ---------------------------------------------------------------------------
#define IMU_PAD_H_UI       60
#define IMU_PAD_H_TEXT     40
#define IMU_PAD_V_UI       40
#define IMU_STATUS_Y      100
#define IMU_COL_HDR_Y     140
#define IMU_ROW_Y0        170
#define IMU_ROW_STEP       32
#define IMU_ORIENT_HDR_Y  280
#define IMU_ORIENT_VAL_Y  314

// ---------------------------------------------------------------------------
// Static widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_parent           = NULL;

static lv_obj_t *s_led_status       = NULL;
static lv_obj_t *s_lbl_header       = NULL;
static lv_obj_t *s_sw_power         = NULL;
static lv_obj_t *s_lbl_status       = NULL;

static lv_obj_t *s_lbl_accel_hdr    = NULL;
static lv_obj_t *s_lbl_gyro_hdr     = NULL;
static lv_obj_t *s_lbl_orient_hdr   = NULL;

static lv_obj_t *s_lbl_ax           = NULL;
static lv_obj_t *s_lbl_ay           = NULL;
static lv_obj_t *s_lbl_az           = NULL;
static lv_obj_t *s_lbl_gx           = NULL;
static lv_obj_t *s_lbl_gy           = NULL;
static lv_obj_t *s_lbl_gz           = NULL;
static lv_obj_t *s_lbl_roll         = NULL;
static lv_obj_t *s_lbl_pitch        = NULL;
static lv_obj_t *s_lbl_temp         = NULL;

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
// imu_tile_init
// ---------------------------------------------------------------------------
void imu_tile_init(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "imu_tile_init");

    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ── Row 0: LED + header + power switch ────────────────────────────────
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, 16, 16);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, IMU_PAD_H_UI, IMU_PAD_V_UI + 12);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    s_lbl_header = lv_label_create(parent);
    lv_label_set_text(s_lbl_header, lsm6dsv16x_get_chip_name());   // chip-name only
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, IMU_PAD_H_UI + 26, IMU_PAD_V_UI);

    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 90, 46);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, -IMU_PAD_H_UI, IMU_PAD_V_UI);
    // Two-way bind -- user tap sets subject; observer in ui_subjects.c
    // pushes to broker. Drain mirrors broker back into subject on next tick.
    lv_obj_bind_checked(s_sw_power, &subj_imu_enabled);

    // ── Status line (subject-bound text; colour owned by update()) ─────────
    s_lbl_status = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_status, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_ACQUIRING, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, IMU_STATUS_Y);
    lv_label_bind_text(s_lbl_status, &subj_imu_status_str, NULL);

    // ── Column headers ────────────────────────────────────────────────────
    s_lbl_accel_hdr = lv_label_create(parent);
    lv_label_set_text(s_lbl_accel_hdr, "ACCEL m/s2");
    lv_obj_set_style_text_font(s_lbl_accel_hdr, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_accel_hdr, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_align(s_lbl_accel_hdr, LV_ALIGN_TOP_LEFT, IMU_PAD_H_TEXT, IMU_COL_HDR_Y);

    s_lbl_gyro_hdr = lv_label_create(parent);
    lv_label_set_text(s_lbl_gyro_hdr, "GYRO deg/s");
    lv_obj_set_style_text_font(s_lbl_gyro_hdr, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_gyro_hdr, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_align(s_lbl_gyro_hdr, LV_ALIGN_TOP_RIGHT, -IMU_PAD_H_TEXT, IMU_COL_HDR_Y);

    // ── Accel column (left, subject-bound) ────────────────────────────────
    const lv_font_t *fnt_row = UI_FONT_LABEL;
    lv_color_t       col_row = theme_text();

    s_lbl_ax = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_ax, fnt_row, 0);
    lv_obj_set_style_text_color(s_lbl_ax, col_row, 0);
    lv_obj_align(s_lbl_ax, LV_ALIGN_TOP_LEFT, IMU_PAD_H_TEXT, IMU_ROW_Y0);
    lv_label_bind_text(s_lbl_ax, &subj_imu_accel_x_str, NULL);

    s_lbl_ay = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_ay, fnt_row, 0);
    lv_obj_set_style_text_color(s_lbl_ay, col_row, 0);
    lv_obj_align(s_lbl_ay, LV_ALIGN_TOP_LEFT, IMU_PAD_H_TEXT, IMU_ROW_Y0 + IMU_ROW_STEP);
    lv_label_bind_text(s_lbl_ay, &subj_imu_accel_y_str, NULL);

    s_lbl_az = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_az, fnt_row, 0);
    lv_obj_set_style_text_color(s_lbl_az, col_row, 0);
    lv_obj_align(s_lbl_az, LV_ALIGN_TOP_LEFT, IMU_PAD_H_TEXT, IMU_ROW_Y0 + 2 * IMU_ROW_STEP);
    lv_label_bind_text(s_lbl_az, &subj_imu_accel_z_str, NULL);

    // ── Gyro column (right, subject-bound) ────────────────────────────────
    s_lbl_gx = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_gx, fnt_row, 0);
    lv_obj_set_style_text_color(s_lbl_gx, col_row, 0);
    lv_obj_align(s_lbl_gx, LV_ALIGN_TOP_RIGHT, -IMU_PAD_H_TEXT, IMU_ROW_Y0);
    lv_label_bind_text(s_lbl_gx, &subj_imu_gyro_x_str, NULL);

    s_lbl_gy = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_gy, fnt_row, 0);
    lv_obj_set_style_text_color(s_lbl_gy, col_row, 0);
    lv_obj_align(s_lbl_gy, LV_ALIGN_TOP_RIGHT, -IMU_PAD_H_TEXT, IMU_ROW_Y0 + IMU_ROW_STEP);
    lv_label_bind_text(s_lbl_gy, &subj_imu_gyro_y_str, NULL);

    s_lbl_gz = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_gz, fnt_row, 0);
    lv_obj_set_style_text_color(s_lbl_gz, col_row, 0);
    lv_obj_align(s_lbl_gz, LV_ALIGN_TOP_RIGHT, -IMU_PAD_H_TEXT, IMU_ROW_Y0 + 2 * IMU_ROW_STEP);
    lv_label_bind_text(s_lbl_gz, &subj_imu_gyro_z_str, NULL);

    // ── Orientation (filtered) ────────────────────────────────────────────
    s_lbl_orient_hdr = lv_label_create(parent);
    lv_label_set_text(s_lbl_orient_hdr, "ORIENTATION");
    lv_obj_set_style_text_font(s_lbl_orient_hdr, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_orient_hdr, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(s_lbl_orient_hdr, LV_ALIGN_TOP_MID, 0, IMU_ORIENT_HDR_Y);

    s_lbl_roll = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_roll, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_roll, theme_text(), 0);
    lv_obj_align(s_lbl_roll, LV_ALIGN_TOP_LEFT, IMU_PAD_H_TEXT, IMU_ORIENT_VAL_Y);
    lv_label_bind_text(s_lbl_roll, &subj_imu_roll_str, NULL);

    s_lbl_pitch = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_pitch, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_pitch, theme_text(), 0);
    lv_obj_align(s_lbl_pitch, LV_ALIGN_TOP_RIGHT, -IMU_PAD_H_TEXT, IMU_ORIENT_VAL_Y);
    lv_label_bind_text(s_lbl_pitch, &subj_imu_pitch_str, NULL);

    // ── Temperature (bottom) ──────────────────────────────────────────────
    s_lbl_temp = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_temp, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_temp, theme_subtext(), 0);
    lv_obj_align(s_lbl_temp, LV_ALIGN_BOTTOM_MID, 0, -IMU_PAD_V_UI);
    lv_label_bind_text(s_lbl_temp, &subj_imu_temp_str, NULL);

    ESP_LOGI(TAG, "%s tile init OK (subject-bound)", lsm6dsv16x_get_chip_name());
}

// ---------------------------------------------------------------------------
// imu_tile_update -- minimal: LED colour + status-label colour tint
//
// Value labels + switch state are subject-driven. Status TEXT is subject-
// driven; only the status COLOUR is per-status semantic and is set here.
// (Deleting this cb entirely is Stage 33.3 scope.)
// ---------------------------------------------------------------------------
void imu_tile_update(void)
{
    if (!s_lbl_status) return;

    bool hw_alive = broker_imu_hw_alive();
    bool enabled  = broker_imu_get_enabled();
    sensor_status_t st = broker_imu_get_status();

    // -- LED dot ---------------------------------------------------------------
    if (!hw_alive)     update_led(SENSOR_OFFLINE);
    else if (!enabled) update_led(SENSOR_DISABLED);
    else               update_led(st);

    // -- Status-label colour (text itself set by drain formatter) --------------
    lv_color_t status_col;
    if (!hw_alive)                                       status_col = COL_STATUS_OFFLINE;
    else if (!enabled)                                   status_col = COL_STATUS_DISABLED;
    else if (st == SENSOR_STALE || st == SENSOR_OFFLINE) status_col = COL_STATUS_STALE;
    else                                                 status_col = COL_STATUS_ONLINE;
    lv_obj_set_style_text_color(s_lbl_status, status_col, 0);
}

// ---------------------------------------------------------------------------
// imu_tile_apply_theme
// ---------------------------------------------------------------------------
void imu_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent, theme_bg(), 0);

    if (s_lbl_header) lv_obj_set_style_text_color(s_lbl_header, theme_text(),    0);
    if (s_lbl_ax)     lv_obj_set_style_text_color(s_lbl_ax,     theme_text(),    0);
    if (s_lbl_ay)     lv_obj_set_style_text_color(s_lbl_ay,     theme_text(),    0);
    if (s_lbl_az)     lv_obj_set_style_text_color(s_lbl_az,     theme_text(),    0);
    if (s_lbl_gx)     lv_obj_set_style_text_color(s_lbl_gx,     theme_text(),    0);
    if (s_lbl_gy)     lv_obj_set_style_text_color(s_lbl_gy,     theme_text(),    0);
    if (s_lbl_gz)     lv_obj_set_style_text_color(s_lbl_gz,     theme_text(),    0);
    if (s_lbl_roll)   lv_obj_set_style_text_color(s_lbl_roll,   theme_text(),    0);
    if (s_lbl_pitch)  lv_obj_set_style_text_color(s_lbl_pitch,  theme_text(),    0);
    if (s_lbl_temp)   lv_obj_set_style_text_color(s_lbl_temp,   theme_subtext(), 0);
    // s_lbl_status: semantic colour owned by update()
    // Column headers: fixed accent colours -- not theme-dependent
}

// ---------------------------------------------------------------------------
// Tile descriptor
// ---------------------------------------------------------------------------
const tile_desc_t imu_tile_desc = {
    .init           = imu_tile_init,
    .update         = imu_tile_update,
    .apply_theme    = imu_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};
