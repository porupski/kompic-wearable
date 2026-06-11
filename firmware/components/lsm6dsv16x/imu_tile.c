/**
 * @file imu_tile.c
 * @brief LSM6DSV16X IMU tile -- Blueprint 5 §7 conformant. Type B (status + data).
 *
 * v7.2 resize (2026-06-11): layout rescaled from 240x280 to 410x502 CO5300.
 * Y positions multiplied by ~1.79, X offsets bumped where the original was
 * already tight on the small panel.
 *
 * Reads broker_imu_read() only — no I2C, no math, no filter state here.
 * Complementary filter runs in task_imu_fn (Core 0); roll/pitch arrive pre-computed.
 *
 * Layout (240×280 tile):
 *   Row  0–42  : Title "QMI8658C" + switch + chip subtitle
 *   Row 42–62  : Status label + LED dot
 *   Row 62–140 : Accel XYZ values (left column)
 *   Row 62–140 : Gyro XYZ values  (right column)
 *   Row 145–175: Roll / Pitch filtered values
 *   Row 180–200: Temperature
 *
 * Float formatting rule (Blueprint 5 §format_rule):
 *   All float values are pre-formatted via snprintf() into char buf[] before
 *   calling lv_label_set_text(). lv_label_set_text_fmt() with %f is NEVER used
 *   because LVGL's lv_snprintf may not support %f on all builds.
 *   With LV_USE_FLOAT_PRINTF=1 in lv_conf.h this is belt-and-suspenders,
 *   but the snprintf pattern is always used regardless to be safe.
 *
 * Theme support:
 *   All label handles promoted to static. apply_theme() walks every owned
 *   label. s_lbl_status excluded (semantic colour owned by update()).
 *
 * Core 1 only. All functions called inside lvgl_port_lock().
 */

#include "imu_tile.h"
#include "lsm6dsv16x.h"     // identity functions for header text
#include "data_broker.h"
#include "ui_theme_colors.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>

static const char *TAG = "IMU_TILE";

// ---------------------------------------------------------------------------
// Widget handles — all static so apply_theme() can reach them
// ---------------------------------------------------------------------------

static lv_obj_t *s_parent           = NULL;

static lv_obj_t *s_led_dot          = NULL;
static lv_obj_t *s_lbl_title        = NULL;
static lv_obj_t *s_lbl_chip         = NULL;
static lv_obj_t *s_sw_power         = NULL;
static lv_obj_t *s_lbl_status       = NULL;

// Accel column
static lv_obj_t *s_lbl_accel_hdr    = NULL;
static lv_obj_t *s_lbl_ax           = NULL;
static lv_obj_t *s_lbl_ay           = NULL;
static lv_obj_t *s_lbl_az           = NULL;

// Gyro column
static lv_obj_t *s_lbl_gyro_hdr     = NULL;
static lv_obj_t *s_lbl_gx           = NULL;
static lv_obj_t *s_lbl_gy           = NULL;
static lv_obj_t *s_lbl_gz           = NULL;

// Orientation row
static lv_obj_t *s_lbl_orient_hdr   = NULL;
static lv_obj_t *s_lbl_roll         = NULL;
static lv_obj_t *s_lbl_pitch        = NULL;

// Temperature
static lv_obj_t *s_lbl_temp         = NULL;

// Re-entrancy guard for power switch
static bool s_syncing_power = false;

// ---------------------------------------------------------------------------
// Event callback
// ---------------------------------------------------------------------------

static void cb_power(lv_event_t *e)
{
    if (s_syncing_power) return;
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    broker_imu_set_enabled(on);
    ESP_LOGI(TAG, "IMU %s", on ? "ON" : "OFF");
}

// ---------------------------------------------------------------------------
// imu_tile_init
// ---------------------------------------------------------------------------

