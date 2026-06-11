/**
 * @file gps_tile.c
 * @brief GPS (TU10F) settings tile — LVGL 9 UI, Core 1 only.
 *
 * Tile type   : A (status + data rows) + sub-tile (raw NMEA debug)
 * Layout      : Header, divider, 5 data rows, ATOMIC SYNC button.
 * Sub-tile    : Raw $GPGGA / $GPRMC sentences, refreshed live.
 *
 * Key patterns used:
 *   - s_syncing guard on power toggle (prevents re-entrant LV_EVENT_VALUE_CHANGED)
 *   - ATOMIC SYNC button: enabled only when gps.time_valid == true
 *   - Sync feedback: button label changes for SYNC_FEEDBACK_MS then reverts
 *   - first_fix_notified flag: consumed here (cleared via broker write) to
 *     prevent repeat NOTIF state after the tile acks it
 *
 * Phase 14 fix:
 *   - Bug 2: On fix loss, lat/lon/alt now hold last known broker values and
 *     display "(last known)" suffix during SENSOR_STALE window instead of
 *     snapping to "---" immediately.
 *
 * Core 1 only. No I2C/UART/NVS calls. No pcf85063 include here.
 * The ATOMIC SYNC action is handled entirely on Core 0 via a posted flag;
 * this tile only sets g_gps_sync_requested = true and reads the result.
 *
 * Architecture: Blueprint 3 §6, Blueprint 5 §4–§6, Blueprint 7 §6
 */

#include "gps_tile.h"
#include "max_m10s.h"      // broker_gps_data_t, max_m10s_get_chip_name/desc,
                           // max_m10s_get_debug_sentences, gps_fix_type_t
#include "data_broker.h"   // broker_gps_read/get_status/set_enabled/get_enabled
#include "ui_theme_colors.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"     // esp_timer_get_time() for sync feedback timing

static const char *TAG = "GPS_TILE";

// ---------------------------------------------------------------------------
// Sync feedback timing
// ---------------------------------------------------------------------------
#define SYNC_FEEDBACK_MS  2000U   // How long to show SYNC OK / FAIL on button

// ---------------------------------------------------------------------------
// Atomic flag posted by this tile, consumed by Core 0 task (boot_hw_init.c).
// Declared extern here; defined in boot_hw_init.c (or main.c).
// Core 0 clears it after attempting the sync and calls gps_tile_show_sync_result().
// ---------------------------------------------------------------------------
extern volatile bool g_gps_sync_requested;

// ---------------------------------------------------------------------------
// Static widget handles — tile owns these, nothing else touches them
// ---------------------------------------------------------------------------

// Shared parent reference (needed for apply_theme bg restyle)
static lv_obj_t *s_parent     = NULL;

// Header row
static lv_obj_t *s_led_status = NULL;
static lv_obj_t *s_lbl_header = NULL;
static lv_obj_t *s_sw_power   = NULL;  // power toggle (Blueprint 7 §6)

// Divider (stored so apply_theme can restyle it)
static lv_obj_t *s_divider    = NULL;

// Data rows — each is a single label "Field:  value" on one line
static lv_obj_t *s_lbl_sats   = NULL;  // "Sats:  8      HDOP: 1.2"
static lv_obj_t *s_lbl_time   = NULL;  // "Time:  14:32:07"
static lv_obj_t *s_lbl_date   = NULL;  // "Date:  2026/02/14"
static lv_obj_t *s_lbl_lat    = NULL;  // "LAT:   46.0511° N"
static lv_obj_t *s_lbl_lon    = NULL;  // "LON:   14.5051° E"
static lv_obj_t *s_lbl_alt    = NULL;  // "ALT:   302 m   SPD: 0.0 km/h"

// ATOMIC SYNC button + its label (label stored separately for text swap)
static lv_obj_t *s_btn_sync       = NULL;
static lv_obj_t *s_lbl_btn_sync   = NULL;

// Sync feedback state
static bool     s_showing_feedback  = false;
static int64_t  s_feedback_start_us = 0;

// Power toggle re-entrancy guard (Blueprint 7 §6)
static bool s_syncing = false;

