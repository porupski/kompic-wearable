/**
 * @file haptic_tile.h
 * @brief Haptic motor tile — public API.
 *
 * Type B tile: status display + calibrate button + test button.
 * Hardware: DRV2605 @ 0x5A (clone, WHO_AM_I=0xE0).
 *
 * Core 1 only. Reads broker_haptic_read(). Calibration request via haptic_request_calibration().
 */

#ifndef HAPTIC_TILE_H
#define HAPTIC_TILE_H

#include "lvgl.h"
#include "ui_theme_colors.h"
#include "tile_registry.h"

void haptic_tile_init(lv_obj_t *parent);
void haptic_tile_update(void);
void haptic_tile_apply_theme(ui_theme_t theme);

extern const tile_desc_t haptic_tile_desc;

#endif // HAPTIC_TILE_H