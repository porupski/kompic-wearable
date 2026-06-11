/**
 * @file gps_tile.h
 * @brief GPS (MAX-M10S) settings tile — public API for Core 1 / LVGL.
 *
 * Tile type : A  (status + data rows) + sub-tile (raw NMEA debug overlay)
 * Tileview  : TILE_IDX_GPS (index 2) in the settings screen horizontal tileview.
 *
 * Layout (Blueprint 7 §6):
 *   Header   : status LED + "MAX-M10S  u-blox M10 GNSS" + power toggle switch
 *   Data rows: Sats, HDOP, Time (UTC), Date, LAT, LON, ALT, SPD
 *   Button   : [ATOMIC SYNC] — enabled only when time_valid == true
 *
 * Sub-tile (swipe UP from GPS tile):
 *   Raw NMEA debug — $GPGGA and $GPRMC sentences live.
 *
 * Core 1 only — no I2C, no UART, no NVS includes.
 * Data flows: broker_gps_read() → widgets.
 *
 * Architecture: Blueprint 3 §5, Blueprint 5 §4, Blueprint 7 §6
 */

#ifndef GPS_TILE_H
#define GPS_TILE_H

#include "lvgl.h"
#include "ui_theme_colors.h"
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Main tile
// ---------------------------------------------------------------------------

/**
 * @brief Build all GPS tile widgets as children of `parent`.
 *        Called once from lvgl_ui.c when constructing the settings tileview.
 *        `parent` is the lv_obj_t* of the GPS tile slot.
 */
void gps_tile_init(lv_obj_t *parent);

/**
 * @brief Refresh all GPS tile widgets from broker data.
 *        Called by task_ui_refresh_fn() when the GPS tile is active.
 *        Must be called inside lvgl_port_lock() / lvgl_port_unlock().
 */
void gps_tile_update(void);

/**
 * @brief Reapply theme colours to all GPS tile widgets.
 *        Called by apply_ui_theme() fan-out in lvgl_ui.c.
 */
void gps_tile_apply_theme(ui_theme_t theme);

// ---------------------------------------------------------------------------
// Sub-tile (debug overlay — raw NMEA sentences)
// ---------------------------------------------------------------------------

/**
 * @brief Build the NMEA debug sub-tile widgets as children of `parent`.
 *        `parent` is the lv_obj_t* of the sub-tile slot (row 1 in tileview).
 */
void gps_subtile_init(lv_obj_t *parent);

/**
 * @brief Refresh raw NMEA sentences in the debug sub-tile.
 *        Called by task_ui_refresh_fn() when the GPS sub-tile is active.
 */
void gps_subtile_update(void);

// ---------------------------------------------------------------------------
// Atomic sync result feedback
// ---------------------------------------------------------------------------

/**
 * @brief Show sync result on the ATOMIC SYNC button for ~2 seconds.
 *        Call this from the sync callback after pcf85063_sync_from_gps()
 *        returns. Must be called inside lvgl_port_lock().
 *
 * @param success  true → button label = "SYNC OK ✓", false → "SYNC FAIL ✗"
 */
void gps_tile_show_sync_result(bool success);

#include "tile_registry.h"

/**
 * @brief GPS tile descriptor for tile_registry.c.
 *        Declares main tile + sub-tile (NMEA debug view).
 */
extern const tile_desc_t gps_tile_desc;

#endif // GPS_TILE_H
