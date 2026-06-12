/**
 * @file ecg_tile.h
 * @brief LVGL tile stub for ECG waveform display -- Core 1.
 *
 * Phase 5 deliverable per brief 18: "Optional: display ECG waveform
 * strip (UI tile, Phase 2+)". This file is the stub -- it defines the
 * tile init entry point so the tile registry can include it without
 * the actual LVGL drawing code being written yet.
 *
 * The real implementation lands in Phase 2+ once:
 *   - UI tile registry pattern is settled (see lvgl_ui/).
 *   - The chosen ECG visualisation (scrolling strip, peak BPM readout)
 *     is sketched.
 *
 * Until then: the tile shows a placeholder label.
 */

#ifndef ECG_TILE_H
#define ECG_TILE_H

#include "esp_err.h"

/** @brief Create the ECG tile in the UI tree -- placeholder for now. */
esp_err_t ecg_tile_init(void);

#endif // ECG_TILE_H
