/**
 * @file ui_event.h
 * @brief UI Event Queue — the only legal path for Core 0 to command Core 1.
 *
 * Lives in components/data_broker/ alongside power_flags.h because it is a
 * cross-core contract, not a UI widget.
 *
 * PRODUCER (Core 0): any task or XD callback.
 *   ui_event_send(&evt);          // non-blocking, drop if full
 *
 * CONSUMER (Core 1): task_ui_refresh_fn() drains every 200 ms inside
 *   lvgl_port_lock. See lvgl_ui.c step 6.
 *
 * QUEUE: depth 8, created in broker_init() before any task starts.
 *
 * RULE: Never use global flags for new Core 0 → Core 1 commands.
 *       Existing flags (g_show_shutdown_overlay, g_wake_display, etc.)
 *       are grandfathered but not the pattern for new features.
 *
 * Architecture: Blueprint 1 §4 (inter-core command path)
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Event types — extend this enum as features are added.
// Keep values explicit for debug logging / serial monitor readability.
// ---------------------------------------------------------------------------

typedef enum {
    UI_EVENT_ALARM_FIRED    = 0,   // RTC match → show alarm overlay + haptic
    UI_EVENT_NOTIF_SHOW     = 1,   // show notification overlay (title + body)
    UI_EVENT_NOTIF_DISMISS  = 2,   // dismiss current notification overlay
    // Future:
    // UI_EVENT_NAV_TO_TILE  = 3,
    // UI_EVENT_POWER_MODE   = 4,
} ui_event_type_t;

// ---------------------------------------------------------------------------
// Event struct — fits in a FreeRTOS queue item by value (no heap alloc).
// Total size: ~104 bytes. Queue depth 8 → ~832 bytes SRAM. Acceptable.
// ---------------------------------------------------------------------------

typedef struct {
    ui_event_type_t type;
    union {
        struct {
            uint8_t alarm_id;           // which alarm (future: multiple)
            uint8_t snooze_minutes;     // 0 = no snooze configured
        } alarm;
        struct {
            char     title[32];         // null-terminated, truncated if longer
            char     body[64];          // null-terminated, truncated if longer
            uint32_t timeout_ms;        // 0 = no auto-dismiss
        } notif;
    } payload;
} ui_event_t;

// ---------------------------------------------------------------------------
// Queue handle — created in broker_init(), before any task starts.
// Core 0 writes (xQueueSend). Core 1 reads (xQueueReceive).
// ---------------------------------------------------------------------------

extern QueueHandle_t g_ui_event_q;

// ---------------------------------------------------------------------------
// Convenience send — non-blocking, drops if queue full.
// Safe to call from any Core 0 context (task, XD callback, ISR deferred).
// Returns true if enqueued, false if dropped.
// ---------------------------------------------------------------------------

static inline bool ui_event_send(const ui_event_t *evt)
{
    if (!g_ui_event_q) return false;
    return xQueueSend(g_ui_event_q, evt, 0) == pdTRUE;
}

#ifdef __cplusplus
}
#endif