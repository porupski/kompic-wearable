/**
 * @file alarm.h
 * @brief Alarm module — data types, broker struct, NVS API, task declaration.
 *
 * Up to 3 independently configurable alarm slots. Each slot has a time,
 * haptic pattern, armed state, and led_strobe flag. The alarm task on
 * Core 0 checks RTC time against armed slots every second and fires haptic
 * sequences with intensity escalation on match. When led_strobe is set,
 * the WS2812 status LED (GPIO42) is driven to WS2812_STATE_ALERT for the
 * firing window and restored on dismiss.
 *
 * No physical hardware — broker_alarm_set_hw_status(true) is called
 * unconditionally at boot.
 *
 * Architecture: Blueprint 16
 * Depends on: Blueprint 4 (broker), Blueprint 8 (RTC), Blueprint 13 (haptic)
 */

#ifndef ALARM_H
#define ALARM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define ALARM_MAX_SLOTS         3
#define ALARM_SLOT_EMPTY        0xFF    // hour == 0xFF means slot is unused
#define ALARM_PATTERN_COUNT     3
#define ALARM_SNOOZE_MINUTES    10
#define ALARM_AUTO_DISMISS_SEC  600     // 10 minutes
#define ALARM_ESCALATION_SEC    60      // ramp from 30% to 100% over this
#define ALARM_INTENSITY_MIN_PCT 30
#define ALARM_INTENSITY_MAX_PCT 100

// ---------------------------------------------------------------------------
// Alarm slot (per-alarm configuration, persisted in NVS)
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t hour;           // 0-23; ALARM_SLOT_EMPTY = unused
    uint8_t minute;         // 0-59
    uint8_t pattern_id;     // haptic pattern index (0 .. ALARM_PATTERN_COUNT-1)
    bool    armed;          // user toggle
    bool    led_strobe;     // WS2812 red ALERT strobe while firing (Phase 6 v7.2:
                            // GPIO42 is the status LED, not a piezo as in v5)
} alarm_slot_t;

// ---------------------------------------------------------------------------
// Haptic alarm pattern definitions
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t  effect_id;     // DRV2605 waveform library effect
    uint16_t duration_ms;   // play duration for this step
} alarm_haptic_step_t;

typedef struct {
    const char                *name;        // display name for roller
    const alarm_haptic_step_t *steps;       // burst step sequence
    uint8_t                    step_count;  // steps per burst
    uint16_t                   pause_ms;    // silence between bursts
    uint16_t                   burst_ms;    // total burst duration
} alarm_haptic_pattern_t;

// Access the pattern table (defined in alarm.c)
const alarm_haptic_pattern_t *alarm_get_patterns(void);
uint8_t                       alarm_get_pattern_count(void);

// ---------------------------------------------------------------------------
// Broker data struct
// ---------------------------------------------------------------------------

typedef struct {
    alarm_slot_t slots[ALARM_MAX_SLOTS];

    // Firing state (runtime — only snooze is persisted):
    bool     firing;        // true while alarm is active
    uint8_t  firing_slot;   // which slot triggered (0-2)
    bool     snoozed;       // true if LATER was pressed
    uint16_t snooze_until;  // minute-of-day (0-1439) to re-fire; 0 = none

    // Mandatory bookkeeping:
    uint32_t last_update_ms;
    bool     enabled;       // always true — alarm cannot be user-disabled
} broker_alarm_data_t;

#define BROKER_ALARM_TIMEOUT_MS  5000

// ---------------------------------------------------------------------------
// Module identity
// ---------------------------------------------------------------------------

const char *alarm_get_module_name(void);   // "Alarm"
const char *alarm_get_module_desc(void);   // "Vibration alarm"

// ---------------------------------------------------------------------------
// NVS API (namespace: "alarm_cfg", owned by alarm.c)
// ---------------------------------------------------------------------------

/**
 * @brief Load all alarm slots + snooze state from NVS into broker.
 * Called once from boot_hw_init.c after broker_alarm_set_hw_status(true).
 */
void alarm_nvs_load_all(void);

/**
 * @brief Save a single alarm slot to NVS. Called from alarm_tile.c (Core 1)
 * after the user edits a slot via the overlay.
 * @param idx  Slot index 0..ALARM_MAX_SLOTS-1
 * @param slot Slot data to persist
 */
void alarm_nvs_save_slot(uint8_t idx, const alarm_slot_t *slot);

/**
 * @brief Clear a slot in NVS (set hour = ALARM_SLOT_EMPTY, armed = false).
 * @param idx  Slot index 0..ALARM_MAX_SLOTS-1
 */
void alarm_nvs_delete_slot(uint8_t idx);

/**
 * @brief Save snooze target to NVS. Called from alarm_tile.c on LATER press.
 * @param minute_of_day  0-1439; 0 = clear snooze
 */
void alarm_nvs_save_snooze(uint16_t minute_of_day);

/**
 * @brief Clear snooze from NVS. Called on STOP, auto-dismiss, or re-fire.
 */
void alarm_nvs_clear_snooze(void);

// ---------------------------------------------------------------------------
// Core 0 task
// ---------------------------------------------------------------------------

/**
 * @brief Alarm task — pinned to Core 0, priority 3, stack 4096.
 * Polls RTC every 1 second. Fires haptic on time match. Manages snooze
 * and auto-dismiss. Sends UI events via g_ui_event_q.
 */
void task_alarm_fn(void *arg);

#ifdef __cplusplus
}
#endif

#endif // ALARM_H