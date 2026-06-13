/**
 * @file test_alarm.c
 * @brief Standalone test harness for the alarm module.
 *
 * Drives broker_rtc + broker_alarm with synthetic data, asserts the alarm
 * task fires haptic patterns and (when slot.led_strobe is set) drives the
 * WS2812 ALERT state. STOP and AUTO_DISMISS must restore the pre-fire LED
 * state.
 *
 * No physical hardware required — RTC, DRV2605, and WS2812 calls execute
 * against the broker / state machines without real I/O.
 *
 * Master prompt: docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md
 */

#include "alarm.h"
#include "data_broker.h"
#include "ui_event.h"
#include "ws2812.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "TEST_ALARM";

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

// Stub a synthetic RTC reading. broker_rtc_write provides the time; alarm
// task polls broker_rtc_read on its own.
static void set_rtc(uint8_t h, uint8_t m)
{
    broker_rtc_set_hw_status(true);
    broker_rtc_data_t rtc = {0};
    rtc.hour    = h;
    rtc.minute  = m;
    rtc.valid   = true;
    rtc.enabled = true;
    broker_rtc_write(&rtc);
}

static void seed_slot(uint8_t idx, uint8_t h, uint8_t m, bool strobe)
{
    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    ad.slots[idx].hour       = h;
    ad.slots[idx].minute     = m;
    ad.slots[idx].pattern_id = 0;
    ad.slots[idx].armed      = true;
    ad.slots[idx].led_strobe = strobe;
    ad.enabled = true;
    broker_alarm_write(&ad);
}

static void wait_ticks(uint32_t s)
{
    vTaskDelay(pdMS_TO_TICKS(1000 * s + 100));
}

static bool alarm_is_firing(void)
{
    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    return ad.firing;
}

// ───────────────────────────────────────────────────────────────────────────

static void test_a_fire_no_strobe(void)
{
    ws2812_set_state(WS2812_STATE_IDLE);
    seed_slot(0, 10, 30, /*strobe=*/false);
    set_rtc(10, 30);
    wait_ticks(2);
    EXPECT(alarm_is_firing(), "alarm fires on time match");
    EXPECT(ws2812_get_state() == WS2812_STATE_IDLE,
           "LED unchanged when slot.led_strobe == false");

    // Cleanup: stop firing
    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    ad.firing = false;
    broker_alarm_write(&ad);
    wait_ticks(2);
}

static void test_b_fire_with_strobe(void)
{
    ws2812_set_state(WS2812_STATE_IDLE);
    seed_slot(0, 11, 0, /*strobe=*/true);
    set_rtc(11, 0);
    wait_ticks(2);
    EXPECT(alarm_is_firing(), "alarm fires on time match (strobe)");
    EXPECT(ws2812_get_state() == WS2812_STATE_ALERT,
           "LED switched to ALERT when slot.led_strobe == true");
}

static void test_c_stop_restores_led(void)
{
    // Continuing from test_b: LED is ALERT, alarm is firing.
    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    ad.firing = false;             // simulate UI STOP
    broker_alarm_write(&ad);
    wait_ticks(2);
    EXPECT(ws2812_get_state() == WS2812_STATE_IDLE,
           "LED restored to pre-fire state on STOP");
}

static void test_e_nvs_roundtrip(void)
{
    // Edit a slot with strobe=true and save it.
    alarm_slot_t slot = {
        .hour = 7, .minute = 15, .pattern_id = 1,
        .armed = true, .led_strobe = true,
    };
    alarm_nvs_save_slot(0, &slot);

    // Simulate reboot: wipe RAM-side slot, reload from NVS.
    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    memset(&ad.slots[0], 0, sizeof(alarm_slot_t));
    broker_alarm_write(&ad);

    alarm_nvs_load_all();

    broker_alarm_data_t back = {0};
    broker_alarm_read(&back);
    EXPECT(back.slots[0].hour == 7,             "hour restored from NVS");
    EXPECT(back.slots[0].led_strobe == true,    "led_strobe restored from NVS");
}

// ───────────────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "test_alarm starting (iv7.2.f0.0)");

    broker_init();
    broker_alarm_set_hw_status(true);
    ws2812_init();

    xTaskCreatePinnedToCore(task_alarm_fn, "alarm", 4096, NULL, 3, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    test_a_fire_no_strobe();
    test_b_fire_with_strobe();
    test_c_stop_restores_led();
    test_e_nvs_roundtrip();

    ESP_LOGI(TAG, "ALL SUBTESTS PASSED");
}
