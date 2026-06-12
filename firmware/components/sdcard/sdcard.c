/**
 * @file sdcard.c
 * @brief SDMMC + FatFS on-demand storage, 1-bit mode.
 *
 * Why 1-bit mode?
 *   v7.2 routes only DAT0; DAT1-3 are unrouted. 1-bit SDMMC peaks around
 *   12.5 MB/s on UHS-I cards -- more than enough for sensor logging and
 *   audio capture (16 kHz / 16-bit PDM is 32 KB/s, three orders of
 *   magnitude under saturation).
 *
 * Why on-demand mount?
 *   The SDMMC host draws ~5-15 mA quiescent. For a watch that logs only
 *   occasionally, keeping the host parked between sessions is the right
 *   power posture. The cost is the one-time mount latency (~100-300 ms
 *   typical, dominated by the card's own init).
 *
 * Why FatFS (not raw blocks)?
 *   Cards out of the box are FAT-formatted; the user can pop the card
 *   into a laptop and read logs without custom tooling. The throughput
 *   ceiling of FatFS over SDMMC is much higher than what we generate.
 *
 * Concurrency:
 *   The IDF SDMMC host is internally serialised, but our own session
 *   state (FILE* + path + suffix counter) needs a mutex. `s_lock` is
 *   recursive so callers can do open + write + close inside a single
 *   critical section.
 */

#include "sdcard.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "SDCARD";

const char *sdcard_get_chip_name(void) { return "SD Card";                 }
const char *sdcard_get_chip_desc(void) { return "SDMMC 1-bit, FatFS @ /sd"; }

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────

static SemaphoreHandle_t s_lock      = NULL;
static sdmmc_card_t     *s_card      = NULL;
static bool              s_mounted   = false;
static FILE             *s_session_f = NULL;
static char              s_session_path[SDCARD_PATH_MAX];

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static inline esp_err_t take(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static inline void give(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

static bool name_is_safe(const char *name)
{
    if (!name || !*name) return false;
    size_t len = strlen(name);
    if (len > SDCARD_SESSION_NAME_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '_')) return false;
    }
    return true;
}

/**
 * Probe /sd/<name>_NNNN.csv for NNNN = 0..9999; return the lowest unused
 * suffix. Worst case is 10000 stat() calls; in practice we expect <100.
 */
static int next_session_suffix(const char *name)
{
    char probe[SDCARD_PATH_MAX];
    struct stat st;
    for (int i = 0; i < 10000; i++) {
        int n = snprintf(probe, sizeof(probe),
                         SDCARD_MOUNT_POINT "/%s_%04d.csv", name, i);
        if (n <= 0 || n >= (int)sizeof(probe)) return -1;
        if (stat(probe, &st) != 0) return i;        // ENOENT -> free
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t sdcard_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_mounted   = false;
    s_card      = NULL;
    s_session_f = NULL;
    s_session_path[0] = '\0';
    ESP_LOGI(TAG, "sdcard_init OK (host not yet mounted)");
    return ESP_OK;
}

esp_err_t sdcard_mount(void)
{
    esp_err_t r = take();
    if (r != ESP_OK) return r;

    if (s_mounted) { give(); return ESP_OK; }

    ESP_LOGI(TAG, "Mounting SDMMC 1-bit at " SDCARD_MOUNT_POINT
                  " (CLK=%d CMD=%d DAT0=%d)",
             SDCARD_GPIO_CLK, SDCARD_GPIO_CMD, SDCARD_GPIO_DAT0);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;       // 40 MHz; falls back if card rejects

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = SDCARD_GPIO_CLK;
    slot.cmd   = SDCARD_GPIO_CMD;
    slot.d0    = SDCARD_GPIO_DAT0;
    slot.width = 1;                                  // 1-bit -- v7.2 hardware
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;    // no external pull-ups on Mk I

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed   = false,           // do NOT auto-format -- user data preservation
        .max_files                = SDCARD_MAX_OPEN_FILES,
        .allocation_unit_size     = SDCARD_ALLOC_UNIT_SIZE,
        .disk_status_check_enable = false,
    };

    r = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (r != ESP_OK) {
        if (r == ESP_FAIL) {
            ESP_LOGE(TAG, "Mount failed -- card may be unformatted or missing");
        } else {
            ESP_LOGE(TAG, "esp_vfs_fat_sdmmc_mount: %s", esp_err_to_name(r));
        }
        s_card = NULL;
        give();
        return r;
    }

    s_mounted = true;
    if (s_card) {
        ESP_LOGI(TAG, "Card mounted: name=%s, capacity=%llu MiB",
                 s_card->cid.name,
                 ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    }
    give();
    return ESP_OK;
}

esp_err_t sdcard_unmount(void)
{
    esp_err_t r = take();
    if (r != ESP_OK) return r;

    if (s_session_f) {
        fflush(s_session_f);
        fclose(s_session_f);
        s_session_f = NULL;
        s_session_path[0] = '\0';
    }

    if (!s_mounted) { give(); return ESP_OK; }

    r = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "unmount returned %s", esp_err_to_name(r));
    }
    s_card    = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "Unmounted " SDCARD_MOUNT_POINT);
    give();
    return r;
}

