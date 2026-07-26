/**
 * @file ws2812.c
 * @brief Single-pixel WS2812B status LED driver -- RMT, Core 0.
 *
 * We build one rmt_bytes_encoder which knows the 0-bit and 1-bit RMT symbols
 * for WS2812B, then call rmt_transmit() with the 3-byte GRB frame whenever
 * we want to update the LED. The encoder adds the > 50 us low reset frame
 * after the bits, so a single rmt_transmit() = a complete LED update.
 *
 * 50 ms animation tick:
 *   - OFF      -- writes WS2812_OFF once on state entry, no further updates.
 *   - IDLE     -- steady WS2812_WHITE at half intensity.
 *   - CHARGING -- sinusoidal pulse in blue, 1 Hz.
 *   - CHARGED  -- steady green.
 *   - ALERT    -- square-wave red, 2 Hz (250 ms on, 250 ms off).
 *
 * ws2812_set_color() bypasses the state machine until a future
 * ws2812_set_state() resumes animations.
 *
 * Core 0 only. No LVGL. No FreeRTOS task created by init() -- the animation
 * task runs off esp_timer, which is lighter than a dedicated task for a
 * 50 ms cadence.
 */

#include "ws2812.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"                 // Stage 11 Item D: PM lock around RMT tx
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "WS2812";

const char *ws2812_get_chip_name(void) { return "WS2812B";          }
const char *ws2812_get_chip_desc(void) { return "Status LED (1 px)"; }

// ────────────────────────────────────────────────────────────────────────────
// RMT handles + animation state
// ────────────────────────────────────────────────────────────────────────────

static rmt_channel_handle_t s_rmt_channel = NULL;
static rmt_encoder_handle_t s_rmt_encoder = NULL;
static esp_timer_handle_t   s_anim_timer  = NULL;

// Stage 11 Item D: PM lock. Acquired around every rmt_transmit so DFS can't
// drop APB mid-frame (which would stretch or crush the WS2812B bit timing).
// Created in ws2812_init(); no-op when CONFIG_PM_ENABLE is not set (lock
// handle stays NULL and the acquire/release helpers short-circuit).
static esp_pm_lock_handle_t s_pm_lock = NULL;

static volatile ws2812_state_t s_state          = WS2812_STATE_OFF;
static volatile bool           s_manual_override = false;
static volatile int64_t        s_state_start_us  = 0;

// ────────────────────────────────────────────────────────────────────────────
// Wire-level encoding
// ────────────────────────────────────────────────────────────────────────────
//
// WS2812B requires per-bit RMT symbols. The IDF "bytes encoder" lets us
// declare the 0-bit and 1-bit symbol pairs and then push raw bytes; the
// encoder turns each bit into the right level/duration combination.
//
// At 10 MHz resolution (100 ns/tick):
//   T0H = 4 ticks (0.4 us) high, T0L = 8 ticks (0.8 us) low
//   T1H = 8 ticks (0.8 us) high, T1L = 4 ticks (0.4 us) low
// These are well inside the WS2812B +/- 150 ns spec.
// ────────────────────────────────────────────────────────────────────────────

static esp_err_t install_encoder(void)
{
    rmt_bytes_encoder_config_t cfg = {
        .bit0 = {
            .level0    = 1,
            .duration0 = 4,    // 0.4 us high
            .level1    = 0,
            .duration1 = 8,    // 0.8 us low
        },
        .bit1 = {
            .level0    = 1,
            .duration0 = 8,    // 0.8 us high
            .level1    = 0,
            .duration1 = 4,    // 0.4 us low
        },
        .flags.msb_first = 1,
    };
    return rmt_new_bytes_encoder(&cfg, &s_rmt_encoder);
}

// ────────────────────────────────────────────────────────────────────────────
// Push one (r,g,b) frame onto the wire. WS2812B accepts GRB, not RGB.
// ────────────────────────────────────────────────────────────────────────────

