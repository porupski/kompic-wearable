/**
 * @file boot_power.h
 * @brief Power primitives -- GPIO16 button (BQ25619 QON), GPIO0 (DRV_EN), PSRAM verify.
 *
 * v7.2 design:
 *   - GPIO16 = BQ_BUTTON: tactile button input, dual-wired to BQ25619 QON,
 *              RTC-wake source. Falling-edge ISR; hold-duration measured via
 *              esp_timer_get_time(). Short press toggles display sleep;
 *              long hold (>= 3 s) calls bq25619_enter_ship_mode().
 *   - GPIO0  = DRV_EN: haptic enable strap. Must be driven LOW after boot
 *              so the DRV2605 stays out of shutdown.
 *   - No GPIO power latch. The BQ25619's BATFET is the only off path.
 *
 * MUST be the first thing called in app_main() (drives GPIO0 LOW before any
 * sensor / display init runs).
 */

#ifndef BOOT_POWER_H
#define BOOT_POWER_H

#include <stdbool.h>
#include <stdint.h>

// -- GPIO pin map (v7.2 §GPIO Quick Pinout) -----------------------------------
#define GPIO_PWR_BTN     16   // INPUT, internal pull-up; falling-edge ISR; RTC wake (button)
#define GPIO_DRV_EN       0   // OUTPUT LOW; haptic enable strap

// -- Hold-duration thresholds (ms) --------------------------------------------
#define PWR_SHORT_PRESS_MAX_MS   800
#define PWR_HOLD_OVERLAY_MS     2500
#define PWR_HOLD_SHIPMODE_MS    3000

/**
 * @brief Drive GPIO0 LOW (DRV_EN), configure GPIO16 as input with INT, verify
 *        PSRAM is available. Call as the very first line of app_main().
 *
 *        Does NOT install the GPIO16 ISR -- that happens in task_power_btn_fn
 *        once g_i2c_mutex and the broker exist (the ISR's task path needs
 *        broker_init to have run).
 */
void boot_power_init(void);

// -- Task functions (declared for boot_tasks.c) -------------------------------

/**
 * @brief Power button task. Installs the GPIO16 falling-edge ISR, then waits
 *        on a task notification for each press. Measures hold duration via
 *        esp_timer_get_time(); classifies short / long / shutdown; drives the
 *        UI flags (g_wake_display / g_display_sleep / g_show_shutdown_overlay)
 *        and calls bq25619_enter_ship_mode() on >= PWR_HOLD_SHIPMODE_MS.
 *        Stack: 3072 bytes. Unpinned.
 */
void task_power_btn_fn(void *arg);

// -- Global power flags (Core 0 -> Core 1, atomics) ---------------------------

extern volatile bool g_wake_display;            // short press while asleep -> wake
extern volatile bool g_display_sleep;           // short press while awake -> sleep
extern volatile bool g_show_shutdown_overlay;   // hold >= overlay_ms
extern volatile bool g_shutdown_latched;        // hold >= shipmode_ms (one-shot)

#endif // BOOT_POWER_H
