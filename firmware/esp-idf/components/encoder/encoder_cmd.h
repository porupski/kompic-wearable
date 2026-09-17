/**
 * @file encoder_cmd.h
 * @brief Crown encoder command surface -- Module_Blueprint.md §3.
 *
 * Thin wrapper over `encoder.h` counters + polled-path notify hook. The tile
 * (Stage 24 §2.4b -- deferred until Mk1b display is up) will consume the
 * same surface. CLI verb `ENC` covers the bench side today.
 *
 * No enable/set: the encoder is always-on and stateless from a control
 * perspective. The only "commands" are diagnostic (dump / status).
 */
#ifndef ENCODER_CMD_H
#define ENCODER_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// -- reset counters ----------------------------------------------------------
// Zero CW/CCW/total-detents so the tile's "test ground" starts fresh.
// Does NOT tear down the driver; counters resume on next detent.
void encoder_cmd_reset(void);

// -- dump --------------------------------------------------------------------
void encoder_cmd_dump(void);

// -- status summary ----------------------------------------------------------
void encoder_cmd_status_summary(char *out, size_t max);

#endif // ENCODER_CMD_H
