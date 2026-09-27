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
#include "boot_hw_init.h"       // g_i2c0_bus_handle (Stage 35.3)
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

extern SemaphoreHandle_t g_i2c_mutex;

static const char *TAG = "CST9217";

// -- I2C device handle (Stage 35.3) -------------------------------------------
static i2c_master_dev_handle_t s_cst_dev = NULL;

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
// Stage 28 §4.4: press-edge-triggered double-tap detector with hold-time
// filter. Rejects swipes (long touches) and controller repeats.
//
// State machine:
//   * s_touch_press_us  -- when the current touch's press-edge landed
//                          (0 = no touch active)
//   * s_prev_valid      -- last report was valid? (edge detection)
//   * s_last_tap_end_us -- when the most recent completed short-tap
//                          released (0 = no eligible first-tap on record)
//
// A "tap" = press-edge + release-edge with hold duration
// <= TAP_MAX_HOLD_US (300 ms). Longer holds are swipes / drags and reset
// the tap chain. Double-tap fires on the SECOND press-edge that lands
// inside [DBL_TAP_MIN_US .. DBL_TAP_MAX_US] after the first tap's release.
// Firing on press-edge (not release-edge) lets us swallow the second tap
// before it reaches LVGL as a widget event.
// ---------------------------------------------------------------------------
static int64_t s_touch_press_us   = 0;
static bool    s_prev_valid       = false;
static int64_t s_last_tap_end_us  = 0;

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
    (void)i2c_num;
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_cst_dev) return ESP_ERR_INVALID_STATE;
    uint8_t reg_be[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_cst_dev, reg_be, 2, buf, len, 20);
}

esp_err_t cst9217_probe_ack(i2c_port_t i2c_num, uint8_t *ack_out)
{
    if (!ack_out) return ESP_ERR_INVALID_ARG;
    return i2c_read_16bit_reg(i2c_num, CST9217_REG_BASE, ack_out, 1);
}

esp_err_t cst9217_ack_probe(i2c_port_t i2c_num)
{
    (void)i2c_num;
    // Zero-byte write via the new API isn't directly supported; use the bus
    // probe helper against our device address. Called only in init.
    return i2c_master_probe(g_i2c0_bus_handle, CST9217_I2C_ADDR, pdMS_TO_TICKS(20));
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
esp_err_t cst9217_reset(void)
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
    (void)i2c_num;
    int64_t t0 = esp_timer_get_time();

    // Stage 35.3: add self to the i2c_master bus.
    if (!g_i2c0_bus_handle) {
        ESP_LOGE(TAG, "init: g_i2c0_bus_handle NULL -- bringup_bus0 must run first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_cst_dev) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = CST9217_I2C_ADDR,
            .scl_speed_hz    = BOOT_I2C_FREQ_HZ,
        };
        esp_err_t add = i2c_master_bus_add_device(g_i2c0_bus_handle, &dev_cfg, &s_cst_dev);
        if (add != ESP_OK) {
            ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(add));
            return add;
        }
    }

    // 1. Output queue
    if (g_touch_q == NULL) {
        g_touch_q = xQueueCreate(1, sizeof(cst9217_point_t));
        if (g_touch_q == NULL) {
            ESP_LOGE(TAG, "queue alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    // 2. Reset the touch IC
    esp_err_t ret = cst9217_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RST GPIO%d setup failed: %s",
                 CST9217_RST_GPIO, esp_err_to_name(ret));
        return ret;
    }

    // 3. I2C ACK probe. The old code read 1 byte at 0xD000 and demanded 0xAB,
    //    but on CST9217 silicon that byte is a status nibble; the 0xAB
    //    validity marker lives at byte 6 of the touch report, not at the
    //    probe offset. Bare I2C ACK matches sketch 16's behavior.
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ret = cst9217_ack_probe(i2c_num);
        xSemaphoreGive(g_i2c_mutex);
    } else {
        ESP_LOGE(TAG, "I2C mutex timeout during probe");
        return ESP_ERR_TIMEOUT;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C ACK probe failed @ 0x%02X: %s",
                 CST9217_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

    // 4. INT GPIO + ISR
    ret = cst9217_int_install();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INT GPIO%d setup failed: %s",
                 CST9217_INT_GPIO, esp_err_to_name(ret));
        return ret;
    }

    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "%s init OK @ 0x%02X, boot=%lld us",
             cst9217_get_chip_name(), CST9217_I2C_ADDR, (long long)(t1 - t0));
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
// Post-report ACK write-back. Sketch 16 writes 0xAB to register 0xD000 after
// every report read; the chip uses this as a handshake to clear the pending
// touch state and re-arm the INT line. Caller holds g_i2c_mutex.
// ---------------------------------------------------------------------------
static esp_err_t cst9217_touch_ack(i2c_port_t i2c_num)
{
    (void)i2c_num;
    if (!s_cst_dev) return ESP_ERR_INVALID_STATE;
    static const uint8_t payload[3] = { 0xD0, 0x00, 0xAB };
    return i2c_master_transmit(s_cst_dev, payload, sizeof(payload), 20);
}

