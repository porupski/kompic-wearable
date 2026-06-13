/**
 * @file cross_driver.h
 * @brief Cross-Driver Interaction Framework — Public API
 *
 * Provides a lightweight publish/subscribe event bus for Core 0 driver-to-driver
 * interactions. All events are fired and consumed exclusively on Core 0.
 * This is NOT a cross-core mechanism — the Data Broker owns that contract.
 *
 * Three-tier interaction model (see Blueprint 12):
 *   Tier 1: Broker-mediated (cross-core reads)    — existing, no new infra needed
 *   Tier 2: Cross-driver callbacks (C0→C0)        — THIS FILE
 *   Tier 3: Derived / fusion data in broker       — see components/fusion/
 *
 * Rules (non-negotiable):
 *   - Callbacks are registered BEFORE any task is created (in boot_hw_init.c).
 *   - Callbacks are fired FROM Core 0 tasks ONLY — never from Core 1 or ISRs.
 *   - Callbacks must NOT hold the i2c_mutex when called.
 *   - Callbacks must be short — no blocking I2C, no vTaskDelay.
 *   - Drivers NEVER #include each other. They only #include cross_driver.h.
 *   - Max listeners per event: CROSS_DRIVER_MAX_LISTENERS (4). Fail-safe at boot.
 *
 * Usage pattern (driver that produces events):
 *   // After writing to broker in task_gps_fn():
 *   cross_driver_fire(XD_EVENT_GPS_TIME_VALID, &bd);
 *
 * Usage pattern (driver that consumes events):
 *   // In boot_hw_init.c, after all driver inits:
 *   cross_driver_register(XD_EVENT_GPS_TIME_VALID, my_on_gps_time_valid);
 *
 * Architecture: Blueprint 12 §Tier2
 * Core: 0 only
 */

#ifndef CROSS_DRIVER_H
#define CROSS_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** Maximum number of listeners per event. Compile-time constant. */
#define CROSS_DRIVER_MAX_LISTENERS  4

// ---------------------------------------------------------------------------
// Event Catalogue
// Add new events here as new cross-driver pairs are identified.
// Never remove or reorder — ordinal values are used as array indices.
// ---------------------------------------------------------------------------

typedef enum {
    // --- GPS events ---
    XD_EVENT_GPS_TIME_VALID    = 0,  /**< GPS has valid UTC time. data = broker_gps_data_t*  */
    XD_EVENT_GPS_FIX_VALID     = 1,  /**< GPS has a position fix. data = broker_gps_data_t*
                                          altitude field is valid when fix >= GPS_FIX_2D    */
    XD_EVENT_GPS_FIX_LOST      = 2,  /**< GPS fix dropped. data = NULL                      */

    // --- IMU events ---
    XD_EVENT_IMU_SWEEP_SAMPLE  = 3,  /**< IMU sample ready during haptic sweep.
                                          data = broker_imu_data_t*                         */
    XD_EVENT_IMU_GESTURE       = 4,  /**< Gesture detected. data = uint8_t* (imu_gesture_t) */

    // --- ENV events (BME688) ---
    XD_EVENT_ENV_UPDATED       = 5,  /**< New pressure/temp/humidity sample.
                                          data = broker_env_data_t*                         */

    // --- HR events (MAX30101) ---
    XD_EVENT_HR_UPDATED        = 6,  /**< New heart rate / SpO2 reading.
                                          data = broker_hr_data_t*                          */

    // --- Light events (VEML6030) ---
    XD_EVENT_LIGHT_UPDATED     = 7,  /**< New lux reading. data = broker_light_data_t*      */

    // --- Phase 6 v7.2 additions ---
    XD_EVENT_SKIN_UPDATED      = 8,  /**< New skin-temp sample (TMP117).
                                          data = broker_skin_data_t*                        */
    XD_EVENT_CROWN_TURN        = 9,  /**< Crown encoder rotation tick.
                                          data = int8_t* (signed delta, +CW / -CCW)         */
    XD_EVENT_ALARM_FIRED       = 10, /**< Alarm slot fired. data = uint8_t* (slot index)    */
    XD_EVENT_BUTTON_LONG       = 11, /**< Boot button long-press (re-exported from
                                          boot_power). data = NULL                          */
    XD_EVENT_SD_MOUNTED        = 12, /**< SD card mount succeeded. data = NULL              */
    XD_EVENT_MIC_FRAME_READY   = 13, /**< PDM frame ready for consumer.
                                          data = mic_pdm_frame_t* (driver-defined)          */
    XD_EVENT_ECG_FRAME_READY   = 14, /**< Qvar ECG frame complete.
                                          data = qvar_ecg_frame_t* (driver-defined)         */

    // ── ADD NEW EVENTS ABOVE THIS LINE ──────────────────────────────────────
    XD_EVENT_COUNT             // sentinel — do not use directly
} cross_driver_event_t;

// ---------------------------------------------------------------------------
// Callback type
// ---------------------------------------------------------------------------

/**
 * @brief Event callback function pointer.
 *
 * @param event  The event that fired.
 * @param data   Pointer to the producer's broker data struct (read-only).
 *               Cast to the correct type per XD_EVENT_* documentation above.
 *               May be NULL for events that carry no payload (e.g. GPS_FIX_LOST).
 *               Do NOT store this pointer — it points to a stack-local snapshot.
 */
typedef void (*cross_driver_cb_t)(cross_driver_event_t event, const void *data);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * @brief Initialise the cross-driver framework.
 *
 * Clears all listener tables. Must be called once from main.c before any
 * driver init or task creation. Typically called right after broker_init().
 */
void cross_driver_init(void);

/**
 * @brief Register a listener for an event.
 *
 * Called during boot_hw_init.c, before tasks start. Not thread-safe — call
 * only before xTaskCreate. Asserts (configASSERT) if the listener table for
 * this event is full (> CROSS_DRIVER_MAX_LISTENERS).
 *
 * @param event  Event to subscribe to.
 * @param cb     Callback function. Must not be NULL.
 */
void cross_driver_register(cross_driver_event_t event, cross_driver_cb_t cb);

/**
 * @brief Fire an event, calling all registered listeners synchronously.
 *
 * Called from Core 0 sensor tasks after a significant state change.
 * All callbacks are invoked inline — this function returns when all
 * listeners have been called. If no listeners are registered, this is a no-op.
 *
 * @param event  Event to fire.
 * @param data   Pointer to event payload (broker data snapshot). May be NULL.
 */
void cross_driver_fire(cross_driver_event_t event, const void *data);

/**
 * @brief Return a human-readable name for an event (for logging).
 */
const char *cross_driver_event_name(cross_driver_event_t event);

#ifdef __cplusplus
}
#endif

#endif // CROSS_DRIVER_H