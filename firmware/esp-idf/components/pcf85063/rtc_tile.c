/**
 * @file rtc_tile.c
 * @brief PCF85063A RTC settings tile — LVGL 9 UI, Core 1 only.
 *
 * Tile type   : A (status + data rows, read-only)
 * Layout      : Header (no power toggle), divider, 5 data rows.
 *
 * Display rows:
 *   UTC:    HH:MM:SS
 *   Local:  HH:MM:SS  (UTC±N)
 *   Date:   Www  YYYY-MM-DD
 *   Uptime: Xd HH:MM:SS
 *   Sync:   Never  | HH:MM:SS (time of last successful GPS sync this session)
 *
 * Timezone applied at display time only:
 *   local_hour = (utc_hour + g_tz_offset_hours + 24) % 24
 *   Day wrap is handled correctly (date not adjusted — tile shows UTC date).
 *
 * GPS sync timestamp: stored as a static uint32_t epoch set by
 *   rtc_tile_notify_gps_sync() which is called from gps_tile's sync callback
 *   path. Display "Never" until first sync this session.
 *
 * Status LED:
 *   ONLINE  — rtc.valid == true and data is fresh
 *   OFFLINE — rtc.valid == false (I2C read failed)
 *   STALE   — data age > BROKER_RTC_TIMEOUT_MS
 *   No DISABLED state (RTC cannot be user-disabled).
 *   No ACQUIRING state (RTC either works or it doesn't).
 *
 * Core 1 only. No I2C/UART/NVS calls.
 * Architecture: Blueprint 3 §6, Blueprint 5 §4–§6, Blueprint 8 §8
 *
 * v7.2 panel resize (2026-06-10):
 *   Coordinates rescaled from the old 240x240 ST7789 sizing to the new
 *   CO5300 410x502 AMOLED. Positions are proportional, not round-mask aware:
 *   if any row clips the inscribed circle, lvgl_ui owns the masking and the
 *   tile constants here are the truth. See [PCF85063 porting .md, Open Q3].
 */

#include "rtc_tile.h"
#include "pcf85063.h"       // broker_rtc_data_t, pcf85063_get_chip_name/desc
#include "data_broker.h"    // broker_rtc_read(), broker_rtc_get_status()
#include "ui_theme_colors.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"      // esp_timer_get_time() for uptime

static const char *TAG = "RTC_TILE";

// ---------------------------------------------------------------------------
// Timezone global — defined in lvgl_ui.c (or ui_broker.c), declared extern.
// Applied here at display time only. RTC always stores UTC.
// ---------------------------------------------------------------------------
extern volatile int8_t g_tz_offset_hours;

// ---------------------------------------------------------------------------
// GPS sync session timestamp
// Set to the ESP32 uptime (seconds) when a successful GPS sync occurs.
// Zero means "never synced this session".
// ---------------------------------------------------------------------------
static uint32_t s_last_sync_uptime_s = 0;   // 0 = never
static uint8_t  s_last_sync_hour     = 0;
static uint8_t  s_last_sync_min      = 0;
static uint8_t  s_last_sync_sec      = 0;

