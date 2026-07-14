/**
 * @file imu_tile.h
 * @brief QMI8658C IMU tile — Blueprint 5 §7 conformant.
 *
 * Displays: filtered roll/pitch, raw accel XYZ, raw gyro XYZ, temperature.
 * All data read from broker_imu_read() — no I2C, no direct driver access.
 * Core 1 only. All functions called inside lvgl_port_lock().
 */

#ifndef IMU_TILE_H
#define IMU_TILE_H

#include "lvgl.h"
#include "tile_registry.h"
#include "data_broker.h"   // ui_theme_t

// ── Tile descriptor — registered in tile_registry.c ──────────────────────────
extern const tile_desc_t imu_tile_desc;

// ── Tile interface — called by tile_registry via descriptor ───────────────────
void imu_tile_init(lv_obj_t *parent);
void imu_tile_update(void);
void imu_tile_apply_theme(ui_theme_t theme);

#endif // IMU_TILE_H