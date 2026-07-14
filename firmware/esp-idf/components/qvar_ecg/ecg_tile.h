/**
 * @file ecg_tile.h
 * @brief LVGL tile stub for ECG waveform display -- Core 1.
 *
 * Phase 5 deliverable per brief 18: "Optional: display ECG waveform
 * strip (UI tile, Phase 2+)". Phase 6 wires it into the tile registry
 * with a placeholder layout so the tileview has a working ECG slot.
 *
 * Until real Qvar register-map verification (Phase 20 datasheet pass),
 * the tile shows a "Stub" label and no waveform.
 *
 * Architecture: Blueprint 3 §5 / Phase 5 brief
 */

#ifndef ECG_TILE_H
#define ECG_TILE_H

#include "esp_err.h"
#include "tile_registry.h"

/** @brief Legacy entry point. Returns ESP_OK; the tile_desc below is the
 *         actual contract used by tile_registry_get(). */
esp_err_t ecg_tile_init(void);

/** @brief Exported tile descriptor for tile_registry.c. */
extern const tile_desc_t ecg_tile_desc;

#endif // ECG_TILE_H
