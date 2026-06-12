/**
 * @file flashlight.h
 * @brief Flashlight LED driver -- LEDC PWM on GPIO41.
 *
 * Hardware (v7.2 §GPIO ASSIGNMENT):
 *   GPIO41 (GPIO_FLASHLIGHT) -- LEDC PWM output.
 *   Drive: low side; GPIO HIGH with duty > 0 lights the LED at the
 *   programmed brightness. GPIO LOW (or duty 0) = LED off.
 *   No hardware enable pin -- the GPIO is the entire interface.
 *
 *   Note: in v5 hardware GPIO41 was the system power latch; in Mk I it
 *   is the flashlight. boot_power.c MUST NOT drive GPIO41 as a power
 *   pin anymore.
 *
 * LEDC config:
 *   Timer 0, channel 0, frequency 1 kHz (avoids visible flicker AND
 *   avoids audible whine), 8-bit resolution (256 duty levels).
 *
 * API surface: brightness 0-100% (linear -- not gamma-corrected; LEDs
 * are already approximately linear in perception for these duty cycles).
 * Convenience helpers `flashlight_on()` / `_off()` map to 100% / 0%.
 *
 * Core 0 or Core 1 -- LEDC API is thread-safe.
 *
 * Architecture: Blueprint 1 §3, Blueprint 5 §2
 */

#ifndef FLASHLIGHT_H
#define FLASHLIGHT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

// ── Pin / LEDC config ────────────────────────────────────────────────────────
#define FLASHLIGHT_GPIO           GPIO_NUM_41
#define FLASHLIGHT_LEDC_TIMER     0
#define FLASHLIGHT_LEDC_CHANNEL   0
#define FLASHLIGHT_FREQ_HZ        1000        // 1 kHz; well above flicker, below audible
#define FLASHLIGHT_RES_BITS       8           // 8-bit = 256 duty levels
#define FLASHLIGHT_DUTY_MAX       ((1u << FLASHLIGHT_RES_BITS) - 1u)

// ── Identity ─────────────────────────────────────────────────────────────────
const char *flashlight_get_chip_name(void);   // "Flashlight"
const char *flashlight_get_chip_desc(void);   // "LEDC PWM, GPIO41"

// ── Lifecycle ────────────────────────────────────────────────────────────────

/**
 * @brief Configure LEDC timer + channel, drive duty = 0 (LED off). Safe to
 *        call multiple times; second call is a no-op after the first
 *        successful init.
 */
esp_err_t flashlight_init(void);

/** @brief Drive duty = 0 and release the LEDC channel. */
void flashlight_deinit(void);

// ── Brightness API ───────────────────────────────────────────────────────────

/**
 * @brief Set brightness, 0..100 (percentage). Values outside the range are
 *        clamped. Safe to call from any task. Returns ESP_OK on success.
 */
esp_err_t flashlight_set_brightness(uint8_t pct);

/** @brief Convenience: brightness = 100. */
esp_err_t flashlight_on(void);

/** @brief Convenience: brightness = 0. */
esp_err_t flashlight_off(void);

/** @brief Last commanded brightness (0..100). 0 if not initialised. */
uint8_t flashlight_get_brightness(void);

#endif // FLASHLIGHT_H
