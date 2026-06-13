/**
 * @file test_data_broker.c
 * @brief Standalone test harness for the data_broker.
 *
 * Boots a minimal ESP-IDF app, calls broker_init(), exercises each slot's
 * status state machine (OFFLINE → DISABLED → ONLINE → STALE), then stress-
 * tests mutex contention with a 100 Hz producer + 200 Hz reader pair on
 * broker_imu. Finally fills/drains the ui_event_q to confirm queue depth 8
 * and FIFO ordering.
 *
 * Flash to any ESP32-S3 dev board. Watch serial @ 115200. PASS lines per
 * subtest; FAIL halts.
 *
 * Build: drop into a bare ESP-IDF project with components/data_broker on the
 * EXTRA_COMPONENT_DIRS path. No physical sensors needed.
 *
 * Master prompt: docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md
 */

#include "data_broker.h"
#include "ui_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "TEST_BROKER";

#define EXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ESP_LOGE(TAG, "FAIL %s:%d  %s", __func__, __LINE__, (msg));        \
            vTaskDelay(pdMS_TO_TICKS(100));                                    \
            abort();                                                           \
        } else {                                                               \
            ESP_LOGI(TAG, "PASS %s  %s", __func__, (msg));                     \
        }                                                                      \
    } while (0)

// ───────────────────────────────────────────────────────────────────────────
// Subtest 1: status state machine on each slot
// Walks OFFLINE → DISABLED → ONLINE for the toggleable slots, OFFLINE →
// ONLINE for always-on slots. STALE is exercised in subtest 2.
// ───────────────────────────────────────────────────────────────────────────

static void subtest_status_states(void)
{
    // IMU is toggleable — full cycle.
    broker_imu_set_hw_status(false);
    EXPECT(broker_imu_get_status() == SENSOR_OFFLINE, "imu OFFLINE on !hw");

    broker_imu_set_hw_status(true);
    broker_imu_set_enabled(false);
    EXPECT(broker_imu_get_status() == SENSOR_DISABLED, "imu DISABLED on !enabled");

    broker_imu_set_enabled(true);
    broker_imu_data_t imu = {0};
    imu.enabled = true;
    broker_imu_write(&imu);
    EXPECT(broker_imu_get_status() == SENSOR_ONLINE, "imu ONLINE after fresh write");

    // RTC is always-on — no enable toggle.
    broker_rtc_set_hw_status(true);
    broker_rtc_data_t rtc = {0};
    broker_rtc_write(&rtc);
    EXPECT(broker_rtc_get_status() == SENSOR_ONLINE, "rtc ONLINE after fresh write");

    // Skin is new in Phase 6 — toggleable.
    broker_skin_set_hw_status(true);
    broker_skin_set_enabled(true);
    broker_skin_data_t skin = {0};
    skin.enabled = true;
    broker_skin_write(&skin);
    EXPECT(broker_skin_get_status() == SENSOR_ONLINE, "skin ONLINE after fresh write");
}

// ───────────────────────────────────────────────────────────────────────────
// Subtest 2: STALE transition after timeout
// ───────────────────────────────────────────────────────────────────────────

static void subtest_stale_transition(void)
{
    broker_imu_set_hw_status(true);
    broker_imu_set_enabled(true);
    broker_imu_data_t imu = {0};
    imu.enabled = true;
    broker_imu_write(&imu);

    vTaskDelay(pdMS_TO_TICKS(BROKER_IMU_TIMEOUT_MS + 100));
    EXPECT(broker_imu_get_status() == SENSOR_STALE, "imu STALE after timeout");
}

// ───────────────────────────────────────────────────────────────────────────
// Subtest 3: mutex contention — 100 Hz writer + 200 Hz reader for 10 s
// ───────────────────────────────────────────────────────────────────────────

static volatile uint32_t s_w_count = 0;
static volatile uint32_t s_r_count = 0;
static volatile bool     s_contention_stop = false;

static void writer_task(void *arg)
{
    (void)arg;
    while (!s_contention_stop) {
        broker_imu_data_t imu = {0};
        imu.enabled = true;
        broker_imu_write(&imu);
        s_w_count++;
        vTaskDelay(pdMS_TO_TICKS(10));   // 100 Hz
    }
    vTaskDelete(NULL);
}

static void reader_task(void *arg)
{
    (void)arg;
    while (!s_contention_stop) {
        broker_imu_data_t out = {0};
        broker_imu_read(&out);
        s_r_count++;
        vTaskDelay(pdMS_TO_TICKS(5));    // 200 Hz
    }
    vTaskDelete(NULL);
}

static void subtest_mutex_contention(void)
{
    s_w_count = 0;
    s_r_count = 0;
    s_contention_stop = false;

    broker_imu_set_hw_status(true);
    broker_imu_set_enabled(true);

    xTaskCreate(writer_task, "broker_w", 4096, NULL, 5, NULL);
    xTaskCreate(reader_task, "broker_r", 4096, NULL, 5, NULL);

    vTaskDelay(pdMS_TO_TICKS(10000));
    s_contention_stop = true;
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "contention: %u writes, %u reads over 10 s",
             (unsigned)s_w_count, (unsigned)s_r_count);
    EXPECT(s_w_count > 800,  "writer hit ~1000 over 10 s");
    EXPECT(s_r_count > 1600, "reader hit ~2000 over 10 s");
}

// ───────────────────────────────────────────────────────────────────────────
// Subtest 4: ui_event_q depth + FIFO ordering
// ───────────────────────────────────────────────────────────────────────────

static void subtest_ui_event_queue(void)
{
    EXPECT(g_ui_event_q != NULL, "g_ui_event_q created");

    // Fill the queue (depth 8).
    for (uint8_t i = 0; i < 8; i++) {
        ui_event_t e = { .type = UI_EVENT_ALARM_FIRED };
        e.payload.alarm.alarm_id = i;
        e.payload.alarm.snooze_minutes = 0;
        EXPECT(ui_event_send(&e) == true, "send i succeeded");
    }
    // 9th send must drop (non-blocking).
    {
        ui_event_t e = { .type = UI_EVENT_ALARM_FIRED };
        EXPECT(ui_event_send(&e) == false, "9th send dropped (queue full)");
    }
    // Drain — must come back FIFO.
    for (uint8_t i = 0; i < 8; i++) {
        ui_event_t out = {0};
        EXPECT(xQueueReceive(g_ui_event_q, &out, 0) == pdTRUE, "receive i");
        EXPECT(out.payload.alarm.alarm_id == i, "FIFO order intact");
    }
}

// ───────────────────────────────────────────────────────────────────────────
// app_main
// ───────────────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "test_data_broker starting (iv7.2.f0.0)");
    int64_t t0 = esp_timer_get_time();
    broker_init();
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "broker_init: %lld us", (long long)(t1 - t0));

    subtest_status_states();
    subtest_stale_transition();
    subtest_mutex_contention();
    subtest_ui_event_queue();

    ESP_LOGI(TAG, "ALL SUBTESTS PASSED");
}
