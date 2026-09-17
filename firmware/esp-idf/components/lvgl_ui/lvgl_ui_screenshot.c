/**
 * @file lvgl_ui_screenshot.c
 * @brief PNG screenshot of the active LVGL screen -- see lvgl_ui_screenshot.h.
 *
 * PNG layout: 8-byte signature + IHDR (13B data) + IDAT (zlib wrapper +
 * one deflate stored block per scanline + adler32) + IEND. One block per
 * row keeps the encoder trivial: block header (5B) is written before the
 * row filter byte + row pixels, so no back-patching. Total overhead per
 * PNG: ~2.3 KB for a 466-row image.
 *
 * CRC32 (chunks) and Adler32 (zlib stream) are streamed as bytes are
 * written so we never buffer the whole IDAT payload -- only the row.
 */

#include "lvgl_ui_screenshot.h"
#include "lvgl_ui_display.h"      // lvgl_port_lock via public API? -- uses lvgl_port
#include "esp_lvgl_port.h"        // lvgl_port_lock / _unlock
#include "lvgl.h"
#include "draw/lv_draw_buf.h"
#include "draw/snapshot/lv_snapshot.h"

#include "sdcard.h"
#include "pcf85063_cmd.h"
#include "boot_display.h"          // LCD_H_RES / LCD_V_RES
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>          // fsync
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "LVGL_SHOT";

#define SHOT_DIR      "/sd/lvgl_fb"
#define SHOT_SUFFIX   "screenshot"

// ── CRC32 (PNG chunk CRC / polynomial 0xEDB88320) ───────────────────────────

static uint32_t crc32_tab[256];
static bool     crc32_tab_ready = false;

static void crc32_init(void)
{
    if (crc32_tab_ready) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_tab[n] = c;
    }
    crc32_tab_ready = true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_tab[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ── Adler32 (zlib) ──────────────────────────────────────────────────────────

static uint32_t adler32_update(uint32_t adler, const uint8_t *buf, size_t len)
{
    uint32_t a = adler & 0xFFFF;
    uint32_t b = (adler >> 16) & 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        a = (a + buf[i]) % 65521u;
        b = (b + a)      % 65521u;
    }
    return (b << 16) | a;
}

// ── Byte-order helpers ──────────────────────────────────────────────────────

static void put_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// ── PNG chunk writers ───────────────────────────────────────────────────────

static bool write_all(FILE *f, const uint8_t *buf, size_t len)
{
    return fwrite(buf, 1, len, f) == len;
}

// Write a small chunk whose entire payload we already have in RAM.
static bool png_chunk(FILE *f, const char type[4],
                      const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    put_u32_be(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    if (!write_all(f, hdr, 8)) return false;
    if (len && !write_all(f, data, len)) return false;

    uint32_t crc = crc32_update(0, hdr + 4, 4);   // type is CRC'd
    if (len) crc = crc32_update(crc, data, len);

    uint8_t crc_be[4]; put_u32_be(crc_be, crc);
    return write_all(f, crc_be, 4);
}

// ── Path composition ────────────────────────────────────────────────────────

static void try_mkdir(const char *path)
{
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir(%s) failed: %s", path, strerror(errno));
    }
}

static void compose_path(char *out, size_t n)
{
    char stamp[24];
    if (pcf85063_cmd_filename_stamp(stamp, sizeof(stamp)) == ESP_OK) {
        snprintf(out, n, "%s/kompic_%s_%s.png", SHOT_DIR, stamp, SHOT_SUFFIX);
    } else {
        uint64_t up_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
        snprintf(out, n, "%s/%s_up%llu.png", SHOT_DIR, SHOT_SUFFIX,
                 (unsigned long long)up_ms);
    }
}

// ── Snapshot + PNG stream ───────────────────────────────────────────────────

