/**
 * @file co5300_panel.c
 * @brief esp_lcd_panel_t implementation for the CO5300AF-51 AMOLED.
 *
 * See co5300_panel.h for the layering + wire-format notes.
 *
 * Instruction wire (Arduino ref sketch, verified on Mk1b):
 *   [0x02 | opcode << 8]  32-bit cmd word on 4 lines (QIO)
 *   [ ...param bytes... ] short single-byte payload
 *
 * Pixel wire:
 *   [0x32 | 0x003C00]     32-bit cmd word on 4 lines (QIO)
 *   [ RGB888 stream ]     many KB on 4 lines (QIO, DMA'd)
 *
 * We rely on esp_lcd_panel_io_spi with quad_mode=1, which puts BOTH the
 * 32-bit cmd word AND the params/color stream on 4 lines. The plan
 * (Stage_30_Mk1b_Display_Port_Migration.md §2.2 Risk) flags that the
 * CO5300 wants single-wire data on the instruction frame -- the sketch
 * sends it that way. If bench proves the panel rejects quad-mode params,
 * fall back to a two-io-handle setup (one io with quad_mode=0 for
 * instructions, one with quad_mode=1 for pixels) -- both share the same
 * SPI bus and only differ in the flags.
 */

#include "co5300_panel.h"
#include "co5300.h"                 /* opcodes + PIXEL_ADDR + geometry */

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "co5300_panel";

/* -------- Identity (moved here after Stage 30.2 co5300.c deletion) -------- */
const char *co5300_get_chip_name(void) { return "CO5300AF-51"; }
const char *co5300_get_chip_desc(void) { return "2.06\" AMOLED QSPI panel"; }

/* -------- 32-bit cmd-word helpers ----------------------------------------- */

/* Instruction frame: prefix 0x02 in the top byte, opcode in the next byte,
 * bottom 16 bits zero. Together this reads on the wire as 0x02 followed
 * by 24-bit (opcode << 8) address, matching the CO5300 wire format that
 * our previous custom driver used.
 */
static inline uint32_t co5300_instr_cmd(uint8_t opcode)
{
    return ((uint32_t)0x02U << 24) | ((uint32_t)opcode << 8);
}

/* Pixel frame: prefix 0x32, then 24-bit pixel address CO5300_PIXEL_ADDR
 * (0x003C00). Together 0x32003C00.
 */
static inline uint32_t co5300_pixel_cmd(void)
{
    return ((uint32_t)CO5300_WIRE_PIXELS << 24) | CO5300_PIXEL_ADDR;
}

/* -------- Panel context --------------------------------------------------- */

typedef struct co5300_panel_ctx_s {
    esp_lcd_panel_t             base;         /* vtable -- must be first */
    esp_lcd_panel_io_handle_t   io;
    gpio_num_t                  rst_gpio;
    bool                        rst_active_high;
    bool                        swap_xy;
    bool                        mirror_x;
    bool                        mirror_y;
    uint16_t                    x_gap;
    uint16_t                    y_gap;
    uint8_t                     madctl;       /* current MADCTL value */
    bool                        awake;
    bool                        on;
} co5300_panel_ctx_t;

/* -------- Small SPI helpers ---------------------------------------------- */

static esp_err_t co5300_send_cmd(esp_lcd_panel_io_handle_t io, uint8_t opcode)
{
    return esp_lcd_panel_io_tx_param(io, co5300_instr_cmd(opcode), NULL, 0);
}

static esp_err_t co5300_send_cmd_data(esp_lcd_panel_io_handle_t io,
                                      uint8_t opcode,
                                      const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(io, co5300_instr_cmd(opcode), data, len);
}

/* -------- Reset pulse (mirrors old co5300.c) ----------------------------- */

static void co5300_hw_reset(co5300_panel_ctx_t *ctx)
{
    if (ctx->rst_gpio < 0) return;
    const int active = ctx->rst_active_high ? 1 : 0;
    const int idle   = ctx->rst_active_high ? 0 : 1;

    gpio_set_level(ctx->rst_gpio, idle);   vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(ctx->rst_gpio, active); vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(ctx->rst_gpio, idle);   vTaskDelay(pdMS_TO_TICKS(300));
}

