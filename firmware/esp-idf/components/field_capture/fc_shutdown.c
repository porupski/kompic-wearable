/**
 * @file fc_shutdown.c
 * @brief Priority shutdown watcher -- owns the 4 s hold ship-mode gesture.
 *
 * Split out of field_capture.c in the Stage 12 refactor. Runs on its own
 * priority-6 task (higher than field_capture task's priority 4) so no
 * recording / ECG loop / mic capture can starve the hold detector.
 *
 * Ship mode is KING: the BQ25619 write path now retries the I2C mutex with
 * escalating timeouts (see bq25619_enter_ship_mode) so an in-flight I2C
 * transaction can't silently swallow a real 4 s ship-mode hold. Release
 * detection is also debounced (SHDN_RELEASE_MS) so a bouncy switch can't
 * abort a genuine hold.
 */

#include "fc_internal.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "data_broker.h"
#include "ws2812.h"
#include "haptic.h"
#include "bq25619.h"

static const char *TAG = "FC_SHDN";

// ── Watcher globals (extern-decl'd in fc_internal.h) ────────────────────────
static volatile bool s_ship_mode_latched = false;
volatile bool g_shutdown_hold_active           = false;
volatile bool g_recording_active               = false;
volatile bool g_watcher_press_reset_pending    = false;
volatile bool g_batt_test_active               = false;

// Best-effort "shut down NOW". Does NOT try to flush open files -- if a
// recording is in progress and the user wants it out, that is their call.
// The 300 ms red LED gives immediate visual confirmation before BATFET drops;
// if USB is attached, BATFET drops but the app keeps running -- unplug USB
// to finish shutdown.
void watcher_ship_mode(void) {
    if (s_ship_mode_latched) return;   // idempotent
    s_ship_mode_latched = true;

    ESP_LOGW(TAG, "PRIORITY SHIP MODE -- long-hold intercepted");

    // Instant red LED so the operator sees it registered even if the
    // field_capture task was mid-print.
    ws2812_set_color(WS_MAX, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(300));

    if (broker_battery_hw_alive()) {
        // bq25619_enter_ship_mode now retries the I2C mutex with escalating
        // timeouts (500 ms -> 1.5 s -> 3 s). An in-flight I2C transaction
        // will complete inside that budget, so ship mode no longer silently
        // fails when drv2605 / BQ polling holds the bus at the moment of
        // the 4 s trigger. See bq25619.c ~line 259.
        (void)bq25619_enter_ship_mode(I2C_NUM_1);
        // On battery only: BATFET drops within milliseconds and this delay
        // never returns because power is gone. On USB: BATFET drops but the
        // device stays alive via USB power, so we come back through -- give
        // the ship attempt half a second to actually kill us if it can.
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGE(TAG, "BQ not alive -- power stays on");
    }
    ESP_LOGW(TAG, "ship mode returned (USB present?) -- device stays running");
}

// Hold-to-shutdown ladder (Stage 17 update):
//   0 -    1000 ms   : still a click. button_poll() treats it as short-click
//                       on release; shutdown watcher does NOT commit yet.
//   1000 - 2000 ms   : LED ramps 0 -> full red (visual confirmation), watcher
//                       has now committed (g_shutdown_hold_active = true).
//                       Clicks are already suppressed by button_poll() thanks
//                       to BTN_LONG_PRESS_MS.
//   2000 - 3700 ms   : DRV click every 500 ms (haptic countdown warning).
//   3700 - 3950 ms   : sustained warm-up buzz -- 3 STRONG_CLICKs 80 ms apart
//                       so the LRA is definitely awake at ship time.
//   >= 4000 ms       : long DRV buzz + fire watcher_ship_mode() unconditionally,
//                       even while the button is still held. If USB is
//                       providing power the ship is a no-op; if not, BATFET
//                       drops within a few ms.
//   sustained release before 4000 ms: reset state, no fire, LED released back
//                       to field_capture.
#define SHDN_COMMIT_MS    1000    // button_poll swallows click past this
#define SHDN_WARN_MS      2000
#define SHDN_BUZZ_WARN_MS 3700    // Stage 17: DRV warm-up burst begins
#define SHDN_FIRE_MS      4000
#define SHDN_BUZZ_MS       500

