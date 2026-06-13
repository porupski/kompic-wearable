/**
 * @file ui_notif_overlay.h
 * @brief Notification / alarm overlay — full-screen UI on lv_layer_top().
 *
 * This overlay is the visual counterpart to UI Event Queue commands from
 * Core 0.  When an alarm fires, Core 0 enqueues UI_EVENT_ALARM_FIRED;
 * the drain loop in lvgl_ui.c calls notif_overlay_show_alarm() which
 * builds a STOP / LATER overlay on demand.
 *
 * LAYER: lv_layer_top()
 *   Above all screens (main, settings, alarm) but below lv_layer_sys()
 *   where the shutdown overlay lives.  Shutdown always wins.
 *
 * LIFECYCLE: lazy build / destroy.
 *   show_alarm()  → creates overlay widgets
 *   STOP / LATER  → destroys overlay widgets
 *   dismiss()     → destroys overlay widgets (auto-dismiss from Core 0)
 *   No pre-built widgets sitting on the layer.
 *
 * THEME: stub support via notif_overlay_apply_theme().  Currently uses
 *   fixed dark styling.  Individual overlay types (alarm vs generic notif)
 *   can specialise later.
 *
 * Core 1 only.  Must always be called inside lvgl_port_lock().
 * Architecture: Blueprint 16 §Stage D, Blueprint 1 §UI Event Queue.
 */

#ifndef UI_NOTIF_OVERLAY_H
#define UI_NOTIF_OVERLAY_H

#include "ui_theme_colors.h"   /* ui_theme_t */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Show the alarm-fired overlay (STOP / LATER buttons).
 *
 * Builds widgets on lv_layer_top() on demand.  If an overlay is already
 * visible it is destroyed first (only one notification at a time).
 *
 * @param alarm_id        Slot index that fired (0–2)
 * @param snooze_minutes  Snooze duration shown on LATER button (e.g. 10)
 *
 * Called from drain_ui_event_queue() on UI_EVENT_ALARM_FIRED.
 */
void notif_overlay_show_alarm(uint8_t alarm_id, uint8_t snooze_minutes);

/**
 * @brief Dismiss any active notification overlay.
 *
 * Safe to call when no overlay is visible (no-op).
 * Called from drain_ui_event_queue() on UI_EVENT_NOTIF_DISMISS and
 * on auto-dismiss timeout from Core 0.
 */
void notif_overlay_dismiss(void);

/**
 * @brief Returns true if a notification overlay is currently visible.
 */
bool notif_overlay_is_visible(void);

/**
 * @brief Apply theme to the notification overlay (stub — future use).
 *
 * Called from apply_ui_theme() fan-out in lvgl_ui.c.  Currently a no-op
 * because the overlay uses fixed dark styling and is typically not on
 * screen during a theme change.  When themed overlays are needed, this
 * is the hook.
 *
 * @param theme  Current UI theme
 */
void notif_overlay_apply_theme(ui_theme_t theme);

#ifdef __cplusplus
}
#endif

#endif /* UI_NOTIF_OVERLAY_H */