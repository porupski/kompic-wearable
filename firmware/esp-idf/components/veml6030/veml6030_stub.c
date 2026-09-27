/**
 * @file veml6030_stub.c
 * @brief Stage 35 stub replacement for veml6030.c + veml6030_cmd.c + light_tile.c.
 *
 * See max30101_stub.c for the rationale.
 */

#include "veml6030.h"

const char *veml6030_get_chip_name(void) { return "VEML6030 (stubbed)"; }
const char *veml6030_get_chip_desc(void) { return "Ambient light sensor (stubbed)"; }

// Called from lvgl_ui.c during UI init to build the blue-light overlay LVGL
// object. With VEML6030 stubbed there's no auto-brightness / blue-light UX
// to drive; the overlay is a no-op.
void light_tile_create_overlay(void) { /* stubbed at Stage 35 */ }
