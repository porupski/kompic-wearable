/**
 * @file light_tile.h
 * @brief Ambient light sensor tile — public API.
 *
 * Type B tile: controls + toggles + live lux display.
 * Hardware: BH1750 @ 0x23 (proto). Future: VEML7700 @ 0x10 (same broker, same tile).
 *
 * Layout (Blueprint 10 §7):
 *   Header  : LED dot + "BH1750" title + power toggle
 *   Row 1   : Lux reading (live, from broker)
 *   Row 2   : Auto-brightness toggle
 *   Row 3   : Manual brightness slider (disabled when auto=ON)
 *   Row 4   : Theme toggle (DARK / LIGHT)
 *   Row 5   : Blue-light filter toggle
 *
 * Core 1 only. Reads broker_light_read(). Writes ui_settings via ui_broker.
 */

#ifndef LIGHT_TILE_H
#define LIGHT_TILE_H

#include "lvgl.h"
#include "ui_theme_colors.h"
#include "tile_registry.h"

void light_tile_init(lv_obj_t *parent);
void light_tile_update(void);
void light_tile_apply_theme(ui_theme_t theme);

// Blue-light amber overlay — call once from settings_screen_build(),
// passing the settings screen object as parent.
void light_tile_create_overlay(void);

extern const tile_desc_t light_tile_desc;

#endif // LIGHT_TILE_H