void imu_tile_init(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "imu_tile_init");

    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // 410x502 layout constants -- old 240x280 values in /* */ for reference.
    static const int LED_X      = 14;   /*  8 */
    static const int LED_Y      = 18;   /* 10 */
    static const int LED_SIZE   = 16;   /* 10 */
    static const int TITLE_Y    = 13;   /*  7 */
    static const int CHIP_Y     = 45;   /* 25 */
    static const int SWITCH_X   = -14;  /* -8 */
    static const int SWITCH_Y   = 13;   /*  7 */
    static const int STATUS_Y   = 79;   /* 44 */
    static const int COL_HDR_Y  = 110;  /* 62 */
    static const int ROW_X_L    = 20;   /* 12 */
    static const int ROW_X_R    = -20;  /* -12 */
    static const int ROW_Y_0    = 140;  /* 78 */
    static const int ROW_Y_1    = 170;  /* 95 */
    static const int ROW_Y_2    = 200;  /* 112 */
    static const int ORIENT_Y   = 240;  /* 135 */
    static const int ORIENT_VAL_Y = 274;/* 153 */
    static const int TEMP_OFF_Y = -16;  /*  -8 */

    // -- LED dot ---------------------------------------------------------------
    s_led_dot = lv_obj_create(parent);
    lv_obj_set_size(s_led_dot, LED_SIZE, LED_SIZE);
    lv_obj_set_style_radius(s_led_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_led_dot, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_border_width(s_led_dot, 0, 0);
    lv_obj_align(s_led_dot, LV_ALIGN_TOP_LEFT, LED_X, LED_Y);

    // -- Title + chip subtitle (driven by lsm6dsv16x identity) -----------------
    s_lbl_title = lv_label_create(parent);
    lv_label_set_text(s_lbl_title, lsm6dsv16x_get_chip_name());
    lv_obj_set_style_text_font(s_lbl_title, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_title, theme_text(), 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    s_lbl_chip = lv_label_create(parent);
    lv_label_set_text(s_lbl_chip, lsm6dsv16x_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_chip, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_chip, theme_subtext(), 0);
    lv_obj_align(s_lbl_chip, LV_ALIGN_TOP_MID, 0, CHIP_Y);

    // -- Power switch ----------------------------------------------------------
    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 60, 30);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, SWITCH_X, SWITCH_Y);
    lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_sw_power, cb_power, LV_EVENT_VALUE_CHANGED, NULL);

    // -- Status label ----------------------------------------------------------
    s_lbl_status = lv_label_create(parent);
    lv_label_set_text(s_lbl_status, "Acquiring...");
    lv_obj_set_style_text_color(s_lbl_status, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_text_font(s_lbl_status, UI_FONT_CHIP, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, STATUS_Y);

    // -- Accel column (left) ---------------------------------------------------
    s_lbl_accel_hdr = lv_label_create(parent);
    lv_label_set_text(s_lbl_accel_hdr, "ACCEL m/s2");
    lv_obj_set_style_text_font(s_lbl_accel_hdr, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_accel_hdr, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_align(s_lbl_accel_hdr, LV_ALIGN_TOP_LEFT, LED_X, COL_HDR_Y);

    s_lbl_ax = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_ax, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_ax, theme_text(), 0);
    lv_obj_align(s_lbl_ax, LV_ALIGN_TOP_LEFT, ROW_X_L, ROW_Y_0);
    lv_label_set_text(s_lbl_ax, "X: ---");

    s_lbl_ay = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_ay, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_ay, theme_text(), 0);
    lv_obj_align(s_lbl_ay, LV_ALIGN_TOP_LEFT, ROW_X_L, ROW_Y_1);
    lv_label_set_text(s_lbl_ay, "Y: ---");

    s_lbl_az = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_az, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_az, theme_text(), 0);
    lv_obj_align(s_lbl_az, LV_ALIGN_TOP_LEFT, ROW_X_L, ROW_Y_2);
    lv_label_set_text(s_lbl_az, "Z: ---");

    // -- Gyro column (right) ---------------------------------------------------
    s_lbl_gyro_hdr = lv_label_create(parent);
    lv_label_set_text(s_lbl_gyro_hdr, "GYRO deg/s");
    lv_obj_set_style_text_font(s_lbl_gyro_hdr, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_gyro_hdr, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_align(s_lbl_gyro_hdr, LV_ALIGN_TOP_RIGHT, -LED_X, COL_HDR_Y);

    s_lbl_gx = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_gx, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_gx, theme_text(), 0);
    lv_obj_align(s_lbl_gx, LV_ALIGN_TOP_RIGHT, ROW_X_R, ROW_Y_0);
    lv_label_set_text(s_lbl_gx, "X: ---");

    s_lbl_gy = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_gy, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_gy, theme_text(), 0);
    lv_obj_align(s_lbl_gy, LV_ALIGN_TOP_RIGHT, ROW_X_R, ROW_Y_1);
    lv_label_set_text(s_lbl_gy, "Y: ---");

    s_lbl_gz = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_gz, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_gz, theme_text(), 0);
    lv_obj_align(s_lbl_gz, LV_ALIGN_TOP_RIGHT, ROW_X_R, ROW_Y_2);
    lv_label_set_text(s_lbl_gz, "Z: ---");

    // -- Orientation (filtered) ------------------------------------------------
    s_lbl_orient_hdr = lv_label_create(parent);
    lv_label_set_text(s_lbl_orient_hdr, "ORIENTATION");
    lv_obj_set_style_text_font(s_lbl_orient_hdr, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_orient_hdr, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(s_lbl_orient_hdr, LV_ALIGN_TOP_MID, 0, ORIENT_Y);

    s_lbl_roll = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_roll, UI_FONT_VALUE, 0);
    lv_obj_set_style_text_color(s_lbl_roll, theme_text(), 0);
    lv_obj_align(s_lbl_roll, LV_ALIGN_TOP_LEFT, LED_X, ORIENT_VAL_Y);
    lv_label_set_text(s_lbl_roll, "Roll:  ---");

    s_lbl_pitch = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_pitch, UI_FONT_VALUE, 0);
    lv_obj_set_style_text_color(s_lbl_pitch, theme_text(), 0);
    lv_obj_align(s_lbl_pitch, LV_ALIGN_TOP_RIGHT, -LED_X, ORIENT_VAL_Y);
    lv_label_set_text(s_lbl_pitch, "Pitch: ---");

    // -- Temperature -----------------------------------------------------------
    s_lbl_temp = lv_label_create(parent);
    lv_obj_set_style_text_font(s_lbl_temp, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_temp, theme_subtext(), 0);
    lv_obj_align(s_lbl_temp, LV_ALIGN_BOTTOM_MID, 0, TEMP_OFF_Y);
    lv_label_set_text(s_lbl_temp, "Temp: ---");
}

