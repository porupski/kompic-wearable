/**
 * @file test_fusion.c
 * @brief Standalone test harness for the fusion task.
 *
 * Drives the broker with synthetic GPS + ENV data, advances the fusion task
 * for one or two ticks, asserts the arbitration produces the expected
 * altitude_source + altitude_m. No physical hardware required.
 *
 * Master prompt: docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md
 */

#include "fusion.h"
#include "cross_driver.h"
#include "data_broker.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "TEST_FUSION";

#define EXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ESP_LOGE(TAG, "FAIL %s:%d  %s", __func__, __LINE__, (msg));        \
            abort();                                                           \
        } else {                                                               \
            ESP_LOGI(TAG, "PASS %s  %s", __func__, (msg));                     \
        }                                                                      \
    } while (0)

static bool close_enough(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

static void wait_for_tick(void)
{
    vTaskDelay(pdMS_TO_TICKS(1200));  // fusion runs at 1 Hz; 1.2s = guaranteed tick
}

// ───────────────────────────────────────────────────────────────────────────

static void scenario_a_gps_fix(void)
{
    broker_gps_set_hw_status(true);
    broker_gps_set_enabled(true);
    broker_env_set_hw_status(false);   // ENV absent

    broker_gps_data_t g = {0};
    g.fix = GPS_FIX_3D;
    g.position_valid = true;
    g.altitude_m = 712.4f;
    g.enabled = true;
    broker_gps_write(&g);

    wait_for_tick();

    broker_fusion_data_t f = {0};
    broker_fusion_read(&f);
    EXPECT(f.altitude_source == ALTITUDE_SRC_GPS, "GPS source selected");
    EXPECT(close_enough(f.altitude_m, 712.4f, 0.05f), "GPS altitude carried through");
}

static void scenario_b_baro_only(void)
{
    broker_gps_set_enabled(false);
    broker_env_set_hw_status(true);
    broker_env_set_enabled(true);

    broker_env_data_t e = {0};
    e.pressure_hpa = 998.0f;
    e.altitude_m = 125.0f;
    e.enabled = true;
    broker_env_write(&e);

    wait_for_tick();

    broker_fusion_data_t f = {0};
    broker_fusion_read(&f);
    EXPECT(f.altitude_source == ALTITUDE_SRC_BARO, "BARO source selected");
    EXPECT(close_enough(f.altitude_m, 125.0f, 0.05f), "Baro altitude carried through");
}

static void scenario_c_carry_last(void)
{
    // pre-load fusion with a previous GPS altitude
    broker_fusion_data_t prev = {0};
    prev.altitude_gps_m = 712.4f;
    prev.altitude_m = 712.4f;
    prev.altitude_source = ALTITUDE_SRC_GPS;
    prev.enabled = true;
    broker_fusion_write(&prev);

    broker_gps_set_enabled(false);
    broker_env_set_enabled(false);
    broker_env_set_hw_status(false);

    wait_for_tick();

    broker_fusion_data_t f = {0};
    broker_fusion_read(&f);
    EXPECT(f.altitude_source == ALTITUDE_SRC_LAST, "LAST source on outage");
    EXPECT(close_enough(f.altitude_m, 712.4f, 0.05f), "carry-forward altitude");
}

static void scenario_d_none(void)
{
    broker_fusion_data_t prev = {0};
    prev.enabled = true;
    broker_fusion_write(&prev);

    broker_gps_set_enabled(false);
    broker_env_set_enabled(false);
    broker_env_set_hw_status(false);

    wait_for_tick();

    broker_fusion_data_t f = {0};
    broker_fusion_read(&f);
    EXPECT(f.altitude_source == ALTITUDE_SRC_NONE, "NONE source when nothing valid");
}

// ───────────────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "test_fusion starting (iv7.1.f0.0)");

    broker_init();
    cross_driver_init();
    fusion_init();
    broker_fusion_set_hw_status(true);
    broker_fusion_set_enabled(true);

    xTaskCreatePinnedToCore(task_fusion_fn, "fusion", 2048, NULL, 2, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));   // let task start

    scenario_a_gps_fix();
    scenario_b_baro_only();
    scenario_c_carry_last();
    scenario_d_none();

    ESP_LOGI(TAG, "ALL SUBTESTS PASSED");
}