// ---------------------------------------------------------------------------
// Sub-tile widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_sub_parent   = NULL;
static lv_obj_t *s_lbl_gga      = NULL;
static lv_obj_t *s_lbl_rmc      = NULL;
static lv_obj_t *s_lbl_sub_hdr  = NULL;

// ---------------------------------------------------------------------------
// Helper: map sensor_status_t → LED colour and set it
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
        case SENSOR_DISABLED:  /* fall-through */
        default:               col = COL_STATUS_DISABLED;  break;
    }
    lv_led_set_color(s_led_status, col);
}

// ---------------------------------------------------------------------------
// Callback: power toggle switch
// ---------------------------------------------------------------------------
static void cb_power_toggle(lv_event_t *e)
{
    if (s_syncing) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool new_val = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    broker_gps_set_enabled(new_val);
    // Core 0 task_gps_fn() reads broker_gps_get_enabled() every cycle —
    // no further signalling needed here.
}

// ---------------------------------------------------------------------------
// Callback: ATOMIC SYNC button
// ---------------------------------------------------------------------------
static void cb_sync_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    // Disable button immediately to prevent double-tap
    lv_obj_add_state(s_btn_sync, LV_STATE_DISABLED);
    // Post the request — Core 0 will pick this up in its next cycle,
    // call pcf85063_sync_from_gps(), then call gps_tile_show_sync_result().
    g_gps_sync_requested = true;
    ESP_LOGI(TAG, "Atomic sync requested");
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

    // ── Status LED (top-left) ──────────────────────────────────────────────
    s_led_status = lv_led_create(parent);
    lv_obj_set_size(s_led_status, 12, 12);
    lv_obj_align(s_led_status, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_led_set_brightness(s_led_status, 200);
    lv_led_set_color(s_led_status, COL_STATUS_DISABLED);

    // ── Chip header label ─────────────────────────────────────────────────
    s_lbl_header = lv_label_create(parent);
    lv_label_set_text_fmt(s_lbl_header, "%s  %s",
        max_m10s_get_chip_name(), max_m10s_get_chip_desc());
    lv_obj_set_style_text_font(s_lbl_header, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(), 0);
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_LEFT, 30, 7);

    // ── Power toggle switch (top-right) ───────────────────────────────────
    s_sw_power = lv_switch_create(parent);
    lv_obj_set_size(s_sw_power, 46, 22);
    lv_obj_align(s_sw_power, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_add_event_cb(s_sw_power, cb_power_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    // ── Divider ───────────────────────────────────────────────────────────
    s_divider = lv_obj_create(parent);
    lv_obj_set_size(s_divider, 216, 1);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(s_divider, theme_divider(), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    // ── Data rows (y starts at 44, steps of 22 px) ────────────────────────
    const lv_font_t *fnt = UI_FONT_LABEL;
    lv_color_t       col = theme_subtext();

    s_lbl_sats = lv_label_create(parent);
    lv_label_set_text(s_lbl_sats, "Sats:  ---   HDOP: ---");
    lv_obj_set_style_text_font(s_lbl_sats, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_sats, col, 0);
    lv_obj_align(s_lbl_sats, LV_ALIGN_TOP_LEFT, 12, 44);

    s_lbl_time = lv_label_create(parent);
    lv_label_set_text(s_lbl_time, "Time:  ---");
    lv_obj_set_style_text_font(s_lbl_time, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_time, col, 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_LEFT, 12, 66);

    s_lbl_date = lv_label_create(parent);
    lv_label_set_text(s_lbl_date, "Date:  ---");
    lv_obj_set_style_text_font(s_lbl_date, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_date, col, 0);
    lv_obj_align(s_lbl_date, LV_ALIGN_TOP_LEFT, 12, 88);

    s_lbl_lat = lv_label_create(parent);
    lv_label_set_text(s_lbl_lat, "LAT:   ---");
    lv_obj_set_style_text_font(s_lbl_lat, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_lat, col, 0);
    lv_obj_align(s_lbl_lat, LV_ALIGN_TOP_LEFT, 12, 110);

    s_lbl_lon = lv_label_create(parent);
    lv_label_set_text(s_lbl_lon, "LON:   ---");
    lv_obj_set_style_text_font(s_lbl_lon, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_lon, col, 0);
    lv_obj_align(s_lbl_lon, LV_ALIGN_TOP_LEFT, 12, 132);

    s_lbl_alt = lv_label_create(parent);
    lv_label_set_text(s_lbl_alt, "ALT:   ---   SPD: ---");
    lv_obj_set_style_text_font(s_lbl_alt, fnt, 0);
    lv_obj_set_style_text_color(s_lbl_alt, col, 0);
    lv_obj_align(s_lbl_alt, LV_ALIGN_TOP_LEFT, 12, 154);

    // ── ATOMIC SYNC button (bottom, full-width) ────────────────────────────
    s_btn_sync = lv_btn_create(parent);
    lv_obj_set_size(s_btn_sync, 200, 34);
    lv_obj_align(s_btn_sync, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_btn_sync, COL_ACCENT, 0);
    lv_obj_add_state(s_btn_sync, LV_STATE_DISABLED);  // disabled until time_valid

    s_lbl_btn_sync = lv_label_create(s_btn_sync);
    lv_label_set_text(s_lbl_btn_sync, "ATOMIC SYNC");
    lv_obj_set_style_text_font(s_lbl_btn_sync, UI_FONT_LABEL, 0);
    lv_obj_center(s_lbl_btn_sync);

    lv_obj_add_event_cb(s_btn_sync, cb_sync_btn, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "%s tile init OK", max_m10s_get_chip_name());
}

// ---------------------------------------------------------------------------
// gps_tile_update  — called every 200 ms by task_ui_refresh_fn()
// ---------------------------------------------------------------------------
void gps_tile_update(void)
{
    broker_gps_data_t  d  = {0};
    broker_gps_read(&d);
    sensor_status_t    st = broker_gps_get_status();

    // ── Status LED ──────────────────────────────────────────────────────────
    update_led(st);

    // ── Power toggle sync (guard prevents re-entrancy crash) ────────────────
    s_syncing = true;
    if (broker_gps_get_enabled()) lv_obj_add_state(s_sw_power, LV_STATE_CHECKED);
    else                          lv_obj_clear_state(s_sw_power, LV_STATE_CHECKED);
    s_syncing = false;

    // ── Data validity gates ──────────────────────────────────────────────────
    // pos_valid: live fix — show all fields normally
    bool pos_valid = d.position_valid &&
                     (st == SENSOR_ONLINE || st == SENSOR_NOTIF);

    // Bug 2: stale_pos — fix lost but broker still holds last good coordinates.
    // Show retained values with "(last known)" suffix during SENSOR_STALE window.
    // Guard against (0, 0) default: only show if we actually had a real fix.
    bool stale_pos = !d.position_valid && (st == SENSOR_STALE) &&
                     (d.latitude != 0.0 || d.longitude != 0.0);

    bool time_valid = d.time_valid;

    // ── Sats + HDOP row ──────────────────────────────────────────────────────
    if (pos_valid) {
        const char *fix_str = (d.fix == GPS_FIX_3D) ? "3D" :
                              (d.fix == GPS_FIX_2D) ? "2D" : "--";

        int hdop_w = (int)d.hdop;
        int hdop_d = (int)((d.hdop - hdop_w) * 10);
        if (hdop_d < 0) hdop_d = -hdop_d;

        lv_label_set_text_fmt(s_lbl_sats, "Sats: %u (%s)   HDOP: %d.%d",
                              d.sats_in_use, fix_str, hdop_w, hdop_d);
    } else {
        lv_label_set_text(s_lbl_sats, "Sats:  ---   HDOP: ---");
    }

    // ── UTC time row ─────────────────────────────────────────────────────────
    if (time_valid) {
        lv_label_set_text_fmt(s_lbl_time, "Time:  %02u:%02u:%02u UTC",
                              d.utc_hour, d.utc_minute, d.utc_second);
    } else {
        lv_label_set_text(s_lbl_time, "Time:  ---");
    }

    // ── Date row ─────────────────────────────────────────────────────────────
    if (time_valid) {
        lv_label_set_text_fmt(s_lbl_date, "Date:  %04u/%02u/%02u",
                              d.utc_year, d.utc_month, d.utc_day);
    } else {
        lv_label_set_text(s_lbl_date, "Date:  ---");
    }

    // ── LAT row ──────────────────────────────────────────────────────────────
    if (pos_valid || stale_pos) {
        double lat = d.latitude;
        char   ns  = (lat >= 0.0) ? 'N' : 'S';
        if (lat < 0.0) lat = -lat;
        int lat_w = (int)lat;
        int lat_d = (int)((lat - lat_w) * 10000);
        if (stale_pos) {
            lv_label_set_text_fmt(s_lbl_lat, "LAT:   %d.%04d\xc2\xb0 %c (last known)",
                                  lat_w, lat_d, ns);
        } else {
            lv_label_set_text_fmt(s_lbl_lat, "LAT:   %d.%04d\xc2\xb0 %c",
                                  lat_w, lat_d, ns);
        }
    } else {
        lv_label_set_text(s_lbl_lat, "LAT:   ---");
    }

    // ── LON row ──────────────────────────────────────────────────────────────
    if (pos_valid || stale_pos) {
        double lon = d.longitude;
        char   ew  = (lon >= 0.0) ? 'E' : 'W';
        if (lon < 0.0) lon = -lon;
        int lon_w = (int)lon;
        int lon_d = (int)((lon - lon_w) * 10000);
        if (stale_pos) {
            lv_label_set_text_fmt(s_lbl_lon, "LON:   %d.%04d\xc2\xb0 %c (last known)",
                                  lon_w, lon_d, ew);
        } else {
            lv_label_set_text_fmt(s_lbl_lon, "LON:   %d.%04d\xc2\xb0 %c",
                                  lon_w, lon_d, ew);
        }
    } else {
        lv_label_set_text(s_lbl_lon, "LON:   ---");
    }

    // ── ALT + SPD row ────────────────────────────────────────────────────────
    if (pos_valid || stale_pos) {
        int alt_w = (int)d.altitude_m;
        int spd_w = (int)d.speed_kmh;
        int spd_d = (int)((d.speed_kmh - spd_w) * 10);
        if (spd_d < 0) spd_d = -spd_d;
        if (stale_pos) {
            // Speed meaningless when stale — show alt only
            lv_label_set_text_fmt(s_lbl_alt, "ALT: %d m (last known)", alt_w);
        } else {
            lv_label_set_text_fmt(s_lbl_alt, "ALT: %d m   SPD: %d.%d km/h",
                                  alt_w, spd_w, spd_d);
        }
    } else {
        lv_label_set_text(s_lbl_alt, "ALT:   ---   SPD: ---");
    }

    // ── ATOMIC SYNC button enable/disable ────────────────────────────────────
    // Enabled only when GPS has valid time AND not currently showing feedback.
    if (!s_showing_feedback) {
        if (time_valid && st != SENSOR_DISABLED && st != SENSOR_OFFLINE) {
            lv_obj_clear_state(s_btn_sync, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_btn_sync, LV_STATE_DISABLED);
        }
    }

    // ── Sync feedback timeout ────────────────────────────────────────────────
    if (s_showing_feedback) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_feedback_start_us) / 1000LL;
        if (elapsed_ms >= SYNC_FEEDBACK_MS) {
            lv_label_set_text(s_lbl_btn_sync, "ATOMIC SYNC");
            s_showing_feedback = false;
        }
    }

    // ── Consume first_fix_notified ───────────────────────────────────────────
    // The broker macro sets first_fix_notified on the write that crosses
    // GPS_FIX_NONE → fix. We read it here and clear it so the NOTIF state
    // only lasts one refresh cycle. We can't write to the broker from Core 1,
    // but the driver (Core 0) manages this flag — the NOTIF status is one-shot
    // by design in broker_gps_get_status() custom logic. No action needed here
    // beyond acknowledging it for display purposes. LED already reflects it.
}

