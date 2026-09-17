/**
 * @file lvgl_ui_display.c
 * @brief LVGL port bring-up via esp_lvgl_port_add_disp (Stage 30.2 rewrite).
 *
 * See lvgl_ui_display.h for the API contract and the force-no-panel path.
 *
 * Migration summary (was: custom lvgl_flush_cb + byte-swap + esp_cache_msync
 * + 617 KB PSRAM full-frame buffer):
 *
 *   Now:  esp_lvgl_port_add_disp(io + panel + double-buffered strip in
 *         internal DMA-capable SRAM). The port drives the CO5300 via
 *         esp_lcd_panel_draw_bitmap; the CO5300 panel wrapper
 *         (components/co5300/co5300_panel.c) does the CASET/RASET/0x32
 *         framing over the io. The SPI ISR fires on_color_trans_done which
 *         the port turns into lv_display_flush_ready -- LVGL can start
 *         rendering the next area into the OTHER strip while the current
 *         one is still on the wire (DMA overlap).
 *
 * Byte order: CO5300 panel init sets MADCTL.BGR (0x08) so the panel accepts
 * LVGL's native RGB888 [B, G, R] byte order verbatim (see co5300_panel.c
 * co5300_op_init). No in-flush byte swap, no cache msync.
 *
 * Buffer sizing: 60-row strip = 60 * 410 pixels = 24 600 px. Double-buffered
 * = ~144 KB in internal SRAM. Strip is above the 32 KB SPI DMA per-transaction
 * cap so full-strip flushes route through the multi-chunk sync fallback in
 * co5300_panel_io.c (loses DMA overlap on the strip; small dirty regions still
 * go single-chunk async and keep DMA overlap). Chosen so the 48 px clock
 * glyph fits in a single flush -- Stage 30 §4.2g showed 25-row strips split
 * the clock and sheared it. See §4.2h for the buffer-size trade-off.
 */

#include "lvgl_ui_display.h"

#include "boot_display.h"      /* io + panel + LCD_*_RES + touch presence */
#include "cst9217.h"           /* g_touch_q, cst9217_point_t, task_touch_fn */
#include "lvgl_ui.h"           /* lvgl_ui_init */
#include "ui_broker.h"         /* ui_settings_t, ui_broker_init */
#include "ui_settings_screen.h"/* settings_screen_get_tileview */
#include "tile_registry.h"     /* tile_entry_t, tile_registry_get/count */
#include "app_nvs.h"           /* app_nvs_load_ui_settings */

#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "LVGL_DISP";

/* Strip height for partial refresh. 60 * 410 * 3 = 73 800 B per buffer;
 * double-buffered = ~144 KB in internal SRAM (fits with the ~276 KB reported
 * post-boot heap and the ~130 KB LVGL keeps for widget objects).
 *
 * Sized to cover the tallest single-flush glyph on the main screen, the
 * 48-px UI_FONT_TIME_XL clock label. Stage 30 §4.2g showed 25-row strips
 * split the clock across ~2 strips, and any per-strip alignment slip
 * shears the glyph -- the same class of bug Stage 29 fixed by going
 * full-frame PSRAM. 60 rows keeps the whole clock in one flush without
 * paying the PSRAM latency tax.
 *
 * Strip > 32 KB so single-chunk async is unreachable -- these flushes route
 * through the multi-chunk sync fallback in co5300_panel_io.c
 * (spi_device_acquire_bus + polling_transmit, chunked to pixel_chunk_bytes).
 * That path is DMA-cap-safe per chunk and CS-held across the burst; the
 * cost is losing DMA overlap on the strip flush. See Stage 30 §4.2h.
 *
 * Small dirty regions (< 32 KB) still route to the single-chunk async
 * path and keep DMA overlap.
 */
#define LVGL_STRIP_ROWS   60
#define LVGL_STRIP_PIXELS ((size_t)LCD_H_RES * LVGL_STRIP_ROWS)

/* -- Module state -------------------------------------------------------- */

static bool             s_up            = false;
static bool             s_forced        = false;
static lv_display_t    *s_lv_disp       = NULL;
static lv_indev_t      *s_lv_indev      = NULL;
static uint32_t         s_touch_events  = 0;
static cst9217_point_t  s_last_pt       = {0};

/* -- Touch indev callback (unchanged from pre-30.2) ---------------------- */

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (boot_display_is_asleep()) {
        data->point.x  = s_last_pt.x;
        data->point.y  = s_last_pt.y;
        data->state    = LV_INDEV_STATE_RELEASED;
        return;
    }
    cst9217_point_t pt;
    if (g_touch_q && xQueuePeek(g_touch_q, &pt, 0) == pdTRUE && pt.fingers > 0) {
        s_last_pt      = pt;
        s_touch_events++;
        extern void field_capture_kick_activity(void);
        field_capture_kick_activity();
        data->point.x  = pt.x;
        data->point.y  = pt.y;
        data->state    = LV_INDEV_STATE_PRESSED;
    } else {
        data->point.x  = s_last_pt.x;
        data->point.y  = s_last_pt.y;
        data->state    = LV_INDEV_STATE_RELEASED;
    }
}

