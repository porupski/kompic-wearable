/**
 * @file veml6030_cmd.h
 * @brief VEML6030 + backlight command surface -- Module_Blueprint.md §3.
 *
 * One surface shared by the CLI `LIGHT` verb and light_tile event handlers.
 * Covers the ambient-light sensor AND the backlight/UI-settings adjacent
 * to it (auto-brightness curve consumes VEML lux, so they live together).
 *
 * Theme (dark/light) is intentionally NOT here -- it belongs to a future
 * ui_settings command surface.
 */
#ifndef VEML6030_CMD_H
#define VEML6030_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// -- enable (light sensor) ---------------------------------------------------
esp_err_t veml6030_cmd_enable_set(bool on);
bool      veml6030_cmd_enable_get(void);

// -- auto brightness (backlight driven by lux LUT) ---------------------------
esp_err_t veml6030_cmd_auto_brightness_set(bool on);
bool      veml6030_cmd_auto_brightness_get(void);

// -- manual brightness (0..100 %) --------------------------------------------
// Ignored while auto is on. On disable of auto, current saved value is
// re-applied on next tile update.
esp_err_t veml6030_cmd_brightness_set(uint8_t pct);
uint8_t   veml6030_cmd_brightness_get(void);

// -- blue-light filter overlay ------------------------------------------------
esp_err_t veml6030_cmd_blue_light_set(bool on);
bool      veml6030_cmd_blue_light_get(void);

// -- dump --------------------------------------------------------------------
void veml6030_cmd_dump(void);

// -- status summary ----------------------------------------------------------
void veml6030_cmd_status_summary(char *out, size_t max);

#endif // VEML6030_CMD_H
