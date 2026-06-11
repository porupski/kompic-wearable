/**
 * @file max30101.c
 * @brief Maxim MAX30101 HR / SpO2 / PPG driver -- Core 0 only.
 *
 * The MAX30101 is the next gen of the MAX30102 used in v5. They share the
 * I2C register map for FIFO + MODE + SPO2 + LED1/LED2; the MAX30101 adds:
 *   - LED3 (green) at REG 0x0E
 *   - MULTI_LED_CTRL1/2 registers (0x11/0x12) for slot configuration
 *   - PART_ID still 0x15 (same family ID)
 *
 * For Phase 3 we run in SpO2 mode (Red + IR only). MULTI_LED with green is
 * exposed via max30101_setup_multi_led_mode() but not used. The legacy
 * MAX30102 driver's FIFO drain logic + beat detector + sleep/wake state
 * machine all carry forward verbatim -- including the Phase 14/15 fixes
 * documented in the old driver.
 *
 * INT pin (GPIO7) is newly routed in Mk I and gets a falling-edge ISR.
 * Phase 3 keeps the ISR count-only; the wake-on-FIFO path is wired to the
 * HR task via task notification so Phase 2+ work can flip from polled to
 * INT-driven without restructuring.
 *
 * Architecture: Blueprint 5 §3, Blueprint 14b §4-§5
 */

#include "max30101.h"
#include "data_broker.h"
#include "cross_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "MAX30101";

extern SemaphoreHandle_t g_i2c_mutex;

#define HR_TASK_PERIOD_MS           40
#define HR_LED_RED_CURRENT          0x1F
#define HR_LED_IR_CURRENT           0x1F
#define HR_FIFO_SETTLE_MS           100
#define HR_FIRST_SAMPLE_GATE        4
#define HR_EMPTY_FIFO_REINIT_TICKS  50    // 50 x 40 ms = 2 s before re-init
#define HR_IR_LOG_SAMPLES           8

const char *max30101_get_chip_name(void) { return "MAX30101";         }
const char *max30101_get_chip_desc(void) { return "HR / SpO2 / PPG"; }

// ── Internal: INT ISR state ──────────────────────────────────────────────────
static volatile uint32_t s_int_count   = 0;
static TaskHandle_t      s_notify_task = NULL;
static bool              s_int_installed = false;

// ── Register helpers (caller holds g_i2c_mutex) ──────────────────────────────

static esp_err_t write_reg(i2c_port_t port, uint8_t reg, uint8_t value)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MAX30101_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t read_reg(i2c_port_t port, uint8_t reg, uint8_t *out)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MAX30101_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MAX30101_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, out, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

esp_err_t max30101_init(i2c_port_t port)
{
    uint8_t part_id = 0;
    esp_err_t ret = read_reg(port, MAX30101_REG_PART_ID, &part_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Device not found on I2C -- OFFLINE");
        return ret;
    }
    // MAX30101 PART_ID is 0x15 (same as MAX30102 family).
    if (part_id != 0x15) {
        ESP_LOGW(TAG, "Unexpected Part ID: 0x%02X (expected 0x15) [DSV]", part_id);
    }

    // Soft reset (REG_MODE_CONFIG bit 6).
    ret = write_reg(port, MAX30101_REG_MODE_CONFIG, 0x40);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(100));

    ret = max30101_set_shutdown(port, true);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MAX30101 init OK, Part ID=0x%02X, sleeping", part_id);
    return ESP_OK;
}

esp_err_t max30101_set_shutdown(i2c_port_t port, bool shutdown)
{
    uint8_t cfg = 0;
    esp_err_t ret = read_reg(port, MAX30101_REG_MODE_CONFIG, &cfg);
    if (ret != ESP_OK) return ret;
    cfg = shutdown ? (cfg | 0x80u) : (cfg & ~0x80u);
    return write_reg(port, MAX30101_REG_MODE_CONFIG, cfg);
}

