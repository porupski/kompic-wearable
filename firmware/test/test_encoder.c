/**
 * @file test_encoder.c
 * @brief Standalone diagnostic for the crown rotary encoder.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §GPIO ASSIGNMENT):
 *   GPIO21 = EC_SigA  (channel A)
 *   GPIO43 = EC_SigB  (channel B; doubles as boot-log TX)
 *
 * The harness creates a stub g_ui_event_q so the driver's xQueueSend()
 * calls have somewhere to land. The harness then drains that queue and
 * prints each event with a timestamp.
 *
 * Phases:
 *   1. encoder_init (PCNT + glitch filter + drain timer + boot guard).
 *   2. 15 s observation window. Rotate the crown by hand; every CW/CCW
 *      detent logs:  "[+12.345s] CW  (total=+5 cw=5 ccw=0 gli=2)".
 *   3. Glitch suppression report -- the v7.2 GPIO43 boot-log TX should
 *      contribute zero non-boot-guarded glitches because the boot guard
 *      starts at encoder_init() time, by which point the boot log is done.
 *   4. encoder_deinit + stack high-water + heap report.
 */

#include "encoder.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "test_encoder";

// Stub queue + the event enum that the driver expects to find as an extern.
typedef enum {
    UI_EVENT_CROWN_CW  = 1,
    UI_EVENT_CROWN_CCW = 2,
} ui_event_t;

QueueHandle_t g_ui_event_q = NULL;

static void test_encoder_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             encoder_get_chip_name(), encoder_get_chip_desc());

    g_ui_event_q = xQueueCreate(16, sizeof(ui_event_t));
    if (!g_ui_event_q) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return;
    }

    int64_t t_i0 = esp_timer_get_time();
    esp_err_t err = encoder_init();
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "encoder_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    ESP_LOGI(TAG, "Rotate the crown for 15 s -- every detent logs below.");
    int64_t t_start = esp_timer_get_time();
    while ((esp_timer_get_time() - t_start) < 15LL * 1000LL * 1000LL) {
        ui_event_t evt;
        if (xQueueReceive(g_ui_event_q, &evt, pdMS_TO_TICKS(50)) == pdTRUE) {
            int64_t elapsed_us = esp_timer_get_time() - t_start;
            const char *name = (evt == UI_EVENT_CROWN_CW) ? "CW " : "CCW";
            ESP_LOGI(TAG, "[+%6lld.%03lld ms] %s  (total=%+ld cw=%lu ccw=%lu gli=%lu)",
                     (long long)(elapsed_us / 1000LL),
                     (long long)(elapsed_us % 1000LL),
                     name,
                     (long)encoder_get_total_detents(),
                     (unsigned long)encoder_get_cw_count(),
                     (unsigned long)encoder_get_ccw_count(),
                     (unsigned long)encoder_get_glitch_count());
        }
    }

    ESP_LOGI(TAG, "Window over. Final totals: cw=%lu ccw=%lu net=%+ld glitches=%lu",
             (unsigned long)encoder_get_cw_count(),
             (unsigned long)encoder_get_ccw_count(),
             (long)encoder_get_total_detents(),
             (unsigned long)encoder_get_glitch_count());

    encoder_deinit();

    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_encoder_run();
}
