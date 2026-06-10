/**
 * @file co5300.c
 * @brief CO5300AF-51 AMOLED driver -- skeleton.
 *
 * Status: stub. Functions log their arguments + measured time, return ESP_OK
 * where safe, ESP_ERR_NOT_SUPPORTED where a real impl is needed but cannot be
 * written without hardware. When the panel arrives, replace each TODO block
 * with the actual SPI transaction.
 *
 * Profiling: every entry point measures wall time via esp_timer_get_time()
 * and logs it. The test harness consumes these logs.
 */

#include "co5300.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "co5300";

// -- Identity ------------------------------------------------------------------
const char *co5300_get_chip_name(void) { return "CO5300AF-51"; }
const char *co5300_get_chip_desc(void) { return "2.06\" AMOLED QSPI panel"; }

// -- Internal device state -----------------------------------------------------
struct co5300_dev_s {
    co5300_config_t   cfg;
    spi_device_handle_t spi;       // set once SPI device is added; NULL until then
    bool              bus_owned;   // true if we called spi_bus_initialize ourselves
    bool              awake;       // tracks SLPIN/SLPOUT state
    uint8_t           last_brightness; // 0..255
};

// -- SPI plumbing --------------------------------------------------------------
//
// The CO5300 talks two wire formats on the same QSPI bus:
//   1. Instruction frame  -- cmd=0x02 (8 bit), addr=cmd_opcode<<8 (24 bit),
//                            optional data on D0 only (1-wire).
//   2. Pixel frame        -- cmd=0x32 (8 bit), addr=panel_addr (24 bit),
//                            payload on D0..D3 (4-wire/QIO).
//
// ESP-IDF's spi_master supports both via spi_transaction_ext_t with
// SPI_TRANS_MODE_QIO flag for #2. The stub below outlines the call shape
// without actually transmitting -- real impl lands when the panel is on the
// bench.
//
// IMPORTANT: spi_bus_initialize() must be called with .quadwp_io_num and
// .quadhd_io_num populated (D2/D3) so QIO transactions are legal.

static esp_err_t spi_bus_up(co5300_handle_t h) {
    (void)h;
    // TODO(hw): spi_bus_initialize(h->cfg.spi_host, &buscfg, SPI_DMA_CH_AUTO);
    // TODO(hw): spi_bus_add_device(h->cfg.spi_host, &devcfg, &h->spi);
    return ESP_OK;
}

static esp_err_t spi_bus_down(co5300_handle_t h) {
    (void)h;
    // TODO(hw): spi_bus_remove_device(h->spi); spi_bus_free(h->cfg.spi_host);
    return ESP_OK;
}

// Sends a single instruction byte using the 0x02 framing.
// Stub: counts the call, no bus traffic.
static esp_err_t spi_send_instruction(co5300_handle_t h,
                                      uint8_t cmd,
                                      const uint8_t *data,
                                      size_t len) {
    (void)h; (void)data;
    const int64_t t0 = esp_timer_get_time();
    // TODO(hw):
    //   spi_transaction_ext_t t = {
    //     .base.flags    = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR,
    //     .base.cmd      = CO5300_WIRE_INSTRUCT,   // 0x02
    //     .base.addr     = ((uint32_t)cmd) << 8,    // 24-bit, cmd in upper byte
    //     .base.length   = 8 * len,
    //     .base.tx_buffer= data,
    //     .command_bits  = 8,
    //     .address_bits  = 24,
    //   };
    //   return spi_device_polling_transmit(h->spi, &t.base);
    const int64_t t1 = esp_timer_get_time();
    ESP_LOGD(TAG, "instr cmd=0x%02X len=%u dt=%lld us (STUB)",
             cmd, (unsigned)len, (long long)(t1 - t0));
    return ESP_OK;
}

// Sends a pixel frame using the 0x32 QIO framing.
// Stub: counts the call, no bus traffic.
static esp_err_t spi_send_pixels(co5300_handle_t h,
                                 uint32_t addr,
                                 const uint8_t *pixels,
                                 size_t bytes) {
    (void)h; (void)pixels;
    const int64_t t0 = esp_timer_get_time();
    // TODO(hw):
    //   spi_transaction_ext_t t = {
    //     .base.flags    = SPI_TRANS_MODE_QIO
    //                    | SPI_TRANS_VARIABLE_CMD
    //                    | SPI_TRANS_VARIABLE_ADDR,
    //     .base.cmd      = CO5300_WIRE_PIXELS,     // 0x32
    //     .base.addr     = addr,                    // 0x003C00 typical
    //     .base.length   = 8 * bytes,
    //     .base.tx_buffer= pixels,
    //     .command_bits  = 8,
    //     .address_bits  = 24,
    //   };
    //   return spi_device_polling_transmit(h->spi, &t.base);
    const int64_t t1 = esp_timer_get_time();
    ESP_LOGD(TAG, "pixels addr=0x%06lX bytes=%u dt=%lld us (STUB)",
             (unsigned long)addr, (unsigned)bytes, (long long)(t1 - t0));
    return ESP_OK;
}

// -- Lifecycle -----------------------------------------------------------------

