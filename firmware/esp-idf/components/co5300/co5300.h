/**
 * @file co5300.h
 * @brief CO5300AF-51 opcodes, geometry, and wire-format constants.
 *
 * Stage 30.2: the panel driver moved to esp_lcd_panel_t via co5300_panel.c.
 * This header now carries only the vendor-side constants (opcodes, pixel
 * format, panel dimensions, MADCTL wire framing) that both boot_display.c
 * and co5300_panel.c reference. See co5300_panel.h for the panel-creation
 * API.
 *
 * Hardware authority: Kompic_Mk1_System_Instructions_v7.2.md  -- DISPLAY.
 *
 * Wire framing summary:
 *   - Instruction frame: cmd byte 0x02, 24-bit address (opcode << 8),
 *                        0..N bytes of param. All phases on 4 lines when
 *                        the underlying io is quad_mode.
 *   - Pixel frame      : cmd byte 0x32, 24-bit address 0x003C00, RGB888
 *                        stream. All phases on 4 lines (QIO).
 *   - COLMOD 0x77 (RGB888) is mandatory. RGB565 QIO lane mapping is broken
 *     on this silicon.
 *   - Panel native: 410 x 502, column offset 22.
 */

#ifndef CO5300_H
#define CO5300_H

/* Version tag kept for STATUS provenance. Bumped when opcodes / wire
 * format constants change. Stage 30.2: bumped for the header trim +
 * panel-wrapper handoff.
 */
#define CO5300_DRIVER_VERSION  "0.3.3"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Identity ------------------------------------------------------------ */
const char *co5300_get_chip_name(void);   /* "CO5300AF-51" */
const char *co5300_get_chip_desc(void);   /* "2.06\" AMOLED QSPI panel" */

/* -- Panel geometry ------------------------------------------------------ */
#define CO5300_PANEL_WIDTH    410
#define CO5300_PANEL_HEIGHT   502
#define CO5300_COL_OFFSET     22        /* portrait column offset (native) */
#define CO5300_PIXEL_BYTES    3         /* RGB888 */

/* -- Bus addresses ------------------------------------------------------- */
#define CO5300_PIXEL_ADDR     0x003C00U /* 24-bit pixel-frame address */

/* -- Command op-codes (sent as opcode inside the 32-bit instruction cmd
 *    word; see co5300_panel.c co5300_instr_cmd()).
 */
#define CO5300_CMD_NOP        0x00
#define CO5300_CMD_SWRESET    0x01
#define CO5300_CMD_RDDID      0x04
#define CO5300_CMD_RDDST      0x09
#define CO5300_CMD_SLPIN      0x10
#define CO5300_CMD_SLPOUT     0x11
#define CO5300_CMD_INVOFF     0x20
#define CO5300_CMD_INVON      0x21
#define CO5300_CMD_DISPOFF    0x28
#define CO5300_CMD_DISPON     0x29
#define CO5300_CMD_CASET      0x2A
#define CO5300_CMD_RASET      0x2B
#define CO5300_CMD_RAMWR      0x2C
#define CO5300_CMD_TEON       0x35
#define CO5300_CMD_TEOFF      0x34
#define CO5300_CMD_MADCTL     0x36
#define CO5300_CMD_IDMOFF     0x38
#define CO5300_CMD_IDMON      0x39
#define CO5300_CMD_COLMOD     0x3A
#define CO5300_CMD_WRDISBV    0x51
#define CO5300_CMD_RDDISBV    0x52
#define CO5300_CMD_WRCTRLD    0x53

/* Wire-level frame prefix opcodes (top byte of the 32-bit cmd word). */
#define CO5300_WIRE_INSTRUCT  0x02
#define CO5300_WIRE_PIXELS    0x32

/* COLMOD value -- RGB888 mandatory on this panel. */
#define CO5300_COLMOD_RGB888  0x77

#ifdef __cplusplus
}
#endif

#endif /* CO5300_H */
