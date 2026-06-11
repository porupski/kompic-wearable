/**
 * @file max30101.h
 * @brief Maxim MAX30101 HR / SpO2 / PPG sensor driver -- Core 0 only.
 *
 * Replaces the MAX30102 driver. The MAX30101 is register-compatible at the
 * I2C level (same handshake, same FIFO layout, same SpO2 mode), so the
 * existing FIFO drain logic + beat detector carry forward verbatim. The
 * delta is a **third LED channel** (green) and an INT pin routed on Mk I
 * to GPIO7 (vs. unrouted on the v5 hardware).
 *
 * The MAX30101 INT pin is open-drain, idles HIGH, and pulls LOW when the
 * FIFO crosses the almost-full threshold (or on data-ready in multi-LED
 * mode). We install a falling-edge GPIO ISR on GPIO7 -- count-only for
 * Phase 3; the wake-on-FIFO path for background HR during sleep is
 * Phase 2+ work.
 *
 * Hardware (v7.2):
 *   Bus       : I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
 *   Address   : 0x57
 *   INT pin   : GPIO7 (RTC-capable, falling-edge ISR)
 *   LEDs      : Red + IR + Green (3 channels vs. MAX30102's 2)
 *   FIFO      : 32 samples (hardware buffer; MAX30102 had the same)
 *
 * The broker_hr_data_t shape carries forward field-for-field so health_tile.c
 * builds with only an include + identity-call rename.
 *
 * Core 0 only. No LVGL includes.
 * Architecture: Blueprint 1 §8, Blueprint 4, Blueprint 5, Blueprint 14b
 */

#ifndef MAX30101_H
#define MAX30101_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ── I2C address + port ────────────────────────────────────────────────────────
#define MAX30101_ADDR               0x57
#define MAX30101_I2C_PORT           I2C_NUM_0
#define MAX30101_INT_GPIO           7

// ── Register addresses ────────────────────────────────────────────────────────
// MAX30101 register map (Maxim 19-7411; 2017 rev) -- shared with MAX30102 for
// the FIFO/MODE/SPO2 group; LED3_PA + the MULTI_LED slot registers are new.
// All tagged [DSV].
#define MAX30101_REG_INTR_STATUS_1  0x00
#define MAX30101_REG_INTR_STATUS_2  0x01
#define MAX30101_REG_INTR_ENABLE_1  0x02
#define MAX30101_REG_INTR_ENABLE_2  0x03
#define MAX30101_REG_FIFO_WR_PTR    0x04
#define MAX30101_REG_OVF_COUNTER    0x05
#define MAX30101_REG_FIFO_RD_PTR    0x06
#define MAX30101_REG_FIFO_DATA      0x07
#define MAX30101_REG_FIFO_CONFIG    0x08
#define MAX30101_REG_MODE_CONFIG    0x09
#define MAX30101_REG_SPO2_CONFIG    0x0A
#define MAX30101_REG_LED1_PA        0x0C   // Red
#define MAX30101_REG_LED2_PA        0x0D   // IR
#define MAX30101_REG_LED3_PA        0x0E   // Green -- new for MAX30101
#define MAX30101_REG_MULTI_LED_1    0x11   // Slot 1+2 control
#define MAX30101_REG_MULTI_LED_2    0x12   // Slot 3+4 control
#define MAX30101_REG_TEMP_INT       0x1F
#define MAX30101_REG_TEMP_FRAC      0x20
#define MAX30101_REG_TEMP_CONFIG    0x21
#define MAX30101_REG_REV_ID         0xFE
#define MAX30101_REG_PART_ID        0xFF   // Expected: 0x15 (same as MAX30102)

// ── Mode configuration ────────────────────────────────────────────────────────
#define MAX30101_MODE_HR_ONLY       0x02   // Red only
#define MAX30101_MODE_SPO2          0x03   // Red + IR
#define MAX30101_MODE_MULTI_LED     0x07   // Up to 4 slots (Red/IR/Green/_)

// ── Sample averaging / rate / pulse-width (same enum as MAX30102) ────────────
#define MAX30101_SMP_AVE_1          0x00
#define MAX30101_SMP_AVE_4          0x02
#define MAX30101_SR_100             0x01
#define MAX30101_PW_411             0x03
#define MAX30101_ADC_4096           0x01

