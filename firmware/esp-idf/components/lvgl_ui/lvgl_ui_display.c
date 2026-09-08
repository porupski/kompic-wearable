/**
 * @file lvgl_ui_display.c
 * @brief LVGL port bring-up + CO5300 flush callback. Stage 22 §4.1b.
 *
 * See lvgl_ui_display.h for the API contract and the force-no-panel path.
 *
 * Buffer sizing (partial refresh, single buffer):
 *   LCD_H_RES × LVGL_STRIP_ROWS × 3 bytes/pixel = 466 × 40 × 3 = 55 920 B.
 *   Allocated MALLOC_CAP_DMA in internal SRAM so spi_device_polling_transmit
 *   can DMA it out over QSPI without a bounce copy.
 *
 * Color format:
 *   CO5300 mandates RGB888 (COLMOD 0x77) — the chip's RGB565 QIO lane mapping
 *   is broken per the datasheet. LVGL is built with LV_COLOR_DEPTH=16 for
 *   memory reasons, so we set this specific display's format to
 *   LV_COLOR_FORMAT_RGB888 and let LVGL convert on flush.
 *
 * Byte order:
 *   LVGL 9's LV_COLOR_FORMAT_RGB888 lays pixels out as [R, G, B] in memory.
 *   CO5300 reads the pixel stream in the same order. First-boot bench on Mk1b
 *   will confirm the on-panel color — if red and blue swap, add a byte swap
 *   in the flush callback loop.
 */

#include "lvgl_ui_display.h"

#include "boot_display.h"      // boot_display_get_co5300(), touch_is_present, LCD_*_RES
#include "co5300.h"            // co5300_set_window, co5300_write_pixels, CO5300_PIXEL_ADDR
#include "cst9217.h"           // g_touch_q, cst9217_point_t, task_touch_fn
#include "lvgl_ui.h"           // lvgl_ui_init
#include "ui_broker.h"         // ui_settings_t, ui_broker_init
#include "ui_settings_screen.h"// settings_screen_get_tileview
#include "tile_registry.h"     // tile_entry_t, tile_registry_get/count
#include "app_nvs.h"           // app_nvs_load_ui_settings

#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "LVGL_DISP";

// Strip height in rows -- partial refresh buffer size = LCD_H_RES * rows * 3B.
// 40 rows on a 466-wide panel is 55 920 B. Bump if flush frequency turns out
// to be a bottleneck; the CO5300 SPI bus is provisioned for up to 64 KB.
#define LVGL_STRIP_ROWS  40

// Module state ---------------------------------------------------------------

static bool             s_up            = false;
static bool             s_forced        = false;
static co5300_handle_t  s_disp          = NULL;
static lv_display_t    *s_lv_disp       = NULL;
static uint8_t         *s_draw_buf      = NULL;
static size_t           s_draw_buf_size = 0;
static lv_indev_t      *s_lv_indev      = NULL;
static uint32_t         s_touch_events  = 0;    // monotonic PRESSED tick counter
static cst9217_point_t  s_last_pt       = {0};  // last non-empty peek (for TOUCH verb)

// Flush callback -------------------------------------------------------------
//
// LVGL 9 signature: void (*)(lv_display_t*, const lv_area_t*, uint8_t*).
// The pixel map is in LV_COLOR_FORMAT_RGB888 order (3 bytes per pixel, R G B).
// Under force_no_panel, s_disp is NULL and we skip the SPI writes entirely.
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    if (s_disp != NULL) {
        (void)co5300_set_window(s_disp,
                                (uint16_t)area->x1, (uint16_t)area->y1,
                                (uint16_t)area->x2, (uint16_t)area->y2);
        const size_t count = (size_t)(area->x2 - area->x1 + 1) *
                             (size_t)(area->y2 - area->y1 + 1);
        (void)co5300_write_pixels(s_disp, CO5300_PIXEL_ADDR, px_map, count);
    }
    // Under force_no_panel (s_disp == NULL) we deliberately drop the pixels;
    // LVGL only needs the flush_ready ack to advance its frame counter.
    lv_display_flush_ready(disp);
}

// Touch indev callback -------------------------------------------------------
//
// Non-blocking peek of g_touch_q. task_touch_fn on Core 0 owns the queue and
// does xQueueOverwrite() on every ISR-driven report, so the depth-1 slot
// always holds the most recent report. We keep the last-seen point in module
// state so the TOUCH CLI verb can dump something meaningful even between
// LVGL polls.
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    cst9217_point_t pt;
    if (g_touch_q && xQueuePeek(g_touch_q, &pt, 0) == pdTRUE && pt.fingers > 0) {
        s_last_pt      = pt;
        s_touch_events++;
        data->point.x  = pt.x;
        data->point.y  = pt.y;
        data->state    = LV_INDEV_STATE_PRESSED;
    } else {
        // Stay on the last-seen coordinates so LVGL sees a clean release edge
        // rather than teleporting the cursor to (0,0) between touches.
        data->point.x  = s_last_pt.x;
        data->point.y  = s_last_pt.y;
        data->state    = LV_INDEV_STATE_RELEASED;
    }
}

// Setup ----------------------------------------------------------------------

