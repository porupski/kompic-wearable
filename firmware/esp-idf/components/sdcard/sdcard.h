/**
 * @file sdcard.h
 * @brief microSD over SDMMC, 1-bit mode -- on-demand mount/unmount, FatFS.
 *
 * Hardware (v7.2 §GPIO ASSIGNMENT):
 *   GPIO38 -- SD_CLK
 *   GPIO39 -- SD_CMD
 *   GPIO40 -- SD_DAT0      (DAT1-3 are NOT routed; 1-bit mode only)
 *   Card-detect: no GPIO (SD_Cd test pad only -- per v7.2 §Signals NOT on
 *   a GPIO). Mount must defensively probe.
 *
 * Card slot: see Mk I main PCB. 3V3 powered; no external pull-ups (the
 * IDF driver enables the internal pull-ups on CMD / DAT0).
 *
 * Mount strategy:
 *   - On-demand: the watch wakes the slot only when logging is active.
 *     `sdcard_mount()` is called from whatever module wants persistence;
 *     `sdcard_unmount()` releases the host so the slot can be powered
 *     down during sleep.
 *   - FatFS via esp_vfs_fat_sdmmc_mount() at MOUNT_POINT ("/sd").
 *   - max_files = 4 -- one log + room for a few diagnostics opens.
 *   - allocation_unit_size = 16K -- a good FatFS default for sequential
 *     log writes on SD cards.
 *   - The mounted state is sticky: a second mount call is a no-op.
 *
 * Session rotation:
 *   - `sdcard_open_session(name)` opens "/sd/<name>_NNNN.csv" where NNNN
 *     is the lowest unused 4-digit suffix. Caller writes via
 *     `sdcard_write(buf, len)` and closes via `sdcard_close_session()`.
 *   - One session at a time. Simple deliberately.
 *
 * Concurrency:
 *   - All file ops + mount are serialised by a private mutex (`s_lock`).
 *     The IDF SDMMC host is itself thread-safe but our session state is
 *     not -- write/close ordering across tasks would otherwise race.
 *
 * Architecture: Blueprint 1 §3 (driver pattern), Blueprint 5 §6 (storage).
 *
 * Brief: docs/18_PHASE_5_BATCH_ADVANCED.md, Module 1.
 */

#ifndef SDCARD_H
#define SDCARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/gpio.h"

// -- Pinout (v7.2 §GPIO ASSIGNMENT) -------------------------------------------
#define SDCARD_GPIO_CLK     GPIO_NUM_38
#define SDCARD_GPIO_CMD     GPIO_NUM_39
#define SDCARD_GPIO_DAT0    GPIO_NUM_40

// -- Mount point + tunables ---------------------------------------------------
#define SDCARD_MOUNT_POINT          "/sd"
#define SDCARD_MAX_OPEN_FILES       4
#define SDCARD_ALLOC_UNIT_SIZE      (16U * 1024U)
#define SDCARD_SESSION_NAME_MAX     24      // base name without _NNNN.csv suffix
#define SDCARD_PATH_MAX             64      // mount point + name + suffix + .csv

// -- Identity (test harness + tiles) ------------------------------------------
const char *sdcard_get_chip_name(void);     // "SD Card"
const char *sdcard_get_chip_desc(void);     // "SDMMC 1-bit, FatFS @ /sd"

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Initialise the private mutex. Does NOT touch the SDMMC host or
 *        mount anything; that is `sdcard_mount()`. Idempotent.
 */
esp_err_t sdcard_init(void);

/**
 * @brief Power up the SDMMC host, probe the card, mount FatFS at /sd.
 *        Returns ESP_ERR_NOT_FOUND if no card is present.
 *        Returns ESP_OK + sticks if already mounted.
 */
esp_err_t sdcard_mount(void);

/**
 * @brief Unmount FatFS and release the SDMMC host. Safe to call when not
 *        mounted (returns ESP_OK).
 */
esp_err_t sdcard_unmount(void);

/** @brief True if currently mounted. */
bool sdcard_is_mounted(void);

// -- Card info ----------------------------------------------------------------

/**
 * @brief Capacity in MiB; -1 if unknown / not mounted.
 */
int32_t sdcard_get_capacity_mib(void);

/**
 * @brief Free space in MiB; -1 if unknown / not mounted.
 */
int32_t sdcard_get_free_mib(void);

// -- Session-style log API ----------------------------------------------------

/**
 * @brief Open "/sd/<name>_NNNN.csv" with the lowest unused suffix.
 *        `name` is sanitised: only [A-Za-z0-9_] are kept; longer than
 *        SDCARD_SESSION_NAME_MAX is rejected.
 *        One session at a time; returns ESP_ERR_INVALID_STATE if a
 *        session is already open.
 *        Out-path is written into `out_path` (SDCARD_PATH_MAX bytes)
 *        for the caller to log.
 */
esp_err_t sdcard_open_session(const char *name, char *out_path, size_t out_path_len);

/**
 * @brief Append bytes to the currently open session. Buffered by the C
 *        runtime; call sdcard_flush() if you need on-disk durability.
 */
esp_err_t sdcard_write(const void *buf, size_t len);

/**
 * @brief fflush() + fsync() on the current session file.
 */
esp_err_t sdcard_flush(void);

/**
 * @brief Close the current session file. Idempotent.
 */
esp_err_t sdcard_close_session(void);

// -- One-shot helpers ---------------------------------------------------------

/**
 * @brief Open + write entire buffer + close. For one-off blobs. Uses the
 *        same name -> path rotation as `sdcard_open_session`.
 */
esp_err_t sdcard_write_oneshot(const char *name, const void *buf, size_t len,
                               char *out_path, size_t out_path_len);

#endif // SDCARD_H
