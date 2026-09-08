/**
 * @file co5300.c
 * @brief CO5300AF-51 AMOLED driver -- QSPI plumbing + panel init.
 *
 * Reference: firmware/arduino/15_amoled_touch_test/15_amoled_touch_test.ino
 * (audited Stage 18 §7.2; ran on iv7.1 breakout + Mk1b bring-up).
 *
 * SPI framing:
 *   - Instruction frame: cmd_bits=8 (0x02), addr_bits=24 (cmd_op<<8),
 *                        optional 1-wire data, flags MULTILINE_CMD|ADDR.
 *   - Pixel frame      : cmd_bits=8 (0x32), addr_bits=24 (0x003C00 typical),
 *                        QIO data, flag SPI_TRANS_MODE_QIO.
 *   - CS is driven manually (spics_io_num = -1) because the LVGL flush path
 *     will later stream multi-chunk pixel bursts with CS held low across
 *     several polling_start/end pairs. For single-shot transactions in this
 *     file we still take CS low/high around each spi_device_polling_transmit.
 *
 * Panel init (per Arduino reference):
 *   RST HIGH 50 ms, LOW 200 ms, HIGH 300 ms.
 *   SLPOUT, delay 120 ms.
 *   FE 00 (page 0), C4 80 (SPI RAM write en), 3A 77 (COLMOD RGB888),
 *   36 00 (MADCTL normal), 53 20 (WRCTRLD brightness ctrl on),
 *   63 FF (HBM max), DISPON, 51 D0 (~80%), 58 00 (sunlight off), INVOFF.
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
    co5300_config_t     cfg;
    spi_device_handle_t spi;
    bool                bus_owned;
    bool                awake;
    uint8_t             last_brightness;
};

// Sized for the panel init writes and modest partial-refresh chunks. The
// LVGL flush path will bump this when it lands; keep in sync with the biggest
// single spi_device_polling_transmit the driver ever issues.
#define CO5300_MAX_TRANSFER_BYTES  (4096 + 8)

// -- Manual CS helpers ---------------------------------------------------------
static inline void cs_low (co5300_handle_t h) { gpio_set_level(h->cfg.pin_cs, 0); }
static inline void cs_high(co5300_handle_t h) { gpio_set_level(h->cfg.pin_cs, 1); }

// -- SPI bus + device bring-up -------------------------------------------------
static esp_err_t spi_bus_up(co5300_handle_t h)
{
    // CS pin as manual GPIO output, idle high.
    gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << h->cfg.pin_cs,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cs_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CS gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    cs_high(h);

    // RST pin as output, idle high (panel out of reset).
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << h->cfg.pin_rst,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&rst_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RST gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(h->cfg.pin_rst, 1);

    // SPI bus: QSPI 4-wire (D0..D3), manual CS.
    spi_bus_config_t buscfg = {
        .mosi_io_num     = h->cfg.pin_d0,
        .miso_io_num     = h->cfg.pin_d1,
        .sclk_io_num     = h->cfg.pin_clk,
        .quadwp_io_num   = h->cfg.pin_d3,
        .quadhd_io_num   = h->cfg.pin_d2,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .max_transfer_sz = CO5300_MAX_TRANSFER_BYTES,
        .flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    err = spi_bus_initialize(h->cfg.spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .command_bits   = 8,
        .address_bits   = 24,
        .dummy_bits     = 0,
        .mode           = 0,
        .clock_speed_hz = h->cfg.freq_hz,
        .spics_io_num   = -1,           // manual CS via gpio_set_level
        .queue_size     = 1,
        .flags          = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(h->cfg.spi_host, &devcfg, &h->spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(h->cfg.spi_host);
        return err;
    }

    return ESP_OK;
}

static esp_err_t spi_bus_down(co5300_handle_t h)
{
    if (h->spi) {
        spi_bus_remove_device(h->spi);
        h->spi = NULL;
    }
    if (h->bus_owned) {
        spi_bus_free(h->cfg.spi_host);
    }
    return ESP_OK;
}

// -- 0x02 instruction frame ---------------------------------------------------
// Writes cmd_op (in 24-bit addr) + up to 4 inline bytes (tx_data path) or a
// caller-provided buffer for longer payloads.
static esp_err_t spi_send_instruction(co5300_handle_t h,
                                      uint8_t cmd,
                                      const uint8_t *data,
                                      size_t len)
{
    if (!h || !h->spi) return ESP_ERR_INVALID_STATE;

    spi_transaction_ext_t t = {0};
    t.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    t.base.cmd   = CO5300_WIRE_INSTRUCT;        // 0x02
    t.base.addr  = ((uint32_t)cmd) << 8;         // 24-bit, opcode in upper byte

    if (len > 0) {
        if (len <= 4) {
            t.base.flags |= SPI_TRANS_USE_TXDATA;
            for (size_t i = 0; i < len; i++) t.base.tx_data[i] = data[i];
        } else {
            t.base.tx_buffer = data;
        }
        t.base.length = 8 * len;
    }

    cs_low(h);
    esp_err_t err = spi_device_polling_transmit(h->spi, (spi_transaction_t *)&t);
    cs_high(h);
    return err;
}

// -- 0x32 pixel frame (single QIO burst, CS pulsed once) ----------------------
// The LVGL flush path may want to keep CS low across multiple bursts; this
// helper is the single-shot version used for anything the driver posts today.
static esp_err_t spi_send_pixels(co5300_handle_t h,
                                 uint32_t addr,
                                 const uint8_t *pixels,
                                 size_t bytes)
{
    if (!h || !h->spi) return ESP_ERR_INVALID_STATE;
    if (!pixels || bytes == 0) return ESP_ERR_INVALID_ARG;

    spi_transaction_ext_t t = {0};
    t.base.flags     = SPI_TRANS_MODE_QIO;
    t.base.cmd       = CO5300_WIRE_PIXELS;      // 0x32
    t.base.addr      = addr;
    t.base.tx_buffer = pixels;
    t.base.length    = 8 * bytes;

    cs_low(h);
    esp_err_t err = spi_device_polling_transmit(h->spi, (spi_transaction_t *)&t);
    cs_high(h);
    return err;
}

// -- Reset pulse (per Arduino reference) --------------------------------------
static void reset_panel(co5300_handle_t h)
{
    gpio_set_level(h->cfg.pin_rst, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(h->cfg.pin_rst, 0); vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(h->cfg.pin_rst, 1); vTaskDelay(pdMS_TO_TICKS(300));
}

// -- Full panel init sequence (per Arduino reference) -------------------------
static esp_err_t panel_init_sequence(co5300_handle_t h)
{
    esp_err_t err;

    err = spi_send_instruction(h, CO5300_CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t page0    = 0x00;
    const uint8_t ram_en   = 0x80;
    const uint8_t colmod   = CO5300_COLMOD_RGB888;  // 0x77 (mandatory)
    const uint8_t madctl   = 0x00;                  // normal orientation, RGB
    const uint8_t wrctrld  = 0x20;                  // brightness ctrl on
    const uint8_t hbm_max  = 0xFF;
    const uint8_t bright   = 0xD0;                  // ~80 %
    const uint8_t sun_off  = 0x00;

    (void)spi_send_instruction(h, 0xFE, &page0,   1);   // command page 0
    (void)spi_send_instruction(h, 0xC4, &ram_en,  1);   // SPI RAM write enable
    (void)spi_send_instruction(h, CO5300_CMD_COLMOD,  &colmod,  1);
    (void)spi_send_instruction(h, CO5300_CMD_MADCTL,  &madctl,  1);
    (void)spi_send_instruction(h, CO5300_CMD_WRCTRLD, &wrctrld, 1);
    (void)spi_send_instruction(h, 0x63, &hbm_max, 1);   // HBM brightness max
    (void)spi_send_instruction(h, CO5300_CMD_DISPON,  NULL,     0);
    (void)spi_send_instruction(h, CO5300_CMD_WRDISBV, &bright,  1);
    (void)spi_send_instruction(h, 0x58, &sun_off, 1);   // sunlight enhancement off
    (void)spi_send_instruction(h, CO5300_CMD_INVOFF,  NULL,     0);

    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

// -- Lifecycle -----------------------------------------------------------------

esp_err_t co5300_init(const co5300_config_t *cfg, co5300_handle_t *out_handle)
{
    ESP_LOGI(TAG, "driver v%s", CO5300_DRIVER_VERSION);
    if (cfg == NULL || out_handle == NULL) return ESP_ERR_INVALID_ARG;
    const int64_t t0 = esp_timer_get_time();
    const size_t  heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    co5300_handle_t h = calloc(1, sizeof(struct co5300_dev_s));
    if (h == NULL) return ESP_ERR_NO_MEM;
    h->cfg             = *cfg;
    h->bus_owned       = true;
    h->awake           = false;
    h->last_brightness = 0xD0;   // matches panel init default

    if (cfg->pin_cs  < 0 || cfg->pin_clk < 0 ||
        cfg->pin_d0  < 0 || cfg->pin_d1  < 0 ||
        cfg->pin_d2  < 0 || cfg->pin_d3  < 0 ||
        cfg->pin_rst < 0) {
        free(h);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = spi_bus_up(h);
    if (err != ESP_OK) { free(h); return err; }

    reset_panel(h);

    err = panel_init_sequence(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_init_sequence failed: %s", esp_err_to_name(err));
        (void)spi_bus_down(h);
        free(h);
        return err;
    }

    h->awake    = true;
    *out_handle = h;

    const int64_t t1 = esp_timer_get_time();
    const size_t  heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "init OK in %lld us, heap delta = %d bytes",
             (long long)(t1 - t0),
             (int)((long)heap_before - (long)heap_after));
    return ESP_OK;
}

esp_err_t co5300_deinit(co5300_handle_t h)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    (void)co5300_sleep(h);
    (void)spi_bus_down(h);
    free(h);
    return ESP_OK;
}

// -- Command / data send -------------------------------------------------------

esp_err_t co5300_write_command(co5300_handle_t h, uint8_t cmd)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    return spi_send_instruction(h, cmd, NULL, 0);
}

esp_err_t co5300_write_command_with_data(co5300_handle_t h,
                                         uint8_t cmd,
                                         const uint8_t *data,
                                         size_t len)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (len > 0 && data == NULL) return ESP_ERR_INVALID_ARG;
    return spi_send_instruction(h, cmd, data, len);
}

esp_err_t co5300_write_pixels(co5300_handle_t h,
                              uint32_t addr,
                              const uint8_t *pixels,
                              size_t count)
{
    if (h == NULL || pixels == NULL) return ESP_ERR_INVALID_ARG;
    return spi_send_pixels(h, addr, pixels, count * CO5300_PIXEL_BYTES);
}

// -- Convenience wrappers ------------------------------------------------------

esp_err_t co5300_set_brightness(co5300_handle_t h, uint8_t pct)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (pct > 100) pct = 100;
    const uint8_t value = (uint8_t)((pct * 255U) / 100U);
    h->last_brightness  = value;
    return spi_send_instruction(h, CO5300_CMD_WRDISBV, &value, 1);
}

esp_err_t co5300_set_window(co5300_handle_t h,
                            uint16_t x0, uint16_t y0,
                            uint16_t x1, uint16_t y1)
{
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
    err = spi_send_instruction(h, CO5300_CMD_RASET, raset, sizeof raset);
    if (err != ESP_OK) return err;
    return spi_send_instruction(h, CO5300_CMD_RAMWR, NULL, 0);
}

esp_err_t co5300_sleep(co5300_handle_t h)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (!h->awake) return ESP_OK;
    esp_err_t err = spi_send_instruction(h, CO5300_CMD_DISPOFF, NULL, 0);
    if (err != ESP_OK) return err;
    err = spi_send_instruction(h, CO5300_CMD_SLPIN, NULL, 0);
    if (err == ESP_OK) h->awake = false;
    return err;
}

esp_err_t co5300_wake(co5300_handle_t h)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (h->awake) return ESP_OK;
    esp_err_t err = spi_send_instruction(h, CO5300_CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    err = spi_send_instruction(h, CO5300_CMD_DISPON, NULL, 0);
    if (err == ESP_OK) h->awake = true;
    return err;
}

esp_err_t co5300_get_status(co5300_handle_t h, uint8_t out_status[4])
{
    // [DEFECT-001] datasheet is silent on WHO_AM_I semantics. RDDID (0x04)
    // half-duplex readback is untested on this chip. Presence detection lives
    // at boot_display.c (via the CST9217 I2C probe on the same FPC); this
    // stays NOT_SUPPORTED until we can bench-verify a real read on Mk1b.
    if (h == NULL || out_status == NULL) return ESP_ERR_INVALID_ARG;
    memset(out_status, 0, 4);
    return ESP_ERR_NOT_SUPPORTED;
}
