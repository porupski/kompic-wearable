/**
 * @file co5300_panel_io.h
 * @brief CO5300-correct esp_lcd_panel_io_t for QSPI wire semantics.
 *
 * Stage 30.2 attempt 2: the stock `esp_lcd_new_panel_io_spi` with
 * `quad_mode = 1` sends only the color DATA phase on 4 lines; the cmd word
 * that leads each `tx_color` chunk still goes on 1 line. CO5300 needs QIO
 * on cmd + addr + data for pixel writes -- otherwise the panel sees a
 * garbled prefix and the pixel stream lands as noise.
 *
 * This factory returns an esp_lcd_panel_io_handle_t implementing the same
 * vtable but with the CO5300 wire format we already proved with our old
 * custom driver:
 *
 *   tx_param  (init / sleep / brightness):
 *     cmd_bits=8  (prefix 0x02, top byte of the 32-bit cmd word),
 *     addr_bits=24 (opcode << 8, lower 24 bits of the cmd word),
 *     MULTILINE_CMD | MULTILINE_ADDR (quad on cmd + addr),
 *     data phase (0..N bytes of param) on 1 line, polling-mode.
 *
 *   tx_color  (pixel burst):
 *     first chunk: cmd_bits=8 (prefix 0x32), addr_bits=24 (0x003C00),
 *                  MODE_QIO on the whole transaction (quad on ALL phases).
 *     subsequent chunks: VARIABLE_CMD/ADDR/DUMMY set to 0 bits so the
 *                        stream continues seamlessly, MODE_QIO, CS kept
 *                        low via SPI_TRANS_CS_KEEP_ACTIVE.
 *     completion: the last chunk carries a post-trans callback that
 *                  invokes the user's on_color_trans_done (registered via
 *                  esp_lcd_panel_io_register_event_callbacks) so
 *                  esp_lvgl_port can call lv_display_flush_ready.
 */

#ifndef CO5300_PANEL_IO_H
#define CO5300_PANEL_IO_H

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t        cs_gpio_num;
    int               pclk_hz;              /* e.g. 40 * 1000 * 1000 */
    size_t            trans_queue_depth;    /* SPI queue depth for pixel chunks */
    size_t            pixel_chunk_bytes;    /* max bytes per pixel-burst chunk */
} co5300_panel_io_config_t;

/**
 * @brief Create the CO5300-specific SPI panel_io. Caller must have
 *        spi_bus_initialize()'d the given host with QSPI 4-wire lines.
 */
esp_err_t co5300_panel_io_new_spi(const co5300_panel_io_config_t *config,
                                  esp_lcd_panel_io_handle_t *ret_io);

#ifdef __cplusplus
}
#endif

#endif /* CO5300_PANEL_IO_H */