// ---------------------------------------------------------------------------
// imu_tile_update
// ---------------------------------------------------------------------------

void imu_tile_update(void)
{
    if (!s_lbl_status) return;

    bool hw_alive = broker_imu_hw_alive();
    bool enabled  = broker_imu_get_enabled();

    // -- LED dot ---------------------------------------------------------------
    if (s_led_dot) {
        lv_color_t col;
        if (!hw_alive)     col = COL_STATUS_OFFLINE;
        else if (!enabled) col = COL_STATUS_DISABLED;
        else               col = COL_STATUS_ONLINE;
        lv_obj_set_style_bg_color(s_led_dot, col, 0);
    }

    // -- Power switch sync -----------------------------------------------------
    if (s_sw_power) {
        bool sw_on = lv_obj_has_state(s_sw_power, LV_STATE_CHECKED);
        if (sw_on != enabled) {
            s_syncing_power = true;
            if (enabled) lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
            else         lv_obj_clear_state(s_sw_power, LV_STATE_CHECKED);
            s_syncing_power = false;
        }
    }

    // -- Early-out for offline / disabled --------------------------------------
    if (!hw_alive) {
        lv_label_set_text(s_lbl_status, "OFFLINE");
        lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_OFFLINE, 0);
        return;
    }
    if (!enabled) {
        lv_label_set_text(s_lbl_status, "Disabled");
        lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_DISABLED, 0);
        if (s_lbl_ax) lv_label_set_text(s_lbl_ax, "X: ---");
        if (s_lbl_ay) lv_label_set_text(s_lbl_ay, "Y: ---");
        if (s_lbl_az) lv_label_set_text(s_lbl_az, "Z: ---");
        if (s_lbl_gx) lv_label_set_text(s_lbl_gx, "X: ---");
        if (s_lbl_gy) lv_label_set_text(s_lbl_gy, "Y: ---");
        if (s_lbl_gz) lv_label_set_text(s_lbl_gz, "Z: ---");
        if (s_lbl_roll)  lv_label_set_text(s_lbl_roll,  "Roll:  ---");
        if (s_lbl_pitch) lv_label_set_text(s_lbl_pitch, "Pitch: ---");
        if (s_lbl_temp)  lv_label_set_text(s_lbl_temp,  "Temp: ---");
        return;
    }

    sensor_status_t status = broker_imu_get_status();
    if (status == SENSOR_STALE || status == SENSOR_OFFLINE) {
        lv_label_set_text(s_lbl_status, "Stale");
        lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_STALE, 0);
        return;
    }

    lv_label_set_text(s_lbl_status, "Online");
    lv_obj_set_style_text_color(s_lbl_status, COL_STATUS_ONLINE, 0);

    // -- Data -----------------------------------------------------------------
    broker_imu_data_t imu = {0};
    broker_imu_read(&imu);

    // Float formatting rule (Blueprint 5 §format_rule):
    // All floats pre-formatted via snprintf into buf, then lv_label_set_text().
    // Never use lv_label_set_text_fmt with %f.
    char buf[32];

    // Accel XYZ
    snprintf(buf, sizeof(buf), "X: %.2f", (double)imu.accel_x);
    lv_label_set_text(s_lbl_ax, buf);
    snprintf(buf, sizeof(buf), "Y: %.2f", (double)imu.accel_y);
    lv_label_set_text(s_lbl_ay, buf);
    snprintf(buf, sizeof(buf), "Z: %.2f", (double)imu.accel_z);
    lv_label_set_text(s_lbl_az, buf);

    // Gyro XYZ
    snprintf(buf, sizeof(buf), "X: %.1f", (double)imu.gyro_x);
    lv_label_set_text(s_lbl_gx, buf);
    snprintf(buf, sizeof(buf), "Y: %.1f", (double)imu.gyro_y);
    lv_label_set_text(s_lbl_gy, buf);
    snprintf(buf, sizeof(buf), "Z: %.1f", (double)imu.gyro_z);
    lv_label_set_text(s_lbl_gz, buf);

    // Orientation
    snprintf(buf, sizeof(buf), "Roll:  %.1f", (double)imu.roll_deg);
    lv_label_set_text(s_lbl_roll, buf);
    snprintf(buf, sizeof(buf), "Pitch: %.1f", (double)imu.pitch_deg);
    lv_label_set_text(s_lbl_pitch, buf);

    // Temperature
    snprintf(buf, sizeof(buf), "Temp: %.1f C", (double)imu.temperature);
    lv_label_set_text(s_lbl_temp, buf);
}

