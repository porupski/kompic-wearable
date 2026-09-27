/**
 * @file max30101_stub.c
 * @brief Stage 35 stub replacement for max30101.c + health_tile.c.
 *
 * The real driver + tile use legacy driver/i2c.h. Stage 35 migrated the
 * whole binary to driver/i2c_master.h; ESP-IDF v5.5 aborts at boot if any
 * TU still references legacy I2C symbols. MAX30101 is on the stub list
 * (no PPG use case right now) so the cleanest fix is to drop the original
 * .c files from CMake SRCS and provide this shim with just the chip
 * identity strings kept exported for any residual linkage.
 *
 * Un-stub: restore max30101.c / health_tile.c to SRCS, delete this file.
 */

#include "max30101.h"

const char *max30101_get_chip_name(void) { return "MAX30101 (stubbed)"; }
const char *max30101_get_chip_desc(void) { return "HR / SpO2 / PPG (stubbed)"; }
