/**
 * @file fusion.h
 * @brief Fusion Module — Derived Data from Multiple Sources
 *
 * When a meaningful value requires data from two or more sensors, it belongs
 * here — not in either individual driver. Fusion reads from the broker,
 * computes a best-available result, and writes back a `broker_fusion_data_t`
 * slot that tiles can read without knowing which hardware provided the answer.
 *
 * Current fusion channels:
 *   - altitude_m       : GPS altitude (primary) or barometric (ENV fallback)
 *   - activity_state   : stub — HR + IMU driven (future)
 *
 * Fusion is computed inline inside `task_fusion_fn()` pinned to Core 0.
 * It reads broker GPS and ENV data (both broker-mutex guarded), computes
 * derived values, then writes broker_fusion in one mutex window.
 *
 * Fusion does NOT fire cross-driver events — it IS the endpoint.
 * Other modules fire events; fusion listens and writes the broker slot.
 *
 * Poll rate: 1 Hz — fusion data changes slowly.
 * Stack: 2048 bytes — no I2C, no heavy computation.
 * Priority: 2 — lowest sensor priority, runs after all raw sensors.
 * Core: 0
 *
 * Architecture: Blueprint 12 §Tier3
 */

#ifndef FUSION_H
#define FUSION_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Altitude source enum
// ---------------------------------------------------------------------------

typedef enum {
    ALTITUDE_SRC_NONE  = 0,  /**< No valid altitude available from any source  */
    ALTITUDE_SRC_GPS   = 1,  /**< GPS fix altitude (primary, most accurate)     */
    ALTITUDE_SRC_BARO  = 2,  /**< Barometric altitude from ENV sensor (fallback)*/
    ALTITUDE_SRC_LAST  = 3,  /**< Last known GPS altitude, fix since lost       */
} altitude_src_t;

// ---------------------------------------------------------------------------
// Activity state enum (stub — HR + IMU future)
// ---------------------------------------------------------------------------

typedef enum {
    ACTIVITY_UNKNOWN   = 0,
    ACTIVITY_STILL     = 1,
    ACTIVITY_WALKING   = 2,
    ACTIVITY_RUNNING   = 3,
    ACTIVITY_SLEEPING  = 4,
} activity_state_t;

// ---------------------------------------------------------------------------
// Broker data struct — owned by this header, stored in broker_state_t
// ---------------------------------------------------------------------------

typedef struct {
    // --- Altitude fusion ---
    float          altitude_m;       /**< Best available altitude in metres       */
    altitude_src_t altitude_source;  /**< Which sensor provided altitude_m        */
    float          altitude_gps_m;   /**< Raw GPS altitude (0 if no fix)          */
    float          altitude_baro_m;  /**< Raw baro altitude (0 if ENV offline)    */

    // --- Activity (stub, filled when HR + IMU implemented) ---
    activity_state_t activity;       /**< Fused activity classification           */
    uint8_t          activity_conf;  /**< Confidence 0–100                        */

    // --- Mandatory bookkeeping ---
    uint32_t last_update_ms;
    bool     enabled;                /**< Always true — fusion has no power switch*/
} broker_fusion_data_t;

/** Timeout: fusion updates at 1 Hz; stale if >3 s without update */
#define BROKER_FUSION_TIMEOUT_MS  3000

// ---------------------------------------------------------------------------
// Fusion lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Initialise fusion module. Call from boot_hw_init.c before tasks start.
 *        Registers cross-driver listeners for GPS and ENV events.
 *        Does not touch hardware.
 */
void fusion_init(void);

/**
 * @brief Core 0 fusion task. Registered in boot_tasks.c.
 *        Polls broker GPS + ENV at 1 Hz, computes derived channels,
 *        writes broker_fusion.
 */
void task_fusion_fn(void *arg);

// Identity (follows module convention)
const char *fusion_get_chip_name(void);
const char *fusion_get_chip_desc(void);

#ifdef __cplusplus
}
#endif

#endif // FUSION_H