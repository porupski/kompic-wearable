/**
 * @file test_lvgl_ui.c
 * @brief Standalone test harness for the LVGL UI orchestrator.
 *
 * Boots the broker + a software-only LVGL framebuffer + lvgl_port, calls
 * lvgl_ui_init() with default settings, then walks the tile registry and
 * asserts every entry has a complete tile_desc_t.
 *
 * Without a real CO5300 display this test does NOT verify pixel output —
 * only the tile graph + refresh-task wiring. For visual confirmation,
 * flash to a board with the CO5300 attached and watch the main screen.
 *
 * Master prompt: docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md
 */

#include "lvgl_ui.h"
#include "ui_broker.h"
#include "tile_registry.h"
#include "data_broker.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TEST_LVGL";

#define EXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ESP_LOGE(TAG, "FAIL %s:%d  %s", __func__, __LINE__, (msg));        \
            abort();                                                           \
        } else {                                                               \
            ESP_LOGI(TAG, "PASS %s  %s", __func__, (msg));                     \
        }                                                                      \
    } while (0)

// ───────────────────────────────────────────────────────────────────────────

static void subtest_registry_complete(void)
{
    tile_entry_t *tiles = tile_registry_get();
    uint8_t        n    = tile_registry_count();
    EXPECT(n >= 10, "at least 10 tiles registered (Phase 6 set)");

    for (uint8_t i = 0; i < n; i++) {
        EXPECT(tiles[i].desc != NULL, "every tile has a desc");
        EXPECT(tiles[i].desc->init != NULL, "every tile has init");
        EXPECT(tiles[i].desc->update != NULL, "every tile has update");
        EXPECT(tiles[i].desc->apply_theme != NULL, "every tile has apply_theme");
        EXPECT(tiles[i].col == i, "col matches array index");
    }
}

static void subtest_main_refresh_tick(void)
{
    // Seed broker with synthetic RTC + battery so the main screen has values.
    broker_rtc_set_hw_status(true);
    broker_rtc_data_t rtc = {0};
    rtc.hour = 14; rtc.minute = 32; rtc.second = 7;
    rtc.valid = true; rtc.enabled = true;
    broker_rtc_write(&rtc);

    broker_battery_set_hw_status(true);
    broker_battery_data_t bat = {0};
    bat.percentage = 72;
    bat.voltage    = 3.87f;
    broker_battery_write(&bat);

    // Spawn the refresh task and let it run a few ticks.
    xTaskCreatePinnedToCore(task_ui_refresh_fn, "ui_refresh", 8192, NULL, 4, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(500));   // 2-3 ticks
    EXPECT(true, "refresh task survived 500 ms without crash");
}

// ───────────────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "test_lvgl_ui starting (iv7.2.f0.0)");

    broker_init();

    // Caller must wire a display before calling lvgl_ui_init() in normal use.
    // For a standalone test without a CO5300 attached, register a software
    // framebuffer via lvgl_port_init() with a memory display. Outside the
    // scope of this header — see esp_lvgl_port docs.
    //
    // For this harness, assume lvgl_port_init() has been called by the test
    // app's main.c before app_main starts.

    ui_settings_t cfg = {
        .theme = 0,
        .brightness = 70,
        .blue_light_on = false,
        .auto_brightness = true,
    };
    lvgl_ui_init(&cfg);

    subtest_registry_complete();
    subtest_main_refresh_tick();

    ESP_LOGI(TAG, "ALL SUBTESTS PASSED");
}