/* -------- Vtable ops ----------------------------------------------------- */

static esp_err_t co5300_op_reset(esp_lcd_panel_t *panel)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    co5300_hw_reset(ctx);
    return ESP_OK;
}

static esp_err_t co5300_op_init(esp_lcd_panel_t *panel)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    esp_lcd_panel_io_handle_t io = ctx->io;

    /* SLPOUT + 120 ms settling. */
    ESP_RETURN_ON_ERROR(co5300_send_cmd(io, CO5300_CMD_SLPOUT),
                        TAG, "SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Command-page 0 + SPI RAM write enable (per Arduino reference). */
    const uint8_t page0  = 0x00;
    const uint8_t ram_en = 0x80;
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, 0xFE, &page0, 1),
                        TAG, "page0 failed");
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, 0xC4, &ram_en, 1),
                        TAG, "ram_en failed");

    /* COLMOD mandatory 0x77 = RGB888. */
    const uint8_t colmod = CO5300_COLMOD_RGB888;
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, CO5300_CMD_COLMOD, &colmod, 1),
                        TAG, "COLMOD failed");

    /* MADCTL. Start with the current ctx->madctl (set to 0x00 or 0x60 in
     * co5300_new_panel per rotate_90_cw); swap_xy/mirror mutate it later.
     */
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, CO5300_CMD_MADCTL,
                                             &ctx->madctl, 1),
                        TAG, "MADCTL failed");

    /* WRCTRLD 0x20 = brightness control on; HBM 0x63 = max; DISPON;
     * WRDISBV 0x51 = ~80%; 0x58 sunlight off; INVOFF.
     */
    const uint8_t wrctrld  = 0x20;
    const uint8_t hbm_max  = 0xFF;
    const uint8_t bright   = 0xD0;  /* ~80 % */
    const uint8_t sun_off  = 0x00;
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, CO5300_CMD_WRCTRLD, &wrctrld, 1),
                        TAG, "WRCTRLD failed");
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, 0x63, &hbm_max, 1),
                        TAG, "HBM failed");
    ESP_RETURN_ON_ERROR(co5300_send_cmd(io, CO5300_CMD_DISPON),
                        TAG, "DISPON failed");
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, CO5300_CMD_WRDISBV, &bright, 1),
                        TAG, "WRDISBV failed");
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, 0x58, &sun_off, 1),
                        TAG, "sun_off failed");
    ESP_RETURN_ON_ERROR(co5300_send_cmd(io, CO5300_CMD_INVOFF),
                        TAG, "INVOFF failed");

    vTaskDelay(pdMS_TO_TICKS(20));

    ctx->awake = true;
    ctx->on    = true;
    return ESP_OK;
}

static esp_err_t co5300_op_del(esp_lcd_panel_t *panel)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    if (ctx->rst_gpio >= 0) {
        gpio_reset_pin(ctx->rst_gpio);
    }
    free(ctx);
    return ESP_OK;
}

