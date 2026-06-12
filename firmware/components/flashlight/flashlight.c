/**
 * @file flashlight.c
 * @brief Flashlight LEDC driver -- Core 0 or Core 1, LEDC is thread-safe.
 *
 * Why 1 kHz?
 *   - Above the flicker-fusion threshold (no perceptible flicker at any
 *     duty cycle).
 *   - Below the audible range (no LEDC inductor whine, which is a real
 *     concern at typical PWM frequencies between 2-10 kHz).
 *   - At 8-bit resolution we get 256 duty steps, which is more than the
 *     eye can resolve as brightness changes -- so the resulting percentage
 *     control feels smooth.
 *
 * Why no gamma correction?
 *   For high-power LEDs at high duty cycles, the perceived brightness
 *   curve is approximately linear in PWM duty (the LED is already in a
 *   regime where the eye's response is roughly linear in raw photon
 *   flux). Adding gamma here would mostly waste low-end resolution.
 *
 * No FreeRTOS task. No esp_timer. Pure on-demand API.
 */

#include "flashlight.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "FLASHLIGHT";

const char *flashlight_get_chip_name(void) { return "Flashlight";       }
const char *flashlight_get_chip_desc(void) { return "LEDC PWM, GPIO41"; }

// ────────────────────────────────────────────────────────────────────────────
// State
// ────────────────────────────────────────────────────────────────────────────

static bool    s_initialised  = false;
static uint8_t s_current_pct  = 0;

// ────────────────────────────────────────────────────────────────────────────
// Init / deinit
// ────────────────────────────────────────────────────────────────────────────

esp_err_t flashlight_init(void)
{
    if (s_initialised) return ESP_OK;

    ESP_LOGI(TAG, "Init LEDC timer %d channel %d on GPIO%d @ %d Hz, %d-bit",
             FLASHLIGHT_LEDC_TIMER, FLASHLIGHT_LEDC_CHANNEL,
             FLASHLIGHT_GPIO, FLASHLIGHT_FREQ_HZ, FLASHLIGHT_RES_BITS);

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = FLASHLIGHT_LEDC_TIMER,
        .duty_resolution = FLASHLIGHT_RES_BITS,
        .freq_hz         = FLASHLIGHT_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t channel_cfg = {
        .gpio_num   = FLASHLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = FLASHLIGHT_LEDC_CHANNEL,
        .timer_sel  = FLASHLIGHT_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .duty       = 0,
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&channel_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialised = true;
    s_current_pct = 0;
    ESP_LOGI(TAG, "Flashlight init OK (duty = 0, LED off)");
    return ESP_OK;
}

void flashlight_deinit(void)
{
    if (!s_initialised) return;
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, FLASHLIGHT_LEDC_CHANNEL, 0);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, FLASHLIGHT_LEDC_CHANNEL);
    (void)ledc_stop(LEDC_LOW_SPEED_MODE, FLASHLIGHT_LEDC_CHANNEL, 0);
    s_initialised = false;
    s_current_pct = 0;
    ESP_LOGI(TAG, "Flashlight deinit -- LED off");
}

// ────────────────────────────────────────────────────────────────────────────
// Brightness API
// ────────────────────────────────────────────────────────────────────────────

esp_err_t flashlight_set_brightness(uint8_t pct)
{
    if (!s_initialised) {
        esp_err_t ret = flashlight_init();
        if (ret != ESP_OK) return ret;
    }
    if (pct > 100) pct = 100;

    // Linear mapping pct (0..100) -> duty (0..255).
    uint32_t duty = ((uint32_t)pct * FLASHLIGHT_DUTY_MAX) / 100u;

    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                   FLASHLIGHT_LEDC_CHANNEL, duty);
    if (ret != ESP_OK) return ret;
    ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, FLASHLIGHT_LEDC_CHANNEL);
    if (ret != ESP_OK) return ret;

    s_current_pct = pct;
    ESP_LOGD(TAG, "brightness = %u%% (duty = %lu / %lu)",
             (unsigned)pct, (unsigned long)duty,
             (unsigned long)FLASHLIGHT_DUTY_MAX);
    return ESP_OK;
}

esp_err_t flashlight_on(void)
{
    return flashlight_set_brightness(100);
}

esp_err_t flashlight_off(void)
{
    return flashlight_set_brightness(0);
}

uint8_t flashlight_get_brightness(void)
{
    return s_current_pct;
}
