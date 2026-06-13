/**
 * @file system_tile.c
 * @brief System info settings tile — LVGL 9 UI, Core 1 only.
 *
 * Tile type   : info-only (no power switch, no control buttons).
 * Component   : components/lvgl_ui/ — settings-side tile, no dedicated driver.
 *
 * Layout:
 *   System
 *   ┌──────────────────────────────────────┐
 *   │ ⊙  ESP32 Clock    14:32:07          │  ← NEW (Phase 15)
 *   │ ⚙  RTC Clock      14:32:07          │
 *   │ ↺  Uptime         0d 00:04:12       │
 *   │ ⚡  Battery        3.87V (72%)       │
 *   │ ≡  CPU            S3 @ 240 MHz      │
 *   │ ⌂  Int. Temp      38°C              │
 *   │ ▣  Flash          16 MB             │
 *   └──────────────────────────────────────┘
 *
 * Phase 15 changes:
 *   - Added "ESP32 Clock" row as first data row. Reads ESP32 system clock
 *     via gettimeofday() — shows the software clock seeded from RTC at boot
 *     and potentially re-seeded from GPS. Displayed as UTC HH:MM:SS.
 *   - MAX_VAL_LABELS bumped from 6 to 7.
 *
 * Core 1 only. No I2C. No NVS writes.
 * Architecture: Blueprint 3 §5, Blueprint 5 §4
 */

#include "system_tile.h"
#include "data_broker.h"
#include "ui_theme_colors.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "driver/temperature_sensor.h"
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "SYS_TILE";

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_parent        = NULL;
static lv_obj_t *s_lbl_title     = NULL;
static lv_obj_t *s_list          = NULL;

// Value labels (right-aligned inside each list row)
static lv_obj_t *s_val_esp_clk   = NULL;   // NEW — ESP32 system clock
static lv_obj_t *s_val_rtc       = NULL;
static lv_obj_t *s_val_uptime    = NULL;
static lv_obj_t *s_val_battery   = NULL;
static lv_obj_t *s_val_cpu       = NULL;
static lv_obj_t *s_val_temp      = NULL;
static lv_obj_t *s_val_flash     = NULL;

// All value labels in one array for fast theme recolouring
#define MAX_VAL_LABELS 7
static lv_obj_t *s_val_labels[MAX_VAL_LABELS];
static uint8_t   s_val_count = 0;

// Internal temperature sensor handle
static temperature_sensor_handle_t s_tsens = NULL;

// lv_timer handle (1 s slow update)
static lv_timer_t *s_timer_1s = NULL;

// ---------------------------------------------------------------------------
// add_row helper
// ---------------------------------------------------------------------------
static lv_obj_t *add_row(lv_obj_t *list, const char *symbol, const char *key)
{
    lv_obj_t *row = lv_list_add_btn(list, symbol, key);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row, 0, 0);

    lv_obj_t *key_lbl = lv_obj_get_child(row, 1);
    if (key_lbl) {
        lv_obj_set_style_text_font(key_lbl, UI_FONT_LABEL, 0);
        lv_obj_set_style_text_color(key_lbl, theme_subtext(), 0);
    }

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "---");
    lv_obj_set_style_text_font(val, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(val, theme_text(), 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -6, 0);

    if (s_val_count < MAX_VAL_LABELS)
        s_val_labels[s_val_count++] = val;

    return val;
}

// ---------------------------------------------------------------------------
// 1-second lv_timer callback: ESP32 clock, uptime, CPU freq, internal temp
// ---------------------------------------------------------------------------
static void cb_1s_timer(lv_timer_t *t)
{
    (void)t;

    // ESP32 system clock (UTC)
    if (s_val_esp_clk) {
        struct timeval tv = {0};
        gettimeofday(&tv, NULL);
        struct tm utc = {0};
        gmtime_r(&tv.tv_sec, &utc);
        lv_label_set_text_fmt(s_val_esp_clk, "%02d:%02d:%02d",
                              utc.tm_hour, utc.tm_min, utc.tm_sec);
    }

    // Uptime
    if (s_val_uptime) {
        uint64_t total_s = esp_timer_get_time() / 1000000ULL;
        uint32_t days    = (uint32_t)(total_s / 86400UL);
        uint32_t hh      = (uint32_t)((total_s % 86400UL) / 3600UL);
        uint32_t mm      = (uint32_t)((total_s % 3600UL)  / 60UL);
        uint32_t ss      = (uint32_t)(total_s % 60UL);
        lv_label_set_text_fmt(s_val_uptime, "%" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
                              days, hh, mm, ss);
    }

    // CPU frequency
    if (s_val_cpu) {
        uint32_t mhz = (uint32_t)(esp_clk_cpu_freq() / 1000000);
        lv_label_set_text_fmt(s_val_cpu, "S3 @ %" PRIu32 " MHz", mhz);
    }

    // Internal temperature
    if (s_val_temp && s_tsens) {
        float t_c = 0.0f;
        if (temperature_sensor_get_celsius(s_tsens, &t_c) == ESP_OK) {
            lv_label_set_text_fmt(s_val_temp, "%d \xc2\xb0""C", (int)t_c);
        }
    }
}

