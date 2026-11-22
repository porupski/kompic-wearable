/**
 * @file tile_registry.c
 * @brief Ordered tile descriptor table.
 *
 * THIS IS THE ONLY FILE THAT CHANGES WHEN ADDING A NEW MODULE TILE.
 *
 * To add a new tile:
 *   1. #include "foo_tile.h"
 *   2. Add one entry to s_tiles[] with &foo_tile_desc
 *   Done. Nothing else changes in lvgl_ui.c or anywhere else.
 *
 * Current order (Stage 38 flight recorder, 2026-10-04):
 *   Col  0: System                          — home tile (settings hub)
 *   Col  1: Flight   (FCM_FLIGHT recorder)  — default start tile (active-iteration)
 *   Col  2: GPS      (MAX-M10S)
 *   Col  3: Env      (BME688)
 *   Col  4: Haptic   (DRV2605)
 *   Col  5: RTC      (PCF85063)
 *   Col  6: IMU      (LSM6DSV16X)
 *
 * Workflow rule: the tile currently being iterated on gets moved to
 * Col 1 so bench access is a single swipe-down (via DEFAULT_TILE_COL
 * = 1 in ui_settings_screen.c). Flight is at Col 1 for Stage 38 flight
 * recording work; GPS moves to Col 2 for the duration.
 *
 * Removed: ECG tile (Stage 24 side quest, 2026-09-10). No ECG on Kompic
 * per feedback_no_ecg_on_kompic.
 *
 * STUBBED Stage 35: Health (MAX30101), Light (VEML6030), Compass
 * (LIS3MDL) tiles removed from registry alongside their driver init.
 * Tile source files still build; only the registry entries + includes
 * are gone. See Stage_35_Mk1b_I2C_Master_And_Sensor_Trim.md.
 *
 * Architecture: Blueprint 3 §5, Blueprint 5 §7 (revised)
 */

#include "tile_registry.h"
#include "haptic_tile.h"
#include "system_tile.h"
#include "gps_tile.h"
#include "rtc_tile.h"
#include "imu_tile.h"
#include "env_tile.h"
#include "flight_tile.h"

static tile_entry_t s_tiles[] = {
    { .desc = &system_tile_desc  },  // Col 0 — home / settings hub
    { .desc = &flight_tile_desc  },  // Col 1 — Stage 38 active-iteration slot
    { .desc = &gps_tile_desc     },  // Col 2
    { .desc = &env_tile_desc     },  // Col 3
    { .desc = &haptic_tile_desc  },  // Col 4
    { .desc = &rtc_tile_desc     },  // Col 5
    { .desc = &imu_tile_desc     },  // Col 6
};

#define TILE_COUNT  (sizeof(s_tiles) / sizeof(s_tiles[0]))

uint8_t tile_registry_count(void)
{
    return (uint8_t)TILE_COUNT;
}

tile_entry_t *tile_registry_get(void)
{
    static bool s_cols_assigned = false;
    if (!s_cols_assigned) {
        for (uint8_t i = 0; i < TILE_COUNT; i++) {
            s_tiles[i].col            = i;
            s_tiles[i].handle         = NULL;
            s_tiles[i].subtile_handle = NULL;
        }
        s_cols_assigned = true;
    }
    return s_tiles;
}

void tile_registry_set_handle(uint8_t idx, lv_obj_t *handle)
{
    if (idx < TILE_COUNT) s_tiles[idx].handle = handle;
}

void tile_registry_set_subtile_handle(uint8_t idx, lv_obj_t *subtile_handle)
{
    if (idx < TILE_COUNT) s_tiles[idx].subtile_handle = subtile_handle;
}
