/**
 * @file boot_display.h
 * @brief CO5300 AMOLED bring-up + backlight facade for lvgl_ui / tiles.
 *
 * boot_display_init() probes for the CST9217 touch controller on I2C bus 0
 * as a proxy for panel presence (touch + panel share the display FPC). On a
 * board without the display module -- iv7.1 today, or a Mk1b that failed
 * post-reflow-A -- the probe fails, the panel init is skipped, and the rest
 * of the firmware runs identically. Callers ignore the return code if they
 * do not care.
 *
 * backlight_set_brightness() forwards to CO5300 register 0x51 (WRDISBV) once
 * the panel is up; before boot_display_init() (or when the panel was skipped)
 * it is a no-op so lvgl_ui / settings-screen code can call it unconditionally.
 */

#ifndef BOOT_DISPLAY_H
#define BOOT_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "co5300.h"     // co5300_handle_t for the getter below

#ifdef __cplusplus
extern "C" {
#endif

// Panel logical size exposed to lvgl_ui / tile code. The CO5300 is a
// 410x502 physical panel; we treat it as a square 466x466 addressable region
// (matching the visible circle) for layout convenience -- CO5300_COL_OFFSET
// in the driver adds the panel's column offset back on.
#define LCD_H_RES   466
#define LCD_V_RES   466

/**
 * @brief Bring up the CO5300 panel (SPI2 QSPI + reset + init sequence).
 *        Presence is inferred from a CST9217 I2C probe on bus 0. If the touch
 *        chip does not ACK with the expected signature, the panel init is
 *        skipped and the function returns ESP_ERR_NOT_FOUND.
 *
 * Must run AFTER boot_hw_init() (needs g_i2c_mutex + I2C0 up) and BEFORE
 * boot_tasks_start().
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no display module is
 *         detected, other esp_err_t on driver failure.
 */
esp_err_t boot_display_init(void);

/**
 * @brief True if boot_display_init() successfully brought the panel up. LVGL
 *        wiring (§4.7 of Stage 21) uses this to decide whether to start the
 *        port task + tile registry.
 */
bool boot_display_is_present(void);

/**
 * @brief Set the panel brightness in percent (0..100). No-op if the panel was
 *        never brought up. Safe to call from any task.
 */
void backlight_set_brightness(uint8_t pct);

/**
 * @brief Return the CO5300 device handle owned by boot_display, or NULL if the
 *        panel is absent (or was not brought up). Used by the LVGL display
 *        module in Stage 22 §4.1b so its flush callback can call
 *        co5300_set_window() / co5300_write_pixels() directly. When the caller
 *        gets NULL under `LVGL_FORCE ON`, it treats the flush as a no-op.
 */
co5300_handle_t boot_display_get_co5300(void);

/**
 * @brief True if boot_display_init() also brought up the CST9217 touch chip
 *        (Stage 22 §4.2). Distinct from boot_display_is_present(): the panel
 *        can be present but the touch init could have failed independently,
 *        and Mk1b bring-up must not block the panel on a bad touch chip.
 *        Under LVGL_FORCE (no real hardware) this is always false.
 */
bool boot_display_touch_is_present(void);

#ifdef __cplusplus
}
#endif

#endif // BOOT_DISPLAY_H