esp_err_t co5300_init(const co5300_config_t *cfg, co5300_handle_t *out_handle) {
    if (cfg == NULL || out_handle == NULL) return ESP_ERR_INVALID_ARG;
    const int64_t t0 = esp_timer_get_time();
    const size_t  heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    co5300_handle_t h = calloc(1, sizeof(struct co5300_dev_s));
    if (h == NULL) return ESP_ERR_NO_MEM;
    h->cfg             = *cfg;
    h->bus_owned       = true;
    h->awake           = false;
    h->last_brightness = 0;

    // Pin sanity: every QSPI pin must be present.
    if (cfg->pin_cs  < 0 || cfg->pin_clk < 0 ||
        cfg->pin_d0  < 0 || cfg->pin_d1  < 0 ||
        cfg->pin_d2  < 0 || cfg->pin_d3  < 0 ||
        cfg->pin_rst < 0) {
        free(h);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = spi_bus_up(h);
    if (err != ESP_OK) { free(h); return err; }

    // Reset pulse (stub for now). On real HW: drive low ~10 ms, release,
    // wait >=120 ms before first command.
    //   gpio_set_level(h->cfg.pin_rst, 0); vTaskDelay(pdMS_TO_TICKS(10));
    //   gpio_set_level(h->cfg.pin_rst, 1); vTaskDelay(pdMS_TO_TICKS(120));

    // Init sequence -- per v7.2 "Wake" path (SLPOUT, COLMOD, DISPON).
    // Each call is a no-op stub today; sequence and ordering are correct.
    (void)spi_send_instruction(h, CO5300_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    const uint8_t colmod = CO5300_COLMOD_RGB888;  // 0x77 mandatory
    (void)spi_send_instruction(h, CO5300_CMD_COLMOD, &colmod, 1);
    (void)spi_send_instruction(h, CO5300_CMD_DISPON, NULL, 0);

    h->awake = true;
    *out_handle = h;

    const int64_t t1 = esp_timer_get_time();
    const size_t  heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "init: %lld us (STUB), heap delta = %d bytes",
             (long long)(t1 - t0), (int)((long)heap_before - (long)heap_after));
    return ESP_OK;
}

esp_err_t co5300_deinit(co5300_handle_t h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    (void)co5300_sleep(h);
    (void)spi_bus_down(h);
    free(h);
    return ESP_OK;
}

// -- Command / data send -------------------------------------------------------

esp_err_t co5300_write_command(co5300_handle_t h, uint8_t cmd) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    return spi_send_instruction(h, cmd, NULL, 0);
}

esp_err_t co5300_write_command_with_data(co5300_handle_t h,
                                         uint8_t cmd,
                                         const uint8_t *data,
                                         size_t len) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (len > 0 && data == NULL) return ESP_ERR_INVALID_ARG;
    return spi_send_instruction(h, cmd, data, len);
}

esp_err_t co5300_write_pixels(co5300_handle_t h,
                              uint32_t addr,
                              const uint8_t *pixels,
                              size_t count) {
    if (h == NULL || pixels == NULL) return ESP_ERR_INVALID_ARG;
    return spi_send_pixels(h, addr, pixels, count * CO5300_PIXEL_BYTES);
}

// -- Convenience wrappers ------------------------------------------------------

esp_err_t co5300_set_brightness(co5300_handle_t h, uint8_t pct) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (pct > 100) pct = 100;
    const uint8_t value = (uint8_t)((pct * 255U) / 100U);
    h->last_brightness  = value;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = spi_send_instruction(h, CO5300_CMD_WRDISBV, &value, 1);
    const int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "brightness pct=%u (raw=0x%02X) dt=%lld us",
             (unsigned)pct, value, (long long)(t1 - t0));
    return err;
}

esp_err_t co5300_set_window(co5300_handle_t h,
                            uint16_t x0, uint16_t y0,
                            uint16_t x1, uint16_t y1) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    const uint16_t cx0 = x0 + CO5300_COL_OFFSET;
    const uint16_t cx1 = x1 + CO5300_COL_OFFSET;
    const uint8_t caset[4] = {
        (uint8_t)(cx0 >> 8), (uint8_t)cx0,
        (uint8_t)(cx1 >> 8), (uint8_t)cx1,
    };
    const uint8_t raset[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)y0,
        (uint8_t)(y1 >> 8), (uint8_t)y1,
    };
    esp_err_t err = spi_send_instruction(h, CO5300_CMD_CASET, caset, sizeof caset);
    if (err != ESP_OK) return err;
    return spi_send_instruction(h, CO5300_CMD_RASET, raset, sizeof raset);
}

esp_err_t co5300_sleep(co5300_handle_t h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (!h->awake) return ESP_OK;
    esp_err_t err = spi_send_instruction(h, CO5300_CMD_DISPOFF, NULL, 0);
    if (err != ESP_OK) return err;
    err = spi_send_instruction(h, CO5300_CMD_SLPIN, NULL, 0);
    if (err == ESP_OK) h->awake = false;
    return err;
}

esp_err_t co5300_wake(co5300_handle_t h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (h->awake) return ESP_OK;
    esp_err_t err = spi_send_instruction(h, CO5300_CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(5));
    const uint8_t colmod = CO5300_COLMOD_RGB888;
    (void)spi_send_instruction(h, CO5300_CMD_COLMOD, &colmod, 1);
    err = spi_send_instruction(h, CO5300_CMD_DISPON, NULL, 0);
    if (err == ESP_OK) h->awake = true;
    return err;
}

esp_err_t co5300_get_status(co5300_handle_t h, uint8_t out_status[4]) {
    if (h == NULL || out_status == NULL) return ESP_ERR_INVALID_ARG;
    // [DEFECT-001]: datasheet is silent on a true WHO_AM_I. RDDST (0x09) is
    // the closest equivalent. Real read needs an spi_transaction with rxlength,
    // so this is left ESP_ERR_NOT_SUPPORTED until hardware is on the bench.
    memset(out_status, 0, 4);
    return ESP_ERR_NOT_SUPPORTED;
}
