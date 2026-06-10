/**
 * @file cst9217.c
 * @brief CST9217 capacitive touch controller -- ISR-driven driver.
 *
 * See cst9217.h for the design rules. This file owns:
 *   - INT GPIO config + ISR install
 *   - The touch task (Core 0) that consumes ISR notifications
 *   - The lock-free output queue (g_touch_q, depth 1, xQueueOverwrite semantics)
 *   - The synchronous probe / report-read helpers
 *
 * Core 0 only. No LVGL. No broker calls.
 */

#include "cst9217.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

// boot_hw_init.h declares g_i2c_mutex once it exists in this tree; until then
// drivers either pull it in via that header or extern-declare it. We follow
// the bme280_drv.c / max30102.c pattern (extern declaration) so the component
// compiles standalone for the test harness.
extern SemaphoreHandle_t g_i2c_mutex;

static const char *TAG = "CST9217";

// ---------------------------------------------------------------------------
// Public lock-free output queue (depth 1).
// ---------------------------------------------------------------------------
QueueHandle_t g_touch_q = NULL;

// ---------------------------------------------------------------------------
// Task notify handle: ISR notifies, task waits.
// ---------------------------------------------------------------------------
static TaskHandle_t s_task_hdl = NULL;
static volatile bool s_isr_installed = false;

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
const char *cst9217_get_chip_name(void) { return "CST9217"; }
const char *cst9217_get_chip_desc(void) { return "Capacitive touch controller"; }

// ---------------------------------------------------------------------------
// ISR -- minimal: notify the task and return. NO I2C from ISR context.
// ---------------------------------------------------------------------------
static void IRAM_ATTR cst9217_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    if (s_task_hdl) {
        vTaskNotifyGiveFromISR(s_task_hdl, &hpw);
    }
    if (hpw) portYIELD_FROM_ISR();
}

// ---------------------------------------------------------------------------
// I2C primitives -- 16-bit register address, MSB first.
// Caller holds g_i2c_mutex.
// ---------------------------------------------------------------------------

static esp_err_t i2c_read_16bit_reg(i2c_port_t i2c_num, uint16_t reg,
                                    uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CST9217_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (uint8_t)(reg >> 8), true);   // reg MSB
    i2c_master_write_byte(cmd, (uint8_t)(reg & 0xFF), true); // reg LSB
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CST9217_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t cst9217_probe_ack(i2c_port_t i2c_num, uint8_t *ack_out)
{
    if (!ack_out) return ESP_ERR_INVALID_ARG;
    return i2c_read_16bit_reg(i2c_num, CST9217_REG_BASE, ack_out, 1);
}

esp_err_t cst9217_read_report(i2c_port_t i2c_num,
                               uint8_t buf[CST9217_REPORT_LEN])
{
    return i2c_read_16bit_reg(i2c_num, CST9217_REG_BASE, buf, CST9217_REPORT_LEN);
}

// ---------------------------------------------------------------------------
// Reset pulse -- RST is active-low. Pull LOW for >= 5 ms, then HIGH and wait
// CST9217_RST_HIGH_MS for the chip to settle before the first I2C transaction.
// ---------------------------------------------------------------------------
static esp_err_t cst9217_hw_reset(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CST9217_RST_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) return ret;

    gpio_set_level(CST9217_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(CST9217_RST_LOW_MS));
    gpio_set_level(CST9217_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(CST9217_RST_HIGH_MS));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// INT GPIO config + ISR install (idempotent).
// Falling-edge -- CST9217 INT is active-low pulse per family convention.
// Confirm polarity on first bench bring-up; see [DEFECT-002] in the .md.
// ---------------------------------------------------------------------------
static esp_err_t cst9217_int_install(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CST9217_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    // INT idles HIGH
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) return ret;

    // Install GPIO ISR service if not yet installed by another driver.
    // ESP_ERR_INVALID_STATE means already installed -- benign.
    esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) return isr_ret;

    ret = gpio_isr_handler_add(CST9217_INT_GPIO, cst9217_isr, NULL);
    if (ret != ESP_OK) return ret;

    s_isr_installed = true;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
esp_err_t cst9217_init(i2c_port_t i2c_num)
{
    int64_t t0 = esp_timer_get_time();

    // 1. Output queue
    if (g_touch_q == NULL) {
        g_touch_q = xQueueCreate(1, sizeof(cst9217_point_t));
        if (g_touch_q == NULL) {
            ESP_LOGE(TAG, "queue alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    // 2. Reset the touch IC
    esp_err_t ret = cst9217_hw_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RST GPIO%d setup failed: %s",
                 CST9217_RST_GPIO, esp_err_to_name(ret));
        return ret;
    }

    // 3. I2C probe -- read ACK byte at 0xD000, expect 0xAB
    uint8_t ack = 0;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ret = cst9217_probe_ack(i2c_num, &ack);
        xSemaphoreGive(g_i2c_mutex);
    } else {
        ESP_LOGE(TAG, "I2C mutex timeout during probe");
        return ESP_ERR_TIMEOUT;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C probe failed @ 0x%02X: %s",
                 CST9217_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }
    if (ack != CST9217_ACK_VALUE) {
        ESP_LOGE(TAG, "ACK mismatch: got 0x%02X, expected 0x%02X", ack, CST9217_ACK_VALUE);
        return ESP_ERR_NOT_FOUND;
    }

    // 4. INT GPIO + ISR
    ret = cst9217_int_install();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INT GPIO%d setup failed: %s",
                 CST9217_INT_GPIO, esp_err_to_name(ret));
        return ret;
    }

    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "%s init OK @ 0x%02X (ACK=0x%02X), boot=%lld us",
             cst9217_get_chip_name(), CST9217_I2C_ADDR, ack, (long long)(t1 - t0));
    return ESP_OK;
}

void cst9217_deinit(void)
{
    if (s_isr_installed) {
        gpio_isr_handler_remove(CST9217_INT_GPIO);
        s_isr_installed = false;
    }
    gpio_reset_pin(CST9217_INT_GPIO);
    gpio_reset_pin(CST9217_RST_GPIO);
    if (g_touch_q) {
        vQueueDelete(g_touch_q);
        g_touch_q = NULL;
    }
    s_task_hdl = NULL;
    ESP_LOGI(TAG, "Deinit complete");
}

// ---------------------------------------------------------------------------
// Task -- waits on ISR notify, reads report, posts to queue.
// ---------------------------------------------------------------------------
void task_touch_fn(void *arg)
{
    (void)arg;
    s_task_hdl = xTaskGetCurrentTaskHandle();

    ESP_LOGI(TAG, "Task started on Core %d", xPortGetCoreID());

    uint8_t buf[CST9217_REPORT_LEN];

    for (;;) {
        // Wait for ISR notification. portMAX_DELAY -- no spurious wakeups.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int64_t t_wake = esp_timer_get_time();

        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "I2C mutex timeout in task path -- dropping report");
            continue;
        }
        esp_err_t ret = cst9217_read_report(I2C_NUM_0, buf);
        xSemaphoreGive(g_i2c_mutex);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "report read failed: %s", esp_err_to_name(ret));
            continue;
        }

        cst9217_point_t p = {
            .fingers = buf[1],
            .gesture = buf[2],
            .x       = (uint16_t)(((buf[4] & 0x0F) << 8) | buf[5]),
            .y       = (uint16_t)(((buf[6] & 0x0F) << 8) | buf[7]),
            .t_us    = (uint32_t)(t_wake & 0xFFFFFFFF),
        };

        // Lock-free hand-off. Last-write-wins -- consumer sees the freshest point.
        xQueueOverwrite(g_touch_q, &p);
    }
}
