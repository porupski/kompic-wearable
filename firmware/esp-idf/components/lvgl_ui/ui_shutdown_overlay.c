/**
 * @file ui_shutdown_overlay.c
 * @brief Shutdown overlay - full-screen dark cover shown during power-off sequence.
 *
 * Implementation notes:
 *
 *   LAYER: lv_layer_sys()
 *     LVGL has three system layers above all screens, in ascending Z-order:
 *       lv_layer_bottom() - below screens (rarely used)
 *       lv_layer_top()    - above screens (used by lock screen)
 *       lv_layer_sys()    - highest; intended for system-critical UI
 *     Parenting to lv_layer_sys() ensures the shutdown overlay is visible
 *     even when the lock screen overlay (lv_layer_top()) is active.
 *
 *   LATCH BEHAVIOUR:
 *     boot_power.c sets g_shutdown_latched = true when the hold crosses the
 *     hard-off threshold.  From that point the overlay must never be hidden -
 *     the device is in its final power-down countdown.  shutdown_overlay_poll()
 *     respects this by skipping the hide branch once latched.
 *
 *   CONTENT:
 *     Red power icon (LV_SYMBOL_POWER, UI_FONT_TITLE) centred slightly above
 *     mid, followed by "Shutting down..." in white.  Always DARK - not themed.
 *
 * Core 1 only.  No I2C.  No NVS.
 * All functions must be called inside lvgl_port_lock() / lvgl_port_unlock().
 */

#include "ui_shutdown_overlay.h"
#include "ui_theme_colors.h"
#include "data_broker.h"       // broker reads
#include "power_flags.h"        // g_show_shutdown_overlay, g_shutdown_latched
#include "boot_display.h"      // LCD_H_RES, LCD_V_RES
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "SHUTDOWN_OVL";

// ---------------------------------------------------------------------------
// Module-static state
// ---------------------------------------------------------------------------

static lv_obj_t *s_overlay  = NULL;
static bool      s_visible  = false;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void overlay_show(void)
{
    if (!s_overlay || s_visible) return;
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(s_overlay, -1);
    s_visible = true;
    ESP_LOGI(TAG, "Shutdown overlay shown");
}

static void overlay_hide(void)
{
    if (!s_overlay || !s_visible) return;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ESP_LOGI(TAG, "Shutdown overlay hidden");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void shutdown_overlay_init(void)
{
    // Parent = lv_layer_sys(): topmost LVGL system layer, above lock screen.
    lv_obj_t *layer = lv_layer_sys();

    s_overlay = lv_obj_create(layer);
    lv_obj_set_size(s_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Absorb touch - no interaction should be possible during shutdown.
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    // Power icon - red, centred above mid
    lv_obj_t *icon = lv_label_create(s_overlay);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(icon, COL_STATUS_OFFLINE, 0);
    lv_obj_set_style_text_font(icon, UI_FONT_TITLE, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -28);

    // Status label - white, below icon
    lv_obj_t *lbl = lv_label_create(s_overlay);
    lv_label_set_text(lbl, "Shutting down...");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, UI_FONT_LABEL, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 6);

    // Hidden until triggered
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    ESP_LOGI(TAG, "Shutdown overlay init OK (lv_layer_sys)");
}

void shutdown_overlay_poll(void)
{
    if (!s_overlay) return;

    if (g_shutdown_latched) {
        // Latched: hard-off imminent - show unconditionally, never hide.
        overlay_show();
        return;
    }

    if (g_show_shutdown_overlay) {
        overlay_show();
    } else {
        overlay_hide();
    }
}