// A single-sample high on PIN_BUTTON must be ignored -- otherwise a bouncy /
// dying encoder switch mid-hold can silently abort a real 4 s ship-mode press.
// Only treat the button as "released" if it stays high for this long.
#define SHDN_RELEASE_MS     80

// Damage-control auto-shutoff: if the firmware has been running for this long
// regardless of what the user has been doing, drop BATFET.
#define SHDN_MAX_UPTIME_MS  (15u * 60u * 1000u)

void task_shutdown_watcher_fn(void *arg) {
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    bool     hold_active     = false;
    uint32_t press_start     = 0;
    uint32_t last_buzz_ms    = 0;
    bool     warn_started    = false;
    bool     warm_up_fired   = false;   // Stage 17: SHDN_BUZZ_WARN_MS one-shot
    uint32_t high_since_ms   = 0;   // release-debounce timer

    uint32_t uptime_cap_deadline = SHDN_MAX_UPTIME_MS;

    ESP_LOGI(TAG, "shutdown watcher armed: hold %d ms to ship (buzz warn at %d ms). "
                  "hard uptime cap = %u s.",
             SHDN_FIRE_MS, SHDN_WARN_MS, (unsigned)(SHDN_MAX_UPTIME_MS / 1000));

    for (;;) {
        uint32_t now = millis_u32();
        bool     low = (gpio_get_level(PIN_BUTTON) == 0);

        // ── Damage-control uptime cap ────────────────────────────────────
        if (g_recording_active || g_batt_test_active) {
            uptime_cap_deadline = now + SHDN_MAX_UPTIME_MS;
        }
        if (!g_recording_active && !g_batt_test_active && now >= uptime_cap_deadline) {
            ESP_LOGW(TAG, "shutdown FIRE (uptime cap reached, now=%u ms)", (unsigned)now);
            g_shutdown_hold_active = true;
            ws2812_set_color(WS_MAX, 0, 0);
            haptic_play_forced(DRV_LONG_BUZZ);
            watcher_ship_mode();
            g_shutdown_hold_active = false;
            uptime_cap_deadline    = now + SHDN_MAX_UPTIME_MS;
        }

        // ── Press-reset request (from MLC_COLLECT after its 2 s stop) ────
        if (g_watcher_press_reset_pending && hold_active && low) {
            press_start   = now;
            last_buzz_ms  = 0;
            warn_started  = false;
            g_watcher_press_reset_pending = false;
        } else if (g_watcher_press_reset_pending && !low) {
            g_watcher_press_reset_pending = false;
        }

        if (low && !hold_active) {
            // -- Fresh press begins --
            hold_active     = true;
            press_start     = now;
            last_buzz_ms    = 0;
            warn_started    = false;
            warm_up_fired   = false;
            high_since_ms   = 0;
            // Stage 17: do NOT set g_shutdown_hold_active until the press
            // crosses SHDN_COMMIT_MS. Sub-1s presses stay in "just a click"
            // territory and let field_capture handle them normally.
            ws2812_set_color(0, 0, 0);
        } else if (!low && hold_active) {
            // -- Possible release. Debounce: only ACT on release if the pin
            //    has been high for SHDN_RELEASE_MS. This eats brief bounces
            //    from a dying switch that would otherwise silently abort a
            //    genuine 4 s ship-mode hold (bench 2026-08 -- user reported
            //    long-press being interpreted as single-click).
            if (high_since_ms == 0) high_since_ms = now;
            if ((now - high_since_ms) < SHDN_RELEASE_MS) {
                // Treat as still held for now -- do not abort. Fall through
                // into the "held" branch logic against press_start.
                uint32_t held = now - press_start;
                uint32_t intensity = (held * WS_MAX) / SHDN_FIRE_MS;
                if (intensity > WS_MAX) intensity = WS_MAX;
                ws2812_set_color((uint8_t)intensity, 0, 0);
                // No buzz retriggers here -- unknown yet if real release or
                // a bounce; continue the timer.
                if (held >= SHDN_FIRE_MS) {
                    // Fire threshold reached DURING a bounce. Ship anyway --
                    // held long enough regardless of the momentary blip.
                    ESP_LOGW(TAG, "shutdown FIRE at %u ms (mid-bounce)",
                             (unsigned)held);
                    haptic_play_forced(DRV_LONG_BUZZ);
                    watcher_ship_mode();
                    g_shutdown_hold_active = false;
                    hold_active            = false;
                    warn_started           = false;
                    high_since_ms          = 0;
                }
            } else {
                // Confirmed release -- either abort or safety-net ship.
                uint32_t held = now - press_start;
                hold_active   = false;
                high_since_ms = 0;
                g_shutdown_hold_active = false;
                if (held >= SHDN_FIRE_MS) {
                    // Stage 17: safety net -- if the FIRE branch didn't run
                    // while the button was still held (e.g. task scheduling
                    // hiccup), fire on release regardless. Once past the
                    // 4 s mark the operator has already committed.
                    ESP_LOGW(TAG, "shutdown FIRE at %u ms (release safety net)",
                             (unsigned)held);
                    haptic_play_forced(DRV_LONG_BUZZ);
                    watcher_ship_mode();
                    warn_started  = false;
                    warm_up_fired = false;
                } else if (held >= SHDN_WARN_MS) {
                    haptic_play(DRV_MEDIUM_CLICK);
                    ESP_LOGI(TAG, "shutdown aborted (released after %u ms)",
                             (unsigned)held);
                    warn_started  = false;
                    warm_up_fired = false;
                }
            }
        } else if (low && hold_active) {
            // Any low sample clears the release-debounce timer.
            high_since_ms = 0;
            uint32_t held = now - press_start;

            // Stage 17: commit at 1000 ms. Until then the press is still
            // ambiguous (could be a slow click) so we don't set the global
            // hold flag nor light the LED.
            if (!g_shutdown_hold_active && held >= SHDN_COMMIT_MS) {
                g_shutdown_hold_active = true;
                ESP_LOGI(TAG, "shutdown commit at %u ms -- clicks now suppressed",
                         (unsigned)held);
            }

            if (g_shutdown_hold_active) {
                uint32_t intensity = (held * WS_MAX) / SHDN_FIRE_MS;
                if (intensity > WS_MAX) intensity = WS_MAX;
                ws2812_set_color((uint8_t)intensity, 0, 0);
            }

            if (held >= SHDN_WARN_MS && held < SHDN_BUZZ_WARN_MS) {
                if (!warn_started) {
                    warn_started = true;
                    ESP_LOGW(TAG, "shutdown warn phase started -- release to abort");
                    haptic_play_forced(DRV_STRONG_CLICK);
                    last_buzz_ms = now;
                } else if ((now - last_buzz_ms) >= SHDN_BUZZ_MS) {
                    last_buzz_ms = now;
                    haptic_play_forced(DRV_STRONG_CLICK);
                }
            }

            // Stage 17: warm-up burst 3700..3950 ms -- 3 STRONG_CLICKs 80 ms
            // apart. Ensures the LRA is already spinning by ship time so the
            // final LONG_BUZZ isn't a cold start.
            if (held >= SHDN_BUZZ_WARN_MS && held < SHDN_FIRE_MS && !warm_up_fired) {
                warm_up_fired = true;
                ESP_LOGW(TAG, "shutdown warm-up at %u ms -- LRA priming",
                         (unsigned)held);
                haptic_play_forced(DRV_STRONG_CLICK);
                vTaskDelay(pdMS_TO_TICKS(80));
                haptic_play_forced(DRV_STRONG_CLICK);
                vTaskDelay(pdMS_TO_TICKS(80));
                haptic_play_forced(DRV_STRONG_CLICK);
            }

            if (held >= SHDN_FIRE_MS) {
                ESP_LOGW(TAG, "shutdown FIRE at %u ms hold", (unsigned)held);
                haptic_play_forced(DRV_LONG_BUZZ);
                watcher_ship_mode();
                g_shutdown_hold_active = false;
                hold_active            = false;
                warn_started           = false;
                warm_up_fired          = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
