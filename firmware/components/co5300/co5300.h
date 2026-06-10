/**
 * @file co5300.h
 * @brief CO5300AF-51 AMOLED display driver (QSPI, 410x502, RGB888).
 *
 * Hardware authority: Kompic_Mk1_System_Instructions_v7.2.md  -- DISPLAY section.
 * Firmware version  : iv7.2.f0.0
 * Status            : SKELETON. Functions are stubbed; real impl lands when
 *                     hardware arrives. Register map and SPI plumbing are
 *                     complete enough to flash and observe bus activity.
 *
 * Bus  : SPI2 / FSPI, IOMUX, QSPI 4-wire.
 *        CS=GPIO10, CLK=GPIO12, D0=GPIO11, D1=GPIO13, D2=GPIO9, D3=GPIO14,
 *        RST=GPIO3, TE=GPIO45 (panel-driven, not consumed in v1).
 * Power: +3V3 only (no separate VBAT, no hardware enable -- SLPIN is the only
 *        power-down). See v7.2 DISPLAY section, "Panel power" and
 *        "Power control" paragraphs.
 *
 * Command framing (v7.2 DISPLAY: Interface):
 *   - Instruction frame: cmd 0x02, 24-bit address, optional data. 1-wire.
 *   - Pixel frame      : cmd 0x32, 24-bit address (0x003C00), pixel stream.
 *                        4-wire (QIO). RGB888, 3 bytes per pixel.
 *   - COLMOD 0x77 (RGB888) is mandatory. RGB565 QIO lane mapping is broken on
 *     this chip.
 *   - Panel native: 410 x 502, column offset 22.
 */

#ifndef CO5300_H
#define CO5300_H

#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// -- Identity ------------------------------------------------------------------
const char *co5300_get_chip_name(void);  // "CO5300AF-51"
const char *co5300_get_chip_desc(void);  // "2.06\" AMOLED QSPI panel"

// -- Panel geometry (v7.2 DISPLAY: Interface) ---------------------------------
#define CO5300_PANEL_WIDTH    410
#define CO5300_PANEL_HEIGHT   502
#define CO5300_COL_OFFSET     22       // column offset, panel-native
#define CO5300_PIXEL_BYTES    3        // RGB888

// -- Bus addresses (v7.2 DISPLAY: Interface) ----------------------------------
#define CO5300_PIXEL_ADDR     0x003C00U  // 24-bit pixel-frame address

// -- Command op-codes ----------------------------------------------------------
// Sent as the 8-bit "command" inside the QSPI cmd phase. The wire encoding is
// 0x02 (instruction frame) for these, except where noted.
#define CO5300_CMD_NOP        0x00
#define CO5300_CMD_SWRESET    0x01
#define CO5300_CMD_RDDID      0x04  // read display ID (may be 24-bit DDB)
#define CO5300_CMD_RDDST      0x09  // read display status
#define CO5300_CMD_SLPIN      0x10  // sleep in   (firmware-only power-down)
#define CO5300_CMD_SLPOUT     0x11  // sleep out  (wake)
#define CO5300_CMD_INVOFF     0x20
#define CO5300_CMD_INVON      0x21
#define CO5300_CMD_DISPOFF    0x28
#define CO5300_CMD_DISPON     0x29
#define CO5300_CMD_CASET      0x2A  // column address set
#define CO5300_CMD_RASET      0x2B  // row address set
#define CO5300_CMD_RAMWR      0x2C  // memory write (legacy 1-wire; pixels use 0x32 QIO)
#define CO5300_CMD_TEON       0x35  // tearing effect on
#define CO5300_CMD_TEOFF      0x34
#define CO5300_CMD_MADCTL     0x36  // memory access control (rotation/mirror)
#define CO5300_CMD_IDMOFF     0x38
#define CO5300_CMD_IDMON      0x39
#define CO5300_CMD_COLMOD     0x3A  // pixel format -- MUST be 0x77 (RGB888)
#define CO5300_CMD_WRDISBV    0x51  // write display brightness (8-bit)
#define CO5300_CMD_RDDISBV    0x52  // read  display brightness
#define CO5300_CMD_WRCTRLD    0x53  // write CTRL display

// Wire-level frame prefix opcodes (sent in the SPI cmd phase).
#define CO5300_WIRE_INSTRUCT  0x02  // 1-wire instruction frame
#define CO5300_WIRE_PIXELS    0x32  // 4-wire (QIO) pixel frame

// COLMOD value -- RGB888 mandatory. (v7.2 DISPLAY)
#define CO5300_COLMOD_RGB888  0x77