// Encode a 466x466 (or w×h) RGB888 buffer as PNG streamed to `f`.
// Uses one deflate stored block per scanline -- simplest possible encoder,
// zero compression, ~640 KB output for 466x466. Bench-only tool.
static esp_err_t write_png_rgb888(FILE *f, uint32_t w, uint32_t h,
                                  const uint8_t *rgb, uint32_t stride)
{
    crc32_init();

    // ── Signature ──────────────────────────────────────────────────────────
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if (!write_all(f, sig, 8)) return ESP_FAIL;

    // ── IHDR ───────────────────────────────────────────────────────────────
    uint8_t ihdr[13];
    put_u32_be(ihdr,     w);
    put_u32_be(ihdr + 4, h);
    ihdr[8]  = 8;  // bit depth
    ihdr[9]  = 2;  // color type: RGB (3 bytes/pixel)
    ihdr[10] = 0;  // compression: deflate
    ihdr[11] = 0;  // filter: standard
    ihdr[12] = 0;  // interlace: none
    if (!png_chunk(f, "IHDR", ihdr, sizeof(ihdr))) return ESP_FAIL;

    // ── IDAT ───────────────────────────────────────────────────────────────
    // Compute IDAT total length up-front (chunk length must be written
    // before the data). Each row -> one stored block of (1 + w*3) payload
    // bytes plus 5-byte block header. Plus 2-byte zlib header, 4-byte
    // adler32 trailer.
    const uint32_t row_payload = 1u + w * 3u;   // filter byte + pixels
    const uint32_t idat_len    = 2u + h * (5u + row_payload) + 4u;

    uint8_t idat_hdr[8];
    put_u32_be(idat_hdr, idat_len);
    memcpy(idat_hdr + 4, "IDAT", 4);
    if (!write_all(f, idat_hdr, 8)) return ESP_FAIL;

    uint32_t crc   = crc32_update(0, idat_hdr + 4, 4);
    uint32_t adler = 1;

    // zlib header: CMF=0x78 (deflate, 32K window), FLG=0x01 (no dict, lowest
    // compression, checksum-consistent).
    uint8_t zhdr[2] = {0x78, 0x01};
    if (!write_all(f, zhdr, 2)) return ESP_FAIL;
    crc = crc32_update(crc, zhdr, 2);

    // Row buffer: filter byte + w * 3 pixel bytes.
    uint8_t *row = (uint8_t *)heap_caps_malloc(row_payload,
                                               MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_8BIT);
    if (!row) return ESP_ERR_NO_MEM;

    for (uint32_t y = 0; y < h; y++) {
        // Stored block header: bit 0 = BFINAL, bits 1-2 = BTYPE (00 = stored).
        uint8_t blk_hdr[5];
        blk_hdr[0] = (y == h - 1) ? 0x01 : 0x00;   // BFINAL on last row
        uint16_t len  = (uint16_t)row_payload;
        uint16_t nlen = (uint16_t)(~len);
        blk_hdr[1] = (uint8_t)(len  & 0xFF);
        blk_hdr[2] = (uint8_t)(len  >> 8);
        blk_hdr[3] = (uint8_t)(nlen & 0xFF);
        blk_hdr[4] = (uint8_t)(nlen >> 8);
        if (!write_all(f, blk_hdr, 5)) { heap_caps_free(row); return ESP_FAIL; }
        crc = crc32_update(crc, blk_hdr, 5);

        // Row data: filter byte (0 = None) + RGB triples.
        row[0] = 0;
        memcpy(row + 1, rgb + (size_t)y * stride, w * 3u);
        if (!write_all(f, row, row_payload)) { heap_caps_free(row); return ESP_FAIL; }
        crc   = crc32_update(crc, row, row_payload);
        adler = adler32_update(adler, row, row_payload);
    }
    heap_caps_free(row);

    // zlib adler32 trailer, then IDAT chunk CRC.
    uint8_t adler_be[4]; put_u32_be(adler_be, adler);
    if (!write_all(f, adler_be, 4)) return ESP_FAIL;
    crc = crc32_update(crc, adler_be, 4);

    uint8_t crc_be[4]; put_u32_be(crc_be, crc);
    if (!write_all(f, crc_be, 4)) return ESP_FAIL;

    // ── IEND ───────────────────────────────────────────────────────────────
    if (!png_chunk(f, "IEND", NULL, 0)) return ESP_FAIL;
    return ESP_OK;
}

