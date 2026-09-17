/**
 * @file boot_display.c
 * @brief SPI2 bus + esp_lcd panel IO + CO5300 panel bring-up.
 *
 * Stage 30.2 rewrite. The old path owned the SPI bus + init + pixel writes
 * end-to-end inside the co5300 driver; this file now assembles three
 * standard IDF layers and hands the io + panel handles to lvgl_ui_display:
 *
 *   1. spi_bus_initialize(SPI2_HOST, QSPI 4-wire, 40 MHz, max_transfer ~64K)
 *   2. esp_lcd_new_panel_io_spi(bus, quad_mode=1, lcd_cmd_bits=32, no DC)
 *   3. co5300_new_panel(io, vendor cfg)     [see co5300_panel.c]
 *
 * Presence probe still keys off CST9217's I2C ACK on bus 0 (touch + panel
 * share the display FPC). No touch chip -> skip the panel too.
 */

#include "boot_display.h"

#include "boot_hw_init.h"    /* g_i2c_mutex */
#include "co5300.h"          /* CO5300_COL_OFFSET + geometry */
#include "co5300_panel.h"    /* co5300_new_panel + brightness API */
#include "co5300_panel_io.h" /* co5300-correct QSPI panel_io factory */
#include "cst9217.h"

#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/* Touch queue lives in the cst9217 driver; used to purge stale reports so
 * a tap that landed during sleep does not fire the moment we wake.
 */
extern QueueHandle_t g_touch_q;

static const char *TAG = "BOOT_DISP";

/* Sized to hold one LVGL partial-refresh strip. Post-Stage-30 the target
 * is 40 rows x 410 x 3 bytes = 49,200 B; add headroom and round to 64 KB.
 * The esp_lcd panel-io internally chunks per its own trans queue if we
 * exceed a single-shot cap; this keeps DMA descriptors happy either way.
 */
#define CO5300_SPI_MAX_TRANSFER  (64 * 1024)

/* -- Pin map (mirrors the CO5300_CONFIG_DEFAULT from the old co5300.h). -- */
#define CO5300_PIN_CS     GPIO_NUM_10
#define CO5300_PIN_CLK    GPIO_NUM_12
#define CO5300_PIN_D0     GPIO_NUM_11
#define CO5300_PIN_D1     GPIO_NUM_13
#define CO5300_PIN_D2     GPIO_NUM_9
#define CO5300_PIN_D3     GPIO_NUM_14
#define CO5300_PIN_RST    GPIO_NUM_3

#define CO5300_SPI_HOST   SPI2_HOST
#define CO5300_PCLK_HZ    (40 * 1000 * 1000)

/* -- Module state --------------------------------------------------------- */

static esp_lcd_panel_io_handle_t s_io            = NULL;
static esp_lcd_panel_handle_t    s_panel         = NULL;
static bool                      s_present       = false;
static bool                      s_touch_present = false;
static bool                      s_asleep        = false;

/* -- Presence probe (unchanged) ------------------------------------------ */

static bool touch_module_present(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "probe: could not take I2C0 mutex");
        return false;
    }
    esp_err_t err = cst9217_ack_probe(I2C_NUM_0);
    xSemaphoreGive(g_i2c_mutex);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no CST9217 on bus 0 (i2c err=%s)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "CST9217 ACK @ 0x%02X -- display module present",
             CST9217_I2C_ADDR);
    return true;
}

/* -- SPI bus + io + panel bring-up --------------------------------------- */

