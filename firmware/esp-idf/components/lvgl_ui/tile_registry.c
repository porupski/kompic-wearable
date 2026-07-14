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
 * Current order (Phase 6, v7.2 part list):
 *   Col -1: Health   (MAX30101)             — wraps to the left of Col 0
 *   Col  0: Haptic   (DRV2605)
 *   Col  1: Light    (VEML6030)
 *   Col  2: System                          — default start tile
 *   Col  3: GPS      (MAX-M10S)             — has sub-tile
 *   Col  4: RTC      (PCF85063)
 *   Col  5: Env      (BME688)
 *   Col  6: Compass  (LIS3MDLTR)
 *   Col  7: IMU      (LSM6DSV16X)
 *   Col  8: ECG      (Qvar via LSM6DSV16X)  — stub label, no waveform yet
 *
 * Architecture: Blueprint 3 §5, Blueprint 5 §7 (revised)
 */

#include "tile_registry.h"
#include "haptic_tile.h"
#include "light_tile.h"
#include "system_tile.h"
#include "gps_tile.h"
#include "rtc_tile.h"
#include "compass_tile.h"
#include "imu_tile.h"
#include "env_tile.h"
#include "health_tile.h"
#include "ecg_tile.h"

static tile_entry_t s_tiles[] = {
    { .desc = &health_tile_desc  }, // -1
    { .desc = &haptic_tile_desc  },  // Col 0
    { .desc = &light_tile_desc   },  // Col 1
    { .desc = &system_tile_desc  },  // Col 2 — default start tile
    { .desc = &gps_tile_desc     },  // Col 3 — has sub-tile
    { .desc = &rtc_tile_desc     },  // Col 4
    { .desc = &env_tile_desc     },  // Col 5
    { .desc = &compass_tile_desc },  // Col 6
    { .desc = &imu_tile_desc     },  // Col 7
    { .desc = &ecg_tile_desc     },  // Col 8 — Phase 6 stub
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
