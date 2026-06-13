/**
 * @file ui_settings_screen.c
 * @brief Settings screen — horizontal tileview driven by tile_registry.
 *
 * Implementation notes:
 *
 *   REGISTRY-DRIVEN BUILD:
 *     settings_screen_build() iterates tile_registry_get() and calls
 *     lv_tileview_add_tile() for each descriptor.  No module name, no module
 *     header, no per-tile conditional logic appears in this file.  The build
 *     loop is identical regardless of how many tiles are registered.
 *
 *   SUB-TILES:
 *     Descriptors with has_subtile=true get a row 1 tile added in the same
 *     column.  Sub-tile direction is always LV_DIR_TOP only — the only legal
 *     action from a sub-tile is to swipe down back to row 0 (native tileview
 *     scroll).  The main tile's main_dirs must include LV_DIR_BOTTOM to permit
 *     the swipe-up into the sub-tile.  Both are declared in the tile descriptor.
 *
 *   DEFAULT START TILE:
 *     After the build loop, lv_obj_set_tile_id() jumps to DEFAULT_TILE_COL=1
 *     (System tile) without animation.  Light (col 0) is left of System and
 *     is reachable by swiping left.  If the registry has fewer tiles than
 *     DEFAULT_TILE_COL, we fall back to col 0 to avoid an LVGL assert.
 *
 *   GESTURE:
 *     This file sets NO gesture callbacks and NO LV_OBJ_FLAG_GESTURE_BUBBLE.
 *     That is entirely the job of ui_navigation_register_settings(), called
 *     after build from lvgl_ui_init().
 *
 *   THEME:
 *     settings_screen_apply_theme() updates only the screen and tileview
 *     container backgrounds.  Per-tile theme fan-out is done by the
 *     apply_ui_theme() loop in lvgl_ui.c via tile_registry.
 *
 *   ACTIVE TILE DISPATCH:
 *     settings_screen_update_active() is O(N) over tile count.  The first
 *     matching handle triggers the update call and breaks — exactly one tile
 *     update per 200 ms cycle.
 *
 * Core 1 only.  No I2C.  No NVS.  No broker writes.
 * All functions must be called inside lvgl_port_lock() / lvgl_port_unlock().
 */

#include "ui_settings_screen.h"
#include "tile_registry.h"
#include "ui_theme_colors.h"
#include "data_broker.h"
#include "boot_display.h"      // LCD_H_RES, LCD_V_RES
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "SETTINGS_SCR";

// Column index of the tile to show at startup (0-based, matches registry order).
// Registry order: [Light(0), System(1), GPS(2), RTC(3), Compass(4), ...]
#define DEFAULT_TILE_COL  1

// ---------------------------------------------------------------------------
// Module-static handles
// ---------------------------------------------------------------------------

static lv_obj_t *s_screen   = NULL;
static lv_obj_t *s_tileview = NULL;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

lv_obj_t *settings_screen_build(void)
{
    // ── Screen ────────────────────────────────────────────────────────────
    s_screen = lv_obj_create(NULL);
    if (!s_screen) {
        ESP_LOGE(TAG, "Failed to create settings screen");
        return NULL;
    }

    lv_obj_set_style_bg_color(s_screen, theme_bg(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Tileview ──────────────────────────────────────────────────────────
    // Full-screen tileview for horizontal tile navigation.
    // NOTE: LV_OBJ_FLAG_GESTURE_BUBBLE is set by ui_navigation_register_settings()
    // after build — it is NOT set here.  Navigation owns the gesture contract.
    s_tileview = lv_tileview_create(s_screen);
    lv_obj_set_size(s_tileview, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_tileview, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_tileview, theme_bg(), 0);

    // ── Registry-driven tile creation ─────────────────────────────────────
    tile_entry_t *tiles = tile_registry_get();
    uint8_t       count = tile_registry_count();

    for (uint8_t i = 0; i < count; i++) {
        const tile_desc_t *d = tiles[i].desc;

        // Row 0: main tile.
        // main_dirs declared in the descriptor controls which swipe directions
        // are permitted from this tile (horizontal nav + optional swipe-up).
        lv_obj_t *tile = lv_tileview_add_tile(s_tileview, i, 0, d->main_dirs);
        lv_obj_set_style_bg_color(tile, theme_bg(), 0);
        d->init(tile);
        tile_registry_set_handle(i, tile);

        // Row 1: sub-tile (optional detail / debug view).
        // Only created when the descriptor declares has_subtile=true.
        // LV_DIR_TOP only — the sole permitted action is swipe-down back to row 0.
        if (d->has_subtile && d->subtile_init) {
            lv_obj_t *subtile = lv_tileview_add_tile(s_tileview, i, 1, LV_DIR_TOP);
            lv_obj_set_style_bg_color(subtile, theme_bg(), 0);
            d->subtile_init(subtile);
            tile_registry_set_subtile_handle(i, subtile);
            ESP_LOGD(TAG, "Col %u: tile + sub-tile built (%s)", i,
                     d->has_subtile ? "has_subtile" : "");
        } else {
            ESP_LOGD(TAG, "Col %u: tile built", i);
        }
    }

    // ── Default start tile ────────────────────────────────────────────────
    // Jump to System tile (col 1) at boot without animation.
    // Guard: fall back to col 0 if registry has fewer tiles than expected.
    uint8_t start_col = (count > DEFAULT_TILE_COL) ? DEFAULT_TILE_COL : 0;
    lv_obj_set_tile_id(s_tileview, start_col, 0, LV_ANIM_OFF);

    ESP_LOGI(TAG, "Settings screen built — %u tiles, start col %u",
             count, start_col);
    return s_screen;
}

lv_obj_t *settings_screen_get_tileview(void)
{
    return s_tileview;
}

void settings_screen_apply_theme(void)
{
    if (!s_screen || !s_tileview) return;

    lv_obj_set_style_bg_color(s_screen,   theme_bg(), 0);
    lv_obj_set_style_bg_color(s_tileview, theme_bg(), 0);

    // Update tile and sub-tile container backgrounds from registry.
    // Per-tile widget recolouring is handled by each tile's apply_theme()
    // function, called from the lvgl_ui.c apply_ui_theme() fan-out loop.
    tile_entry_t *tiles = tile_registry_get();
    uint8_t       count = tile_registry_count();

    for (uint8_t i = 0; i < count; i++) {
        if (tiles[i].handle)
            lv_obj_set_style_bg_color(tiles[i].handle, theme_bg(), 0);
        if (tiles[i].subtile_handle)
            lv_obj_set_style_bg_color(tiles[i].subtile_handle, theme_bg(), 0);
    }
}

void settings_screen_update_active(void)
{
    if (!s_tileview) return;

    lv_obj_t     *active = lv_tileview_get_tile_active(s_tileview);
    tile_entry_t *tiles  = tile_registry_get();
    uint8_t       count  = tile_registry_count();

    for (uint8_t i = 0; i < count; i++) {
        // Check main tile (row 0)
        if (active == tiles[i].handle) {
            if (tiles[i].desc->update) {
                tiles[i].desc->update();
            }
            return;   // found — stop, one tile per cycle
        }

        // Check sub-tile (row 1)
        if (tiles[i].desc->has_subtile &&
            active == tiles[i].subtile_handle) {
            if (tiles[i].desc->subtile_update) {
                tiles[i].desc->subtile_update();
            }
            return;   // found — stop
        }
    }
    // No match: active tile is mid-scroll transition — benign, do nothing.
}
