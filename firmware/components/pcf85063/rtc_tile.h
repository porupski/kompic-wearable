/**
 * @file rtc_tile.h
 * @brief PCF85063A RTC settings tile — public API for Core 1 / LVGL.
 *
 * Tile type : A  (status + data rows, read-only — no power toggle)
 * Tileview  : TILE_IDX_RTC (handled by calling code in lvgl_ui.c)
 *
 * Layout (Blueprint 8 §8):
 *   Header   : status LED + "PCF85063A  Battery-backed RTC"  (no power toggle)
 *   Data rows: UTC time, Local time + offset, Date + weekday,
 *              Uptime, GPS sync timestamp
 *
 * RTC is always-on — no power toggle, no DISABLED state.
 * Timezone is display-only: local = (utc_hour + g_tz_offset_hours + 24) % 24.
 * g_tz_offset_hours is an atomic global defined in ui_broker.h / lvgl_ui.c.
 *
 * Core 1 only — no I2C, no UART, no NVS includes.
 * Data flows: broker_rtc_read() → widgets.
 *
 * Architecture: Blueprint 3 §5, Blueprint 5 §4, Blueprint 8 §8
 */

#ifndef RTC_TILE_H
#define RTC_TILE_H

#include "lvgl.h"
#include "ui_theme_colors.h"

/**
 * @brief Build all RTC tile widgets as children of `parent`.
 *        Called once from lvgl_ui.c when constructing the settings tileview.
 */
void rtc_tile_init(lv_obj_t *parent);

/**
 * @brief Refresh all RTC tile widgets from broker data.
 *        Called by task_ui_refresh_fn() when the RTC tile is active.
 *        Must be called inside lvgl_port_lock() / lvgl_port_unlock().
 */
void rtc_tile_update(void);

/**
 * @brief Reapply theme colours to all RTC tile widgets.
 *        Called by apply_ui_theme() fan-out in lvgl_ui.c.
 */
void rtc_tile_apply_theme(ui_theme_t theme);

void rtc_tile_notify_gps_sync(uint8_t utc_hour, uint8_t utc_min, uint8_t utc_sec);

#include "tile_registry.h"
extern const tile_desc_t rtc_tile_desc;

#endif // RTC_TILE_H
