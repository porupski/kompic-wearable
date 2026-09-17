/**
 * @file co5300_panel_io.c
 * @brief CO5300-correct esp_lcd_panel_io implementation.
 *
 * See co5300_panel_io.h for the "why" and the wire-format contract.
 * Modeled on the pixel-write logic in our pre-Stage-30 co5300.c that
 * bench-verified on Mk1b, with the vtable wrapper and asynchronous
 * completion needed by esp_lvgl_port.
 */

#include "co5300_panel_io.h"

#include "esp_check.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "co5300_pio";

/* Per-transaction descriptor pool. One entry per queued chunk. */
typedef struct {
    spi_transaction_ext_t base;   /* SPI transaction (ext variant for VARIABLE_* flags) */
    void                 *ctx;    /* back-pointer for post_trans cb */
    bool                  fire_done;  /* last chunk in the color burst -> invoke on_color_trans_done */
} co5300_pio_trans_t;

typedef struct co5300_pio_s {
    esp_lcd_panel_io_t                       base;   /* vtable first */
    spi_device_handle_t                      spi;
    size_t                                   trans_queue_depth;
    size_t                                   pixel_chunk_bytes;
    esp_lcd_panel_io_color_trans_done_cb_t   on_color_trans_done;
    void                                    *user_ctx;
    co5300_pio_trans_t                      *pool;   /* trans_queue_depth entries */
    size_t                                   pool_next;
    size_t                                   inflight;
    /* Serialize concurrent callers on this device. Stage 30.1 landed this
     * mutex on the old co5300.c which we deleted in 30.2; the regression
     * showed up as `spi_master:1338 Cannot acquire bus when a polling
     * transaction is in progress` when field_capture fired idle-sleep
     * while LVGL was mid-flush. Every tx_param and tx_color entry takes
     * the mutex around its whole SPI activity.
     */
    SemaphoreHandle_t                        bus_mutex;
} co5300_pio_t;

/* -------- Post-trans ISR callback -------------------------------------- */
/*
 * Fires from the SPI master ISR for EVERY completed transaction on this
 * device -- both tx_param polling transmits and queued tx_color chunks.
 * We discriminate via `t->user`: tx_param leaves it NULL, tx_color sets it
 * to the wrapping `co5300_pio_trans_t *` so we can find the `fire_done`
 * flag and the io context. Do NOT `__containerof` on `t` -- for polling
 * transmits `t` may be a stack-local `spi_transaction_ext_t` with no
 * enclosing `co5300_pio_trans_t`, and the resulting pointer would be
 * garbage (fw 0.4.65 InstrFetchProhibited was this bug).
 */
static IRAM_ATTR void co5300_pio_post_trans_cb(spi_transaction_t *t)
{
    if (t->user == NULL) return;    /* tx_param, no completion needed */
    co5300_pio_trans_t *lt = (co5300_pio_trans_t *)t->user;
    if (!lt->fire_done) return;
    co5300_pio_t *pio = lt->ctx;
    if (pio && pio->on_color_trans_done) {
        esp_lcd_panel_io_event_data_t edata = { 0 };
        (void)pio->on_color_trans_done(&pio->base, &edata, pio->user_ctx);
    }
}

/* -------- Small helpers ------------------------------------------------ */

static inline uint8_t cmd_word_prefix(uint32_t cmd_word)
{
    return (uint8_t)(cmd_word >> 24);   /* top byte: 0x02 or 0x32 */
}

static inline uint32_t cmd_word_addr(uint32_t cmd_word)
{
    return cmd_word & 0x00FFFFFFU;      /* lower 24 bits */
}

static void wait_all_inflight(co5300_pio_t *pio)
{
    /* Block until every previously-queued chunk has been reaped. Called
     * before tx_param so the LVGL flush burst can drain before an
     * instruction slips in on the same SPI device.
     */
    spi_transaction_t *done = NULL;
    while (pio->inflight > 0) {
        esp_err_t err = spi_device_get_trans_result(pio->spi, &done, portMAX_DELAY);
        if (err != ESP_OK) return;
        pio->inflight--;
    }
}

/* -------- tx_param: CO5300 instruction frame -------------------------- */