// -- Config struct -------------------------------------------------------------
typedef struct {
    spi_host_device_t spi_host;     // SPI2_HOST on this design
    gpio_num_t        pin_cs;       // GPIO10 (FSPICS0)
    gpio_num_t        pin_clk;      // GPIO12
    gpio_num_t        pin_d0;       // GPIO11
    gpio_num_t        pin_d1;       // GPIO13
    gpio_num_t        pin_d2;       // GPIO9
    gpio_num_t        pin_d3;       // GPIO14
    gpio_num_t        pin_rst;      // GPIO3   (panel + FW drive; see v7.2 strap note)
    gpio_num_t        pin_te;       // GPIO45  (placeholder; not consumed in v1)
    int               freq_hz;      // 40_000_000 to start (v7.2 KICKOFF target)
} co5300_config_t;

// Default config that mirrors v7.2 wiring. Use unless you are bench-testing on
// a breakout with different pins.
#define CO5300_CONFIG_DEFAULT() ((co5300_config_t){            \
    .spi_host = SPI2_HOST,                                     \
    .pin_cs   = GPIO_NUM_10,                                   \
    .pin_clk  = GPIO_NUM_12,                                   \
    .pin_d0   = GPIO_NUM_11,                                   \
    .pin_d1   = GPIO_NUM_13,                                   \
    .pin_d2   = GPIO_NUM_9,                                    \
    .pin_d3   = GPIO_NUM_14,                                   \
    .pin_rst  = GPIO_NUM_3,                                    \
    .pin_te   = GPIO_NUM_45,                                   \
    .freq_hz  = 40 * 1000 * 1000,                              \
})

// -- Device handle (opaque struct, allocated by co5300_init) ------------------
typedef struct co5300_dev_s *co5300_handle_t;

// -- Lifecycle -----------------------------------------------------------------

/**
 * @brief Bring up the SPI bus and CO5300 panel.
 *
 * Steps (when implemented for real hardware):
 *   1. spi_bus_initialize(SPI2_HOST, ...QIO...)
 *   2. spi_bus_add_device(... 40 MHz, half-duplex, cmd_bits=8, addr_bits=24)
 *   3. Drive RST low for >=10 ms, release, wait >=120 ms.
 *   4. Send init sequence: SLPOUT, COLMOD=0x77, CASET, RASET, DISPON.
 *   5. Set brightness to a safe default (e.g. 50 %).
 *
 * @return ESP_OK if SPI plumbing succeeded. Stub today.
 */
esp_err_t co5300_init(const co5300_config_t *cfg, co5300_handle_t *out_handle);

/**
 * @brief Release SPI bus and de-power the panel (SLPIN, then bus teardown).
 */
esp_err_t co5300_deinit(co5300_handle_t h);

// -- Command / data send -------------------------------------------------------

/**
 * @brief Send a single 8-bit command via the 0x02 (1-wire instruction) frame
 *        with no payload.
 */
esp_err_t co5300_write_command(co5300_handle_t h, uint8_t cmd);

/**
 * @brief Send a command + payload via the 0x02 instruction frame.
 *
 * @param data Pointer to payload bytes (1-wire after the command).
 * @param len  Payload length in bytes.
 */
esp_err_t co5300_write_command_with_data(co5300_handle_t h,
                                         uint8_t cmd,
                                         const uint8_t *data,
                                         size_t len);

/**
 * @brief Send pixel data via the 0x32 (4-wire QIO) frame.
 *
 * @param addr     24-bit panel address. Usually CO5300_PIXEL_ADDR.
 * @param pixels   RGB888 buffer, 3 bytes per pixel, packed.
 * @param count    Pixel count (not byte count).
 */
esp_err_t co5300_write_pixels(co5300_handle_t h,
                              uint32_t addr,
                              const uint8_t *pixels,
                              size_t count);

// -- Convenience wrappers ------------------------------------------------------

/**
 * @brief Set panel brightness via register 0x51 (WRDISBV).
 * @param pct 0..100 mapped linearly to 0..255.
 */
esp_err_t co5300_set_brightness(co5300_handle_t h, uint8_t pct);

/**
 * @brief Set the active window (CASET + RASET) using panel-native coordinates.
 *        Column offset CO5300_COL_OFFSET is applied automatically.
 */
esp_err_t co5300_set_window(co5300_handle_t h,
                            uint16_t x0, uint16_t y0,
                            uint16_t x1, uint16_t y1);

/**
 * @brief DISPOFF + SLPIN. Firmware-only power-down (no hw enable on this panel).
 */
esp_err_t co5300_sleep(co5300_handle_t h);

/**
 * @brief SLPOUT + full re-init + DISPON. Caller is expected to repaint.
 */
esp_err_t co5300_wake(co5300_handle_t h);

/**
 * @brief Read display status via 0x09 (RDDST) or 0x04 (RDDID).
 *        Used by test harness as a "chip is alive" handshake.
 *        See [DEFECT-001] -- WHO_AM_I semantics not certain.
 *
 * @param out_status  4-byte buffer (RDDST returns 32 bits).
 */
esp_err_t co5300_get_status(co5300_handle_t h, uint8_t out_status[4]);

#ifdef __cplusplus
}
#endif

#endif // CO5300_H
