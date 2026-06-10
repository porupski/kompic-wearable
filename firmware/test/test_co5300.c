/**
 * @file test_co5300.c
 * @brief Standalone diagnostic for the CO5300 AMOLED driver skeleton.
 *
 * Not pretty. Just facts. Logs each step + wall-time so we can sanity-check
 * the driver before real hardware lands, and so the first bench bring-up has
 * a known-good baseline to compare to.
 *
 * Wiring: see Kompic_Mk1_System_Instructions_v7.2.md  -- DISPLAY section.
 *   CS=GPIO10, CLK=GPIO12, D0=GPIO11, D1=GPIO13, D2=GPIO9, D3=GPIO14,
 *   RST=GPIO3, TE=GPIO45 (unused in v1).
 *
 * Build: drop into your test project's main/ and add components/co5300 to the
 *        EXTRA_COMPONENT_DIRS, or build it inside this project's main with
 *        app_main() pointing here.
 *
 * Expected output (skeleton; numbers will be near-zero today, real on hw):
 *   I (xx) test_co5300: SPI bus init: <us> us
 *   I (xx) test_co5300: CO5300 init: <us> us
 *   I (xx) test_co5300: status read: ESP_ERR_NOT_SUPPORTED (see DEFECT-001)
 *   I (xx) test_co5300: DISPON  -> ESP_OK
 *   I (xx) test_co5300: brightness 50% -> ESP_OK in <us> us
 *   I (xx) test_co5300: brightness 100% -> ESP_OK in <us> us
 *   I (xx) test_co5300: sleep / wake cycle -> ESP_OK
 *   I (xx) test_co5300: All checks completed (driver is a stub).
 */

#include "co5300.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_co5300";

static void test_co5300_run(void) {
    ESP_LOGI(TAG, "Chip: %s -- %s", co5300_get_chip_name(), co5300_get_chip_desc());

    const size_t  heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const int64_t t_bus0 = esp_timer_get_time();
    // Bus init currently happens inside co5300_init (skeleton). When real
    // spi_bus_initialize lands we can split this out.
    const int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "SPI bus init: %lld us (bus init is fused into co5300_init in skeleton)",
             (long long)(t_bus1 - t_bus0));

    co5300_config_t cfg = CO5300_CONFIG_DEFAULT();
    co5300_handle_t h   = NULL;

    const int64_t t_init0 = esp_timer_get_time();
    esp_err_t err = co5300_init(&cfg, &h);
    const int64_t t_init1 = esp_timer_get_time();
    ESP_LOGI(TAG, "CO5300 init: %lld us -> %s",
             (long long)(t_init1 - t_init0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    uint8_t status[4] = {0};
    const int64_t t_st0 = esp_timer_get_time();
    err = co5300_get_status(h, status);
    const int64_t t_st1 = esp_timer_get_time();
    ESP_LOGI(TAG, "status read: %s (raw %02X %02X %02X %02X) dt=%lld us",
             esp_err_to_name(err), status[0], status[1], status[2], status[3],
             (long long)(t_st1 - t_st0));

    err = co5300_write_command(h, CO5300_CMD_DISPON);
    ESP_LOGI(TAG, "DISPON  -> %s", esp_err_to_name(err));

    const int64_t t_b50_0 = esp_timer_get_time();
    err = co5300_set_brightness(h, 50);
    const int64_t t_b50_1 = esp_timer_get_time();
    ESP_LOGI(TAG, "brightness 50%%  -> %s in %lld us",
             esp_err_to_name(err), (long long)(t_b50_1 - t_b50_0));

    const int64_t t_b100_0 = esp_timer_get_time();
    err = co5300_set_brightness(h, 100);
    const int64_t t_b100_1 = esp_timer_get_time();
    ESP_LOGI(TAG, "brightness 100%% -> %s in %lld us",
             esp_err_to_name(err), (long long)(t_b100_1 - t_b100_0));

    err = co5300_sleep(h);
    ESP_LOGI(TAG, "sleep  -> %s", esp_err_to_name(err));
    err = co5300_wake(h);
    ESP_LOGI(TAG, "wake   -> %s", esp_err_to_name(err));

    // Tiny synthetic pixel write -- 1 pixel, just to exercise the QIO path.
    const uint8_t one_red_pixel[3] = {0xFF, 0x00, 0x00};
    err = co5300_write_pixels(h, CO5300_PIXEL_ADDR, one_red_pixel, 1);
    ESP_LOGI(TAG, "pixels x1 -> %s", esp_err_to_name(err));

    (void)co5300_deinit(h);

    const size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "heap delta after init+deinit: %d bytes",
             (int)((long)heap_before - (long)heap_after));
    ESP_LOGI(TAG, "All checks completed (driver is a stub).");
}

void app_main(void) {
    // Small delay so the UART log isn't clipped by the boot banner.
    vTaskDelay(pdMS_TO_TICKS(200));
    test_co5300_run();
}