static void ws2812_push(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rmt_channel || !s_rmt_encoder) return;

    // GRB on the wire.
    uint8_t frame[3] = { g, r, b };

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    // Stage 11 PM fix (0.4.10): enable the RMT channel just before transmit,
    // disable immediately after. rmt_enable/disable acquire/release the
    // ESP_PM_CPU_FREQ_MAX lock. Without disable, the channel keeps the lock
    // for the entire session -- confirmed by PM_DUMP showing rmt_0_0
    // Active=1 Time=100% across all prior Stage-11 runs. With disable, the
    // lock is only held during the ~60 us of actual bit-banging.
    //
    // WS2812B needs a >50 us LOW "reset latch" between frames. rmt_disable
    // yanks the driver low, which serves as our reset. rmt_tx_wait_all_done
    // ensures the last bit is on the wire before we cut power.
    //
    // Our old ws2812 PM lock (s_pm_lock, ESP_PM_APB_FREQ_MAX) is redundant
    // now -- rmt_enable already ensures APB is at max during transmit.
    // Left in place for defense-in-depth on non-PM builds; acquire/release
    // both here for clarity.
    if (s_pm_lock) esp_pm_lock_acquire(s_pm_lock);
    (void)rmt_enable(s_rmt_channel);
    (void)rmt_transmit(s_rmt_channel, s_rmt_encoder, frame, sizeof(frame), &tx_cfg);
    (void)rmt_tx_wait_all_done(s_rmt_channel, pdMS_TO_TICKS(20));
    (void)rmt_disable(s_rmt_channel);
    if (s_pm_lock) esp_pm_lock_release(s_pm_lock);
}

void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_manual_override = true;
    ws2812_push(r, g, b);
}

// ────────────────────────────────────────────────────────────────────────────
// State machine + 50 ms animation tick
// ────────────────────────────────────────────────────────────────────────────

static void ws2812_anim_cb(void *arg)
{
    (void)arg;

    if (s_manual_override) return;
    if (!s_rmt_channel)    return;

    int64_t now_us = esp_timer_get_time();
    int64_t age_us = now_us - s_state_start_us;

    switch (s_state) {
    case WS2812_STATE_OFF:
        ws2812_push(0, 0, 0);
        break;

    case WS2812_STATE_IDLE:
        ws2812_push(0x40, 0x40, 0x40);  // dim white
        break;

    case WS2812_STATE_CHARGING: {
        // 1 Hz sinusoidal pulse on blue. Phase = (age_us % 1e6) / 1e6 * 2*pi
        float t = (float)(age_us % 1000000LL) / 1000000.0f;   // 0..1
        float s = (1.0f - cosf(t * 2.0f * 3.14159265f)) * 0.5f;  // 0..1
        uint8_t b = (uint8_t)(s * 0xFF);
        ws2812_push(0, 0, b);
        break;
    }

    case WS2812_STATE_CHARGED:
        ws2812_push(0, 0xC0, 0);  // steady green
        break;

    case WS2812_STATE_ALERT: {
        // 2 Hz square-wave red.
        bool on = ((age_us / 250000LL) & 1LL) == 0;
        ws2812_push(on ? 0xFF : 0x00, 0, 0);
        break;
    }
    }
}

void ws2812_set_state(ws2812_state_t state)
{
    s_state            = state;
    s_state_start_us   = esp_timer_get_time();
    s_manual_override  = false;
    // Drive an immediate update so the LED responds without waiting for the
    // next 50 ms tick.
    ws2812_anim_cb(NULL);
}

ws2812_state_t ws2812_get_state(void)
{
    return s_state;
}

// ────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────────────────