esp_err_t max30101_setup_hr_mode(i2c_port_t port,
                                  uint8_t red_led_current,
                                  uint8_t ir_led_current)
{
    esp_err_t ret;
    ret = write_reg(port, MAX30101_REG_MODE_CONFIG, MAX30101_MODE_SPO2);
    if (ret != ESP_OK) return ret;
    // FIFO_CONFIG: SMP_AVE=4, FIFO_ROLLOVER_EN=1, almost-full = 17 samples free.
    ret = write_reg(port, MAX30101_REG_FIFO_CONFIG,
                    (uint8_t)(0x50u | (MAX30101_SMP_AVE_4 << 5)));
    if (ret != ESP_OK) return ret;
    uint8_t spo2_cfg = (uint8_t)((MAX30101_ADC_4096 << 5) |
                                  (MAX30101_SR_100   << 2) |
                                   MAX30101_PW_411);
    ret = write_reg(port, MAX30101_REG_SPO2_CONFIG, spo2_cfg);
    if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_LED1_PA, red_led_current);
    if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_LED2_PA, ir_led_current);
    if (ret != ESP_OK) return ret;
    // LED3 (green) explicitly OFF in SpO2 mode.
    ret = write_reg(port, MAX30101_REG_LED3_PA, 0x00);
    if (ret != ESP_OK) return ret;
    ret = max30101_clear_fifo(port);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "HR(SpO2) mode: Red=0x%02X IR=0x%02X Green=off",
             red_led_current, ir_led_current);
    return ESP_OK;
}

esp_err_t max30101_setup_multi_led_mode(i2c_port_t port,
                                         uint8_t red_pa, uint8_t ir_pa,
                                         uint8_t green_pa)
{
    // Slot assignment: slot1=Red, slot2=IR, slot3=Green, slot4=none.
    // MULTI_LED_CTRL1: bits[6:4]=SLOT2, bits[2:0]=SLOT1.
    // MULTI_LED_CTRL2: bits[6:4]=SLOT4, bits[2:0]=SLOT3.
    // LED# encoding: 1=LED1(Red), 2=LED2(IR), 3=LED3(Green), 0=disabled. [DSV]
    esp_err_t ret;
    ret = write_reg(port, MAX30101_REG_MODE_CONFIG, MAX30101_MODE_MULTI_LED);
    if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_FIFO_CONFIG,
                    (uint8_t)(0x50u | (MAX30101_SMP_AVE_4 << 5)));
    if (ret != ESP_OK) return ret;
    uint8_t spo2_cfg = (uint8_t)((MAX30101_ADC_4096 << 5) |
                                  (MAX30101_SR_100   << 2) |
                                   MAX30101_PW_411);
    ret = write_reg(port, MAX30101_REG_SPO2_CONFIG, spo2_cfg);
    if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_LED1_PA,     red_pa);
    if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_LED2_PA,     ir_pa);
    if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_LED3_PA,     green_pa);
    if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_MULTI_LED_1, (2u << 4) | 1u);
    if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_MULTI_LED_2, (0u << 4) | 3u);
    if (ret != ESP_OK) return ret;
    ret = max30101_clear_fifo(port);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "MULTI_LED mode: Red=0x%02X IR=0x%02X Green=0x%02X",
             red_pa, ir_pa, green_pa);
    return ESP_OK;
}

// ── FIFO read --------------------------------------------------------------------

esp_err_t max30101_read_fifo(i2c_port_t port,
                              max30101_sample_t *sample,
                              bool multi_led)
{
    const size_t nbytes = multi_led ? 9 : 6;
    uint8_t buf[9] = {0};

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MAX30101_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, MAX30101_REG_FIFO_DATA, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MAX30101_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, nbytes, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        sample->red   = (((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2]) & 0x3FFFFu;
        sample->ir    = (((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 8) | buf[5]) & 0x3FFFFu;
        sample->green = multi_led
                      ? ((((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 8) | buf[8]) & 0x3FFFFu)
                      : 0;
        sample->valid = true;
    } else {
        sample->valid = false;
    }
    return ret;
}

esp_err_t max30101_get_fifo_available(i2c_port_t port, uint8_t *count)
{
    uint8_t wr = 0, rd = 0;
    esp_err_t ret;
    ret = read_reg(port, MAX30101_REG_FIFO_WR_PTR, &wr); if (ret != ESP_OK) return ret;
    ret = read_reg(port, MAX30101_REG_FIFO_RD_PTR, &rd); if (ret != ESP_OK) return ret;
    *count = (wr >= rd) ? (wr - rd) : (32u - rd + wr);
    return ESP_OK;
}

esp_err_t max30101_clear_fifo(i2c_port_t port)
{
    esp_err_t ret;
    ret = write_reg(port, MAX30101_REG_FIFO_WR_PTR, 0x00); if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_OVF_COUNTER, 0x00); if (ret != ESP_OK) return ret;
    ret = write_reg(port, MAX30101_REG_FIFO_RD_PTR, 0x00);
    return ret;
}

// ── INT ISR ──────────────────────────────────────────────────────────────────

static void IRAM_ATTR max30101_int_isr(void *arg)
{
    (void)arg;
    s_int_count++;
    if (s_notify_task) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s_notify_task, &hp);
        portYIELD_FROM_ISR(hp);
    }
}

