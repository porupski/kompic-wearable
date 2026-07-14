/**
 * @file ui_broker.h
 * @brief Core 1 UI settings state manager and async NVS save queue.
 *
 * This is NOT a cross-core component. It is a Core 1 state manager.
 * ui_settings_t is owned entirely by Core 1. It is never accessed from Core 0.
 *
 * Async save flow (Blueprint 1 §5, Blueprint 3 §7):
 *   Core 1 tile event → ui_settings_save_async() → xQueueOverwrite (non-blocking)
 *   task_settings_saver_fn (unpinned, pri 2) → xQueueReceive → app_nvs_save_ui_settings()
 *
 * task_settings_saver_fn is defined here and declared for boot_tasks.c.
 */

#ifndef UI_BROKER_H
#define UI_BROKER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ─── UI settings struct ────────────────────────────────────────────────────────
// Working in-RAM copy. Loaded from NVS at boot. Saved asynchronously on change.

typedef struct {
    uint8_t theme;           // ui_theme_t: 0=DARK, 1=LIGHT
    uint8_t brightness;      // 1–100 %
    bool    blue_light_on;
    bool    auto_brightness;
} ui_settings_t;

// ─── Queue handle ──────────────────────────────────────────────────────────────
// Accessible to any Core 1 code that needs to trigger a settings save.

extern QueueHandle_t ui_settings_save_q;

// ─── API ──────────────────────────────────────────────────────────────────────

/**
 * @brief Create the async save queue. Call once from main.c before tasks start.
 *        Returns the queue handle (also stored in ui_settings_save_q).
 */
QueueHandle_t ui_broker_init(void);

/**
 * @brief Non-blocking enqueue of current UI settings for NVS write.
 *        Safe to call from any Core 1 / LVGL event handler.
 *        Uses xQueueOverwrite — only the most recent state is written. Last-write-wins.
 */
void ui_settings_save_async(const ui_settings_t *s);

// Task function — defined here, extern'd in boot_tasks.c
void task_settings_saver_fn(void *arg);  // arg = QueueHandle_t

extern volatile bool g_auto_brightness;  // Core 1 only

#endif // UI_BROKER_H