// ---------------------------------------------------------------------------
// Task -- waits on ISR notify, reads report, posts to queue.
// Report layout per sketch 16 (Mk1b-verified):
//   buf[0] & 0x0F == 0x06  -> touch present
//   buf[1]                 -> x[11:4]
//   buf[2]                 -> y[11:4]
//   buf[3] high nibble     -> x[3:0]     buf[3] low nibble -> y[3:0]
//   buf[5] & 0x7F          -> finger count (1 or 2 accepted)
//   buf[6] == 0xAB         -> validity marker (drop the report otherwise)
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
        if (ret == ESP_OK) {
            (void)cst9217_touch_ack(I2C_NUM_0);
        }
        xSemaphoreGive(g_i2c_mutex);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "report read failed: %s", esp_err_to_name(ret));
            continue;
        }

        // Validity + touch-present filter matches sketch 16.
        const uint8_t fingers = buf[5] & 0x7F;
        const bool    valid   = (buf[6] == 0xAB)
                             && ((buf[0] & 0x0F) == 0x06)
                             && (fingers >= 1 && fingers <= 2);

        // Press-edge-triggered double-tap detector. See file-scope comment
        // block above. Detected pair calls field_capture_back_gesture()
        // which unifies asleep-wake / submenu-exit / top-level-lock in one
        // pipeline. On fire, the second tap is swallowed so LVGL doesn't
        // also dispatch a widget event.
        extern void field_capture_back_gesture(const char *reason);
        extern void field_capture_kick_activity(void);
        const int64_t TAP_MAX_HOLD_US = 300 * 1000;   // reject swipes
        const int64_t DBL_TAP_MAX_US  = 500 * 1000;
        const int64_t DBL_TAP_MIN_US  = 150 * 1000;

        bool fire_double_tap = false;
        bool swallow_this_report = false;

        if (valid && !s_prev_valid) {
            // ── PRESS EDGE ──────────────────────────────────────────────
            s_touch_press_us = t_wake;
            if (s_last_tap_end_us != 0) {
                int64_t gap = t_wake - s_last_tap_end_us;
                if (gap >= DBL_TAP_MIN_US && gap <= DBL_TAP_MAX_US) {
                    ESP_LOGI(TAG,
                             "[TOUCH] double-tap detected (gap=%lld ms)",
                             (long long)(gap / 1000));
                    fire_double_tap = true;
                    swallow_this_report = true;
                    s_last_tap_end_us = 0;
                    s_touch_press_us  = 0;
                }
            }
            field_capture_kick_activity();
        } else if (!valid && s_prev_valid) {
            // ── RELEASE EDGE ────────────────────────────────────────────
            int64_t hold = (s_touch_press_us != 0)
                           ? (t_wake - s_touch_press_us) : 0;
            if (s_touch_press_us != 0 && hold <= TAP_MAX_HOLD_US) {
                s_last_tap_end_us = t_wake;   // eligible first-tap on record
            } else {
                s_last_tap_end_us = 0;        // swipe / hold -- reset chain
            }
            s_touch_press_us = 0;
        }
        s_prev_valid = valid;

        if (fire_double_tap) {
            field_capture_back_gesture("touch-double");
            continue;   // swallow the second tap so LVGL doesn't see it
        }
        if (swallow_this_report) continue;

        cst9217_point_t p = {
            .fingers = valid ? fingers : 0,
            .gesture = 0,   // gesture bytes not confirmed for CST9217; leave 0
            .x       = (uint16_t)(((uint16_t)buf[1] << 4) | (buf[3] >> 4)),
            .y       = (uint16_t)(((uint16_t)buf[2] << 4) | (buf[3] & 0x0F)),
            .t_us    = (uint32_t)(t_wake & 0xFFFFFFFF),
        };

        // Lock-free hand-off. Last-write-wins -- consumer sees the freshest point.
        xQueueOverwrite(g_touch_q, &p);
    }
}
