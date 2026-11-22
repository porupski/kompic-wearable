/**
 * @file fc_flight.h
 * @brief FCM_FLIGHT public surface (Stage 38).
 *
 * Mode entry point is `run_flight_mode()` (declared in fc_internal.h so
 * field_capture.c can dispatch to it). This header exposes the tile-side
 * read surface + CLI stop verb only.
 */

#ifndef FC_FLIGHT_H
#define FC_FLIGHT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     active;         // true while run_flight_mode() loop is running
    uint32_t start_ms;       // millis_u32() captured at session start
    uint32_t elapsed_s;      // refreshed every ~500 ms during recording
    uint32_t sample_count;   // rows written to CSV
    uint32_t write_errors;   // fsync failures (also triggers long buzz)
    bool     sd_ok;          // false after any fsync failure (sticky until next OK)
    uint32_t sd_free_mib;    // refreshed every ~500 ms
    int      gps_fix;        // broker_gps_data_t.fix snapshot (0/2/3)
} fc_flight_state_t;

/**
 * @brief Snapshot pointer to the flight recorder state. Read-only; the
 *        pointer is stable across the firmware lifetime. Fields update
 *        under the flight task without a lock -- single-reader tiles see
 *        occasional torn uint32_t reads which are visually harmless.
 */
const fc_flight_state_t *fc_flight_get_state(void);

/**
 * @brief Start a flight recording session. Opens a datetime-named CSV
 *        on SD with the §2.2 header block, enables IMU/MAG/ENV brokers,
 *        and spawns a background task that writes rows at ~125 Hz.
 *
 *        Returns:
 *            0  session started
 *           -1  already active
 *           -2  SD / CSV open failed
 *           -3  task creation failed
 *
 *        Safe to call from any task (LVGL button handler, CLI, etc.).
 *        The returned task runs independently of the caller -- display
 *        sleep + tile navigation + CLI do not stall recording.
 */
int fc_flight_cmd_start(void);

/**
 * @brief Request flight recording to stop at the next loop iteration.
 *        Returns 0 if a session was active (stop requested), -1 if idle.
 *        Safe to call from any task (CLI or UI). The task closes the
 *        CSV + self-deletes asynchronously; `fc_flight_get_state()->active`
 *        drops to false once the file is durably closed.
 */
int fc_flight_cmd_stop(void);

#ifdef __cplusplus
}
#endif

#endif  // FC_FLIGHT_H