// ── INT_ENABLE_1 bit defines ─────────────────────────────────────────────────
#define MAX30101_INTR_A_FULL        (1 << 7)   // FIFO almost-full
#define MAX30101_INTR_DATA_RDY      (1 << 6)
#define MAX30101_INTR_ALC_OVF       (1 << 5)

// ── Beat detection thresholds (carried forward from MAX30102 driver) ─────────
#define MAX30101_IR_FINGER_THRESHOLD  50000U
#define MAX30101_BEAT_THRESHOLD       1000
#define MAX30101_MIN_BPM              20
#define MAX30101_MAX_BPM              255

// ── Raw FIFO sample ───────────────────────────────────────────────────────────
// In SpO2 mode the FIFO yields 6 bytes per sample (Red + IR, 18 bits each).
// In MULTI_LED mode with Red/IR/Green it yields 9 bytes (3 channels).
// We expose all three channels; `green` is 0 in SpO2 mode.
typedef struct {
    uint32_t red;
    uint32_t ir;
    uint32_t green;
    bool     valid;
} max30101_sample_t;

// ── Beat detector state (Core 0; per-instance) ────────────────────────────────
typedef struct {
    int32_t  dc_estimate;
    int32_t  dc_filtered_ir;
    int32_t  prev_filtered;
    bool     rising_edge;
    bool     finger_detected;
    uint32_t last_beat_time;
    uint8_t  rates[4];
    uint8_t  rate_spot;
    uint8_t  bpm;
} max30101_beat_detector_t;

// ── Broker data struct (driver owns shape, broker stores it) ──────────────────
// Field-for-field preserved from old max30102.h.
typedef struct {
    uint8_t  bpm;
    bool     finger_detected;
    uint8_t  signal_quality;
    float    spo2_pct;
    bool     spo2_valid;

    uint32_t beat_count;
    bool     buzz_beat;

    uint32_t last_update_ms;
    bool     enabled;
} broker_hr_data_t;

#define BROKER_HR_TIMEOUT_MS  5000

// ── Identity ──────────────────────────────────────────────────────────────────
const char *max30101_get_chip_name(void);   // "MAX30101"
const char *max30101_get_chip_desc(void);   // "HR / SpO2 / PPG"

// ── Low-level I2C API (called inside i2c_mutex) ───────────────────────────────
esp_err_t max30101_init(i2c_port_t i2c_num);
esp_err_t max30101_set_shutdown(i2c_port_t i2c_num, bool shutdown);
esp_err_t max30101_setup_hr_mode(i2c_port_t i2c_num,
                                  uint8_t red_led_current,
                                  uint8_t ir_led_current);
/**
 * @brief Configure multi-LED mode using all three channels.
 *        Reads from green LED in addition to red/IR. Not used today
 *        (Phase 3 stays in SpO2 mode) but exposed for Phase 4+ work.
 */
esp_err_t max30101_setup_multi_led_mode(i2c_port_t i2c_num,
                                         uint8_t red_pa, uint8_t ir_pa,
                                         uint8_t green_pa);
esp_err_t max30101_read_fifo(i2c_port_t i2c_num,
                              max30101_sample_t *sample,
                              bool multi_led);
esp_err_t max30101_get_fifo_available(i2c_port_t i2c_num, uint8_t *count);
esp_err_t max30101_clear_fifo(i2c_port_t i2c_num);

// ── INT ISR install (Phase 3: count-only, wake-on-FIFO deferred) ─────────────
/**
 * @brief Install a falling-edge ISR on GPIO7. Pass NULL to uninstall.
 *        When set, the ISR sends a task notification to `notify_task`.
 *        Today the task is the HR task itself; in Phase 2+ this becomes a
 *        wake source for background HR during sleep.
 */
esp_err_t max30101_install_int_isr(TaskHandle_t notify_task);

/** @brief Cumulative INT pin falling-edge count since boot. */
uint32_t max30101_get_int_count(void);

// ── Beat detector API (pure CPU, no I2C, call outside mutex) ──────────────────
void max30101_beat_detector_init(max30101_beat_detector_t *detector);
bool max30101_check_for_beat(max30101_beat_detector_t *detector,
                              uint32_t ir_value,
                              uint32_t current_time_ms);

// ── FreeRTOS task (pinned to Core 0 by boot_tasks.c) ─────────────────────────
void task_hr_fn(void *arg);

#endif // MAX30101_H
