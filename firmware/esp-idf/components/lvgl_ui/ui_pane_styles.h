/**
 * @file ui_pane_styles.h
 * @brief Shared LVGL styles for pane roots, drawers, rows, and cards.
 *
 * Stage 31.1 -- foundations rebuild. Every satellite pane (main screen,
 * settings, drawers, overlays) shares a small set of static lv_style_t
 * objects that carry theme colours + layout properties. Any object that
 * uses `lv_obj_add_style(obj, kw_ui_style_*(), 0)` inherits the current
 * theme automatically; a theme swap mutates each style once and calls
 * lv_obj_invalidate() -- no per-widget theme fan-out required.
 *
 * Threading: all functions must run inside lvgl_port_lock() /
 * lvgl_port_unlock(). Style objects are module-scope statics; do not
 * free the returned pointers.
 *
 * Log tiers (per feedback_esp_log_verbosity_practice.md):
 *   LOGI -- init done + on each theme reapply (one-shot events)
 *   LOGD -- per-style property writes during reapply
 *   LOGV -- per-object invalidate calls (firehose, off by default)
 */

#ifndef UI_PANE_STYLES_H
#define UI_PANE_STYLES_H

#include "lvgl.h"

/**
 * @brief Initialise every shared style. Idempotent -- safe to call
 *        twice; the second call is a no-op.
 *
 * Must be called BEFORE any pane's build routine adds the styles to
 * an object; kw_ui_style_*() getters will still return valid (uninit)
 * pointers if called earlier, but LVGL will complain about applying
 * an un-initialised style.
 */
void kw_ui_pane_styles_init(void);

/**
 * @brief Refresh every style's theme-tracked properties (bg colour,
 *        text colour, row colour) to match the current g_ui_theme,
 *        then invalidate the active screen so LVGL redraws.
 *
 * If styles are not yet initialised, this call falls through to
 * kw_ui_pane_styles_init() and returns.
 */
void kw_ui_pane_styles_reapply_theme(void);

/**
 * @brief Style accessors. Return module-scope singletons; do not free.
 *
 * Add to any object via lv_obj_add_style(obj, kw_ui_style_root(), 0).
 * Local styles set on the same object override style properties in the
 * usual LVGL precedence order.
 *
 *   pane_root   -- full-screen root container (bg + text colour).
 *   pane_drawer -- root variant with rounded top corners (for drawers).
 *   row         -- horizontal flex row with row-bg colour + padding.
 *   card        -- rounded card container (row-bg colour + padding).
 *   subtext     -- text-only style carrying theme_subtext(); add to a
 *                  label to render it dim (date sub-lines, chip
 *                  annotations) without hardcoding a colour.
 */
lv_style_t *kw_ui_style_pane_root(void);
lv_style_t *kw_ui_style_pane_drawer(void);
lv_style_t *kw_ui_style_row(void);
lv_style_t *kw_ui_style_card(void);
lv_style_t *kw_ui_style_subtext(void);

#endif // UI_PANE_STYLES_H