bool sdcard_is_mounted(void) { return s_mounted; }

int32_t sdcard_get_capacity_mib(void)
{
    if (!s_mounted || !s_card) return -1;
    uint64_t bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    return (int32_t)(bytes >> 20);
}

int32_t sdcard_get_free_mib(void)
{
    if (!s_mounted) return -1;
    FATFS *fs;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) return -1;
    uint64_t free_bytes = (uint64_t)free_clusters * fs->csize * 512u;
    return (int32_t)(free_bytes >> 20);
}

// ─────────────────────────────────────────────────────────────────────────────
// Session API
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t sdcard_open_session(const char *name, char *out_path, size_t out_path_len)
{
    if (!name_is_safe(name)) return ESP_ERR_INVALID_ARG;

    esp_err_t r = take();
    if (r != ESP_OK) return r;

    if (!s_mounted)   { give(); return ESP_ERR_INVALID_STATE; }
    if (s_session_f)  { give(); return ESP_ERR_INVALID_STATE; }

    int suffix = next_session_suffix(name);
    if (suffix < 0) { give(); return ESP_ERR_NOT_FOUND; }

    int n = snprintf(s_session_path, sizeof(s_session_path),
                     SDCARD_MOUNT_POINT "/%s_%04d.csv", name, suffix);
    if (n <= 0 || n >= (int)sizeof(s_session_path)) {
        give();
        return ESP_ERR_INVALID_SIZE;
    }

    s_session_f = fopen(s_session_path, "wb");
    if (!s_session_f) {
        ESP_LOGE(TAG, "fopen(%s, wb) failed", s_session_path);
        s_session_path[0] = '\0';
        give();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "session open: %s", s_session_path);
    if (out_path && out_path_len) {
        strncpy(out_path, s_session_path, out_path_len - 1);
        out_path[out_path_len - 1] = '\0';
    }
    give();
    return ESP_OK;
}

esp_err_t sdcard_write(const void *buf, size_t len)
{
    if (!buf || !len) return ESP_ERR_INVALID_ARG;
    esp_err_t r = take();
    if (r != ESP_OK) return r;
    if (!s_session_f) { give(); return ESP_ERR_INVALID_STATE; }
    size_t w = fwrite(buf, 1, len, s_session_f);
    give();
    return (w == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t sdcard_flush(void)
{
    esp_err_t r = take();
    if (r != ESP_OK) return r;
    if (!s_session_f) { give(); return ESP_ERR_INVALID_STATE; }
    fflush(s_session_f);
    int fd = fileno(s_session_f);
    if (fd >= 0) fsync(fd);
    give();
    return ESP_OK;
}

esp_err_t sdcard_close_session(void)
{
    esp_err_t r = take();
    if (r != ESP_OK) return r;
    if (!s_session_f) { give(); return ESP_OK; }
    fflush(s_session_f);
    fclose(s_session_f);
    s_session_f = NULL;
    ESP_LOGI(TAG, "session closed: %s", s_session_path);
    s_session_path[0] = '\0';
    give();
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// One-shot helper
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t sdcard_write_oneshot(const char *name, const void *buf, size_t len,
                               char *out_path, size_t out_path_len)
{
    esp_err_t r = sdcard_open_session(name, out_path, out_path_len);
    if (r != ESP_OK) return r;
    r = sdcard_write(buf, len);
    if (r != ESP_OK) {
        (void)sdcard_close_session();
        return r;
    }
    return sdcard_close_session();
}
