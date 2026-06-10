/**
 * @file battery_tile.c
 * @brief BQ25619 battery settings tile -- LVGL 9 UI, Core 1 only.
 *
 * Tile type   : A (status + data rows, read-only)
 * Layout      : Header + divider + 6 data rows (Vbat / SoC / charge state /
 *               power-good / fault / boost).
 *
 * Status LED maps from broker_battery_get_status():
 *   ONLINE  -- fresh broker data, fault == 0
 *   OFFLINE -- I2C reads failing
 *   STALE   -- broker timeout
 *   NOTIF   -- low-battery threshold reached (existing convention in broker)
 *
 * Sized for the 410x502 CO5300 panel from day one. Constants mirror the
 * rtc_tile layout so the two tiles look visually consistent.
 */

#include "battery_tile.h"
#include "bq25619.h"
#include "data_broker.h"
#include "ui_theme_colors.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "BAT_TILE";

// Widgets
static lv_obj_t *s_parent      = NULL;
static lv_obj_t *s_led_status  = NULL;
static lv_obj_t *s_lbl_header  = NULL;
static lv_obj_t *s_divider     = NULL;
static lv_obj_t *s_lbl_vbat    = NULL;  // "Vbat:   3.85 V"
static lv_obj_t *s_lbl_soc     = NULL;  // "SoC:    62 %"
static lv_obj_t *s_lbl_chrg    = NULL;  // "State:  Fast charge"
static lv_obj_t *s_lbl_pg      = NULL;  // "Power:  USB OK"
static lv_obj_t *s_lbl_fault   = NULL;  // "Fault:  0x00"
static lv_obj_t *s_lbl_boost   = NULL;  // "Boost:  OFF"

// Charge state -> short string (matches bq25619_charge_state_t order)
static const char *k_chrg[] = { "Idle", "Pre-charge", "Fast charge", "Done" };

static void update_led(sensor_status_t st)
{
    lv_color_t col;
    switch (st) {
        case SENSOR_ONLINE:   col = COL_STATUS_ONLINE;   break;
        case SENSOR_OFFLINE:  col = COL_STATUS_OFFLINE;  break;
        case SENSOR_STALE:    col = COL_STATUS_STALE;    break;
        case SENSOR_NOTIF:    col = COL_STATUS_NOTIF;    break;
        case SENSOR_DISABLED: col = COL_STATUS_OFFLINE;  break;
        default:              col = COL_STATUS_OFFLINE;  break;
    }
    lv_led_set_color(s_led_status, col);
}

void battery_tile_init(lv_obj_t *parent)
{
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // 410x502 layout constants -- consistent with rtc_tile.c
    static const int X_MARGIN    = 20;
    static const int LED_SIZE    = 16;
    static const int LED_X       = 20;
    static const int LED_Y       = 20;
    static const int HEADER_X    = 44;
    static const int HEADER_Y    = 18;
    static const int DIVIDER_W   = 360;
    static const int DIVIDER_Y   = 50;
    static const int ROW0_Y      = 64;
    static const int ROW_STEP    = 32;

    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, LED_SIZE, LED_SIZE);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, LED_X, LED_Y);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_OFFLINE);

    s_lbl_header = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_header, "%s  %s",
        bq25619_get_chip_name(), bq25619_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, HEADER_X, HEADER_Y);

    s_divider = lv_obj_create(parent);
    lv_obj_set_size(s_divider, DIVIDER_W, 1);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, DIVIDER_Y);
    lv_obj_set_style_bg_color(s_divider, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t *fnt = UI_FONT_LABEL;
    lv_color_t       col = theme_subtext();

    struct row { lv_obj_t **w; const char *initial; };
    struct row rows[] = {
        { &s_lbl_vbat,  "Vbat:   --.-- V"     },
        { &s_lbl_soc,   "SoC:    --- %"       },
        { &s_lbl_chrg,  "State:  ---"         },
        { &s_lbl_pg,    "Power:  ---"         },
        { &s_lbl_fault, "Fault:  0x--"        },
        { &s_lbl_boost, "Boost:  ---"         },
    };
    for (int i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++) {
        *rows[i].w = lv_label_create(parent);
        lv_label_set_text(*rows[i].w, rows[i].initial);
        lv_obj_set_style_text_font(*rows[i].w, fnt, 0);
        lv_obj_set_style_text_color(*rows[i].w, col, 0);
        lv_obj_align(*rows[i].w, LV_ALIGN_TOP_LEFT, X_MARGIN, ROW0_Y + i * ROW_STEP);
    }

    ESP_LOGI(TAG, "%s tile init OK", bq25619_get_chip_name());
}

void battery_tile_update(void)
{
    broker_battery_data_t d = {0};
    broker_battery_read(&d);
    sensor_status_t st = broker_battery_get_status();
    update_led(st);

    if (d.last_update_ms == 0) {
        lv_label_set_text(s_lbl_vbat,  "Vbat:   --.-- V");
        lv_label_set_text(s_lbl_soc,   "SoC:    --- %");
        lv_label_set_text(s_lbl_chrg,  "State:  ---");
        lv_label_set_text(s_lbl_pg,    "Power:  ---");
        lv_label_set_text(s_lbl_fault, "Fault:  0x--");
        lv_label_set_text(s_lbl_boost, "Boost:  ---");
        return;
    }

    lv_label_set_text_fmt(s_lbl_vbat,  "Vbat:   %.2f V", (double)d.voltage);
    lv_label_set_text_fmt(s_lbl_soc,   "SoC:    %u %%", (unsigned)d.percentage);

    const char *cs = (d.charge_state < 4) ? k_chrg[d.charge_state] : "???";
    lv_label_set_text_fmt(s_lbl_chrg,  "State:  %s", cs);

    lv_label_set_text_fmt(s_lbl_pg,    "Power:  %s",
                          d.power_good ? "USB OK" : "Battery only");

    lv_label_set_text_fmt(s_lbl_fault, "Fault:  0x%02X", (unsigned)d.fault);

    lv_label_set_text_fmt(s_lbl_boost, "Boost:  %s",
                          d.boost_enabled ? "ON (5V PMID)" : "OFF");
}

void battery_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent,  theme_bg(),      0);
    lv_obj_set_style_bg_color(s_divider, theme_divider(), 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_vbat,   theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_soc,    theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_chrg,   theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_pg,     theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_fault,  theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_boost,  theme_subtext(), 0);
}

const tile_desc_t battery_tile_desc = {
    .init           = battery_tile_init,
    .update         = battery_tile_update,
    .apply_theme    = battery_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};