static esp_err_t co5300_op_draw_bitmap(esp_lcd_panel_t *panel,
                                       int x_start, int y_start,
                                       int x_end,   int y_end,
                                       const void *color_data)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    esp_lcd_panel_io_handle_t io = ctx->io;

    /* esp_lcd convention: x_end / y_end are EXCLUSIVE. CO5300 CASET/RASET
     * take an inclusive last-pixel index, so subtract 1.
     */
    if (x_start >= x_end || y_start >= y_end) return ESP_ERR_INVALID_ARG;
    x_end -= 1;
    y_end -= 1;

    /* With MADCTL 0x60 (MV=1 MX=1) the CO5300 scans its 410x502 RAM as a
     * 502x410 landscape image. LVGL hands us landscape coords in that mode
     * (x in [0..501], y in [0..409]); native col = y + COL_OFFSET,
     * native row = x. Portrait: native col = x + COL_OFFSET, native row = y.
     */
    uint16_t cx0, cx1, ry0, ry1;
    if (ctx->swap_xy) {
        cx0 = (uint16_t)(y_start + ctx->x_gap);
        cx1 = (uint16_t)(y_end   + ctx->x_gap);
        ry0 = (uint16_t)(x_start + ctx->y_gap);
        ry1 = (uint16_t)(x_end   + ctx->y_gap);
    } else {
        cx0 = (uint16_t)(x_start + ctx->x_gap);
        cx1 = (uint16_t)(x_end   + ctx->x_gap);
        ry0 = (uint16_t)(y_start + ctx->y_gap);
        ry1 = (uint16_t)(y_end   + ctx->y_gap);
    }

    const uint8_t caset[4] = {
        (uint8_t)(cx0 >> 8), (uint8_t)cx0,
        (uint8_t)(cx1 >> 8), (uint8_t)cx1,
    };
    const uint8_t raset[4] = {
        (uint8_t)(ry0 >> 8), (uint8_t)ry0,
        (uint8_t)(ry1 >> 8), (uint8_t)ry1,
    };
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, CO5300_CMD_CASET, caset, sizeof caset),
                        TAG, "CASET failed");
    ESP_RETURN_ON_ERROR(co5300_send_cmd_data(io, CO5300_CMD_RASET, raset, sizeof raset),
                        TAG, "RASET failed");

    /* RAMWR opens the RAM-write window; the CO5300 needs it after CASET/RASET
     * so the following 0x32 pixel burst lands at the intended coordinates.
     * The bench-verified pre-Stage-30 co5300_set_window sent this every flush;
     * dropping it produced glitched blotches on the first real flush of the
     * custom panel_io (Stage 30 §4.2e). Arduino ref: sketch line "cmd(0x2C)".
     */
    ESP_RETURN_ON_ERROR(co5300_send_cmd(io, CO5300_CMD_RAMWR),
                        TAG, "RAMWR failed");

    /* RGB888 stream: 3 bytes per pixel; our panel_io chunks internally per
     * pixel_chunk_bytes. quad_mode on the io means the 0x32 prefix + 24-bit
     * addr + data all go on 4 lines (QIO).
     */
    const size_t pixels = (size_t)(x_end - x_start + 1) * (size_t)(y_end - y_start + 1);
    const size_t bytes  = pixels * CO5300_PIXEL_BYTES;
    return esp_lcd_panel_io_tx_color(io, co5300_pixel_cmd(), color_data, bytes);
}

static esp_err_t co5300_op_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    const uint8_t opcode = invert ? CO5300_CMD_INVON : CO5300_CMD_INVOFF;
    return co5300_send_cmd(ctx->io, opcode);
}

/* MADCTL bits (per Arduino ref + our old driver):
 *   0x80 = MY  (row flip)
 *   0x40 = MX  (col flip)
 *   0x20 = MV  (row/col swap = landscape)
 *   0x08 = BGR (color order -- 1 = panel expects [B,G,R] byte order)
 *
 * Stage 30.2: BGR bit set by default so LVGL's native RGB888 memory order
 * [B,G,R] can be sent to the panel verbatim, no per-flush byte swap.
 */
#define CO5300_MADCTL_BGR   0x08

static esp_err_t co5300_apply_madctl(co5300_panel_ctx_t *ctx)
{
    uint8_t v = CO5300_MADCTL_BGR;
    if (ctx->swap_xy)  v |= 0x20;
    if (ctx->mirror_x) v |= 0x40;
    if (ctx->mirror_y) v |= 0x80;
    ctx->madctl = v;
    return co5300_send_cmd_data(ctx->io, CO5300_CMD_MADCTL, &ctx->madctl, 1);
}

static esp_err_t co5300_op_mirror(esp_lcd_panel_t *panel, bool x_axis, bool y_axis)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    ctx->mirror_x = x_axis;
    ctx->mirror_y = y_axis;
    return co5300_apply_madctl(ctx);
}

static esp_err_t co5300_op_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    ctx->swap_xy = swap_axes;
    return co5300_apply_madctl(ctx);
}

static esp_err_t co5300_op_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    ctx->x_gap = (uint16_t)x_gap;
    ctx->y_gap = (uint16_t)y_gap;
    return ESP_OK;
}

static esp_err_t co5300_op_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    const uint8_t opcode = on ? CO5300_CMD_DISPON : CO5300_CMD_DISPOFF;
    esp_err_t err = co5300_send_cmd(ctx->io, opcode);
    if (err == ESP_OK) ctx->on = on;
    return err;
}

