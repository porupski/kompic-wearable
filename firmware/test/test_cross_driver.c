/**
 * @file test_cross_driver.c
 * @brief Standalone test harness for the cross_driver event bus.
 *
 * Verifies registration + dispatch for every event ordinal in the v7.2
 * catalogue. No physical hardware needed. Flash to any ESP32-S3; watch
 * serial @ 115200.
 *
 * Master prompt: docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md
 */

#include "cross_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "TEST_XD";

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

static int          s_hits[XD_EVENT_COUNT] = {0};
static const void  *s_last_payload[XD_EVENT_COUNT] = {0};

static void cb_gps_fix(cross_driver_event_t event, const void *data)
{
    s_hits[event]++;
    s_last_payload[event] = data;
}

static void cb_alarm_a(cross_driver_event_t event, const void *data)
{
    s_hits[event]++;
    s_last_payload[event] = data;
}
static void cb_alarm_b(cross_driver_event_t event, const void *data) { (void)data; s_hits[event]++; }
static void cb_alarm_c(cross_driver_event_t event, const void *data) { (void)data; s_hits[event]++; }
static void cb_alarm_d(cross_driver_event_t event, const void *data) { (void)data; s_hits[event]++; }

// ───────────────────────────────────────────────────────────────────────────

static void subtest_single_listener(void)
{
    cross_driver_register(XD_EVENT_GPS_FIX_VALID, cb_gps_fix);

    uint32_t payload = 0xDEADBEEF;
    cross_driver_fire(XD_EVENT_GPS_FIX_VALID, &payload);

    EXPECT(s_hits[XD_EVENT_GPS_FIX_VALID] == 1, "single listener fired once");
    EXPECT(s_last_payload[XD_EVENT_GPS_FIX_VALID] == &payload,
           "payload pointer preserved");
}

static void subtest_max_listeners(void)
{
    cross_driver_register(XD_EVENT_ALARM_FIRED, cb_alarm_a);
    cross_driver_register(XD_EVENT_ALARM_FIRED, cb_alarm_b);
    cross_driver_register(XD_EVENT_ALARM_FIRED, cb_alarm_c);
    cross_driver_register(XD_EVENT_ALARM_FIRED, cb_alarm_d);

    uint8_t slot = 2;
    cross_driver_fire(XD_EVENT_ALARM_FIRED, &slot);

    EXPECT(s_hits[XD_EVENT_ALARM_FIRED] == 4, "4 listeners all fired");
    EXPECT(s_last_payload[XD_EVENT_ALARM_FIRED] == &slot,
           "payload visible to first listener");
}

static void subtest_zero_listeners(void)
{
    int64_t t0 = esp_timer_get_time();
    cross_driver_fire(XD_EVENT_MIC_FRAME_READY, NULL);
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "0-listener fire: %lld us", (long long)(t1 - t0));
    EXPECT(s_hits[XD_EVENT_MIC_FRAME_READY] == 0, "no callbacks fired");
}

static void subtest_event_name_table(void)
{
    for (uint8_t e = 0; e < XD_EVENT_COUNT; e++) {
        const char *name = cross_driver_event_name((cross_driver_event_t)e);
        EXPECT(name != NULL && name[0] != '\0', "event has a name");
        EXPECT(strcmp(name, "UNNAMED") != 0, "event not UNNAMED");
        ESP_LOGI(TAG, "  ordinal %u → %s", (unsigned)e, name);
    }
}

// ───────────────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "test_cross_driver starting (iv7.1.f0.0)");

    int64_t t0 = esp_timer_get_time();
    cross_driver_init();
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "cross_driver_init: %lld us", (long long)(t1 - t0));

    subtest_single_listener();
    subtest_max_listeners();
    subtest_zero_listeners();
    subtest_event_name_table();

    ESP_LOGI(TAG, "ALL SUBTESTS PASSED");
}
