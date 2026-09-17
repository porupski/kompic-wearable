/**
 * @file ws2812_cmd.h
 * @brief WS2812 status-LED command surface -- Module_Blueprint.md §3.
 *
 * Ties the raw ws2812 driver together with rgb_policy (the background
 * animation policy). Manual overrides pause the policy so the LED holds
 * the requested colour; AUTO releases the pause and hands the pixel back
 * to the policy's bright→fade-with-idle animation.
 *
 * The Stage 24 side-quest audit surfaced that rgb_policy_init() was
 * defined but never called at boot -- fixed in boot_hw_init.c during the
 * same batch. LED bright-at-boot behaviour depends on that init call.
 */
#ifndef WS2812_CMD_H
#define WS2812_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// -- manual override --------------------------------------------------------
// Latches the LED to a fixed RGB triple and pauses rgb_policy. Bench tool +
// notifications. `_auto` releases the pause.
esp_err_t ws2812_cmd_set_rgb(uint8_t r, uint8_t g, uint8_t b);
esp_err_t ws2812_cmd_auto(void);

// -- dump / status ----------------------------------------------------------
void ws2812_cmd_dump(void);
void ws2812_cmd_status_summary(char *out, size_t max);

#endif // WS2812_CMD_H
