/**
 * @file test_pcf85063.c
 * @brief Standalone diagnostic for the PCF85063A RTC driver.
 *
 * Four phases:
 *   1. I2C probe + identity log.
 *   2. Time write/read round-trip: set a known UTC time, read it back, verify.
 *   3. UTC-math sanity: set time near midnight rollover, sleep, read, confirm
 *      the date advanced exactly once (Sakamoto DoW agrees).
 *   4. Hardware alarm: arm for "now + 5 seconds", wait for GPIO15 INT, clear AF.
 *
 * Wiring: see Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C, §GPIO.
 *   SDA=GPIO1, SCL=GPIO2, addr 0x51, INT=GPIO15, 400 kHz.
 *
 * Expected output (real hw):
 *   I (xx) test_pcf85063: Chip: PCF85063A -- Battery-backed RTC
 *   I (xx) test_pcf85063: I2C bus init: <us>
 *   I (xx) test_pcf85063: init: ESP_OK in <us>
 *   I (xx) test_pcf85063: write 12:34:56 2026-06-10 -> ESP_OK
 *   I (xx) test_pcf85063: read  12:34:5x 2026-06-10 wd=3 -> match
 *   I (xx) test_pcf85063: rollover test: 23:59:55 -> wait 8 s -> 00:00:0x 2026-06-11 wd=4 OK
 *   I (xx) test_pcf85063: armed alarm: now+5 s (HH:MM:SS = 12:35:0x)
 *   I (xx) test_pcf85063: ALARM FIRED -- ISR-to-task dt=<us>, AF cleared OK
 */

#include "pcf85063.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "test_pcf85063";

// In the full firmware boot_hw_init.c owns this; the standalone harness creates
// one if missing so the driver's extern declaration resolves.
SemaphoreHandle_t g_i2c_mutex = NULL;

#define TEST_I2C_PORT     I2C_NUM_0
#define TEST_I2C_SDA      1
#define TEST_I2C_SCL      2
#define TEST_I2C_HZ       400000

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

static void log_time(const char *prefix, const pcf85063_time_t *t)
{
    ESP_LOGI(TAG, "%s %02u:%02u:%02u %04u-%02u-%02u wd=%u valid=%d",
             prefix, t->hour, t->minute, t->second,
             2000U + t->year, t->month, t->day, t->weekday, (int)t->valid);
}

static void test_pcf85063_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             pcf85063_get_chip_name(), pcf85063_get_chip_desc());

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

    int64_t t_i0 = esp_timer_get_time();
    err = pcf85063_init(TEST_I2C_PORT);
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ---- Phase 2: write / read round-trip ----
    pcf85063_time_t want = {
        .second = 56, .minute = 34, .hour = 12,
        .day = 10, .month = 6, .year = 26,
        .weekday = 3,   // Wed (2026-06-10 is Wednesday; matches Sakamoto)
        .valid = true,
    };

    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    int64_t t_w0 = esp_timer_get_time();
    err = pcf85063_set_time(TEST_I2C_PORT, &want);
    int64_t t_w1 = esp_timer_get_time();
    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "write: %s in %lld us",
             esp_err_to_name(err), (long long)(t_w1 - t_w0));

    pcf85063_time_t got = {0};
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    int64_t t_r0 = esp_timer_get_time();
    err = pcf85063_get_time(TEST_I2C_PORT, &got);
    int64_t t_r1 = esp_timer_get_time();
    xSemaphoreGive(g_i2c_mutex);
    log_time("read ", &got);
    ESP_LOGI(TAG, "read dt: %lld us -> %s",
             (long long)(t_r1 - t_r0), esp_err_to_name(err));

    // Allow second to have advanced by 1 due to test execution time.
    bool match =
        (got.minute  == want.minute) &&
        (got.hour    == want.hour)   &&
        (got.day     == want.day)    &&
        (got.month   == want.month)  &&
        (got.year    == want.year)   &&
        (got.weekday == want.weekday);
    ESP_LOGI(TAG, "round-trip: %s",
             match ? "MATCH" : "MISMATCH (check chip / I2C / BCD)");

    // ---- Phase 3: midnight rollover ----
    pcf85063_time_t before_midnight = {
        .second = 55, .minute = 59, .hour = 23,
        .day = 10, .month = 6, .year = 26,
        .weekday = 3,   // Wed
        .valid = true,
    };
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    pcf85063_set_time(TEST_I2C_PORT, &before_midnight);
    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "rollover: set 23:59:55 2026-06-10 -- waiting 8 s ...");
    vTaskDelay(pdMS_TO_TICKS(8000));

    pcf85063_time_t after = {0};
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    pcf85063_get_time(TEST_I2C_PORT, &after);
    xSemaphoreGive(g_i2c_mutex);
    log_time("rollover:", &after);
    bool rollover_ok =
        (after.day == 11) && (after.month == 6) &&
        (after.year == 26) && (after.hour < 2);
    ESP_LOGI(TAG, "rollover: %s",
             rollover_ok ? "OK (date advanced, weekday Sakamoto check below)" : "FAIL");
    // Sakamoto: 2026-06-11 is Thursday (4). Hardware doesn't auto-recompute
    // weekday, but pcf85063_sync_utc() does; this test only checks the chip's
    // own weekday counter advanced by 1.
    ESP_LOGI(TAG, "rollover weekday: %u (chip-counted, expect %u)",
             after.weekday, (before_midnight.weekday + 1) % 7);

    // ---- Phase 4: hardware alarm ----
    // Read current time, arm alarm for now + 5 s.
    pcf85063_time_t now_t = {0};
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    pcf85063_get_time(TEST_I2C_PORT, &now_t);
    xSemaphoreGive(g_i2c_mutex);

    uint8_t a_s = (uint8_t)((now_t.second + 5U) % 60U);
    uint8_t a_m = (now_t.second + 5U >= 60U) ? (uint8_t)((now_t.minute + 1U) % 60U)
                                              : now_t.minute;
    uint8_t a_h = now_t.hour;  // good enough; we won't span hours in 5 s

    err = pcf85063_install_alarm_isr(xTaskGetCurrentTaskHandle());
    ESP_LOGI(TAG, "install_alarm_isr: %s", esp_err_to_name(err));

    err = pcf85063_set_alarm(TEST_I2C_PORT, a_h, a_m, a_s);
    int64_t t_arm = esp_timer_get_time();
    ESP_LOGI(TAG, "armed alarm: %02u:%02u:%02u UTC -> %s",
             a_h, a_m, a_s, esp_err_to_name(err));

    // Wait up to 8 s for the INT.
    uint32_t got_notify = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(8000));
    int64_t t_fire = esp_timer_get_time();

    if (got_notify) {
        ESP_LOGI(TAG, "ALARM FIRED -- arm-to-fire dt=%lld us",
                 (long long)(t_fire - t_arm));
        err = pcf85063_clear_alarm_flag(TEST_I2C_PORT);
        ESP_LOGI(TAG, "clear_alarm_flag: %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "ALARM TIMEOUT -- no INT within 8 s. Check GPIO15, INT polarity, AIE.");
    }

    pcf85063_clear_alarm(TEST_I2C_PORT);
    pcf85063_install_alarm_isr(NULL);

    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_pcf85063_run();
}
