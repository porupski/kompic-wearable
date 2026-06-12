/**
 * @file test_ws2812.c
 * @brief Standalone diagnostic for the WS2812B single-pixel status LED.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §GPIO ASSIGNMENT):
 *   GPIO42 -> WS2812B Din. Single pixel (not a strip).
 *
 * Visual inspection is the primary test signal -- if you can see the LED,
 * the timings are within spec. For a more rigorous check, a logic analyzer
 * on GPIO42 will show 0.4/0.8 us bit-time pairs at 800 kHz nominal.
 *
 * Phases:
 *   1. ws2812_init (RMT + bytes encoder + 50 ms animation tick).
 *   2. Color sweep: RED -> GREEN -> BLUE -> YELLOW -> MAGENTA -> CYAN ->
 *      WHITE -> OFF, 500 ms each, via ws2812_set_color().
 *   3. State machine demo: IDLE 2 s, CHARGING 4 s (you should see the
 *      sinusoidal blue pulse), CHARGED 2 s, ALERT 4 s (250 ms red square
 *      wave), OFF.
 *   4. ws2812_deinit + stack high-water + heap.
 */

#include "ws2812.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_ws2812";

static void test_ws2812_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             ws2812_get_chip_name(), ws2812_get_chip_desc());

    int64_t t_i0 = esp_timer_get_time();
    esp_err_t err = ws2812_init();
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "ws2812_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ── Phase 2: 8-step color sweep ─────────────────────────────────────────
    const struct { const char *name; uint32_t rgb; } colors[] = {
        { "RED",     WS2812_RED     },
        { "GREEN",   WS2812_GREEN   },
        { "BLUE",    WS2812_BLUE    },
        { "YELLOW",  WS2812_YELLOW  },
        { "MAGENTA", WS2812_MAGENTA },
        { "CYAN",    WS2812_CYAN    },
        { "WHITE",   WS2812_WHITE   },
        { "OFF",     WS2812_OFF     },
    };
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        uint8_t r = (colors[i].rgb >> 16) & 0xFF;
        uint8_t g = (colors[i].rgb >>  8) & 0xFF;
        uint8_t b = (colors[i].rgb)       & 0xFF;
        ESP_LOGI(TAG, "[Phase 2] %-8s  (0x%06lx)", colors[i].name,
                 (unsigned long)colors[i].rgb);
        ws2812_set_color(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // ── Phase 3: state machine demo ─────────────────────────────────────────
    const struct { const char *name; ws2812_state_t st; int dwell_ms; } states[] = {
        { "IDLE",     WS2812_STATE_IDLE,     2000 },
        { "CHARGING", WS2812_STATE_CHARGING, 4000 },
        { "CHARGED",  WS2812_STATE_CHARGED,  2000 },
        { "ALERT",    WS2812_STATE_ALERT,    4000 },
        { "OFF",      WS2812_STATE_OFF,      1000 },
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        ESP_LOGI(TAG, "[Phase 3] state=%s for %d ms",
                 states[i].name, states[i].dwell_ms);
        ws2812_set_state(states[i].st);
        vTaskDelay(pdMS_TO_TICKS(states[i].dwell_ms));
    }

    // ── Phase 4: teardown + memory ──────────────────────────────────────────
    ws2812_deinit();
    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_ws2812_run();
}
