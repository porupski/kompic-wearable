/**
 * @file bq25619_cmd.h
 * @brief BQ25619 command surface -- Module_Blueprint.md §3.
 *
 * One UI-agnostic surface shared by CLI (fc_cli `BQ` verb), tile handlers,
 * and any future outlet. Wraps the low-level `bq25619.h` primitives; owns
 * the g_i2c2_mutex acquisition for verb-level ops so callers don't have to.
 *
 * batt_test mode is NOT part of this surface -- it's a field_capture mode
 * flag that happens to log battery data; it will migrate to fc_cmd when
 * field_capture gets its own command surface.
 */
#ifndef BQ25619_CMD_H
#define BQ25619_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// -- enable ------------------------------------------------------------------
// Toggles REG_POC.CHG_CONFIG. Charger continues to observe VBUS + BATFET
// hardware constraints; disabling with USB plugged just prevents current
// flow into the pack.
esp_err_t bq_cmd_enable_set(bool on);
bool      bq_cmd_enable_get(void);

// -- boost (PMID 5 V) --------------------------------------------------------
// Mutually exclusive with charging (BQ25619 HW enforces).
esp_err_t bq_cmd_boost_set(bool on);
bool      bq_cmd_boost_get(void);

// -- action: ship-mode -------------------------------------------------------
// Raw charger action -- writes BATFET_DIS in REG_MISC. NB: fc_shutdown's
// watcher_ship_mode() is the higher-level orchestrator (long-buzz + this).
// Call this directly only if you want the raw drop with no UX.
esp_err_t bq_cmd_ship_mode(void);

// -- dump --------------------------------------------------------------------
// Human-readable full-register dump to stdout (CLI outlet).
void bq_cmd_dump(void);

// -- status summary ----------------------------------------------------------
// One-liner for the global STATUS printout. Writes at most `max` bytes
// (including terminating NUL) into `out`.
void bq_cmd_status_summary(char *out, size_t max);

#endif // BQ25619_CMD_H