// ---------------------------------------------------------------------------
// Weekday name table (0 = Sunday … 6 = Saturday, matches PCF85063)
// ---------------------------------------------------------------------------
static const char * const k_weekday[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

// ---------------------------------------------------------------------------
// Static widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_parent      = NULL;

// Header
static lv_obj_t *s_led_status  = NULL;
static lv_obj_t *s_lbl_header  = NULL;

// Divider
static lv_obj_t *s_divider     = NULL;

// Data rows
static lv_obj_t *s_lbl_utc     = NULL;  // "UTC:    HH:MM:SS"
static lv_obj_t *s_lbl_local   = NULL;  // "Local:  HH:MM:SS  (UTC±N)"
static lv_obj_t *s_lbl_date    = NULL;  // "Date:   Www  YYYY-MM-DD"
static lv_obj_t *s_lbl_uptime  = NULL;  // "Uptime: Xd HH:MM:SS"
static lv_obj_t *s_lbl_sync    = NULL;  // "GPS sync: Never" | "HH:MM:SS"

// ---------------------------------------------------------------------------
// Helper: map sensor_status_t → LED colour
// ---------------------------------------------------------------------------
static void update_led(sensor_status_t st)
{
    lv_color_t col;
    switch (st) {
        case SENSOR_ONLINE:  col = COL_STATUS_ONLINE;  break;
        case SENSOR_OFFLINE: col = COL_STATUS_OFFLINE; break;
        case SENSOR_STALE:   col = COL_STATUS_STALE;   break;
        default:             col = COL_STATUS_OFFLINE; break;
        // DISABLED / ACQUIRING never used for RTC
    }
    lv_led_set_color(s_led_status, col);
}

// ---------------------------------------------------------------------------
// rtc_tile_init
// ---------------------------------------------------------------------------
void rtc_tile_init(lv_obj_t *parent)
{
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // -- Layout constants for 410x502 CO5300 (v7.2) -----------------------------
    // Old 240x240 values shown in /* */ for reference.
    static const int X_MARGIN     = 20;   /* was 12 */
    static const int LED_SIZE     = 16;   /* was 12 */
    static const int LED_X        = 20;   /* was 12 */
    static const int LED_Y        = 20;   /* was 12 */
    static const int HEADER_X     = 44;   /* was 30 */
    static const int HEADER_Y     = 18;   /* was  7 */
    static const int DIVIDER_W    = 360;  /* was 216 */
    static const int DIVIDER_Y    = 50;   /* was 34 */
    static const int ROW0_Y       = 64;   /* was 44 */
    static const int ROW_STEP     = 32;   /* was 22 */

    // ── Status LED (top-left) ──────────────────────────────────────────────
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, LED_SIZE, LED_SIZE);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, LED_X, LED_Y);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_OFFLINE);

    // ── Chip header label — no power toggle (RTC is always-on) ───────────
    s_lbl_header = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_header, "%s  %s",
        pcf85063_get_chip_name(), pcf85063_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, HEADER_X, HEADER_Y);
    // Note: no switch widget here — RTC has no enable/disable.

    // ── Divider ───────────────────────────────────────────────────────────
    s_divider = lv_obj_create(parent);
    lv_obj_set_size(s_divider, DIVIDER_W, 1);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, DIVIDER_Y);
    lv_obj_set_style_bg_color(s_divider, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    // ── Data rows (5 rows, step ROW_STEP px) ──────────────────────────────
    const lv_font_t *fnt = UI_FONT_LABEL;
    lv_color_t       col = theme_subtext();

    s_lbl_utc = lv_label_create(parent);
    lv_label_set_text(s_lbl_utc, "UTC:    --:--:--");
    lv_obj_set_style_text_font(s_lbl_utc, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_utc, col, 0);
    lv_obj_align(s_lbl_utc, LV_ALIGN_TOP_LEFT, X_MARGIN, ROW0_Y + 0 * ROW_STEP);

    s_lbl_local = lv_label_create(parent);
    lv_label_set_text(s_lbl_local, "Local:  --:--:--  (UTC+0)");
    lv_obj_set_style_text_font(s_lbl_local, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_local, col, 0);
    lv_obj_align(s_lbl_local, LV_ALIGN_TOP_LEFT, X_MARGIN, ROW0_Y + 1 * ROW_STEP);

    s_lbl_date = lv_label_create(parent);
    lv_label_set_text(s_lbl_date, "Date:   ---  ----/--/--");
    lv_obj_set_style_text_font(s_lbl_date, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_date, col, 0);
    lv_obj_align(s_lbl_date, LV_ALIGN_TOP_LEFT, X_MARGIN, ROW0_Y + 2 * ROW_STEP);

    s_lbl_uptime = lv_label_create(parent);
    lv_label_set_text(s_lbl_uptime, "Uptime: 0d 00:00:00");
    lv_obj_set_style_text_font(s_lbl_uptime, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_uptime, col, 0);
    lv_obj_align(s_lbl_uptime, LV_ALIGN_TOP_LEFT, X_MARGIN, ROW0_Y + 3 * ROW_STEP);

    s_lbl_sync = lv_label_create(parent);
    lv_label_set_text(s_lbl_sync, "GPS sync: Never");
    lv_obj_set_style_text_font(s_lbl_sync, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_sync, col, 0);
    lv_obj_align(s_lbl_sync, LV_ALIGN_TOP_LEFT, X_MARGIN, ROW0_Y + 4 * ROW_STEP);

    ESP_LOGI(TAG, "%s tile init OK", pcf85063_get_chip_name());
}

