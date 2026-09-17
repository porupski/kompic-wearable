/**
 * @file co5300_panel.h
 * @brief CO5300AF-51 as an esp_lcd_panel_t. Stage 30.2.
 *
 * The old `co5300.c` driver owned the whole stack: SPI bus, transaction
 * framing, panel init sequence, sleep/wake, brightness. Stage 30 migrates
 * to Espressif's blessed layering:
 *
 *   spi_bus_initialize()          <-- caller (boot_display)
 *   esp_lcd_new_panel_io_spi(bus) <-- caller (io handle)
 *   co5300_new_panel(io, cfg)     <-- this file (panel handle)
 *   lvgl_port_add_disp(io+panel)  <-- lvgl_ui_display
 *
 * The panel handle implements esp_lcd_panel_t's vtable:
 *   - init / reset / del
 *   - draw_bitmap (calls esp_lcd_panel_io_tx_color with cmd = 0x32 prefix
 *                  + 0x003C00 addr; quad_mode on the io does the QIO burst)
 *   - disp_on_off / disp_sleep (DISPON/DISPOFF, SLPIN/SLPOUT via tx_param)
 *   - mirror / swap_xy (rewrites MADCTL for landscape 90 CW in Stage 30.3)
 *
 * Instruction wire format (from co5300.h + Arduino reference sketch):
 *   - Prefix byte 0x02, then 24-bit opcode-in-address (opcode << 8), then
 *     0..N param bytes. Packed as lcd_cmd_bits=32 for esp_lcd:
 *     cmd_word = (0x02 << 24) | (opcode << 8) | 0x00.
 *
 * Pixel wire format:
 *   - Prefix byte 0x32, then 24-bit address 0x003C00, then RGB888 stream.
 *   - cmd_word = (0x32 << 24) | 0x003C00 = 0x32003C00.
 *
 * Extension surface (not in esp_lcd_panel_t):
 *   - co5300_panel_set_brightness(panel, pct) -- WRDISBV (0x51) as an
 *     extra command; kept as a first-class API so backlight_set_brightness
 *     doesn't need to reach past the panel handle.
 */

#ifndef CO5300_PANEL_H
#define CO5300_PANEL_H

#define CO5300_PANEL_DRIVER_VERSION  "0.1.3"

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vendor-specific config for the CO5300 panel wrapper.
 *
 * The panel geometry + pixel format are baked into the driver (CO5300
 * mandates COLMOD 0x77 = RGB888 per its datasheet; the RGB565 QIO lane
 * mapping is broken on this silicon).
 */
typedef struct {
    gpio_num_t reset_gpio_num;   /*!< RST pin (active low). -1 = tie high externally. */
    bool       reset_active_high;/*!< Kept false for the CO5300 wiring. */
    uint16_t   x_gap;            /*!< Panel column offset (22 for CO5300 portrait). */
    uint16_t   y_gap;            /*!< Panel row offset (0 for CO5300 portrait). */
    bool       rotate_90_cw;     /*!< Land Stage 30.3: MV=1 MX=1 MADCTL for landscape. */
} co5300_panel_vendor_config_t;

/**
 * @brief Create the CO5300 esp_lcd_panel handle.
 *
 * The panel handle uses the io handle for all SPI activity; the caller
 * remains the owner of both. `init()` runs the vendor init sequence
 * (SLPOUT, COLMOD, MADCTL, WRCTRLD, DISPON, WRDISBV, INVOFF) so callers
 * do not need to send the init opcodes themselves.
 *
 * @param[in]  io      Panel IO handle from esp_lcd_new_panel_io_spi.
 * @param[in]  vendor  Vendor config (RST pin, gaps, rotation).
 * @param[out] ret_panel Returned panel handle.
 */
esp_err_t co5300_new_panel(esp_lcd_panel_io_handle_t io,
                           const co5300_panel_vendor_config_t *vendor,
                           esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief Set panel brightness via WRDISBV (0x51). Panel-side, not backlight.
 *
 * @param[in] panel  Handle returned by co5300_new_panel.
 * @param[in] pct    0..100 mapped linearly to 0..255.
 */
esp_err_t co5300_panel_set_brightness(esp_lcd_panel_handle_t panel, uint8_t pct);

#ifdef __cplusplus
}
#endif

#endif /* CO5300_PANEL_H */