static esp_err_t spi_bus_up(void)
{
    /* Quad SPI: MOSI=D0, MISO=D1, WP=D3, HD=D2, CLK=CLK, no DC.
     * CS is managed by esp_lcd_new_panel_io_spi (cs_gpio_num field).
     */
    spi_bus_config_t bus = {
        .mosi_io_num     = CO5300_PIN_D0,
        .miso_io_num     = CO5300_PIN_D1,
        .sclk_io_num     = CO5300_PIN_CLK,
        .quadwp_io_num   = CO5300_PIN_D3,
        .quadhd_io_num   = CO5300_PIN_D2,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .max_transfer_sz = CO5300_SPI_MAX_TRANSFER,
        .flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    return spi_bus_initialize(CO5300_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
}

static esp_err_t panel_io_up(void)
{
    /* Stage 30.2 attempt 2: our own panel_io that matches CO5300's wire
     * format. Stock esp_lcd_new_panel_io_spi(quad_mode=1) only quads the
     * DATA phase of tx_color; the leading cmd word stays on 1 line and the
     * panel misreads the pixel prefix -> glitched pixels + IRQ WDT trip.
     * co5300_panel_io_new_spi puts the cmd + addr + data all on QIO for
     * pixel writes and MULTILINE_CMD|MULTILINE_ADDR for instructions,
     * matching our proven pre-Stage-30 wire behavior.
     */
    co5300_panel_io_config_t io_cfg = {
        .spi_host          = CO5300_SPI_HOST,
        .cs_gpio_num       = CO5300_PIN_CS,
        .pclk_hz           = CO5300_PCLK_HZ,
        .trans_queue_depth = 8,
        /* Set to the ESP32-S3 SPI DMA per-transaction ceiling
         * (SPI_LL_DMA_MAX_BIT_LEN = 2^18 bits = 32 768 B). LVGL strip
         * bumped to 60 rows (73 800 B) in Stage 30 §4.2h so full-strip
         * flushes route through the multi-chunk sync fallback (each chunk
         * <= 32 KB DMA cap); small dirty regions <= 32 KB still stay in
         * the single-chunk async path and keep DMA overlap. Bounded by
         * max_transfer_sz below.
         */
        .pixel_chunk_bytes = 32 * 1024,
    };
    return co5300_panel_io_new_spi(&io_cfg, &s_io);
}

/* -- Public API ---------------------------------------------------------- */

esp_err_t boot_display_init(void)
{
    ESP_LOGI(TAG, "boot_logic v%s", BOOT_LOGIC_DRIVER_VERSION);
    if (s_present) {
        ESP_LOGW(TAG, "already initialised");
        return ESP_OK;
    }

    if (!touch_module_present()) {
        ESP_LOGI(TAG, "no CO5300 module -- skipping display bring-up");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = spi_bus_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    err = panel_io_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        spi_bus_free(CO5300_SPI_HOST);
        return err;
    }

    co5300_panel_vendor_config_t vendor = {
        .reset_gpio_num    = CO5300_PIN_RST,
        .reset_active_high = false,
        .x_gap             = CO5300_COL_OFFSET,
        .y_gap             = 0,
        .rotate_90_cw      = false,   /* Stage 30.3 flips via lvgl_port rotation */
    };
    err = co5300_new_panel(s_io, &vendor, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "co5300_new_panel failed: %s", esp_err_to_name(err));
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
        spi_bus_free(CO5300_SPI_HOST);
        return err;
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_reset failed: %s", esp_err_to_name(err));
    }
    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        esp_lcd_panel_io_del(s_io);
        s_panel = NULL;
        s_io    = NULL;
        spi_bus_free(CO5300_SPI_HOST);
        return err;
    }

    s_present = true;
    ESP_LOGI(TAG, "CO5300 ready (%dx%d logical, brightness ~80%%)",
             LCD_H_RES, LCD_V_RES);

    /* Touch chip lives on the same FPC. If it faults, log + continue --
     * the panel is still useful with encoder-only nav.
     */
    esp_err_t terr = cst9217_init(I2C_NUM_0);
    if (terr == ESP_OK) {
        s_touch_present = true;
        ESP_LOGI(TAG, "CST9217 touch ready (task_touch_fn will run on Core 0)");
    } else {
        s_touch_present = false;
        ESP_LOGW(TAG, "CST9217 init failed: %s -- panel OK, touch disabled",
                 esp_err_to_name(terr));
    }

    return ESP_OK;
}

bool boot_display_is_present(void)         { return s_present; }
bool boot_display_touch_is_present(void)   { return s_touch_present; }

void backlight_set_brightness(uint8_t pct)
{
    if (!s_present || s_panel == NULL) return;
    (void)co5300_panel_set_brightness(s_panel, pct);
}

esp_lcd_panel_io_handle_t boot_display_get_panel_io(void)
{
    return s_present ? s_io : NULL;
}

esp_lcd_panel_handle_t boot_display_get_panel(void)
{
    return s_present ? s_panel : NULL;
}

esp_err_t boot_display_sleep(void)
{
    if (!s_present || s_panel == NULL) return ESP_OK;
    if (s_asleep) return ESP_OK;
    esp_err_t err = esp_lcd_panel_disp_sleep(s_panel, true);
    if (err == ESP_OK) {
        s_asleep = true;
        if (g_touch_q) {
            cst9217_point_t drop;
            (void)xQueueReceive(g_touch_q, &drop, 0);
        }
        ESP_LOGI(TAG, "sleep -- panel to DISPOFF+SLPIN");
    }
    return err;
}

esp_err_t boot_display_wake(void)
{
    if (!s_present || s_panel == NULL) return ESP_OK;
    if (!s_asleep) return ESP_OK;
    esp_err_t err = esp_lcd_panel_disp_sleep(s_panel, false);
    if (err == ESP_OK) {
        s_asleep = false;
        if (g_touch_q) {
            cst9217_point_t drop;
            (void)xQueueReceive(g_touch_q, &drop, 0);
        }
        ESP_LOGI(TAG, "wake -- panel to SLPOUT+DISPON");
    }
    return err;
}

bool boot_display_is_asleep(void)
{
    return s_present && s_asleep;
}