static esp_err_t co5300_op_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    /* Symmetric with our old co5300_sleep/wake: pair DISPOFF+SLPIN on sleep,
     * SLPOUT+DISPON on wake, so the caller doesn't have to sequence both.
     */
    if (sleep) {
        if (!ctx->awake) return ESP_OK;
        ESP_RETURN_ON_ERROR(co5300_send_cmd(ctx->io, CO5300_CMD_DISPOFF),
                            TAG, "sleep: DISPOFF failed");
        ESP_RETURN_ON_ERROR(co5300_send_cmd(ctx->io, CO5300_CMD_SLPIN),
                            TAG, "sleep: SLPIN failed");
        ctx->awake = false;
        ctx->on    = false;
    } else {
        if (ctx->awake) return ESP_OK;
        ESP_RETURN_ON_ERROR(co5300_send_cmd(ctx->io, CO5300_CMD_SLPOUT),
                            TAG, "wake: SLPOUT failed");
        vTaskDelay(pdMS_TO_TICKS(120));
        ESP_RETURN_ON_ERROR(co5300_send_cmd(ctx->io, CO5300_CMD_DISPON),
                            TAG, "wake: DISPON failed");
        ctx->awake = true;
        ctx->on    = true;
    }
    return ESP_OK;
}

/* -------- Factory + brightness extension --------------------------------- */

esp_err_t co5300_new_panel(esp_lcd_panel_io_handle_t io,
                           const co5300_panel_vendor_config_t *vendor,
                           esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && vendor && ret_panel, ESP_ERR_INVALID_ARG, TAG,
                        "null arg");

    co5300_panel_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "ctx alloc");

    ctx->io              = io;
    ctx->rst_gpio        = vendor->reset_gpio_num;
    ctx->rst_active_high = vendor->reset_active_high;
    ctx->x_gap           = vendor->x_gap;
    ctx->y_gap           = vendor->y_gap;
    ctx->swap_xy         = vendor->rotate_90_cw;
    ctx->mirror_x        = vendor->rotate_90_cw;
    ctx->mirror_y        = false;
    ctx->madctl          = CO5300_MADCTL_BGR | (vendor->rotate_90_cw ? 0x60 : 0x00);
    ctx->awake           = false;
    ctx->on              = false;

    /* Configure RST pin (idle high = out of reset). */
    if (ctx->rst_gpio >= 0) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << ctx->rst_gpio,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&rst_cfg);
        if (err != ESP_OK) {
            free(ctx);
            ESP_LOGE(TAG, "RST gpio_config failed: %s", esp_err_to_name(err));
            return err;
        }
        gpio_set_level(ctx->rst_gpio, ctx->rst_active_high ? 0 : 1);
    }

    ctx->base.reset        = co5300_op_reset;
    ctx->base.init         = co5300_op_init;
    ctx->base.del          = co5300_op_del;
    ctx->base.draw_bitmap  = co5300_op_draw_bitmap;
    ctx->base.mirror       = co5300_op_mirror;
    ctx->base.swap_xy      = co5300_op_swap_xy;
    ctx->base.set_gap      = co5300_op_set_gap;
    ctx->base.invert_color = co5300_op_invert_color;
    ctx->base.disp_on_off  = co5300_op_disp_on_off;
    ctx->base.disp_sleep   = co5300_op_disp_sleep;
    ctx->base.user_data    = ctx;

    *ret_panel = &ctx->base;
    ESP_LOGI(TAG, "co5300_panel v%s created (rst=%d, rotate90=%d)",
             CO5300_PANEL_DRIVER_VERSION,
             (int)ctx->rst_gpio, ctx->swap_xy ? 1 : 0);
    return ESP_OK;
}

esp_err_t co5300_panel_set_brightness(esp_lcd_panel_handle_t panel, uint8_t pct)
{
    if (!panel) return ESP_ERR_INVALID_ARG;
    co5300_panel_ctx_t *ctx = __containerof(panel, co5300_panel_ctx_t, base);
    if (pct > 100) pct = 100;
    const uint8_t value = (uint8_t)((pct * 255U) / 100U);
    return co5300_send_cmd_data(ctx->io, CO5300_CMD_WRDISBV, &value, 1);
}