// ---------------------------------------------------------------------------
// rtc_tile_update  — called every 200 ms by task_ui_refresh_fn()
// ---------------------------------------------------------------------------
void rtc_tile_update(void)
{
    broker_rtc_data_t d  = {0};
    broker_rtc_read(&d);
    sensor_status_t   st = broker_rtc_get_status();

    // ── Status LED ──────────────────────────────────────────────────────────
    update_led(st);

    // ── UTC time row ─────────────────────────────────────────────────────────
    if (d.valid) {
        lv_label_set_text_fmt(s_lbl_utc, "UTC:    %02u:%02u:%02u",
                              d.hour, d.minute, d.second);
    } else {
        lv_label_set_text(s_lbl_utc, "UTC:    --:--:--");
    }

    // ── Local time row ───────────────────────────────────────────────────────
    // Apply timezone offset (display only — RTC stores UTC).
    // Clamp to valid range with (+24) % 24 trick.
    if (d.valid) {
        int tz = (int)g_tz_offset_hours;
        int local_h = ((int)d.hour + tz + 24) % 24;
        // Format timezone sign explicitly
        char tz_str[16];
        if (tz >= 0) {
            snprintf(tz_str, sizeof(tz_str), "(UTC+%d)", tz);
        } else {
            snprintf(tz_str, sizeof(tz_str), "(UTC%d)",  tz);  // minus already in tz
        }
        lv_label_set_text_fmt(s_lbl_local, "Local:  %02d:%02u:%02u  %s",
                              local_h, d.minute, d.second, tz_str);
    } else {
        lv_label_set_text(s_lbl_local, "Local:  --:--:--");
    }

    // ── Date + weekday row ───────────────────────────────────────────────────
    if (d.valid) {
        const char *wd = (d.weekday <= 6) ? k_weekday[d.weekday] : "???";
        lv_label_set_text_fmt(s_lbl_date, "Date:   %s  %04u-%02u-%02u",
                              wd, d.year, d.month, d.day);
    } else {
        lv_label_set_text(s_lbl_date, "Date:   ---  ----/--/--");
    }

    // ── Uptime row ───────────────────────────────────────────────────────────
    // esp_timer_get_time() returns microseconds since boot.
    uint32_t up_s   = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t days   =  up_s / 86400U;
    uint32_t rem    =  up_s % 86400U;
    uint32_t hours  =  rem  / 3600U;
    uint32_t mins   = (rem  % 3600U) / 60U;
    uint32_t secs   =  rem  % 60U;
    lv_label_set_text_fmt(s_lbl_uptime, "Uptime: %ud %02u:%02u:%02u",
                          (unsigned)days, (unsigned)hours,
                          (unsigned)mins, (unsigned)secs);

    // ── GPS sync row ─────────────────────────────────────────────────────────
    if (s_last_sync_uptime_s == 0) {
        lv_label_set_text(s_lbl_sync, "GPS sync: Never");
    } else {
        lv_label_set_text_fmt(s_lbl_sync, "GPS sync: %02u:%02u:%02u",
                              s_last_sync_hour, s_last_sync_min, s_last_sync_sec);
    }
}

// ---------------------------------------------------------------------------
// rtc_tile_notify_gps_sync
// Called from the GPS sync success path (Core 0, inside lvgl_port_lock).
// Records the UTC time at which the sync occurred so the tile can display it.
// ---------------------------------------------------------------------------
void rtc_tile_notify_gps_sync(uint8_t utc_hour, uint8_t utc_min, uint8_t utc_sec)
{
    s_last_sync_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_last_sync_hour     = utc_hour;
    s_last_sync_min      = utc_min;
    s_last_sync_sec      = utc_sec;
}

// ---------------------------------------------------------------------------
// rtc_tile_apply_theme
// ---------------------------------------------------------------------------
void rtc_tile_apply_theme(ui_theme_t theme)
{
    (void)theme;  // theme_xxx() helpers read g_ui_theme internally
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent,      theme_bg(),      0);
    lv_obj_set_style_bg_color(s_divider,     theme_divider(), 0);
    lv_obj_set_style_text_color(s_lbl_header,  theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_utc,     theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_local,   theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_date,    theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_uptime,  theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_sync,    theme_subtext(), 0);
}

const tile_desc_t rtc_tile_desc = {
    .init           = rtc_tile_init,
    .update         = rtc_tile_update,
    .apply_theme    = rtc_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};