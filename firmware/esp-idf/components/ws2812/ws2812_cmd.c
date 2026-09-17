/**
 * @file ws2812_cmd.c
 * @brief WS2812 command surface -- see ws2812_cmd.h.
 */

#include "ws2812_cmd.h"
#include "ws2812.h"
#include "rgb_policy.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "WS2812_CMD";

// Track the most recently latched override so dump/status can report it.
// (The ws2812 driver doesn't expose the current pixel colour; only its
// state machine enum is queryable.)
static bool     s_override_active = false;
static uint8_t  s_last_r = 0, s_last_g = 0, s_last_b = 0;

esp_err_t ws2812_cmd_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    rgb_policy_pause();
    ws2812_set_color(r, g, b);
    s_override_active = true;
    s_last_r = r; s_last_g = g; s_last_b = b;
    ESP_LOGI(TAG, "override (%u, %u, %u) -- rgb_policy paused",
             (unsigned)r, (unsigned)g, (unsigned)b);
    return ESP_OK;
}

esp_err_t ws2812_cmd_auto(void)
{
    rgb_policy_resume();
    s_override_active = false;
    ESP_LOGI(TAG, "AUTO -- rgb_policy resumed");
    return ESP_OK;
}

static const char *state_name(ws2812_state_t s)
{
    switch (s) {
        case WS2812_STATE_OFF:      return "OFF";
        case WS2812_STATE_IDLE:     return "IDLE";
        case WS2812_STATE_CHARGING: return "CHARGING";
        case WS2812_STATE_CHARGED:  return "CHARGED";
        case WS2812_STATE_ALERT:    return "ALERT";
        default:                    return "?";
    }
}

void ws2812_cmd_dump(void)
{
    printf("[RGB] %s @ GPIO 42 (RMT, 1 px)\n", ws2812_get_chip_name());
    printf("[RGB]   state         = %s\n", state_name(ws2812_get_state()));
    printf("[RGB]   override      = %s\n", s_override_active ? "ACTIVE" : "released (policy owns)");
    if (s_override_active) {
        printf("[RGB]   last colour   = (%u, %u, %u)\n",
               (unsigned)s_last_r, (unsigned)s_last_g, (unsigned)s_last_b);
    }
}

void ws2812_cmd_status_summary(char *out, size_t max)
{
    if (!out || max == 0) return;
    if (s_override_active) {
        snprintf(out, max, "override=(%u,%u,%u) state=%s",
                 (unsigned)s_last_r, (unsigned)s_last_g, (unsigned)s_last_b,
                 state_name(ws2812_get_state()));
    } else {
        snprintf(out, max, "auto state=%s", state_name(ws2812_get_state()));
    }
}
