/**
 * @file lvgl_ui.h
 * @brief LVGL UI manager — screen construction, theme fan-out, refresh task.
 *
 * Call order from main.c (Blueprint 6 §3):
 *   lvgl_port_lock(portMAX_DELAY);
 *   lvgl_ui_init(&ui_cfg);        ← builds all screens, applies theme frame 0
 *   lvgl_port_unlock();
 *
 * task_ui_refresh_fn is defined in lvgl_ui.c and declared here for boot_tasks.c.
 * It runs on Core 1, polls every 200 ms, reads broker, updates widgets.
 */

#ifndef LVGL_UI_H
#define LVGL_UI_H

#include "lvgl.h"
#include "ui_broker.h"   // ui_settings_t

// ─── Screen state enum ─────────────────────────────────────────────────────────

typedef enum {
    UI_SCREEN_MAIN     = 0,
    UI_SCREEN_SETTINGS = 1,
    UI_SCREEN_ALARM    = 2,
} ui_screen_state_t;

// ─── API ──────────────────────────────────────────────────────────────────────

/**
 * @brief Build all LVGL screens and apply initial theme.
 *        Must be called inside lvgl_port_lock() / lvgl_port_unlock().
 *        Must be called after lvgl_port_init().
 *
 * @param cfg  UI settings loaded from NVS. Theme + brightness applied immediately.
 */
void lvgl_ui_init(const ui_settings_t *cfg);

/**
 * @brief Apply current theme to all themed screens and tiles.
 *        Main screen is always DARK — exempt from this call. (Blueprint 3 §6)
 *        Must be called inside lvgl_port_lock().
 */
void apply_ui_theme(void);

// Task function — declared for boot_tasks.c extern reference
void task_ui_refresh_fn(void *arg);   // Pinned to Core 1

#endif // LVGL_UI_H