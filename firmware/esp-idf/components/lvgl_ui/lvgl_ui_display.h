/**
 * @file lvgl_ui_display.h
 * @brief LVGL port + display bring-up. Stage 22 §4.1b.
 *
 * Split from lvgl_ui.c so main.c can call one entry point without importing
 * esp_lvgl_port headers. Handles:
 *
 *   - lvgl_port_init() -- LVGL timer + task + global mutex (Core -1, prio 4).
 *   - Draw-buffer allocation in internal SRAM (partial refresh, 40-row strip).
 *   - lv_display_create() + LV_COLOR_FORMAT_RGB888 + flush callback that wraps
 *     co5300_set_window() + co5300_write_pixels().
 *
 * The whole path is gated: on iv7.1 default boot (no panel) nothing runs. When
 * boot_display_is_present() returns true, or when the NVS lvgl_force_on flag
 * is set as a bench override, lvgl_ui_display_setup() is called from main.c.
 *
 * The `force_no_panel` mode brings LVGL up in memory-only form so tile
 * transitions, event handlers, and the GPS photo view can be exercised on
 * iv7.1 without a real CO5300 -- the flush callback becomes a no-op that just
 * calls lv_display_flush_ready() to keep LVGL's frame counter moving.
 */

#ifndef LVGL_UI_DISPLAY_H
#define LVGL_UI_DISPLAY_H

// Driver version: MAJOR.MINOR.PATCH -- bump PATCH on any change here,
// MINOR on feature adds, MAJOR on release quality (beta / RC / GA).
#define LVGL_UI_DISPLAY_DRIVER_VERSION  "0.1.0"

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the LVGL port and register a display against CO5300 (or a
 *        no-op flush callback in force mode).
 *
 * Must be called AFTER boot_display_init() has returned, so the CO5300 handle
 * (via boot_display_get_co5300()) is either valid or explicitly NULL.
 *
 * @param force_no_panel If true, LVGL comes up regardless of panel presence
 *                       and the flush callback is a no-op. Used by
 *                       `LVGL_FORCE ON` on iv7.1 for bench-testing without a
 *                       real CO5300.
 *
 * @return ESP_OK on success. On failure, LVGL is not up and the boot continues
 *         headless (caller ignores the return code except for the boot log).
 */
esp_err_t lvgl_ui_display_setup(bool force_no_panel);

/**
 * @brief True after lvgl_ui_display_setup() succeeded. STATUS uses this to
 *        print `lvgl = up (buf=Nkb)` / `lvgl = off`.
 */
bool lvgl_ui_display_is_up(void);

/**
 * @brief True when the LVGL side is up under the force-no-panel override
 *        (i.e. flush callback is a no-op). Distinct from is_up() so STATUS
 *        can distinguish a real panel from bench-only mode.
 */
bool lvgl_ui_display_is_forced(void);

/**
 * @brief Byte count of the LVGL draw buffer allocated in setup(). 0 before
 *        setup() runs or if allocation failed. Reported by STATUS so tuning
 *        the strip height is a live decision.
 */
size_t lvgl_ui_display_buf_bytes(void);

/**
 * @brief Build all LVGL screens (main + settings + alarm + overlays) and
 *        register every tile from tile_registry.c. Loads ui_settings from NVS
 *        (safe defaults on first boot). Applies theme + initial backlight.
 *
 * Must be called AFTER lvgl_ui_display_setup(). Wraps the whole screen build
 * in lvgl_port_lock() as required by lvgl_ui_init().
 *
 * @return ESP_OK on success. ESP_ERR_INVALID_STATE if setup() was skipped.
 */
esp_err_t lvgl_ui_display_boot_screens(void);

/**
 * @brief Spawn the LVGL refresh task (Core 1) and the settings-saver task
 *        (unpinned). Lives here rather than boot_tasks.c because
 *        boot_logic ↔ lvgl_ui would form a component dependency cycle.
 *        Safe to call unconditionally -- no-op if the LVGL side is not up.
 */
void lvgl_ui_display_start_tasks(void);

/**
 * @brief Total number of tiles the registry holds (10 today). 0 when LVGL is
 *        not up. Feeds the `TILE list` CLI verb.
 */
int lvgl_ui_display_tile_count(void);

/**
 * @brief Jump the settings-screen tileview to column `col_idx` (0..count-1).
 *        Wraps the call in lvgl_port_lock. Bench-testable under LVGL_FORCE:
 *        the state moves even when nothing is rendering to the panel.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if LVGL isn't up or the
 *         requested tile handle is NULL, ESP_ERR_INVALID_ARG if col out of
 *         range, ESP_FAIL if the port lock could not be taken.
 */
esp_err_t lvgl_ui_display_jump_tile(int col_idx);

/**
 * @brief True if the LVGL touch indev is registered (i.e., CST9217 came up
 *        AND lv_indev_create succeeded). STATUS + TOUCH verb use this.
 */
bool lvgl_ui_display_touch_indev_ready(void);

/**
 * @brief Snapshot the LVGL indev's view of touch state. Fills in x/y from the
 *        last non-empty g_touch_q peek, the monotonic PRESSED-event count seen
 *        by the read callback, and the current queue depth (0 or 1). Feeds the
 *        `TOUCH` CLI verb for headless bench debug (see cli-first-testability).
 *
 * @param out_x        Last-seen touch X (0 if never touched).
 * @param out_y        Last-seen touch Y.
 * @param out_events   Monotonic PRESSED tick counter from the read cb.
 * @param out_pressed  True if the queue currently reports fingers > 0.
 */
void lvgl_ui_display_touch_snapshot(uint16_t *out_x, uint16_t *out_y,
                                    uint32_t *out_events, bool *out_pressed);

#ifdef __cplusplus
}
#endif

#endif // LVGL_UI_DISPLAY_H