// ── Public entry point ──────────────────────────────────────────────────────

esp_err_t lvgl_ui_screenshot_write(char *out_path, size_t out_path_len)
{
    // LVGL's default lv_snapshot_take() would allocate the pixel buffer via
    // lv_malloc (LV_MEM_SIZE = 64 KB); a 466x466 RGB888 snapshot is ~650 KB
    // and won't fit. Allocate in PSRAM ourselves and drive the "take-to"
    // variant so LVGL fills our buffer instead.
    //
    // Stride math: LVGL uses lv_draw_buf_width_to_stride() internally which
    // aligns width*bpp up. Over-allocate by a couple KB to cover alignment
    // without having to duplicate LVGL's private stride helper.
    const size_t buf_size = (size_t)LCD_H_RES * LCD_V_RES * 3u + 4096u;
    uint8_t *pix = (uint8_t *)heap_caps_malloc(buf_size,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pix) {
        ESP_LOGE(TAG, "PSRAM alloc %u failed", (unsigned)buf_size);
        return ESP_ERR_NO_MEM;
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        heap_caps_free(pix);
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_draw_buf_t draw_buf;
    // The 1,1 placeholder + buf_size stride mirror what LVGL's own
    // lv_snapshot_take_to_buf() sets before calling take_to_draw_buf --
    // take_to_draw_buf reshapes the buf using the object's actual size.
    lv_draw_buf_init(&draw_buf, 1, 1, LV_COLOR_FORMAT_RGB888,
                     (uint32_t)buf_size, pix, (uint32_t)buf_size);
    lv_result_t rv = scr
        ? lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB888, &draw_buf)
        : LV_RESULT_INVALID;
    uint32_t w = draw_buf.header.w;
    uint32_t h = draw_buf.header.h;
    uint32_t stride = draw_buf.header.stride;
    lvgl_port_unlock();

    if (rv != LV_RESULT_OK || w == 0 || h == 0) {
        ESP_LOGE(TAG, "snapshot failed (rv=%d scr=%p w=%u h=%u)",
                 (int)rv, (void *)scr, (unsigned)w, (unsigned)h);
        heap_caps_free(pix);
        return ESP_FAIL;
    }

    // Mount SD if needed. Undo on exit so we don't hold the card for a
    // one-shot verb.
    bool mount_here = false;
    if (!sdcard_is_mounted()) {
        if (sdcard_mount() != ESP_OK) {
            ESP_LOGE(TAG, "sdcard_mount failed");
            heap_caps_free(pix);
            return ESP_ERR_INVALID_STATE;
        }
        mount_here = true;
    }
    try_mkdir(SHOT_DIR);

    char path[128];
    compose_path(path, sizeof(path));

    FILE *f = fopen(path, "wb");
    esp_err_t r = ESP_OK;
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: %s", path, strerror(errno));
        r = ESP_FAIL;
    } else {
        r = write_png_rgb888(f, w, h, pix, stride);
        fflush(f);
        int fd = fileno(f);
        if (fd >= 0) fsync(fd);
        fclose(f);
    }

    if (mount_here) (void)sdcard_unmount();
    heap_caps_free(pix);

    if (r == ESP_OK) {
        ESP_LOGI(TAG, "wrote %s (%ux%u RGB888)", path, (unsigned)w, (unsigned)h);
        if (out_path && out_path_len) {
            strncpy(out_path, path, out_path_len - 1);
            out_path[out_path_len - 1] = '\0';
        }
    }
    return r;
}
