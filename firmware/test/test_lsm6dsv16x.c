/**
 * @file test_lsm6dsv16x.c
 * @brief Standalone diagnostic for the LSM6DSV16X IMU driver.
 *
 * Wiring: see Kompic_Mk1_System_Instructions_v7.2.md  -- §I2C bus 1, §GPIO.
 *   SDA=GPIO1, SCL=GPIO2, addr 0x6B, INT1=GPIO8, 400 kHz.
 *
 * Phases:
 *   1. I2C bus 1 init + WHO_AM_I read (expect 0x70).
 *   2. Soft-reset + configure (120 Hz accel ±4g, 240 Hz gyro ±2000 dps).
 *   3. 10 successive reads at 50 ms, logging accel/gyro/temp + per-read dt.
 *   4. INT1 ISR install on GPIO8, 15 s "tilt-to-wake" window. Each event
 *      logged with ISR-to-task latency.
 *
 * Note: tilt-detection embedded function is NOT configured here -- the v1
 * driver just routes DRDY_XL by default. Wake-up function config requires
 * MD1_CFG + WAKE_UP_THS + WAKE_UP_DUR + TAP_CFG[012] writes, all [DSV].
 */

#include "lsm6dsv16x.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "test_lsm6dsv16x";

SemaphoreHandle_t g_i2c_mutex = NULL;

#define TEST_I2C_PORT  I2C_NUM_0
#define TEST_I2C_SDA   1
#define TEST_I2C_SCL   2
#define TEST_I2C_HZ    400000

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

static void test_lsm6dsv16x_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             lsm6dsv16x_get_chip_name(), lsm6dsv16x_get_chip_desc());

    if (g_i2c_mutex == NULL) g_i2c_mutex = xSemaphoreCreateMutex();

    int64_t t_bus0 = esp_timer_get_time();
    esp_err_t err = test_i2c_bus_init();
    int64_t t_bus1 = esp_timer_get_time();
    ESP_LOGI(TAG, "I2C bus 1 init: %lld us -> %s",
             (long long)(t_bus1 - t_bus0), esp_err_to_name(err));
    if (err != ESP_OK) return;

    int64_t t_i0 = esp_timer_get_time();
    err = lsm6dsv16x_init(TEST_I2C_PORT);
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ---- Phase 3: 10 successive reads ----
    broker_imu_data_t bd = {0};
    for (int i = 0; i < 10; i++) {
        xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
        int64_t t_r0 = esp_timer_get_time();
        err = lsm6dsv16x_read(TEST_I2C_PORT, &bd);
        int64_t t_r1 = esp_timer_get_time();
        xSemaphoreGive(g_i2c_mutex);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "#%2d  A=(%+6.2f,%+6.2f,%+6.2f) m/s2  "
                     "G=(%+7.1f,%+7.1f,%+7.1f) dps  T=%5.1fC  dt=%lld us",
                     i,
                     (double)bd.accel_x, (double)bd.accel_y, (double)bd.accel_z,
                     (double)bd.gyro_x,  (double)bd.gyro_y,  (double)bd.gyro_z,
                     (double)bd.temperature,
                     (long long)(t_r1 - t_r0));
        } else {
            ESP_LOGE(TAG, "#%2d read failed: %s", i, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // ---- Phase 4: INT1 ISR ----
    ESP_LOGI(TAG, "Installing INT1 ISR on GPIO%d -- 15 s observation window",
             LSM6DSV16X_INT1_GPIO);
    err = lsm6dsv16x_install_int1_isr(xTaskGetCurrentTaskHandle());
    ESP_LOGI(TAG, "install_int1_isr: %s", esp_err_to_name(err));

    int events = 0;
    int64_t deadline = esp_timer_get_time() + 15LL * 1000000;
    while (esp_timer_get_time() < deadline) {
        uint32_t got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        if (got) {
            int64_t t_now = esp_timer_get_time();
            events++;
            ESP_LOGI(TAG, "INT1 event #%d at t=%lld us", events, (long long)t_now);
        }
    }
    ESP_LOGI(TAG, "window closed -- %d INT1 events", events);

    lsm6dsv16x_install_int1_isr(NULL);

    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_lsm6dsv16x_run();
}