esp_err_t ws2812_init(void)
{
    ESP_LOGI(TAG, "Init RMT TX on GPIO%d @ %u Hz, %d pixel",
             WS2812_GPIO, WS2812_RMT_HZ, WS2812_PIXEL_COUNT);

#ifdef CONFIG_PM_ENABLE
    // Stage 11 Item D: PM lock created lazily. Failure is non-fatal -- we
    // continue without the lock, at the cost of possible LED timing glitches
    // when DFS drops APB. Better than refusing to boot.
    if (s_pm_lock == NULL) {
        esp_err_t lr = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0,
                                          "ws2812", &s_pm_lock);
        if (lr != ESP_OK) {
            ESP_LOGW(TAG, "PM lock create failed: %s (continuing without)",
                     esp_err_to_name(lr));
            s_pm_lock = NULL;
        }
    }
#endif

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = WS2812_GPIO,
        // Stage 11 PM notes:
        //
        // - 0.4.7: switched clk_src from APB → XTAL hoping the RMT driver
        //   would skip acquiring ESP_PM_CPU_FREQ_MAX. Bench PM_DUMP after
        //   0.4.7 showed rmt_0_0 STILL held Active=1 for the whole session.
        //   XTAL alone is not enough on IDF 5.5.
        //
        // - 0.4.8: added .flags.allow_pd = 1 hoping to force lock-free init.
        //   The RMT driver refused the channel creation:
        //     E (1424) rmt: rmt_new_tx_channel(257): not able to power down
        //                    in light sleep
        //   → RGB was DEAD for the whole boot. Reverted in 0.4.9.
        //
        // - 0.4.9 conclusion: keep XTAL as harmless (may still reduce peak
        //   APB pressure). Accept that rmt_0_0 holds ESP_PM_CPU_FREQ_MAX
        //   for the entire session. This does NOT block light-sleep --
        //   only ESP_PM_APB_FREQ_MAX and ESP_PM_NO_LIGHT_SLEEP do. See
        //   Stage_11_PM_Plan.md for the full analysis.
        .clk_src           = RMT_CLK_SRC_XTAL,
        .resolution_hz     = WS2812_RMT_HZ,
        // 64-symbol memory block: a single 24-bit pixel needs 24 symbols, so
        // a 64-symbol block has comfortable headroom.
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    esp_err_t ret = rmt_new_tx_channel(&tx_cfg, &s_rmt_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = install_encoder();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "install_encoder: %s", esp_err_to_name(ret));
        goto fail;
    }

    // Stage 11 PM (0.4.10): do NOT enable the channel here -- ws2812_push
    // now enables/disables per transmit so the RMT PM lock is only held for
    // the ~60 us of actual bit-banging. Enabling at init would leave the
    // lock permanently acquired, defeating the whole point.

    // Force LED off so the first update has a known starting state. WS2812B
    // boots in an undefined state; even the briefest random colour is ugly.
    ws2812_push(0, 0, 0);

    const esp_timer_create_args_t timer_args = {
        .callback = ws2812_anim_cb,
        .arg      = NULL,
        .name     = "ws2812_anim",
    };
    ret = esp_timer_create(&timer_args, &s_anim_timer);
    if (ret != ESP_OK) goto fail;
    ret = esp_timer_start_periodic(s_anim_timer, 50 * 1000ULL);
    if (ret != ESP_OK) goto fail;

    s_state           = WS2812_STATE_OFF;
    s_state_start_us  = esp_timer_get_time();
    s_manual_override = false;

    ESP_LOGI(TAG, "WS2812 init OK (state=OFF)");
    return ESP_OK;

fail:
    ws2812_deinit();
    return ret;
}

void ws2812_deinit(void)
{
    if (s_anim_timer) {
        esp_timer_stop(s_anim_timer);
        esp_timer_delete(s_anim_timer);
        s_anim_timer = NULL;
    }
    if (s_rmt_channel) {
        ws2812_push(0, 0, 0);  // dark on the way out
        rmt_disable(s_rmt_channel);
        rmt_del_channel(s_rmt_channel);
        s_rmt_channel = NULL;
    }
    if (s_rmt_encoder) {
        rmt_del_encoder(s_rmt_encoder);
        s_rmt_encoder = NULL;
    }
}