esp_err_t max30101_install_int_isr(TaskHandle_t notify_task)
{
    if (notify_task == NULL) {
        if (s_int_installed) {
            gpio_isr_handler_remove(MAX30101_INT_GPIO);
            s_int_installed = false;
            s_notify_task   = NULL;
        }
        return ESP_OK;
    }

    s_notify_task = notify_task;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << MAX30101_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,        // open-drain idles HIGH
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) return ret;

    esp_err_t svc = gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1);
    if (svc != ESP_OK && svc != ESP_ERR_INVALID_STATE) return svc;

    ret = gpio_isr_handler_add(MAX30101_INT_GPIO, max30101_int_isr, NULL);
    if (ret != ESP_OK) return ret;
    s_int_installed = true;
    ESP_LOGI(TAG, "INT ISR installed on GPIO%d (open-drain, falling edge)",
             MAX30101_INT_GPIO);
    return ESP_OK;
}

uint32_t max30101_get_int_count(void) { return s_int_count; }

// ── Beat detector (verbatim from MAX30102 driver) ─────────────────────────────

void max30101_beat_detector_init(max30101_beat_detector_t *d)
{
    memset(d, 0, sizeof(*d));
}

bool max30101_check_for_beat(max30101_beat_detector_t *d,
                              uint32_t ir_value,
                              uint32_t current_time_ms)
{
    d->finger_detected = (ir_value > MAX30101_IR_FINGER_THRESHOLD);

    if (!d->finger_detected) {
        d->dc_estimate   = 0;
        d->prev_filtered = 0;
        d->rising_edge   = false;
        d->bpm           = 0;
        return false;
    }

    if (d->dc_estimate == 0) {
        d->dc_estimate    = (int32_t)ir_value;
        d->dc_filtered_ir = 0;
        return false;
    }

    d->dc_estimate    = (int32_t)(0.95f * (float)d->dc_estimate + 0.05f * (float)ir_value);
    d->dc_filtered_ir = (int32_t)ir_value - d->dc_estimate;

    bool beat_detected = false;

    if (d->dc_filtered_ir > MAX30101_BEAT_THRESHOLD &&
        d->prev_filtered  <= MAX30101_BEAT_THRESHOLD) {
        d->rising_edge = true;
    }

    if (d->rising_edge && d->dc_filtered_ir < d->prev_filtered) {
        d->rising_edge = false;
        beat_detected  = true;

        if (d->last_beat_time > 0) {
            uint32_t delta = current_time_ms - d->last_beat_time;
            if (delta > 0) {
                float bpm_f = 60000.0f / (float)delta;
                if (bpm_f >= MAX30101_MIN_BPM && bpm_f < MAX30101_MAX_BPM) {
                    d->rates[d->rate_spot++] = (uint8_t)bpm_f;
                    d->rate_spot %= 4;
                    uint16_t sum = 0;
                    for (int i = 0; i < 4; i++) sum += d->rates[i];
                    d->bpm = (uint8_t)(sum / 4);
                }
            }
        }
        d->last_beat_time = current_time_ms;
    }

    d->prev_filtered = d->dc_filtered_ir;
    return beat_detected;
}

// ── Core 0 task (carried forward from MAX30102 driver, all Phase 14/15 fixes
//                preserved -- FIX-1..FIX-9 -- only the chip prefix renamed) ──