esp_err_t lvgl_ui_display_setup(bool force_no_panel)
{
    ESP_LOGI(TAG, "lvgl_ui_display v%s (force_no_panel=%d)",
             LVGL_UI_DISPLAY_DRIVER_VERSION, force_no_panel ? 1 : 0);

    if (s_up) {
        ESP_LOGW(TAG, "already up");
        return ESP_OK;
    }

    s_disp   = boot_display_get_co5300();   // NULL if headless or force mode
    s_forced = force_no_panel && (s_disp == NULL);

    if (s_disp == NULL && !force_no_panel) {
        ESP_LOGI(TAG, "no CO5300 handle and no force flag -- LVGL skipped");
        return ESP_ERR_NOT_FOUND;
    }

    // 1. LVGL port (timer + task + global mutex). Pinned Core -1 so the
    //    scheduler places it opposite of whichever core is busier at boot.
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority    = 4,
        .task_stack       = 7168,
        .task_affinity    = -1,
        .task_max_sleep_ms = 500,
        .task_stack_caps  = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms  = 5,
    };
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 2. Draw buffer -- partial refresh, RGB888.
    s_draw_buf_size = (size_t)LCD_H_RES * LVGL_STRIP_ROWS * 3U;
    s_draw_buf      = heap_caps_aligned_alloc(4, s_draw_buf_size,
                                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_draw_buf == NULL) {
        ESP_LOGE(TAG, "draw buffer alloc failed (%zu B DMA)", s_draw_buf_size);
        (void)lvgl_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    memset(s_draw_buf, 0, s_draw_buf_size);

    // 3. Create the LVGL display + register the flush callback. Wrapped in
    //    lvgl_port_lock so nothing races with the LVGL task started above.
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        heap_caps_free(s_draw_buf);
        s_draw_buf = NULL;
        (void)lvgl_port_deinit();
        return ESP_FAIL;
    }

    s_lv_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (s_lv_disp == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_display_create failed");
        heap_caps_free(s_draw_buf);
        s_draw_buf = NULL;
        (void)lvgl_port_deinit();
        return ESP_FAIL;
    }
    lv_display_set_color_format(s_lv_disp, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(s_lv_disp, s_draw_buf, NULL,
                           (uint32_t)s_draw_buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_lv_disp, lvgl_flush_cb);

    // 4. Touch indev -- only when boot_display_init() also brought up CST9217.
    //    Under LVGL_FORCE ON on iv7.1 the queue does not exist; skip the indev
    //    so the LVGL pointer state stays at default RELEASED.
    if (boot_display_touch_is_present()) {
        s_lv_indev = lv_indev_create();
        if (s_lv_indev == NULL) {
            ESP_LOGW(TAG, "lv_indev_create failed -- LVGL up, touch off");
        } else {
            lv_indev_set_type(s_lv_indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_display(s_lv_indev, s_lv_disp);
            lv_indev_set_read_cb(s_lv_indev, lvgl_touch_read_cb);
            ESP_LOGI(TAG, "LVGL touch indev bound to g_touch_q");
        }
    }
    lvgl_port_unlock();

    s_up = true;
    ESP_LOGI(TAG, "LVGL up -- panel=%s touch=%s buf=%zu B (%d rows × %d px × 3B)",
             s_forced ? "forced-off" : "CO5300",
             s_lv_indev ? "CST9217" : "off",
             s_draw_buf_size, LVGL_STRIP_ROWS, LCD_H_RES);
    return ESP_OK;
}

// Accessors ------------------------------------------------------------------

bool   lvgl_ui_display_is_up(void)      { return s_up; }
bool   lvgl_ui_display_is_forced(void)  { return s_forced; }
size_t lvgl_ui_display_buf_bytes(void)  { return s_draw_buf_size; }

// Boot screens ---------------------------------------------------------------
//
// Runs AFTER lvgl_ui_display_setup(). Loads NVS-persisted UI settings, creates
// the settings-save queue (ui_broker_init), then calls lvgl_ui_init inside the
// port lock -- that builds main + settings + alarm screens, registers gesture
// callbacks and overlays, iterates tile_registry to build all 10 tiles, and
// applies the theme.

esp_err_t lvgl_ui_display_boot_screens(void)
{
    if (!s_up) {
        ESP_LOGW(TAG, "boot_screens called before setup -- skipping");
        return ESP_ERR_INVALID_STATE;
    }

    // Load persisted UI settings; on first boot / partial NVS, defaults come
    // from app_nvs_load_ui_settings itself (theme=DARK, brightness=70).
    ui_settings_t cfg = {0};
    (void)app_nvs_load_ui_settings(&cfg);

    // Create the async save queue used by tile event handlers.
    (void)ui_broker_init();

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "boot_screens: lvgl_port_lock failed");
        return ESP_FAIL;
    }
    lvgl_ui_init(&cfg);
    lvgl_port_unlock();

    // Restore last-known brightness on the real panel (no-op in forced mode).
    if (!s_forced) {
        backlight_set_brightness(cfg.brightness);
    }

    ESP_LOGI(TAG, "screens built (theme=%u bright=%u%%%s)",
             cfg.theme, cfg.brightness,
             s_forced ? ", flush=no-op" : "");
    return ESP_OK;
}

// Task spawn -----------------------------------------------------------------

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

    // Touch task -- only when the CST9217 came up (skipped under LVGL_FORCE
    // on iv7.1). Pinned Core 0 for I2C0 mutex locality with the sensor tasks.
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

// Tile navigation ------------------------------------------------------------

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
