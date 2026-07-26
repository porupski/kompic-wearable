/**
 * @file nvs_cfg.h
 * @brief Per-driver NVS command state (Stage 11 Item B).
 *
 * Parallel config-flow store distinct from data_broker (which is data-flow).
 * Every serial command that mutates persistent driver state calls into this
 * component so the value survives reboot. NVS is treated as the source of
 * truth; code defaults are factory-reset fallbacks only.
 *
 * NAMESPACES:
 *   "cfg_rtc"  -- RTC command log (SET_TIME wall-clock, boot_seq, last cmd)
 *   "cfg_sys"  -- system knobs (boot-time NVS printout toggle, future globals)
 *
 * Called from Core 1 / RTC CLI context only. Not thread-safe across writers
 * -- add a mutex if a second concurrent-write path lands.
 *
 * Every function returns esp_err_t. app_nvs_init() from app_logic must have
 * been called earlier in boot (this component does not call nvs_flash_init).
 */

#ifndef NVS_CFG_H
#define NVS_CFG_H

// Driver version: MAJOR.MINOR.PATCH -- bump PATCH on any change here,
// MINOR on feature adds, MAJOR on release quality (beta / RC / GA).
#define NVS_CFG_DRIVER_VERSION  "0.3.1"

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -- Sizes ---------------------------------------------------------------------
#define NVS_CFG_LAST_CMD_MAX      32   // includes NUL terminator
#define NVS_CFG_LAST_SET_TIME_MAX NVS_CFG_LAST_CMD_MAX  // alias for readability

// -- RTC command state ---------------------------------------------------------
//
// last_set_time is the full serial line of the most recent SET_TIME command.
// Named specifically -- reserved for SET_TIME only, so a future "last generic
// RTC command" (SET_ALARM etc.) can live in its own field. Renamed from
// "last_cmd" in 0.4.12; older NVS layouts stored the same string under
// K_RTC_LAST_CMD and are read back transparently for backward compat.

typedef struct {
    uint64_t wall_ts;                              // Unix seconds at last SET_TIME
    uint64_t wr_ms;                                // esp_timer millis at write (in-boot)
    uint32_t boot_seq;                             // boot_seq at write
    char     last_set_time[NVS_CFG_LAST_CMD_MAX];  // full serial line of last SET_TIME
    bool     valid;                                // false if never written
} nvs_cfg_rtc_t;

/** @brief Load the RTC record. On first boot returns ESP_OK with valid=false. */
esp_err_t nvs_cfg_rtc_load(nvs_cfg_rtc_t *out);

/**
 * @brief Save the RTC record. `last_set_time` should be the full SET_TIME
 *        command line as typed by the user (used for redundancy vs the
 *        derived wall_ts + boot_seq).
 */
esp_err_t nvs_cfg_rtc_save(uint64_t wall_ts, uint64_t wr_ms,
                            uint32_t boot_seq, const char *last_set_time);

// -- System / global knobs -----------------------------------------------------

/** @brief True if boot-time NVS printout is enabled (default: true). */
bool nvs_cfg_sys_get_print_on_boot(void);

/** @brief Set the boot-time NVS printout flag. Persists immediately. */
esp_err_t nvs_cfg_sys_set_print_on_boot(bool enabled);

/**
 * @brief True if battery-test mode should be entered on next boot.
 *
 * When true:
 *   - task_shutdown_watcher_fn's 15-minute uptime cap is disabled (see
 *     g_batt_test_active flag).
 *   - field_capture task enters run_battery_test_mode() at startup instead
 *     of its normal FCM state machine. That loop logs one CSV row every
 *     10 s to /sd/data/battery/batt_<boot_seq>.csv with UTC timestamp, ESP
 *     SoC die temperature, and battery voltage/soc.
 *   - All I2C-driven sensors stay parked (their brokers default disabled).
 *
 * Default: false. Toggle via serial "BATT_TEST ON" / "BATT_TEST OFF".
 * Reboot required to enter/leave the mode.
 */
bool nvs_cfg_sys_get_batt_test(void);

/** @brief Set the battery-test flag. Persists immediately. */
esp_err_t nvs_cfg_sys_set_batt_test(bool enabled);

/**
 * @brief BLACKBOX telemetry logger flag.
 *
 * When true, on boot the field_capture component spawns a low-priority task
 * that snapshots every broker + system stat to
 * /sd/data/blackbox/bb_<boot_seq>.csv every bb_cadence_s seconds. Runs
 * alongside normal FCM operation (unlike batt_test which replaces it).
 * If batt_test is also on, blackbox exits at startup to avoid double-CSV.
 *
 * Default: false. Toggle via serial "BLACKBOX ON" / "BLACKBOX OFF".
 * Reboot required to enter/leave the mode.
 */
bool nvs_cfg_sys_get_blackbox(void);
esp_err_t nvs_cfg_sys_set_blackbox(bool enabled);

/**
 * @brief BLACKBOX sample cadence in seconds. Default 10. Range 1..3600.
 *        Live-tunable via "BLACKBOX_CADENCE <s>" -- takes effect on next
 *        sample tick (no reboot needed for cadence changes).
 */
uint16_t nvs_cfg_sys_get_bb_cadence_s(void);
esp_err_t nvs_cfg_sys_set_bb_cadence_s(uint16_t s);

// -- Firmware version bookkeeping ---------------------------------------------

#define NVS_CFG_FW_STR_MAX  16   // "MAJOR.MINOR.PATCH" + NUL, generous

/**
 * @brief Read the fw version this NVS was last synced with. Returns empty
 *        string if never written (first boot after adding this feature).
 *
 * Buffer must be at least NVS_CFG_FW_STR_MAX bytes.
 */
esp_err_t nvs_cfg_sys_get_last_fw(char *out, size_t out_len);

/** @brief Persist the current fw version string. */
esp_err_t nvs_cfg_sys_set_last_fw(const char *fw);

/**
 * @brief Boot-time sync: compare the passed-in current fw against the stored
 *        last-known fw. If different, log "upgraded X -> Y" (or "first boot")
 *        and update NVS to the current value. Idempotent within a boot.
 */
void nvs_cfg_sys_check_fw_version(const char *current_fw);

// -- Boot printout -------------------------------------------------------------

/**
 * @brief Pretty-print every NVS record this component owns, plus the
 *        PCF85063A RAM_byte for redundancy. No-op if
 *        nvs_cfg_sys_get_print_on_boot() is false. Uses printf() so it
 *        renders on both UART0 and USB-Serial-JTAG.
 *
 * @param i2c_num  I2C port for the PCF85063 read (usually I2C_NUM_0)
 */
void nvs_cfg_boot_print(int i2c_num);

#ifdef __cplusplus
}
#endif

#endif // NVS_CFG_H