// ---------------------------------------------------------------------------
// imu_tile_apply_theme
// ---------------------------------------------------------------------------

void imu_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent, theme_bg(), 0);

    if (s_lbl_title)      lv_obj_set_style_text_color(s_lbl_title,      theme_text(),    0);
    if (s_lbl_chip)       lv_obj_set_style_text_color(s_lbl_chip,       theme_subtext(), 0);
    if (s_lbl_ax)         lv_obj_set_style_text_color(s_lbl_ax,         theme_text(),    0);
    if (s_lbl_ay)         lv_obj_set_style_text_color(s_lbl_ay,         theme_text(),    0);
    if (s_lbl_az)         lv_obj_set_style_text_color(s_lbl_az,         theme_text(),    0);
    if (s_lbl_gx)         lv_obj_set_style_text_color(s_lbl_gx,         theme_text(),    0);
    if (s_lbl_gy)         lv_obj_set_style_text_color(s_lbl_gy,         theme_text(),    0);
    if (s_lbl_gz)         lv_obj_set_style_text_color(s_lbl_gz,         theme_text(),    0);
    if (s_lbl_roll)       lv_obj_set_style_text_color(s_lbl_roll,       theme_text(),    0);
    if (s_lbl_pitch)      lv_obj_set_style_text_color(s_lbl_pitch,      theme_text(),    0);
    if (s_lbl_temp)       lv_obj_set_style_text_color(s_lbl_temp,       theme_subtext(), 0);
    // s_lbl_status: semantic colour owned by update() — not overridden here
    // Column headers: fixed accent colours — not theme-dependent
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