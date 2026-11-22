/**
 * @file flight_tile.h
 * @brief FCM_FLIGHT recorder tile (Stage 38). Read-only status surface:
 *        elapsed time, sample count, SD write status, GPS fix. The mode
 *        is still driven by encoder click / CLI; the tile just shows
 *        what's happening.
 */

#ifndef FLIGHT_TILE_H
#define FLIGHT_TILE_H

#include "tile_registry.h"

extern const tile_desc_t flight_tile_desc;

#endif  // FLIGHT_TILE_H
