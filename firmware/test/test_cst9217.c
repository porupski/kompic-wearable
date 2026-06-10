/**
 * @file test_cst9217.c
 * @brief Standalone diagnostic for the CST9217 touch driver.
 *
 * Two phases:
 *   1. Synchronous probe: install I2C, ACK byte check (0xAB), report dump.
 *   2. ISR-driven loop: start the touch task, wait on g_touch_q, print each
 *      point + ISR-to-task latency. Runs for ~30 seconds, then bails out.
 *
 * Wiring: see Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C, §GPIO.
 *   SDA=GPIO1, SCL=GPIO2, addr 0x5A, INT=GPIO6, RST=GPIO44, 400 kHz.
 *
 * Build: drop into your test project's main/ and add components/cst9217 to
 *        the EXTRA_COMPONENT_DIRS. Make sure g_i2c_mutex is exposed by the
 *        test's bootstrap (this file creates one if it does not exist).
 *
 * Expected output (real hw):
 *   I (xx) test_cst9217: Chip: CST9217 -- Capacitive touch controller
 *   I (xx) test_cst9217: I2C bus init: <us>
 *   I (xx) test_cst9217: probe ACK: 0xAB OK, dt=<us>
 *   I (xx) test_cst9217: report dump: AB 00 00 00 00 00 00 00 (no touch)
 *   I (xx) test_cst9217: cst9217_init: <us>
 *   I (xx) test_cst9217: waiting for touch events (30 s) ...
 *   I (xx) test_cst9217: pt #1 x=205 y=251 fingers=1 gesture=0x00 isr_dt=<us>
 *   ...
 *   I (xx) test_cst9217: 30 s window closed -- N events received
 */

#include "cst9217.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <inttypes.h>

static const char *TAG = "test_cst9217";

// g_i2c_mutex is owned by boot_hw_init.c in the full firmware. The standalone
// test harness creates one if none exists -- and exports it under the same
// symbol so the driver's extern declaration resolves.
SemaphoreHandle_t g_i2c_mutex = NULL;

#define TEST_I2C_PORT     I2C_NUM_0
#define TEST_I2C_SDA      1
#define TEST_I2C_SCL      2
#define TEST_I2C_HZ       400000
#define TEST_WINDOW_S     30

static esp_err_t test_i2c_bus_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = TEST_I2C_SDA,
        .scl_io_num       = TEST_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TEST_I2C_HZ,
    };
    esp_err_t ret = i2c_param_config(TEST_I2C_PORT, &cfg);
    if (ret != ESP_OK) return ret;
    return i2c_driver_install(TEST_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

static void test_cst9217_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             cst9217_get_chip_name(), cst9217_get_chip_desc());

    if (g_i2c_mutex == NULL) {
        g_i2c_mutex = xSemaphoreCreateMutex();
        configASSERT(g_i2c_mutex != NULL);
    }

    int64_t t_bus0 = esp_timer_get_time();
    esp_err_t err = test_i2c_bus_init();
    int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "I2C bus init: %lld us -> %s",
             (long long)(t_bus1 - t_bus0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    // ---- Phase 1: synchronous probe + raw report dump ----
    uint8_t ack = 0;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    int64_t t_p0 = esp_timer_get_time();
    err = cst9217_probe_ack(TEST_I2C_PORT, &ack);
    int64_t t_p1 = esp_timer_get_time();
    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "probe ACK: 0x%02X  expect 0xAB  dt=%lld us  -> %s",
             ack, (long long)(t_p1 - t_p0), esp_err_to_name(err));
    if (err != ESP_OK || ack != CST9217_ACK_VALUE) {
        ESP_LOGE(TAG, "probe failed -- aborting");
        return;
    }

    uint8_t report[CST9217_REPORT_LEN] = {0};
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    int64_t t_r0 = esp_timer_get_time();
    err = cst9217_read_report(TEST_I2C_PORT, report);
    int64_t t_r1 = esp_timer_get_time();
    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "report dump: %02X %02X %02X %02X %02X %02X %02X %02X  dt=%lld us  -> %s",
             report[0], report[1], report[2], report[3],
             report[4], report[5], report[6], report[7],
             (long long)(t_r1 - t_r0), esp_err_to_name(err));

    // ---- Phase 2: ISR-driven loop ----
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int64_t t_init0 = esp_timer_get_time();
    err = cst9217_init(TEST_I2C_PORT);
    int64_t t_init1 = esp_timer_get_time();
    ESP_LOGI(TAG, "cst9217_init: %lld us -> %s",
             (long long)(t_init1 - t_init0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    BaseType_t ok = xTaskCreatePinnedToCore(task_touch_fn, "task_touch",
                                            3072, NULL, 4, NULL, 0);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "waiting for touch events (%d s) ...", TEST_WINDOW_S);

    int n = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)TEST_WINDOW_S * 1000000;
    cst9217_point_t p;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) break;
        if (xQueueReceive(g_touch_q, &p, pdMS_TO_TICKS(500)) == pdTRUE) {
            // p.t_us was the task wakeup time; "now" is when we read the queue.
            int64_t isr_dt = (int64_t)((uint32_t)(now & 0xFFFFFFFF) - p.t_us);
            n++;
            ESP_LOGI(TAG, "pt #%d  x=%u y=%u fingers=%u gesture=0x%02X  task_to_q_dt=%lld us",
                     n, p.x, p.y, p.fingers, p.gesture, (long long)isr_dt);
        }
    }

    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%d s window closed -- %d events received", TEST_WINDOW_S, n);
    ESP_LOGI(TAG, "heap delta after init+run: %d bytes",
             (int)((long)heap_before - (long)heap_after));
    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_cst9217_run();
}
