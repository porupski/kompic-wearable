/**
 * @file ui_lock_screen.h
 * @brief Display sleep state machine — public API.
 *
 * Replaces the old lock screen. No overlay, no "Locked" UI.
 * Just backlight on/off + invisible touch absorber.
 */

#ifndef UI_LOCK_SCREEN_H
#define UI_LOCK_SCREEN_H

#include <stdbool.h>

// Called once from lvgl_ui_init() inside lvgl_port_lock()
void lock_screen_init(void);

// Called every refresh cycle from task_ui_refresh_fn() inside lvgl_port_lock()
void lock_screen_poll(void);

// Call from Core 1 on any user interaction to reset the idle timer
void display_sleep_reset_timer(void);

// Query current sleep state (used by boot_power.c)
bool display_is_asleep(void);

#endif // UI_LOCK_SCREEN_H