/**
 * @file fc_modes_ppg_bcg.c
 * @brief FCM_PPG_BCG mode -- STUBBED at Stage 35.
 *
 * The PPG side of this mode is MAX30101-driven; MAX30101 was stubbed as
 * part of the Stage 35 sensor-surface trim. The whole mode is therefore
 * non-functional and lives here as a no-op so `run_ppg_bcg_mode()` (called
 * from field_capture.c's mode dispatcher) resolves at link time without
 * pulling any legacy driver/i2c.h symbols into the binary via max30101.c.
 *
 * Un-stub steps: restore max30101 driver init in boot_hw_init.c, restore
 * task_hr in boot_tasks.c, restore health_tile in tile_registry.c, then
 * restore the original body of this file from git history
 * (commit predating Stage 35 close).
 */

#include "fc_internal.h"
#include "esp_log.h"

static const char *TAG = "FC_PPG_BCG";

void run_ppg_bcg_mode(void)
{
    ESP_LOGW(TAG, "PPG+BCG mode stubbed at Stage 35 -- MAX30101 driver disabled.");
}
