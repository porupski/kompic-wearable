/**
 * @file pcf85063_cmd.h
 * @brief PCF85063A RTC command surface -- Module_Blueprint.md §3.
 *
 * One surface shared by CLI (fc_cli GET_TIME/SET_TIME/RTC_DUMP + new RTC verb),
 * rtc_tile (read-only today), and future outlets. Wraps the low-level driver
 * primitives; owns g_i2c_mutex acquisition where needed.
 *
 * Also owns the RTC-derived timestamp helpers used by SD writers:
 *   - pcf85063_cmd_iso_now(): "YYYY-MM-DDTHH:MM:SS" or "oscstop"
 *   - pcf85063_cmd_filename_stamp(): "YYYY-MM-DD_HH-MM-SS" for SD paths
 *
 * The filename-stamp helper is why datetime-based SD filenames live in this
 * cmd surface -- RTC access is the shared venue.
 */
#ifndef PCF85063_CMD_H
#define PCF85063_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pcf85063.h"       // pcf85063_time_t

// -- time get / set ----------------------------------------------------------
// Note: `time_get` reads directly from the chip (not the broker cache) so the
// CLI GET_TIME verb sees fresh data even if the 1 Hz task hasn't polled yet.
esp_err_t pcf85063_cmd_time_get(pcf85063_time_t *out);
esp_err_t pcf85063_cmd_time_set(const pcf85063_time_t *t);

// -- sync from GPS UTC -------------------------------------------------------
esp_err_t pcf85063_cmd_sync_utc(uint8_t hour, uint8_t minute, uint8_t second,
                                uint8_t day,  uint8_t month,  uint16_t year);

// -- ISO timestamps ----------------------------------------------------------
// "YYYY-MM-DDTHH:MM:SS" using broker cache. Writes "oscstop" if RTC not valid.
// Callers that require strict validity should use pcf85063_cmd_filename_stamp.
void pcf85063_cmd_iso_now(char *out, size_t n);

// "YYYY-MM-DD_HH-MM-SS" filename-safe stamp (colons replaced by dashes so it
// works on FAT/exFAT + macOS/Windows without escaping). Returns
// ESP_ERR_INVALID_STATE if the RTC broker is not valid (caller should
// fall back to a boot-seq name).
esp_err_t pcf85063_cmd_filename_stamp(char *out, size_t n);

// -- dump --------------------------------------------------------------------
void pcf85063_cmd_dump(void);

// -- status summary ----------------------------------------------------------
void pcf85063_cmd_status_summary(char *out, size_t max);

#endif // PCF85063_CMD_H
