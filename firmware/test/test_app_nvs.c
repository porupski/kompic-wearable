/**
 * @file test_app_nvs.c
 * @brief Standalone test harness for app_nvs (mag-cal + GPS-date + UI settings).
 *
 * Exercises NVS round-trips. Always starts by erasing the NVS partition so
 * the test is repeatable. Flash to any ESP32-S3, watch serial @ 115200.
 *
 * Master prompt: docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md
 */

#include "app_nvs.h"
#include "ui_broker.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "TEST_NVS";

#define EXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ESP_LOGE(TAG, "FAIL %s:%d  %s", __func__, __LINE__, (msg));        \
            abort();                                                           \
        } else {                                                               \
            ESP_LOGI(TAG, "PASS %s  %s", __func__, (msg));                     \
        }                                                                      \
    } while (0)

static bool float_close(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

// ───────────────────────────────────────────────────────────────────────────

static void subtest_first_boot_defaults(void)
{
    nvs_flash_erase();   // simulate first boot
    app_calibration_t cal = {0};
    ESP_ERROR_CHECK(app_nvs_init(&cal));
    EXPECT(cal.mag_calibrated == false, "uncalibrated by default");
    EXPECT(cal.height_zeroed == false,  "no height ref by default");
}

static void subtest_mag_cal_roundtrip(void)
{
    const float ox = 1.234f, oy = -2.345f;
    const float sx = 0.987f, sy = 1.023f, savg = 1.005f;

    ESP_ERROR_CHECK(app_nvs_save_mag_calibration(ox, oy, sx, sy, savg));

    float lox=0, loy=0, lsx=0, lsy=0, lsavg=0;
    bool  loaded = false;
    ESP_ERROR_CHECK(app_nvs_load_mag_calibration(&lox, &loy, &lsx, &lsy, &lsavg, &loaded));
    EXPECT(loaded == true, "calibrated flag asserted");
    EXPECT(float_close(lox, ox, 0.002f), "offset_x round-trip");
    EXPECT(float_close(loy, oy, 0.002f), "offset_y round-trip");
    EXPECT(float_close(lsx, sx, 0.002f), "scale_x round-trip");
    EXPECT(float_close(lsy, sy, 0.002f), "scale_y round-trip");
    EXPECT(float_close(lsavg, savg, 0.002f), "scale_avg round-trip");

    ESP_ERROR_CHECK(app_nvs_clear_mag_calibration());
    bool calibrated_after_clear = true;
    app_nvs_load_mag_calibration(&lox, &loy, &lsx, &lsy, &lsavg, &calibrated_after_clear);
    EXPECT(calibrated_after_clear == false, "clear resets the calibrated flag");
}

static void subtest_gps_date_guard(void)
{
    ESP_ERROR_CHECK(app_nvs_save_gps_date(2026, 6, 13));

    // older-than-stored must fail
    esp_err_t err = app_nvs_save_gps_date(2026, 6, 12);
    EXPECT(err == ESP_ERR_INVALID_STATE, "older date rejected");

    // newer must succeed
    ESP_ERROR_CHECK(app_nvs_save_gps_date(2026, 6, 14));

    uint16_t y; uint8_t m, d;
    ESP_ERROR_CHECK(app_nvs_load_gps_date(&y, &m, &d));
    EXPECT(y == 2026 && m == 6 && d == 14, "newest date came back");
}

static void subtest_ui_settings_roundtrip(void)
{
    ui_settings_t s = {
        .theme = 1,
        .brightness = 42,
        .blue_light_on = true,
        .auto_brightness = false,
    };
    ESP_ERROR_CHECK(app_nvs_save_ui_settings(&s));

    ui_settings_t out = {0};
    ESP_ERROR_CHECK(app_nvs_load_ui_settings(&out));
    EXPECT(out.theme == 1, "theme round-trip");
    EXPECT(out.brightness == 42, "brightness round-trip");
    EXPECT(out.blue_light_on == true, "blue_light round-trip");
    EXPECT(out.auto_brightness == false, "auto_brightness round-trip");
}

static void subtest_async_queue_overwrite(void)
{
    QueueHandle_t q = app_nvs_settings_queue_create();
    EXPECT(q != NULL, "queue created");

    ui_settings_t a = { .brightness = 11 };
    ui_settings_t b = { .brightness = 22 };
    xQueueOverwrite(q, &a);
    xQueueOverwrite(q, &b);

    ui_settings_t out = {0};
    EXPECT(xQueueReceive(q, &out, 0) == pdTRUE, "drain once succeeds");
    EXPECT(out.brightness == 22, "last-write-wins overwrite confirmed");
    EXPECT(xQueueReceive(q, &out, 0) == pdFALSE, "queue empty after one drain");
}

// ───────────────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "test_app_nvs starting (iv7.1.f0.0)");
    subtest_first_boot_defaults();
    subtest_mag_cal_roundtrip();
    subtest_gps_date_guard();
    subtest_ui_settings_roundtrip();
    subtest_async_queue_overwrite();
    ESP_LOGI(TAG, "ALL SUBTESTS PASSED");
}
