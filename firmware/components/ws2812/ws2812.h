/**
 * @file ws2812.h
 * @brief Single-pixel WS2812B status LED -- RMT driver on GPIO42.
 *
 * Hardware (v7.2 §GPIO ASSIGNMENT):
 *   GPIO42 (LED_Din) -- RMT TX, NRZ-encoded WS2812B protocol.
 *   One physical pixel (not a strip). 24 bits per pixel, GRB order on the
 *   wire (NOT RGB -- WS2812B convention).
 *
 * Timing (WS2812B datasheet, +/- 150 ns tolerance):
 *   T0H = 0.4 us  high pulse for a "0" bit
 *   T0L = 0.85 us low pulse for a "0" bit
 *   T1H = 0.8 us  high pulse for a "1" bit
 *   T1L = 0.45 us low pulse for a "1" bit
 *   Reset (latch) = > 50 us low after the frame
 *
 * We use the IDF v5 RMT encoder with 10 MHz resolution (100 ns/tick), giving
 * 4 ticks = 0.4 us, 8 ticks = 0.8 us etc. -- inside the +/- 150 ns spec.
 *
 * State machine:
 *   off          -- LED dark
 *   idle         -- steady dim white (system alive, no notifications)
 *   charging     -- blue, sinusoidal pulse 1 Hz
 *   charged      -- steady green (full charge)
 *   alert        -- red, square-wave blink 2 Hz
 *
 * A 50 ms tick task drives the animations; `ws2812_set_color(r,g,b)` bypasses
 * the state machine for ad-hoc colour overrides.
 *
 * Core 0 only.
 *
 * Architecture: Blueprint 1 §3, Blueprint 5 §2
 */

#ifndef WS2812_H
#define WS2812_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

// ── Pin / RMT config ─────────────────────────────────────────────────────────
#define WS2812_GPIO          GPIO_NUM_42
#define WS2812_RMT_HZ        10000000U     // 10 MHz; 100 ns/tick
#define WS2812_PIXEL_COUNT   1             // single pixel
#define WS2812_RESET_US      55            // > 50 us low to latch the frame

// ── State machine ────────────────────────────────────────────────────────────
typedef enum {
    WS2812_STATE_OFF      = 0,
    WS2812_STATE_IDLE     = 1,
    WS2812_STATE_CHARGING = 2,
    WS2812_STATE_CHARGED  = 3,
    WS2812_STATE_ALERT    = 4,
} ws2812_state_t;

// ── Identity ─────────────────────────────────────────────────────────────────
const char *ws2812_get_chip_name(void);   // "WS2812B"
const char *ws2812_get_chip_desc(void);   // "Status LED (1 px)"

// ── Lifecycle ────────────────────────────────────────────────────────────────

/**
 * @brief Install the RMT TX channel + bytes encoder, start the 50 ms
 *        animation tick task. Initial state is WS2812_STATE_OFF.
 */
esp_err_t ws2812_init(void);

/** @brief Stop the animation task and free the RMT channel. */
void ws2812_deinit(void);

// ── Color / state API ────────────────────────────────────────────────────────

/**
 * @brief Set the LED to an exact RGB value. Bypasses the state machine
 *        until ws2812_set_state() is called again.
 *        Safe to call from any task.
 */
void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set the LED state. The animation task drives the colour from then
 *        on according to the state (e.g. CHARGING = pulsing blue).
 *        Safe to call from any task.
 */
void ws2812_set_state(ws2812_state_t state);

/** @brief Get the current state. */
ws2812_state_t ws2812_get_state(void);

// ── Color macros (24-bit RGB) ────────────────────────────────────────────────
#define WS2812_RGB(r, g, b)    ((uint32_t)(((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define WS2812_OFF             0x000000u
#define WS2812_RED             0xFF0000u
#define WS2812_GREEN           0x00FF00u
#define WS2812_BLUE            0x0000FFu
#define WS2812_YELLOW          0xFFFF00u
#define WS2812_MAGENTA         0xFF00FFu
#define WS2812_CYAN            0x00FFFFu
#define WS2812_WHITE           0x808080u   // half-intensity; full white is blinding on a 1px

#endif // WS2812_H
