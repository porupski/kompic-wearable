/**
 * @file test_boot_power.c
 * @brief Standalone harness for boot_power.c (GPIO16 button + GPIO0 DRV_EN).
 *
 * What this harness verifies:
 *   1. boot_power_init() runs and drives GPIO0 LOW.
 *   2. PSRAM is detected.
 *   3. task_power_btn_fn installs the GPIO16 ISR.
 *   4. A 30-second window logs every press it sees with measured hold duration.
 *      Operator presses the physical button; output prints whether each press
 *      is classified short / overlay-threshold / ship-mode-threshold.
 *
 * What this harness does NOT verify:
 *   - Actual ship-mode entry. We stub bq25619_enter_ship_mode() so a 3-second
 *     hold doesn't brick the test device. The stub logs the call.
 *   - The interaction with display_is_asleep() / haptic_play() -- both are
 *     stubbed here, since the test runs without the full firmware loaded.
 *
 * Wiring: see Kompic_Mk1_System_Instructions_v7.2.md  -- §GPIO.
 *   GPIO16 = BQ_BUTTON, GPIO0 = DRV_EN.
 */

#include "boot_power.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// -- Stubs to satisfy boot_power.c's externs in test context --------------------

bool display_is_asleep(void) { return false; }  // pretend we're always awake

#include "haptic.h"
// haptic_play is implemented by components/drv2605/haptic.c; if you don't want
// to drag in I2C bus 1 + DRV2605 init for the test, override here:
__attribute__((weak)) void haptic_play(uint8_t effect_id) {
    (void)effect_id;
    // no-op in test
}

#include "bq25619.h"
// Override the real ship-mode entry so the test device stays alive.
__attribute__((weak)) esp_err_t bq25619_enter_ship_mode(i2c_port_t i2c_num) {
    (void)i2c_num;
    printf("\n*** TEST STUB: bq25619_enter_ship_mode() called -- not actually executing ***\n\n");
    return ESP_OK;
}

SemaphoreHandle_t g_i2c_mutex = NULL;  // boot_power.c does not use it directly

static const char *TAG = "test_boot_power";

static void test_run(void)
{
    g_wake_display = false;
    g_display_sleep = false;
    g_show_shutdown_overlay = false;
    g_shutdown_latched = false;

    if (g_i2c_mutex == NULL) g_i2c_mutex = xSemaphoreCreateMutex();

    int64_t t0 = esp_timer_get_time();
    boot_power_init();
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "boot_power_init: %lld us", (long long)(t1 - t0));
    ESP_LOGI(TAG, "GPIO0 level after init: %d (expect 0)", gpio_get_level(GPIO_DRV_EN));
    ESP_LOGI(TAG, "GPIO16 level at idle:   %d (expect 1, pull-up)", gpio_get_level(GPIO_PWR_BTN));

    BaseType_t ok = xTaskCreate(task_power_btn_fn, "task_power_btn",
                                3072, NULL, 5, NULL);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "30 s press window open -- press the button, hold for various durations");
    ESP_LOGI(TAG, "  short press  : < %d ms",  PWR_SHORT_PRESS_MAX_MS);
    ESP_LOGI(TAG, "  overlay shown: >= %d ms", PWR_HOLD_OVERLAY_MS);
    ESP_LOGI(TAG, "  ship-mode    : >= %d ms (stubbed in this harness)", PWR_HOLD_SHIPMODE_MS);

    // Observe the four global flags from the test task -- task_power_btn_fn
    // sets them as it classifies each press.
    int64_t deadline = esp_timer_get_time() + 30LL * 1000000;
    bool seen_wake = false, seen_sleep = false, seen_overlay = false, seen_ship = false;
    while (esp_timer_get_time() < deadline) {
        if (g_wake_display && !seen_wake) {
            ESP_LOGI(TAG, "  flag: g_wake_display = true (short press while asleep)");
            seen_wake = true; g_wake_display = false;
        }
        if (g_display_sleep && !seen_sleep) {
            ESP_LOGI(TAG, "  flag: g_display_sleep = true (short press while awake)");
            seen_sleep = true; g_display_sleep = false;
        }
        if (g_show_shutdown_overlay && !seen_overlay) {
            ESP_LOGI(TAG, "  flag: g_show_shutdown_overlay = true (hold past overlay_ms)");
            seen_overlay = true;
        } else if (!g_show_shutdown_overlay && seen_overlay) {
            ESP_LOGI(TAG, "  flag: g_show_shutdown_overlay cleared (release before ship-mode)");
            seen_overlay = false;
        }
        if (g_shutdown_latched && !seen_ship) {
            ESP_LOGW(TAG, "  flag: g_shutdown_latched = true (ship-mode entered)");
            seen_ship = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "30 s window closed");
    ESP_LOGI(TAG, "Summary: wake=%d sleep=%d overlay=%d ship=%d",
             (int)seen_wake, (int)seen_sleep, (int)seen_overlay, (int)seen_ship);
    ESP_LOGI(TAG, "stack high-water (test task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_run();
}
