/**
 * @file battery_tile.h
 * @brief BQ25619 battery settings tile -- public API for Core 1 / LVGL.
 *
 * Tile type : A  (status + data rows, read-only)
 * Layout (410x502 CO5300):
 *   Header   : status LED + "BQ25619  Li-ion charger + PMID boost"
 *   Data rows: Voltage, SoC, Charging state, Power-good, Fault, Boost
 *
 * Core 1 only -- no I2C, no NVS, no GPIO.
 * Data flows: broker_battery_read() -> widgets.
 *
 * Architecture: Blueprint 3 §5, Blueprint 5 §4, Blueprint 10 (battery UX).
 */

#ifndef BATTERY_TILE_H
#define BATTERY_TILE_H

#include "lvgl.h"
#include "ui_theme_colors.h"

void battery_tile_init(lv_obj_t *parent);
void battery_tile_update(void);
void battery_tile_apply_theme(ui_theme_t theme);

#include "tile_registry.h"
extern const tile_desc_t battery_tile_desc;

#endif // BATTERY_TILE_H
