/**
 * @file rgb_policy.c
 * @brief Background LED policy -- see rgb_policy.h for the timeline.
 *
 * Timeline after any activity (encoder scroll or notif):
 *   0 - 5000  ms : solid mode colour (preview)
 *   5 - 15000 ms : full-amplitude mode-colour pulse ("normal")
 *   15 - 25000 ms: pulse with amplitude fading linearly to zero ("fade")
 *   25000+ ms    : OFF
 *
 * Reactivate on rgb_policy_preview_start or rgb_policy_notify. Callers may
 * also call rgb_policy_pause / _resume to hand the LED to a running mode.
 *
 * No automatic battery or charging colour today. Bench feedback 2026-08-27:
 * a 1 Hz blue charging pulse in a lit menu is disorienting and hides the
 * mode-select colour. If a charging indicator is needed later, layer it in
 * only when the charger physically toggles (not from the BQ hw_alive path).
 */

#include "rgb_policy.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "ws2812.h"

static const char *TAG = "RGB_POLICY";

#define TICK_MS             50
#define PREVIEW_MS         5000     // 0..PREVIEW_MS         -> solid
#define NORMAL_END_MS     15000     // PREVIEW_MS..NORMAL_END -> full pulse
#define FADE_END_MS       25000     // NORMAL_END..FADE_END   -> fading pulse
#define PULSE_PERIOD_MS    3000     // heartbeat period
#define PULSE_MIN_FRAC     0.15f    // floor -- never fully dark between beats

static esp_timer_handle_t s_timer = NULL;

static volatile bool     s_paused           = false;
static volatile int64_t  s_last_activity_us = 0;
static volatile uint8_t  s_mode_r           = 0;
static volatile uint8_t  s_mode_g           = 0;
static volatile uint8_t  s_mode_b           = 0;

// The shutdown watcher paints red during a 4 s hold and owns the LED then.
extern volatile bool g_shutdown_hold_active;

static inline int64_t now_us(void) { return esp_timer_get_time(); }

// Heartbeat envelope 0.15..1.0 at PULSE_PERIOD_MS.
static float pulse_envelope(int64_t t_us)
{
    uint32_t phase = (uint32_t)((t_us / 1000LL) % PULSE_PERIOD_MS);
    uint32_t half  = PULSE_PERIOD_MS / 2;
    uint32_t up    = phase < half ? phase : PULSE_PERIOD_MS - phase;
    float env      = PULSE_MIN_FRAC + (1.0f - PULSE_MIN_FRAC) * ((float)up / (float)half);
    return env;
}

// Cache the last painted RGB so we skip identical repeats. In the OFF phase
// (25 s+ since activity) this cuts 20 Hz of pointless RMT transmits, and
// during the solid preview it drops the tick to effectively 0 Hz until the
// pulse phase starts changing colour again. Real power win on battery.
static uint8_t s_last_r = 0xFF, s_last_g = 0xFF, s_last_b = 0xFF;

static void paint_scaled(float amp)
{
    uint8_t r, g, b;
    if (amp <= 0.0f) {
        r = g = b = 0;
    } else {
        if (amp > 1.0f) amp = 1.0f;
        r = (uint8_t)((float)s_mode_r * amp);
        g = (uint8_t)((float)s_mode_g * amp);
        b = (uint8_t)((float)s_mode_b * amp);
    }
    if (r == s_last_r && g == s_last_g && b == s_last_b) return;
    s_last_r = r;
    s_last_g = g;
    s_last_b = b;
    ws2812_set_color(r, g, b);
}

static void tick_cb(void *arg)
{
    (void)arg;
    if (s_paused) return;
    if (g_shutdown_hold_active) return;   // watcher owns LED

    int64_t t         = now_us();
    int64_t elapsed_ms = (t - s_last_activity_us) / 1000LL;

    if (elapsed_ms < PREVIEW_MS) {
        // 0..5 s solid mode colour.
        paint_scaled(1.0f);
    } else if (elapsed_ms < NORMAL_END_MS) {
        // 5..15 s full-amplitude pulse.
        paint_scaled(pulse_envelope(t));
    } else if (elapsed_ms < FADE_END_MS) {
        // 15..25 s pulse with amplitude fading linearly to zero.
        float fade = 1.0f - ((float)(elapsed_ms - NORMAL_END_MS) /
                             (float)(FADE_END_MS - NORMAL_END_MS));
        paint_scaled(pulse_envelope(t) * fade);
    } else {
        // 25 s+ OFF. paint_scaled with amp=0 drops through the cache and
        // fires the RMT transmit exactly once on entry to this phase.
        paint_scaled(0.0f);
    }
}

// Public API -----------------------------------------------------------------

esp_err_t rgb_policy_init(void)
{
    if (s_timer) return ESP_OK;

    const esp_timer_create_args_t args = {
        .callback = tick_cb,
        .arg      = NULL,
        .name     = "rgb_policy",
    };
    esp_err_t r = esp_timer_create(&args, &s_timer);
    if (r != ESP_OK) { ESP_LOGE(TAG, "esp_timer_create: %s", esp_err_to_name(r)); return r; }
    r = esp_timer_start_periodic(s_timer, TICK_MS * 1000ULL);
    if (r != ESP_OK) { ESP_LOGE(TAG, "esp_timer_start: %s", esp_err_to_name(r)); return r; }

    // Prime the activity timer so the LED shows something at boot instead
    // of jumping straight to the OFF branch until the first encoder turn.
    s_last_activity_us = now_us();

    ESP_LOGI(TAG, "rgb_policy armed (%d ms tick; preview %d ms, normal %d ms, "
                  "fade %d ms, then off)",
             TICK_MS, PREVIEW_MS,
             (NORMAL_END_MS - PREVIEW_MS),
             (FADE_END_MS - NORMAL_END_MS));
    return ESP_OK;
}

void rgb_policy_pause(void)  { s_paused = true;  }
void rgb_policy_resume(void) { s_paused = false; }

void rgb_policy_set_mode_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_mode_r = r;
    s_mode_g = g;
    s_mode_b = b;
}

void rgb_policy_preview_start(uint8_t r, uint8_t g, uint8_t b)
{
    rgb_policy_set_mode_color(r, g, b);
    s_last_activity_us = now_us();
}

void rgb_policy_notify(void)
{
    s_last_activity_us = now_us();
}
