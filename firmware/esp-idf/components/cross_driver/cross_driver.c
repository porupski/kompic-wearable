/**
 * @file cross_driver.c
 * @brief Cross-Driver Interaction Framework — Implementation
 *
 * Thin event dispatcher. No dynamic allocation. No FreeRTOS primitives.
 * Registration is boot-time only; dispatch is Core 0 task-time only.
 *
 * Architecture: Blueprint 12 §Tier2
 * Core: 0 only
 */

#include "cross_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"  // configASSERT

#include <string.h>

static const char *TAG = "XD";

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

typedef struct {
    cross_driver_cb_t listeners[CROSS_DRIVER_MAX_LISTENERS];
    uint8_t           count;
} xd_slot_t;

static xd_slot_t s_slots[XD_EVENT_COUNT];

// ---------------------------------------------------------------------------
// Event name table (logging only)
// ---------------------------------------------------------------------------

static const char *const s_event_names[XD_EVENT_COUNT] = {
    [XD_EVENT_GPS_TIME_VALID]   = "GPS_TIME_VALID",
    [XD_EVENT_GPS_FIX_VALID]    = "GPS_FIX_VALID",
    [XD_EVENT_GPS_FIX_LOST]     = "GPS_FIX_LOST",
    [XD_EVENT_IMU_SWEEP_SAMPLE] = "IMU_SWEEP_SAMPLE",
    [XD_EVENT_IMU_GESTURE]      = "IMU_GESTURE",
    [XD_EVENT_ENV_UPDATED]      = "ENV_UPDATED",
    [XD_EVENT_HR_UPDATED]       = "HR_UPDATED",
    [XD_EVENT_LIGHT_UPDATED]    = "LIGHT_UPDATED",
    [XD_EVENT_SKIN_UPDATED]     = "SKIN_UPDATED",
    [XD_EVENT_CROWN_TURN]       = "CROWN_TURN",
    [XD_EVENT_ALARM_FIRED]      = "ALARM_FIRED",
    [XD_EVENT_BUTTON_LONG]      = "BUTTON_LONG",
    [XD_EVENT_SD_MOUNTED]       = "SD_MOUNTED",
    [XD_EVENT_MIC_FRAME_READY]  = "MIC_FRAME_READY",
    [XD_EVENT_ECG_FRAME_READY]  = "ECG_FRAME_READY",
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void cross_driver_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    ESP_LOGI(TAG, "Cross-driver framework init OK (%d event slots)", XD_EVENT_COUNT);
}

void cross_driver_register(cross_driver_event_t event, cross_driver_cb_t cb)
{
    configASSERT(event < XD_EVENT_COUNT);
    configASSERT(cb != NULL);

    xd_slot_t *slot = &s_slots[event];

    // Guard against table overflow — hard fault at boot, not silent runtime drop
    configASSERT(slot->count < CROSS_DRIVER_MAX_LISTENERS);

    slot->listeners[slot->count++] = cb;

    ESP_LOGI(TAG, "Registered listener for %s (slot %u/%u)",
             cross_driver_event_name(event),
             slot->count,
             CROSS_DRIVER_MAX_LISTENERS);
}

void cross_driver_fire(cross_driver_event_t event, const void *data)
{
    if (event >= XD_EVENT_COUNT) {
        ESP_LOGW(TAG, "fire: invalid event %d", (int)event);
        return;
    }

    xd_slot_t *slot = &s_slots[event];

    if (slot->count == 0) {
        return; // no listeners — common case, early-out is free
    }

    for (uint8_t i = 0; i < slot->count; i++) {
        slot->listeners[i](event, data);
    }
}

const char *cross_driver_event_name(cross_driver_event_t event)
{
    if (event >= XD_EVENT_COUNT) return "UNKNOWN";
    const char *name = s_event_names[event];
    return name ? name : "UNNAMED";
}