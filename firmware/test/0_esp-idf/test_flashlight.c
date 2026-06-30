/**
 * @file test_flashlight.c
 * @brief Standalone diagnostic for the flashlight LEDC driver.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §GPIO ASSIGNMENT):
 *   GPIO41 -> high-power LED (via driver transistor / FET).
 *
 * Phases:
 *   1. flashlight_init.
 *   2. Sweep brightness 0 -> 100% in 10% steps with 500 ms dwell each.
 *      flashlight_get_brightness() is read back at each step.
 *   3. Bracket test: off / on / off using the convenience helpers.
 *   4. Pulse pattern: 5x (200 ms at 100% / 200 ms at 0%) to check for
 *      audible LEDC whine at the 1 kHz carrier (none expected).
 *   5. Off + deinit, stack high-water + heap report.
 *
 * Bench note: pair this with a current-meter on the LED rail; the
 * per-step current draw is the only profiling signal that matters for
 * the battery budget. Roughly linear in duty cycle for a high-power LED
 * in this regime.
 */

#include "flashlight.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_flashlight";

static void test_flashlight_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             flashlight_get_chip_name(), flashlight_get_chip_desc());

    int64_t t_i0 = esp_timer_get_time();
    esp_err_t err = flashlight_init();
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "flashlight_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ── Phase 2: 0 -> 100% in 10% steps ─────────────────────────────────────
    for (int pct = 0; pct <= 100; pct += 10) {
        int64_t t_s0 = esp_timer_get_time();
        err = flashlight_set_brightness((uint8_t)pct);
        int64_t t_s1 = esp_timer_get_time();
        ESP_LOGI(TAG, "set_brightness(%3d%%): %s in %lld us  readback=%u%%",
                 pct, esp_err_to_name(err), (long long)(t_s1 - t_s0),
                 (unsigned)flashlight_get_brightness());
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // ── Phase 3: off / on / off via convenience helpers ─────────────────────
    flashlight_off();
    vTaskDelay(pdMS_TO_TICKS(500));
    flashlight_on();
    vTaskDelay(pdMS_TO_TICKS(500));
    flashlight_off();
    vTaskDelay(pdMS_TO_TICKS(500));

    // ── Phase 4: 5x 200/200 ms pulse pattern ────────────────────────────────
    ESP_LOGI(TAG, "[Phase 4] 5x pulse 200/200 ms (listen for LEDC whine)");
    for (int i = 0; i < 5; i++) {
        flashlight_on();
        vTaskDelay(pdMS_TO_TICKS(200));
        flashlight_off();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // ── Phase 5: off + deinit + memory ──────────────────────────────────────
    flashlight_off();
    flashlight_deinit();

    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_flashlight_run();
}
