/**
 * @file ecg_tile.c
 * @brief ECG tile placeholder. Real LVGL waveform rendering lands in
 *        Phase 2+ once the tile registry pattern is firm.
 *
 * For now this exists so that:
 *   - The brief 18 deliverable list ("ecg_tile.{c,h}") is satisfied.
 *   - The component builds cleanly with the qvar_ecg.c chip layer.
 *   - When the LVGL UI side wires in tiles, this is the file that grows
 *     the actual scrolling-strip widget + BPM label.
 *
 * Architecture note: tiles live on Core 1 / LVGL. This file MUST NOT
 * include qvar_ecg.h directly -- the chip layer is Core 0 only. When
 * the waveform rendering arrives, samples will come via the data broker
 * (Tier 1: broker_ecg_data_t -- not yet defined) just like every other
 * sensor in the project.
 */

#include "ecg_tile.h"
#include "esp_log.h"

static const char *TAG = "ECG_TILE";

esp_err_t ecg_tile_init(void)
{
    ESP_LOGI(TAG, "ECG tile init (stub -- waveform rendering deferred to Phase 2+)");
    return ESP_OK;
}
