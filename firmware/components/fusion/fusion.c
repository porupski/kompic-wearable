/**
 * @file fusion.c
 * @brief Fusion Module — Derived Data from Multiple Sources
 *
 * Reads broker GPS and ENV slots, computes best-available altitude,
 * writes broker_fusion. Runs at 1 Hz on Core 0.
 *
 * To add a new fusion channel:
 *   1. Add fields to broker_fusion_data_t in fusion.h
 *   2. Read the relevant broker slot(s) in task_fusion_fn()
 *   3. Compute the derived value
 *   4. Write into `fd` before broker_fusion_write()
 *   Done. Nothing else to touch.
 *
 * Architecture: Blueprint 12 §Tier3
 * Core: 0
 */

#include "fusion.h"
#include "cross_driver.h"
#include "data_broker.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "FUSION";

// ---------------------------------------------------------------------------
// Barometric altitude is computed by the BME688 driver itself
// (broker_env_data_t.altitude_m, hypsometric formula with P0 = 1013.25 hPa).
// Fusion just reads the broker — no chip-layer math here.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Cross-driver event listeners (registered at init, called from C0 tasks)
// ---------------------------------------------------------------------------

/** Called when GPS fires XD_EVENT_GPS_FIX_VALID — could update sea-level ref */
static void on_gps_fix_valid(cross_driver_event_t event, const void *data)
{
    (void)event;
    (void)data;
    // Future: could use GPS altitude to calibrate BME688's sea-level reference
    // via bme688_set_sea_level_hpa() — datasheet pass (Phase 20) will confirm.
    // For now, fusion task does all the work at its 1 Hz poll.
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void fusion_init(void)
{
    cross_driver_register(XD_EVENT_GPS_FIX_VALID, on_gps_fix_valid);
    // Future: cross_driver_register(XD_EVENT_ENV_UPDATED, on_env_updated);
    // Future: cross_driver_register(XD_EVENT_HR_UPDATED,  on_hr_updated);
    ESP_LOGI(TAG, "Fusion init OK");
}

// ---------------------------------------------------------------------------
// Core 0 task
// ---------------------------------------------------------------------------

void task_fusion_fn(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(1000); // 1 Hz
    TickType_t       last   = xTaskGetTickCount();

    ESP_LOGI(TAG, "Fusion task started");

    while (1) {
        vTaskDelayUntil(&last, period);

        broker_fusion_data_t fd;
        memset(&fd, 0, sizeof(fd));
        fd.enabled = true;

        // ── Read GPS ────────────────────────────────────────────────────────
        broker_gps_data_t gps = {0};
        broker_gps_read(&gps);
        const bool gps_fix = (gps.fix >= 1) && gps.position_valid; // GPS_FIX_2D or better

        if (gps_fix) {
            fd.altitude_gps_m = gps.altitude_m;
        }

        // ── Read ENV (barometric altitude fallback) ──────────────────────────
        // BME688 driver pre-computes altitude_m in its own poll; we just read it.
        broker_env_data_t env = {0};
        broker_env_read(&env);
        const bool env_valid = broker_env_hw_alive()
                            && broker_env_get_enabled()
                            && env.pressure_hpa > 500.0f;
        if (env_valid) {
            fd.altitude_baro_m = env.altitude_m;
        }

        // ── Altitude arbitration ─────────────────────────────────────────────
        if (gps_fix) {
            fd.altitude_m      = fd.altitude_gps_m;
            fd.altitude_source = ALTITUDE_SRC_GPS;
        } else if (fd.altitude_baro_m != 0.0f) {
            fd.altitude_m      = fd.altitude_baro_m;
            fd.altitude_source = ALTITUDE_SRC_BARO;
        } else {
            // Hold last GPS altitude if we had one previously
            broker_fusion_data_t prev = {0};
            broker_fusion_read(&prev);
            if (prev.altitude_gps_m != 0.0f) {
                fd.altitude_m      = prev.altitude_gps_m;
                fd.altitude_gps_m  = prev.altitude_gps_m; // carry forward
                fd.altitude_source = ALTITUDE_SRC_LAST;
            } else {
                fd.altitude_source = ALTITUDE_SRC_NONE;
            }
        }

        // ── Activity (stub) ──────────────────────────────────────────────────
        // Future: read broker_imu (step count, gyro magnitude) + broker_hr (bpm)
        // Classify into activity_state_t, write fd.activity / fd.activity_conf.
        fd.activity      = ACTIVITY_UNKNOWN;
        fd.activity_conf = 0;

        // ── Write to broker ──────────────────────────────────────────────────
        broker_fusion_write(&fd);
    }
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

const char *fusion_get_chip_name(void) { return "FUSION"; }
const char *fusion_get_chip_desc(void) { return "Derived data aggregator"; }