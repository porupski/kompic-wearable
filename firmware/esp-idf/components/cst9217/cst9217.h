/**
 * @file cst9217.h
 * @brief CST9217 Capacitive Touch Controller -- I2C, 16-bit register, ISR-driven.
 *
 * Hardware:
 *   Chip    : Hynitron CST9217
 *   I2C bus : 1  (GPIO1 SDA, GPIO2 SCL, 400 kHz)
 *   I2C addr: 0x5A (fixed)
 *   INT pin : GPIO6   (active-low pulse on touch event)
 *   RST pin : GPIO44  (active-low; driven HIGH after >= 5 ms low pulse)
 *   Register base: 0xD000  (16-bit register address, MSB first)
 *   ACK probe    : reading 1 byte at 0xD000 returns 0xAB on a live chip
 *
 * Design (per Phase-0.2 brief + Blueprint patterns):
 *   - INT pin is a falling-edge GPIO ISR; the ISR is the ONLY hot path.
 *   - ISR -> xTaskNotifyFromISR() -> task wakes, takes g_i2c_mutex (50 ms ceiling),
 *     burst-reads the touch report (status + 1st point), gives mutex.
 *   - Output: xQueueOverwrite(g_touch_q, &point). Depth-1, last-write-wins,
 *     lock-free from the consumer's side (LVGL indev callback in lvgl_ui).
 *   - This driver does NOT publish to the data_broker. Touch latency budget
 *     is too tight to round-trip the broker mutex; it stays on its own queue.
 *   - No LVGL tile (touch is consumed via lvgl indev callback elsewhere).
 *
 * Core 0 only. No LVGL includes here.
 * Architecture: Blueprint 1 §1 (Core 0 owns I2C), Blueprint 4 §6 (lock-free fast path).
 */

#ifndef CST9217_H
#define CST9217_H

#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stdbool.h>

// -- Identity -------------------------------------------------------------------
const char *cst9217_get_chip_name(void);  // returns "CST9217"
const char *cst9217_get_chip_desc(void);  // returns "Capacitive touch controller"

// -- Wire-level constants -------------------------------------------------------
#define CST9217_I2C_ADDR     0x5A
#define CST9217_REG_BASE     0xD000   // 16-bit register, MSB first on the wire
#define CST9217_ACK_VALUE    0xAB     // first byte returned at 0xD000 on a live chip

#define CST9217_INT_GPIO     GPIO_NUM_6
#define CST9217_RST_GPIO     GPIO_NUM_44

// Reset pulse widths (from v7.2 + datasheet generic OLED-class touch IC practice).
// Confirm against CST9217 datasheet on first bench bring-up.
#define CST9217_RST_LOW_MS   5
#define CST9217_RST_HIGH_MS  50

// Touch report layout at 0xD000 (subject to datasheet confirmation -- the brief
// names 0xD000 as the report base; the byte layout below mirrors the CST816S
// family and is the assumption documented in the porting .md).
//
//   [0]  ACK byte (0xAB on a live chip)
//   [1]  finger count (0 or 1 for single-touch path)
//   [2]  gesture flags  (NONE / SWIPE_UP / DOWN / LEFT / RIGHT / SINGLE_TAP /
//                        DOUBLE_TAP / LONG_PRESS -- gesture set is family-typical;
//                        do not act on it until verified against CST9217 datasheet)
//   [3]  reserved
//   [4]  x_high (4 bits)
//   [5]  x_low  (8 bits)
//   [6]  y_high (4 bits)
//   [7]  y_low  (8 bits)
//
// Total = 8 bytes per report (one burst read).
#define CST9217_REPORT_LEN   8

// -- Public data types ----------------------------------------------------------

typedef enum {
    CST9217_GESTURE_NONE        = 0x00,
    CST9217_GESTURE_SWIPE_UP    = 0x01,
    CST9217_GESTURE_SWIPE_DOWN  = 0x02,
    CST9217_GESTURE_SWIPE_LEFT  = 0x03,
    CST9217_GESTURE_SWIPE_RIGHT = 0x04,
    CST9217_GESTURE_SINGLE_TAP  = 0x05,
    CST9217_GESTURE_DOUBLE_TAP  = 0x0B,
    CST9217_GESTURE_LONG_PRESS  = 0x0C,
} cst9217_gesture_t;

typedef struct {
    uint16_t x;                // 0 .. CO5300_PANEL_WIDTH-1  (410)
    uint16_t y;                // 0 .. CO5300_PANEL_HEIGHT-1 (502)
    uint8_t  fingers;          // 0 = release, 1 = touch
    uint8_t  gesture;          // cst9217_gesture_t
    uint32_t t_us;             // esp_timer_get_time() at task wakeup
} cst9217_point_t;

// -- Lock-free touch queue (depth-1, xQueueOverwrite semantics) -----------------
// Defined in cst9217.c. Consumer = LVGL indev_read callback.
// Producer = task_touch_fn (Core 0). Use xQueuePeek (non-blocking) or
// xQueueReceive(timeout = 0) from the consumer side.
extern QueueHandle_t g_touch_q;

// -- Lifecycle ------------------------------------------------------------------

/**
 * @brief Initialise CST9217 -- reset pulse, I2C probe (read 0xD000, expect 0xAB),
 *        INT GPIO configured as falling-edge interrupt, ISR installed.
 *        Creates the g_touch_q queue (depth 1) if it does not yet exist.
 *
 *        Caller MUST already have g_i2c_mutex created and the I2C bus driver
 *        installed on i2c_num. Caller must NOT hold g_i2c_mutex.
 *
 *        Called from boot_hw_init.c after the I2C scan confirms ACK at 0x5A.
 *
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND if probe byte != 0xAB;
 *         ESP_FAIL on GPIO/ISR install failure.
 */
esp_err_t cst9217_init(i2c_port_t i2c_num);

/**
 * @brief Uninstall ISR, disable INT GPIO, delete queue. Used by tests only.
 */
void cst9217_deinit(void);

/**
 * @brief FreeRTOS task function. Pinned to Core 0 via boot_tasks.c.
 *        Blocks on ulTaskNotifyTake (ISR -> task notify), then reads the
 *        8-byte touch report under g_i2c_mutex and posts to g_touch_q.
 *        Stack: 3072 bytes (no float math, no large locals).
 */
void task_touch_fn(void *arg);

// -- Synchronous I2C helpers (caller holds g_i2c_mutex) -------------------------
// Used by the task path and by test_cst9217.c. Not for normal application use.

/**
 * @brief Burst-read CST9217_REPORT_LEN bytes starting at 0xD000.
 *        Caller MUST hold g_i2c_mutex.
 */
esp_err_t cst9217_read_report(i2c_port_t i2c_num, uint8_t buf[CST9217_REPORT_LEN]);

/**
 * @brief Read just the ACK byte at 0xD000. Used by the probe path.
 *        Caller MUST hold g_i2c_mutex.
 */
esp_err_t cst9217_probe_ack(i2c_port_t i2c_num, uint8_t *ack_out);

#endif // CST9217_H
