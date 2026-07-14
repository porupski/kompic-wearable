/**
 * @file ui_shutdown_overlay.h
 * @brief Shutdown overlay — full-screen dark cover shown during power-off sequence.
 *
 * Triggered by boot_power.c when the power button is held ≥ PWR_HOLD_OVERLAY_MS.
 * Cleared when the button is released before the hard-off threshold.
 *
 * The overlay is parented to lv_layer_sys() — the highest LVGL system layer,
 * above lv_layer_top() where the lock screen lives.  This guarantees the
 * shutdown overlay is always the topmost visible element, even over a locked screen.
 *
 * g_shutdown_latched is set by boot_power.c at the hard-off threshold.  Once
 * latched, shutdown_overlay_poll() stops hiding the overlay even if
 * g_show_shutdown_overlay is cleared — the device is about to power off.
 *
 * SET IN STONE once implemented.
 * Core 1 only.  Must always be called inside lvgl_port_lock().
 */

#ifndef UI_SHUTDOWN_OVERLAY_H
#define UI_SHUTDOWN_OVERLAY_H

/**
 * @brief Pre-create overlay widgets (hidden).
 *
 * Call once from lvgl_ui_init(), inside lvgl_port_lock().
 */
void shutdown_overlay_init(void);

/**
 * @brief Drive the shutdown overlay state machine.
 *
 * Call every 200 ms from task_ui_refresh_fn(), inside lvgl_port_lock().
 * Reads g_show_shutdown_overlay and g_shutdown_latched.
 *
 * Behaviour:
 *   g_show_shutdown_overlay == true  && !g_shutdown_latched → show overlay
 *   g_show_shutdown_overlay == false && !g_shutdown_latched → hide overlay
 *   g_shutdown_latched == true                              → show, never hide
 */
void shutdown_overlay_poll(void);

#endif // UI_SHUTDOWN_OVERLAY_H
