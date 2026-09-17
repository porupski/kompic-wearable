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
 * Stage 30.2 layering:
 *   spi_bus_initialize(SPI2_HOST)           <-- boot_display_init
 *   esp_lcd_new_panel_io_spi(quad_mode=1)   <-- boot_display_init, io handle
 *   co5300_new_panel(io, vendor cfg)        <-- boot_display_init, panel handle
 *   lvgl_port_add_disp(io + panel)          <-- lvgl_ui_display_setup
 *
 * The io + panel handles are exposed via getters below so lvgl_ui_display
 * can hand them to esp_lvgl_port. backlight_set_brightness forwards to
 * co5300_panel_set_brightness (WRDISBV) once the panel is up.
 */

#ifndef BOOT_DISPLAY_H
#define BOOT_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Panel LOGICAL dimensions. Portrait native = 410 x 502. Stage 30.3 will
 * flip to landscape via esp_lvgl_port's rotation config (swap_xy + mirror).
 * The LCD_*_RES constants stay in portrait terms; the rotated setup lives
 * inside lvgl_ui_display.
 */
#define LCD_H_RES     410
#define LCD_V_RES     502

/**
 * @brief Bring up the CO5300 panel (SPI2 QSPI bus + esp_lcd panel IO +
 *        CO5300 panel driver + panel init sequence).
 *
 *        Presence is inferred from a CST9217 I2C probe on bus 0. If the
 *        touch chip does not ACK, the panel bring-up is skipped and the
 *        function returns ESP_ERR_NOT_FOUND -- lvgl_ui_display then boots
 *        headless (or forced-no-panel under LVGL_FORCE).
 *
 *        Must run AFTER boot_hw_init() (needs g_i2c_mutex + I2C0 up) and
 *        BEFORE boot_tasks_start().
 */
esp_err_t boot_display_init(void);

/** @brief True after a successful boot_display_init(). */
bool boot_display_is_present(void);

/** @brief Set panel brightness 0..100 (WRDISBV). No-op when absent. */
void backlight_set_brightness(uint8_t pct);

/**
 * @brief esp_lcd panel-IO handle for the CO5300, or NULL when absent.
 *        Consumed by lvgl_ui_display -> lvgl_port_add_disp.
 */
esp_lcd_panel_io_handle_t boot_display_get_panel_io(void);

/**
 * @brief esp_lcd panel handle for the CO5300, or NULL when absent.
 *        Consumed by lvgl_ui_display -> lvgl_port_add_disp.
 */
esp_lcd_panel_handle_t    boot_display_get_panel(void);

/**
 * @brief True if boot_display_init() also brought up the CST9217 touch chip.
 *        Panel can be present without touch (touch failure is logged +
 *        non-fatal). Under LVGL_FORCE with no hardware this is always false.
 */
bool boot_display_touch_is_present(void);

/**
 * @brief DISPOFF+SLPIN via esp_lcd_panel_disp_sleep(true). No-op when absent.
 *        Used by field_capture's idle-timeout sleep path.
 */
esp_err_t boot_display_sleep(void);

/**
 * @brief SLPOUT+DISPON via esp_lcd_panel_disp_sleep(false). No-op when absent.
 */
esp_err_t boot_display_wake(void);

/** @brief True if panel is present AND currently in sleep. */
bool boot_display_is_asleep(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_DISPLAY_H */
