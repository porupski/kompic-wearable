/**
 * @file tile_registry.h
 * @brief Tile descriptor type and registry API for lvgl_ui.c.
 *
 * PATTERN: Static descriptor export (Linux kernel platform_driver style).
 *   Each tile module defines one `const tile_desc_t foo_tile_desc` in its
 *   .c file and declares it `extern` in its .h file.
 *   tile_registry.c holds an ordered array of pointers to those descriptors.
 *   lvgl_ui.c calls tile_registry_build() once and then only touches the
 *   tileview through the registry — zero per-module code in lvgl_ui.c.
 *
 * SUB-TILE SUPPORT:
 *   A tile can declare one optional sub-tile (vertical row 1, swipe-up detail
 *   view). Set subtile_init / subtile_update to non-NULL and set
 *   has_subtile = true. The tileview column is shared with the parent tile;
 *   lvgl_ui.c allocates row 0 + row 1 automatically.
 *
 * NAVIGATION DIRECTIONS:
 *   main_dirs: LV_DIR_LEFT | LV_DIR_RIGHT for most tiles.
 *              Add LV_DIR_BOTTOM if the tile has a sub-tile (swipe-up → row 1).
 *   The global swipe-down-to-exit is handled at screen level, not tile level.
 *
 * Architecture: Blueprint 3 §5, Blueprint 5 §7 (revised)
 */

#ifndef TILE_REGISTRY_H
#define TILE_REGISTRY_H

#include "lvgl.h"
#include "ui_theme_colors.h"
#include <stdbool.h>
#include <stdint.h>

// ── Tile descriptor ───────────────────────────────────────────────────────────

typedef struct {
    // Mandatory function pointers — must not be NULL
    void (*init)        (lv_obj_t *parent);
    void (*update)      (void);
    void (*apply_theme) (ui_theme_t theme);

    // Sub-tile (optional vertical detail view, row 1)
    bool  has_subtile;
    void (*subtile_init)   (lv_obj_t *parent);   // NULL if has_subtile == false
    void (*subtile_update) (void);               // NULL if has_subtile == false

    // Navigation directions for the main tile slot
    // Use LV_DIR_LEFT | LV_DIR_RIGHT for standard horizontal-only tiles.
    // Add LV_DIR_BOTTOM when has_subtile == true.
    lv_dir_t main_dirs;
} tile_desc_t;

// ── Registry entry (internal — populated by tile_registry_build) ──────────────

typedef struct {
    const tile_desc_t *desc;
    lv_obj_t          *handle;         // filled by lvgl_ui.c after add_tile()
    lv_obj_t          *subtile_handle; // filled by lvgl_ui.c, NULL if !has_subtile
    uint8_t            col;            // tileview column index (set at build time)
} tile_entry_t;

// ── Public API ─────────────────────────────────────────────────────────────────

/**
 * @brief Return the number of registered tiles.
 *        Valid to call before tile_registry_build().
 */
uint8_t tile_registry_count(void);

/**
 * @brief Return a pointer to the flat tile entry array.
 *        Array length == tile_registry_count().
 *        Caller must not free or modify entries directly.
 */
tile_entry_t *tile_registry_get(void);

/**
 * @brief Set the lv_obj_t* handle for entry at index `idx`.
 *        Called by lvgl_ui.c immediately after lv_tileview_add_tile().
 */
void tile_registry_set_handle(uint8_t idx, lv_obj_t *handle);

/**
 * @brief Set the lv_obj_t* sub-tile handle for entry at index `idx`.
 *        Called by lvgl_ui.c immediately after lv_tileview_add_tile() for row 1.
 */
void tile_registry_set_subtile_handle(uint8_t idx, lv_obj_t *subtile_handle);

#endif // TILE_REGISTRY_H
