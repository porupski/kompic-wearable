/**
 * @file boot_display.h
 * @brief Display bringup declarations -- Phase-1 stub.
 *
 * Real display path (CO5300 466x466 AMOLED via QSPI) is offline until the
 * FPC connector is re-worked. This header exists so lvgl_ui / tile files
 * that reference LCD_H_RES / LCD_V_RES / backlight_set_brightness compile.
 *
 * Constants match the CO5300 panel physical resolution. When Phase 2 brings
 * the display back, boot_display.c will drive the LEDC backlight against
 * `backlight_set_brightness()`; the Phase-1 stub is a no-op.
 */

#ifndef BOOT_DISPLAY_H
#define BOOT_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_H_RES   466
#define LCD_V_RES   466

/**
 * @brief Set the backlight duty (0-100). Phase-1 stub: no-op.
 *        Called from lvgl_ui lock screen and settings.
 */
void backlight_set_brightness(uint8_t pct);

#ifdef __cplusplus
}
#endif

#endif // BOOT_DISPLAY_H
