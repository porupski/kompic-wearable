/**
 * @file lis3mdl_stub.c
 * @brief Stage 35 stub replacement for lis3mdl.c + compass_tile.c.
 *
 * See max30101_stub.c for the rationale.
 */

#include "lis3mdl.h"

const char *lis3mdl_get_chip_name(void) { return "LIS3MDLTR (stubbed)"; }
const char *lis3mdl_get_chip_desc(void) { return "3-axis magnetometer (stubbed)"; }