/* -- Setup --------------------------------------------------------------- */

esp_err_t lvgl_ui_display_setup(bool force_no_panel)
{
    ESP_LOGI(TAG, "lvgl_ui_display v%s (force_no_panel=%d)",
             LVGL_UI_DISPLAY_DRIVER_VERSION, force_no_panel ? 1 : 0);

    if (s_up) {
        ESP_LOGW(TAG, "already up");
        return ESP_OK;
    }

    esp_lcd_panel_io_handle_t io    = boot_display_get_panel_io();
    esp_lcd_panel_handle_t    panel = boot_display_get_panel();
    s_forced = force_no_panel && (io == NULL);

    if (io == NULL && !force_no_panel) {
        ESP_LOGI(TAG, "no CO5300 handles and no force flag -- LVGL skipped");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_forced) {
        /* Force mode is a bench-only path for iv7.1 (no display module).
         * lvgl_port_add_disp asserts on a non-NULL io; skip the display
         * creation entirely and keep LVGL running headless so gestures /
         * event handlers can still be smoke-tested via CLI.
         */
        const lvgl_port_cfg_t lvgl_cfg = {
            .task_priority     = 4,
            .task_stack        = 7168,
            .task_affinity     = -1,
            .task_max_sleep_ms = 500,
            .task_stack_caps   = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
            .timer_period_ms   = 5,
        };
        esp_err_t err = lvgl_port_init(&lvgl_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "lvgl_port_init (forced) failed: %s", esp_err_to_name(err));
            return err;
        }
        s_up = true;
        ESP_LOGI(TAG, "LVGL up (forced-no-panel; no display created)");
        return ESP_OK;
    }

    /* 1. LVGL port -- timer + task + global mutex, Core -1. */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority     = 4,
        .task_stack        = 7168,
        .task_affinity     = -1,
        .task_max_sleep_ms = 500,
        .task_stack_caps   = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms   = 5,
    };
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. Display -- double-buffered 40-row strip in internal DMA SRAM.
     *    Port handles alloc, cache sync, flush handoff, byte order (for
     *    RGB565 only; RGB888 byte order is handled panel-side via MADCTL.BGR).
     */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = (uint32_t)LVGL_STRIP_PIXELS,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .color_format  = LV_COLOR_FORMAT_RGB888,
        .rotation      = {
            /* Landscape reverted to portrait after Stage 30 §4.3 first attempt
             * (fw 0.4.73) glitched fundamentally: green borders + black+glitch
             * middle. Root cause is the double-transform: our co5300_panel.c
             * op_draw_bitmap does a SW coord swap when ctx->swap_xy is set,
             * AND MADCTL.MV=1 makes the panel HW-swap its own address
             * interpretation. On top of that x_gap (CO5300_COL_OFFSET) gets
             * applied to whichever axis the SW branch names "col", which no
             * longer matches the panel's physical column direction under MV.
             *
             * Proper landscape lands in a later batch. Two viable paths:
             *   (a) LVGL software rotation (lv_display_set_rotation) --
             *       panel stays portrait via MADCTL, LVGL rotates the pixel
             *       buffer before flush. Costs CPU per flush; zero panel
             *       driver changes; guaranteed correct.
             *   (b) Rewrite co5300_panel.c op_draw_bitmap to trust MADCTL
             *       only (no SW swap; x_gap re-anchored to physical col
             *       under MV). Matches the ST7789 esp_lcd pattern; needs
             *       careful validation of the QSPI 0x32 pixel command and
             *       COL_OFFSET semantics under MV.
             */
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            /* buff_dma=1 + RGB888 trips a hard guard in esp_lvgl_port
             * ("DMA buffer can be used only in RGB565 (not aligned copy)").
             * Leave it off -- the port then allocates via MALLOC_CAP_DEFAULT
             * which on ESP32-S3 lands in internal SRAM, which IS DMA-capable
             * for the SPI DMA path. buff_spiram stays off so we keep the
             * fast internal-memory placement.
             */
            .buff_dma    = 0,
            .buff_spiram = 0,
            .swap_bytes  = 0,        /* RGB565-only; RGB888 handled by MADCTL.BGR */
        },
    };
    s_lv_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_lv_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        (void)lvgl_port_deinit();
        return ESP_FAIL;
    }

    /* 3. Touch indev -- CST9217 owned by cst9217 driver, our indev cb peeks
     *    g_touch_q. Registered under the port lock like any other LVGL
     *    object creation.
     */
    if (boot_display_touch_is_present()) {
        if (!lvgl_port_lock(0)) {
            ESP_LOGW(TAG, "port_lock failed while creating touch indev");
        } else {
            s_lv_indev = lv_indev_create();
            if (s_lv_indev == NULL) {
                ESP_LOGW(TAG, "lv_indev_create failed -- LVGL up, touch off");
            } else {
                lv_indev_set_type(s_lv_indev, LV_INDEV_TYPE_POINTER);
                lv_indev_set_display(s_lv_indev, s_lv_disp);
                lv_indev_set_read_cb(s_lv_indev, lvgl_touch_read_cb);
                ESP_LOGI(TAG, "LVGL touch indev bound to g_touch_q");
            }
            lvgl_port_unlock();
        }
    }

    s_up = true;
    ESP_LOGI(TAG, "LVGL up -- panel=CO5300 touch=%s strip=%dx%d px (%zu B x2)",
             s_lv_indev ? "CST9217" : "off",
             LCD_H_RES, LVGL_STRIP_ROWS,
             LVGL_STRIP_PIXELS * 3U);
    return ESP_OK;
}