static esp_err_t co5300_pio_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd,
                                     const void *param, size_t param_size)
{
    co5300_pio_t *pio = __containerof(io, co5300_pio_t, base);
    if (lcd_cmd < 0) return ESP_ERR_INVALID_ARG;

    if (pio->bus_mutex) xSemaphoreTake(pio->bus_mutex, portMAX_DELAY);
    wait_all_inflight(pio);

    const uint32_t cmd_word = (uint32_t)lcd_cmd;
    const uint8_t  prefix   = cmd_word_prefix(cmd_word);
    const uint32_t addr     = cmd_word_addr(cmd_word);

    /* Instruction frame: prefix on QIO cmd (8 bits), opcode-in-addr on QIO
     * addr (24 bits), optional data on 1-wire. This is exactly what our
     * pre-Stage-30 driver did via SPI_TRANS_MULTILINE_CMD | _ADDR.
     */
    spi_transaction_ext_t t = { 0 };
    t.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    t.base.cmd   = prefix;
    t.base.addr  = addr;

    if (param && param_size > 0) {
        if (param_size <= 4) {
            t.base.flags |= SPI_TRANS_USE_TXDATA;
            for (size_t i = 0; i < param_size; i++) {
                t.base.tx_data[i] = ((const uint8_t *)param)[i];
            }
        } else {
            t.base.tx_buffer = param;
        }
        t.base.length = 8 * param_size;
    }
    /* Explicit NULL so the device-level post_cb short-circuits for
     * instruction transactions (see co5300_pio_post_trans_cb).
     */
    t.base.user = NULL;

    esp_err_t err = spi_device_acquire_bus(pio->spi, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "acquire failed: %s", esp_err_to_name(err));
        if (pio->bus_mutex) xSemaphoreGive(pio->bus_mutex);
        return err;
    }
    err = spi_device_polling_transmit(pio->spi,
                                      (spi_transaction_t *)&t);
    spi_device_release_bus(pio->spi);
    if (pio->bus_mutex) xSemaphoreGive(pio->bus_mutex);
    return err;
}

/* -------- rx_param (not supported on this panel) ----------------------- */

static esp_err_t co5300_pio_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd,
                                     void *param, size_t param_size)
{
    (void)io; (void)lcd_cmd; (void)param; (void)param_size;
    return ESP_ERR_NOT_SUPPORTED;
}

/* -------- tx_color: chunked QIO-everything pixel burst ---------------- */

/* Single-chunk async path removed in Stage 30 §4.2i: the queued+ISR path
 * rendered clean on full-strip flushes (which routed to multi_sync) but
 * sheared LVGL's small partial re-flushes (clock minute update, dot
 * refreshes) even though wire-format was identical. Root cause unclear --
 * possibly a race between the ISR-fired on_color_trans_done and the next
 * LVGL flush entering CASET/RASET before the SPI driver had fully quiesced.
 * We now route every pixel burst through the synchronous acquire_bus +
 * polling_transmit path below regardless of size. Costs ~20 us of extra
 * bus-lock overhead per small flush; buys shear-free rendering.
 *
 * Multi-chunk sync path (handles single-chunk case correctly too).
 * Acquires the bus so SPI_TRANS_CS_KEEP_ACTIVE is accepted, uses polling
 * transmits (bus-held path), releases the bus, then calls
 * on_color_trans_done synchronously so LVGL sees flush_ready.
 */
static esp_err_t co5300_pio_tx_color_multi_sync(co5300_pio_t *pio,
                                                uint8_t prefix, uint32_t addr,
                                                const void *color,
                                                size_t color_size)
{
    wait_all_inflight(pio);

    esp_err_t err = spi_device_acquire_bus(pio->spi, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "acquire failed: %s", esp_err_to_name(err));
        return err;
    }

    const uint8_t *src   = (const uint8_t *)color;
    size_t         rem   = color_size;
    bool           first = true;

    while (rem > 0) {
        const size_t chunk = (rem > pio->pixel_chunk_bytes)
                                ? pio->pixel_chunk_bytes : rem;
        const bool   last  = (chunk == rem);

        spi_transaction_ext_t t = { 0 };
        if (first) {
            t.base.flags = SPI_TRANS_MODE_QIO;
            t.base.cmd   = prefix;
            t.base.addr  = addr;
            first = false;
        } else {
            t.base.flags   = SPI_TRANS_MODE_QIO
                           | SPI_TRANS_VARIABLE_CMD
                           | SPI_TRANS_VARIABLE_ADDR
                           | SPI_TRANS_VARIABLE_DUMMY;
            t.command_bits = 0;
            t.address_bits = 0;
            t.dummy_bits   = 0;
        }
        if (!last) t.base.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
        t.base.tx_buffer = src;
        t.base.length    = 8 * chunk;
        t.base.user      = NULL;   /* sync path -> post_cb short-circuits */

        err = spi_device_polling_transmit(pio->spi, (spi_transaction_t *)&t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "polling_transmit (multi) failed: %s", esp_err_to_name(err));
            spi_device_release_bus(pio->spi);
            return err;
        }

        src += chunk;
        rem -= chunk;
    }

    spi_device_release_bus(pio->spi);

    /* Sync path: fire the completion callback ourselves so esp_lvgl_port
     * calls lv_display_flush_ready. Called from the LVGL task, not ISR,
     * so no IRAM constraint.
     */
    if (pio->on_color_trans_done) {
        esp_lcd_panel_io_event_data_t edata = { 0 };
        (void)pio->on_color_trans_done(&pio->base, &edata, pio->user_ctx);
    }
    return ESP_OK;
}