// ---------------------------------------------------------------------------
// gps_tile_show_sync_result  — called from Core 0 after sync attempt
// Must be called inside lvgl_port_lock().
// ---------------------------------------------------------------------------
void gps_tile_show_sync_result(bool success)
{
    if (!s_lbl_btn_sync) return;
    if (success) {
        lv_label_set_text(s_lbl_btn_sync, "SYNC OK \xe2\x9c\x93");
    } else {
        lv_label_set_text(s_lbl_btn_sync, "SYNC FAIL \xe2\x9c\x97");
    }
    // Re-enable button so user can retry on failure
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
    (void)theme;  // theme_xxx() helpers read g_ui_theme internally
    if (!s_parent) return;

    lv_obj_set_style_bg_color(s_parent,     theme_bg(),      0);
    lv_obj_set_style_bg_color(s_divider,    theme_divider(), 0);
    lv_obj_set_style_text_color(s_lbl_header, theme_text(),    0);
    lv_obj_set_style_text_color(s_lbl_sats,   theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_time,   theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_date,   theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_lat,    theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_lon,    theme_subtext(), 0);
    lv_obj_set_style_text_color(s_lbl_alt,    theme_subtext(), 0);
    // Button accent colour is constant — no theme dependency.
}

// ---------------------------------------------------------------------------
// gps_subtile_init  — raw NMEA debug overlay
// ---------------------------------------------------------------------------
void gps_subtile_init(lv_obj_t *parent)
{
    s_sub_parent = parent;
    lv_obj_set_style_bg_color(parent, theme_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    s_lbl_sub_hdr = lv_label_create(parent);
    lv_label_set_text(s_lbl_sub_hdr, "RAW NMEA DEBUG");
    lv_obj_set_style_text_font(s_lbl_sub_hdr, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_sub_hdr, theme_text(), 0);
    lv_obj_align(s_lbl_sub_hdr, LV_ALIGN_TOP_MID, 0, 10);

    // GGA sentence label — long_mode wrap so it wraps on screen
    s_lbl_gga = lv_label_create(parent);
    lv_label_set_text(s_lbl_gga, "$GPGGA: ---");
    lv_obj_set_style_text_font(s_lbl_gga, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_gga, theme_subtext(), 0);
    lv_label_set_long_mode(s_lbl_gga, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_gga, 220);
    lv_obj_align(s_lbl_gga, LV_ALIGN_TOP_LEFT, 10, 34);

    // RMC sentence label
    s_lbl_rmc = lv_label_create(parent);
    lv_label_set_text(s_lbl_rmc, "$GPRMC: ---");
    lv_obj_set_style_text_font(s_lbl_rmc, UI_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_lbl_rmc, theme_subtext(), 0);
    lv_label_set_long_mode(s_lbl_rmc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_rmc, 220);
    lv_obj_align(s_lbl_rmc, LV_ALIGN_TOP_LEFT, 10, 120);

    ESP_LOGI(TAG, "GPS sub-tile init OK");
}

// ---------------------------------------------------------------------------
// gps_subtile_update
// ---------------------------------------------------------------------------
void gps_subtile_update(void)
{
    char gga[96] = {0};
    char rmc[96] = {0};
    max_m10s_get_debug_sentences(gga, sizeof(gga), rmc, sizeof(rmc));

    // tu10f_get_debug_sentences() returns empty strings when stale (Bug 3 fix)
    if (gga[0] != '\0') {
        lv_label_set_text(s_lbl_gga, gga);
    } else {
        lv_label_set_text(s_lbl_gga, "-- no signal --");
    }

    if (rmc[0] != '\0') {
        lv_label_set_text(s_lbl_rmc, rmc);
    } else {
        lv_label_set_text(s_lbl_rmc, "-- no signal --");
    }
}

const tile_desc_t gps_tile_desc = {
    .init           = gps_tile_init,
    .update         = gps_tile_update,
    .apply_theme    = gps_tile_apply_theme,
    .has_subtile    = true,
    .subtile_init   = gps_subtile_init,
    .subtile_update = gps_subtile_update,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT | LV_DIR_BOTTOM,
    //                                              ^^^^^^^^^^^^
    // LV_DIR_BOTTOM permits swipe-up to reach the debug sub-tile (row 1).
};