/* -- Accessors ----------------------------------------------------------- */

bool   lvgl_ui_display_is_up(void)      { return s_up; }
bool   lvgl_ui_display_is_forced(void)  { return s_forced; }
size_t lvgl_ui_display_buf_bytes(void)
{
    /* Two strips in DMA SRAM. Kept for STATUS provenance. */
    return s_forced ? 0 : (LVGL_STRIP_PIXELS * 3U * 2U);
}

/* -- Boot screens (unchanged) ------------------------------------------- */

esp_err_t lvgl_ui_display_boot_screens(void)
{
    if (!s_up) {
        ESP_LOGW(TAG, "boot_screens called before setup -- skipping");
        return ESP_ERR_INVALID_STATE;
    }

    ui_settings_t cfg = {0};
    (void)app_nvs_load_ui_settings(&cfg);

    (void)ui_broker_init();

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "boot_screens: lvgl_port_lock failed");
        return ESP_FAIL;
    }
    lvgl_ui_init(&cfg);
    lvgl_port_unlock();

    if (!s_forced) {
        backlight_set_brightness(cfg.brightness);
    }

    ESP_LOGI(TAG, "screens built (theme=%u bright=%u%%%s)",
             cfg.theme, cfg.brightness,
             s_forced ? ", flush=no-op" : "");
    return ESP_OK;
}

/* -- Task spawn --------------------------------------------------------- */

void lvgl_ui_display_start_tasks(void)
{
    if (!s_up) {
        ESP_LOGI(TAG, "start_tasks: LVGL not up -- skipping refresh + saver");
        return;
    }

    BaseType_t r1 = xTaskCreatePinnedToCore(task_ui_refresh_fn,
                                            "task_ui", 6144,
                                            NULL, 3, NULL, 1);
    BaseType_t r2 = xTaskCreate(task_settings_saver_fn,
                                "task_uisave", 3072,
                                (void *)ui_settings_save_q, 2, NULL);
    if (r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "LVGL task create failed (ui=%d saver=%d)",
                 (int)r1, (int)r2);
        configASSERT(false);
    }

    if (boot_display_touch_is_present()) {
        BaseType_t r3 = xTaskCreatePinnedToCore(task_touch_fn,
                                                "task_touch", 3072,
                                                NULL, 4, NULL, 0);
        if (r3 != pdPASS) {
            ESP_LOGE(TAG, "task_touch create failed (%d)", (int)r3);
            configASSERT(false);
        }
        ESP_LOGI(TAG, "LVGL tasks up -- UI_REFRESH + SETTINGS_SAVER + TOUCH");
    } else {
        ESP_LOGI(TAG, "LVGL tasks up -- UI_REFRESH + SETTINGS_SAVER (no touch)");
    }
}

/* -- Tile navigation ---------------------------------------------------- */

int lvgl_ui_display_tile_count(void)
{
    return s_up ? (int)tile_registry_count() : 0;
}

bool lvgl_ui_display_touch_indev_ready(void)
{
    return s_lv_indev != NULL;
}

void lvgl_ui_display_touch_snapshot(uint16_t *out_x, uint16_t *out_y,
                                    uint32_t *out_events, bool *out_pressed)
{
    if (out_x)      *out_x      = s_last_pt.x;
    if (out_y)      *out_y      = s_last_pt.y;
    if (out_events) *out_events = s_touch_events;
    if (out_pressed) {
        cst9217_point_t pt;
        *out_pressed = (g_touch_q &&
                        xQueuePeek(g_touch_q, &pt, 0) == pdTRUE &&
                        pt.fingers > 0);
    }
}

esp_err_t lvgl_ui_display_jump_tile(int col_idx)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    const int count = (int)tile_registry_count();
    if (col_idx < 0 || col_idx >= count) return ESP_ERR_INVALID_ARG;

    tile_entry_t *tiles = tile_registry_get();
    lv_obj_t *tile = tiles[col_idx].handle;
    lv_obj_t *tv   = settings_screen_get_tileview();
    if (tile == NULL || tv == NULL) return ESP_ERR_INVALID_STATE;

    if (!lvgl_port_lock(0)) return ESP_FAIL;
    lv_tileview_set_tile(tv, tile, LV_ANIM_ON);
    lvgl_port_unlock();
    return ESP_OK;
}