static esp_err_t co5300_pio_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd,
                                     const void *color, size_t color_size)
{
    co5300_pio_t *pio = __containerof(io, co5300_pio_t, base);
    if (lcd_cmd < 0 || color == NULL || color_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t cmd_word = (uint32_t)lcd_cmd;
    const uint8_t  prefix   = cmd_word_prefix(cmd_word);
    const uint32_t addr     = cmd_word_addr(cmd_word);

    /* All flushes -- small partial and full-strip alike -- go through the
     * sync path (see Stage 30 §4.2i). Chunks below pixel_chunk_bytes send in
     * one transaction; larger ones chunk internally.
     */
    if (pio->bus_mutex) xSemaphoreTake(pio->bus_mutex, portMAX_DELAY);
    esp_err_t err = co5300_pio_tx_color_multi_sync(pio, prefix, addr, color, color_size);
    if (pio->bus_mutex) xSemaphoreGive(pio->bus_mutex);
    return err;
}

/* -------- register_event_callbacks + del ------------------------------ */

static esp_err_t co5300_pio_register_event_callbacks(esp_lcd_panel_io_handle_t io,
                                                     const esp_lcd_panel_io_callbacks_t *cbs,
                                                     void *user_ctx)
{
    co5300_pio_t *pio = __containerof(io, co5300_pio_t, base);
    if (cbs && cbs->on_color_trans_done) {
        pio->on_color_trans_done = cbs->on_color_trans_done;
    } else {
        pio->on_color_trans_done = NULL;
    }
    pio->user_ctx = user_ctx;
    return ESP_OK;
}

static esp_err_t co5300_pio_del(esp_lcd_panel_io_t *io)
{
    co5300_pio_t *pio = __containerof(io, co5300_pio_t, base);
    wait_all_inflight(pio);
    if (pio->spi) spi_bus_remove_device(pio->spi);
    if (pio->bus_mutex) vSemaphoreDelete(pio->bus_mutex);
    free(pio->pool);
    free(pio);
    return ESP_OK;
}

/* -------- Factory ------------------------------------------------------ */

esp_err_t co5300_panel_io_new_spi(const co5300_panel_io_config_t *config,
                                  esp_lcd_panel_io_handle_t *ret_io)
{
    ESP_RETURN_ON_FALSE(config && ret_io, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(config->trans_queue_depth > 0, ESP_ERR_INVALID_ARG,
                        TAG, "trans_queue_depth must be > 0");
    ESP_RETURN_ON_FALSE(config->pixel_chunk_bytes > 0, ESP_ERR_INVALID_ARG,
                        TAG, "pixel_chunk_bytes must be > 0");

    co5300_pio_t *pio = calloc(1, sizeof(*pio));
    if (!pio) return ESP_ERR_NO_MEM;
    pio->pool = calloc(config->trans_queue_depth, sizeof(co5300_pio_trans_t));
    if (!pio->pool) { free(pio); return ESP_ERR_NO_MEM; }
    pio->bus_mutex = xSemaphoreCreateMutex();
    if (!pio->bus_mutex) { free(pio->pool); free(pio); return ESP_ERR_NO_MEM; }

    pio->trans_queue_depth = config->trans_queue_depth;
    pio->pixel_chunk_bytes = config->pixel_chunk_bytes;

    /* SPI device: 8-bit cmd + 24-bit addr baseline. Per-transaction flags
     * override to QIO (pixels) or MULTILINE_CMD|MULTILINE_ADDR (instructions).
     * Manual CS via `spics_io_num = cs_gpio_num` -- the SPI driver toggles
     * CS around each transaction, honoring SPI_TRANS_CS_KEEP_ACTIVE for
     * multi-chunk pixel bursts.
     */
    spi_device_interface_config_t devcfg = {
        .command_bits   = 8,
        .address_bits   = 24,
        .dummy_bits     = 0,
        .mode           = 0,
        .clock_speed_hz = config->pclk_hz,
        .spics_io_num   = config->cs_gpio_num,
        .queue_size     = (int)config->trans_queue_depth,
        .flags          = SPI_DEVICE_HALFDUPLEX,
        .post_cb        = co5300_pio_post_trans_cb,
    };
    esp_err_t err = spi_bus_add_device(config->spi_host, &devcfg, &pio->spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(pio->bus_mutex);
        free(pio->pool);
        free(pio);
        return err;
    }

    pio->base.rx_param                 = co5300_pio_rx_param;
    pio->base.tx_param                 = co5300_pio_tx_param;
    pio->base.tx_color                 = co5300_pio_tx_color;
    pio->base.del                      = co5300_pio_del;
    pio->base.register_event_callbacks = co5300_pio_register_event_callbacks;

    *ret_io = &pio->base;
    ESP_LOGI(TAG, "co5300_panel_io ready (queue=%zu, chunk=%zuB, cs=%d)",
             pio->trans_queue_depth, pio->pixel_chunk_bytes, (int)config->cs_gpio_num);
    return ESP_OK;
}