// ---------------------------------------------------------------------------
// system_tile_init
// ---------------------------------------------------------------------------
void system_tile_init(lv_obj_t *parent)
{
    s_parent    = parent;
    s_val_count = 0;

    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ── Title ──────────────────────────────────────────────────────────────
    s_lbl_title = lv_label_create(parent);
    lv_label_set_text(s_lbl_title, "System");
    lv_obj_set_style_text_font(s_lbl_title, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_title, theme_text(), 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_LEFT, 12, 8);

    // ── List ───────────────────────────────────────────────────────────────
    s_list = lv_list_create(parent);
    lv_obj_set_size(s_list, LV_PCT(96), 220);  // slightly taller for 7 rows
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(s_list, theme_row_bg(), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 6, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 0, 0);

    // ── Rows ───────────────────────────────────────────────────────────────
    s_val_esp_clk = add_row(s_list, LV_SYMBOL_EYE_OPEN, "ESP32 Clock");  // NEW
    s_val_rtc     = add_row(s_list, LV_SYMBOL_SETTINGS, "RTC Clock");
    s_val_uptime  = add_row(s_list, LV_SYMBOL_LOOP,     "Uptime");
    s_val_battery = add_row(s_list, LV_SYMBOL_CHARGE,   "Battery");
    s_val_cpu     = add_row(s_list, LV_SYMBOL_LIST,     "CPU");
    s_val_temp    = add_row(s_list, LV_SYMBOL_HOME,     "Int. Temp");
    s_val_flash   = add_row(s_list, LV_SYMBOL_SD_CARD,  "Flash");

    // ── Flash size (static, read once) ────────────────────────────────────
    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK && flash_bytes > 0) {
        lv_label_set_text_fmt(s_val_flash, "%" PRIu32 " MB",
                              flash_bytes / (1024UL * 1024UL));
    } else {
        lv_label_set_text(s_val_flash, "N/A");
    }

    // ── Internal temperature sensor ───────────────────────────────────────
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    esp_err_t terr = temperature_sensor_install(&tcfg, &s_tsens);
    if (terr == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        ESP_LOGW(TAG, "Internal temp sensor init failed: %s", esp_err_to_name(terr));
        s_tsens = NULL;
        lv_label_set_text(s_val_temp, "N/A");
    }

    // ── 1-second timer (esp32 clock + uptime + CPU + temp) ────────────────
    s_timer_1s = lv_timer_create(cb_1s_timer, 1000, NULL);
    cb_1s_timer(NULL);   // populate immediately on first frame

    ESP_LOGI(TAG, "System tile init OK");
}

// ---------------------------------------------------------------------------
// system_tile_update — called every 200 ms by task_ui_refresh_fn()
// ---------------------------------------------------------------------------
void system_tile_update(void)
{
    if (!s_val_rtc) return;

    // ── RTC time ───────────────────────────────────────────────────────────
    broker_rtc_data_t rtc = {0};
    broker_rtc_read(&rtc);

    if (rtc.valid) {
        lv_label_set_text_fmt(s_val_rtc, "%02u:%02u:%02u",
                              rtc.hour, rtc.minute, rtc.second);
    } else {
        lv_label_set_text(s_val_rtc, "--:--:--");
    }

    // ── Battery ───────────────────────────────────────────────────────────
    broker_battery_data_t bat = {0};
    broker_battery_read(&bat);

    if (s_val_battery) {
        char vbuf[10];
        snprintf(vbuf, sizeof(vbuf), "%.2f", (double)bat.voltage);
        if (bat.charging) {
            lv_label_set_text_fmt(s_val_battery, "%sV (%u%%) \xe2\x9a\xa1",
                                  vbuf, (unsigned)bat.percentage);
        } else {
            lv_label_set_text_fmt(s_val_battery, "%sV (%u%%)",
                                  vbuf, (unsigned)bat.percentage);
        }
    }
}

// ---------------------------------------------------------------------------
// system_tile_apply_theme
// ---------------------------------------------------------------------------
void system_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent, theme_bg(),     0);
    lv_obj_set_style_bg_color(s_list,   theme_row_bg(), 0);

    if (s_lbl_title)
        lv_obj_set_style_text_color(s_lbl_title, theme_text(), 0);

    uint32_t row_cnt = lv_obj_get_child_count(s_list);
    for (uint32_t i = 0; i < row_cnt; i++) {
        lv_obj_t *row = lv_obj_get_child(s_list, (int32_t)i);
        if (!row) continue;
        lv_obj_set_style_bg_color(row, theme_row_bg(), 0);
        lv_obj_t *key_lbl = lv_obj_get_child(row, 1);
        if (key_lbl) lv_obj_set_style_text_color(key_lbl, theme_subtext(), 0);
    }

    for (uint8_t i = 0; i < s_val_count; i++) {
        if (s_val_labels[i])
            lv_obj_set_style_text_color(s_val_labels[i], theme_text(), 0);
    }
}

const tile_desc_t system_tile_desc = {
    .init           = system_tile_init,
    .update         = system_tile_update,
    .apply_theme    = system_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};