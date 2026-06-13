/**
 * @file power_flags.h
 * @brief Volatile inter-core flags owned by boot_power.c.
 *
 * Single-writer (Core 0 power task), single-reader (Core 1 UI task).
 * ESP32 atomic bool access — no mutex needed.
 */

#ifndef POWER_FLAGS_H
#define POWER_FLAGS_H

#include <stdbool.h>

// Defined in boot_power.c
extern volatile bool g_wake_display;          // Wake display from sleep
extern volatile bool g_display_sleep;         // Put display to sleep manually
extern volatile bool g_show_shutdown_overlay; // Button held 2.5s
extern volatile bool g_shutdown_latched;      // Shutdown committed

// g_screen_locked REMOVED — replaced by display_is_asleep() in ui_lock_screen.c
// Navigation flags REMOVED — native LVGL gesture callbacks own navigation

#endif // POWER_FLAGS_H