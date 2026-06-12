/**
 * @file test_sdcard.c
 * @brief Standalone diagnostic for the SD card driver.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §GPIO ASSIGNMENT):
 *   GPIO38 -> SD CLK
 *   GPIO39 -> SD CMD
 *   GPIO40 -> SD DAT0     (DAT1-3 not connected; 1-bit mode)
 *
 * Phases:
 *   1. sdcard_init  -> mutex up.
 *   2. sdcard_mount -> SDMMC host + FatFS mount; log capacity / free MiB.
 *   3. Session open ("test_NN.csv") + write 256 KiB of synthetic data in
 *      4 KiB chunks; per-chunk throughput logged. Close.
 *   4. Re-open the file read-only and verify the first + last 16 bytes
 *      match the synthesis pattern.
 *   5. One-shot helper: 1 KiB blob to "oneshot_NN.csv".
 *   6. sdcard_unmount, stack high-water + heap.
 */

#include "sdcard.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_sdcard";

#define WRITE_TOTAL_KIB     256
#define WRITE_CHUNK_BYTES   4096

static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t x = seed;
    for (size_t i = 0; i < len; i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(x >> 24);
    }
}

static void test_sdcard_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             sdcard_get_chip_name(), sdcard_get_chip_desc());

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = sdcard_init();
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "sdcard_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t1 - t0));
    if (err != ESP_OK) return;

    // ── Phase 2: mount ──────────────────────────────────────────────────────
    int64_t tm0 = esp_timer_get_time();
    err = sdcard_mount();
    int64_t tm1 = esp_timer_get_time();
    ESP_LOGI(TAG, "sdcard_mount: %s in %lld us",
             esp_err_to_name(err), (long long)(tm1 - tm0));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no card or mount failed -- abort");
        return;
    }
    ESP_LOGI(TAG, "Capacity: %ld MiB, free: %ld MiB",
             (long)sdcard_get_capacity_mib(),
             (long)sdcard_get_free_mib());

    // ── Phase 3: write 256 KiB in 4 KiB chunks ──────────────────────────────
    char open_path[SDCARD_PATH_MAX];
    err = sdcard_open_session("test", open_path, sizeof(open_path));
    ESP_LOGI(TAG, "open_session: %s -> %s",
             esp_err_to_name(err), open_path);
    if (err != ESP_OK) { sdcard_unmount(); return; }

    uint8_t *chunk = (uint8_t *)malloc(WRITE_CHUNK_BYTES);
    if (!chunk) { ESP_LOGE(TAG, "malloc chunk failed"); sdcard_unmount(); return; }

    const size_t total_bytes  = (size_t)WRITE_TOTAL_KIB * 1024u;
    const size_t total_chunks = total_bytes / WRITE_CHUNK_BYTES;
    uint8_t first_bytes[16] = {0};
    uint8_t last_bytes[16]  = {0};

    int64_t tw0 = esp_timer_get_time();
    for (size_t c = 0; c < total_chunks; c++) {
        fill_pattern(chunk, WRITE_CHUNK_BYTES, (uint32_t)c * 0x9E3779B1u);
        if (c == 0) memcpy(first_bytes, chunk, sizeof(first_bytes));
        if (c == total_chunks - 1)
            memcpy(last_bytes, chunk + WRITE_CHUNK_BYTES - sizeof(last_bytes),
                   sizeof(last_bytes));
        err = sdcard_write(chunk, WRITE_CHUNK_BYTES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write failed at chunk %u: %s",
                     (unsigned)c, esp_err_to_name(err));
            break;
        }
    }
    sdcard_flush();
    int64_t tw1 = esp_timer_get_time();
    int64_t wall_us = tw1 - tw0;
    float mib_per_s = (wall_us > 0)
        ? ((float)total_bytes / (1024.0f * 1024.0f)) / ((float)wall_us / 1e6f)
        : 0.0f;
    ESP_LOGI(TAG, "Wrote %u KiB in %lld us -> %.2f MiB/s",
             (unsigned)WRITE_TOTAL_KIB, (long long)wall_us, (double)mib_per_s);

    sdcard_close_session();
    free(chunk);

    // ── Phase 4: read-back verification ─────────────────────────────────────
    FILE *f = fopen(open_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "read-back fopen failed");
    } else {
        uint8_t verify_first[16] = {0};
        uint8_t verify_last[16]  = {0};
        if (fread(verify_first, 1, sizeof(verify_first), f) != sizeof(verify_first)) {
            ESP_LOGE(TAG, "read first 16 bytes failed");
        }
        if (fseek(f, -(long)sizeof(verify_last), SEEK_END) == 0) {
            if (fread(verify_last, 1, sizeof(verify_last), f) != sizeof(verify_last)) {
                ESP_LOGE(TAG, "read last 16 bytes failed");
            }
        }
        fclose(f);

        bool first_ok = (memcmp(first_bytes, verify_first, sizeof(first_bytes)) == 0);
        bool last_ok  = (memcmp(last_bytes,  verify_last,  sizeof(last_bytes))  == 0);
        ESP_LOGI(TAG, "Verify first16=%s  last16=%s",
                 first_ok ? "OK" : "MISMATCH",
                 last_ok  ? "OK" : "MISMATCH");
    }

    // ── Phase 5: one-shot blob ──────────────────────────────────────────────
    uint8_t blob[1024];
    fill_pattern(blob, sizeof(blob), 0xDEADBEEFu);
    char one_path[SDCARD_PATH_MAX];
    int64_t to0 = esp_timer_get_time();
    err = sdcard_write_oneshot("oneshot", blob, sizeof(blob),
                                one_path, sizeof(one_path));
    int64_t to1 = esp_timer_get_time();
    ESP_LOGI(TAG, "oneshot: %s -> %s in %lld us",
             esp_err_to_name(err), one_path, (long long)(to1 - to0));

    // ── Phase 6: unmount + memory ───────────────────────────────────────────
    int64_t tu0 = esp_timer_get_time();
    sdcard_unmount();
    int64_t tu1 = esp_timer_get_time();
    ESP_LOGI(TAG, "unmount in %lld us", (long long)(tu1 - tu0));

    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_sdcard_run();
}