void task_hr_fn(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(HR_TASK_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();

    max30101_beat_detector_t detector = {0};
    max30101_beat_detector_init(&detector);

    bool     sensor_awake       = false;
    uint32_t samples_since_wake = 0;
    uint32_t empty_fifo_ticks   = 0;

    bool     last_logged_en    = false;
    bool     last_logged_awake = false;
    uint32_t tick_count        = 0;

    while (1) {
        vTaskDelayUntil(&last, period);

        if (!broker_hr_hw_alive()) continue;

        bool en = broker_hr_get_enabled();
        tick_count++;

        if (en != last_logged_en || sensor_awake != last_logged_awake ||
            (tick_count % 25u) == 0u) {
            last_logged_en    = en;
            last_logged_awake = sensor_awake;
        }

        // Wake transition
        if (en && !sensor_awake) {
            if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                max30101_set_shutdown(MAX30101_I2C_PORT, false);
                uint8_t mode_rb = 0xFF;
                read_reg(MAX30101_I2C_PORT, MAX30101_REG_MODE_CONFIG, &mode_rb);
                xSemaphoreGive(g_i2c_mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                max30101_setup_hr_mode(MAX30101_I2C_PORT,
                                        HR_LED_RED_CURRENT, HR_LED_IR_CURRENT);
                xSemaphoreGive(g_i2c_mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(HR_FIFO_SETTLE_MS));
            max30101_beat_detector_init(&detector);
            samples_since_wake = 0;
            empty_fifo_ticks   = 0;
            sensor_awake       = true;
        }

        // Sleep transition
        if (!en && sensor_awake) {
            if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                max30101_set_shutdown(MAX30101_I2C_PORT, true);
                xSemaphoreGive(g_i2c_mutex);
            }
            sensor_awake       = false;
            samples_since_wake = 0;
            empty_fifo_ticks   = 0;

            broker_hr_data_t bd = {0};
            broker_hr_read(&bd);
            bd.bpm             = 0;
            bd.finger_detected = false;
            bd.signal_quality  = 0;
            bd.spo2_pct        = 0.0f;
            bd.spo2_valid      = false;
            broker_hr_write(&bd);
            continue;
        }

        if (!en) continue;

        // Drain FIFO
        uint8_t avail = 0;
        bool    got_s = false;
        bool    beat  = false;

        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            (void)max30101_get_fifo_available(MAX30101_I2C_PORT, &avail);
            if (avail > 4) avail = 4;

            for (uint8_t i = 0; i < avail; i++) {
                max30101_sample_t s = {0};
                if (max30101_read_fifo(MAX30101_I2C_PORT, &s, false) == ESP_OK && s.valid) {
                    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                    bool b = max30101_check_for_beat(&detector, s.ir, now_ms);
                    beat   = beat || b;
                    got_s  = true;
                    samples_since_wake++;
                    (void)HR_IR_LOG_SAMPLES;   // hook for re-enabling raw-IR log if needed
                }
            }
            xSemaphoreGive(g_i2c_mutex);
        }

        // Empty-FIFO watchdog: sensor silently lost mode config -> re-init.
        if (avail == 0) {
            empty_fifo_ticks++;
            if (empty_fifo_ticks >= HR_EMPTY_FIFO_REINIT_TICKS) {
                ESP_LOGW(TAG, "FIFO empty %lu ticks -- re-running wake sequence",
                         (unsigned long)empty_fifo_ticks);
                if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                    max30101_set_shutdown(MAX30101_I2C_PORT, false);
                    uint8_t mode_rb = 0xFF;
                    read_reg(MAX30101_I2C_PORT, MAX30101_REG_MODE_CONFIG, &mode_rb);
                    xSemaphoreGive(g_i2c_mutex);
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                    max30101_setup_hr_mode(MAX30101_I2C_PORT,
                                            HR_LED_RED_CURRENT, HR_LED_IR_CURRENT);
                    xSemaphoreGive(g_i2c_mutex);
                }
                vTaskDelay(pdMS_TO_TICKS(HR_FIFO_SETTLE_MS));
                max30101_beat_detector_init(&detector);
                samples_since_wake = 0;
                empty_fifo_ticks   = 0;
            }
        } else {
            empty_fifo_ticks = 0;
        }

        if (!got_s) continue;

        if (!detector.finger_detected && samples_since_wake < HR_FIRST_SAMPLE_GATE) {
            continue;
        }

        uint8_t quality = 0;
        if (detector.dc_estimate > 0 && detector.finger_detected) {
            int32_t ac_abs = detector.dc_filtered_ir;
            if (ac_abs < 0) ac_abs = -ac_abs;
            int32_t q = (ac_abs * 100) / detector.dc_estimate;
            quality = (uint8_t)(q > 100 ? 100 : q);
        }

        broker_hr_data_t bd = {0};
        broker_hr_read(&bd);
        uint32_t prev_beat_count = bd.beat_count;
        bd.bpm             = detector.bpm;
        bd.finger_detected = detector.finger_detected;
        bd.signal_quality  = quality;
        bd.spo2_pct        = 0.0f;       // stub
        bd.spo2_valid      = false;      // stub
        bd.beat_count      = prev_beat_count + (beat ? 1u : 0u);
        broker_hr_write(&bd);

        if (beat && detector.finger_detected) {
            cross_driver_fire(XD_EVENT_HR_UPDATED, &bd);
        }
    }
}
