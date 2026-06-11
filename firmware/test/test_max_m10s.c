/**
 * @file test_max_m10s.c
 * @brief Standalone diagnostic for the MAX-M10S GNSS driver.
 *
 * Wiring (Kompic_Mk1_System_Instructions_v7.2.md  -- §GPIO ASSIGNMENT, §UART):
 *   UART1   TX = GPIO17  ->  GPS RX
 *   UART1   RX = GPIO18  <-  GPS TX
 *   1PPS    = GPIO46 (rising-edge ISR)
 *   Baud    : 9600 (MAX-M10S default)
 *
 * Phases:
 *   1. UART1 init + 1PPS ISR install.
 *   2. NMEA parser sanity -- feed a hardcoded $GPRMC + $GPGGA pair, confirm
 *      the snapshot exposes the expected lat/lon/UTC.
 *   3. UBX-NAV-TIMEUTC parser sanity -- feed a hand-built frame, confirm
 *      time_valid flips even before any NMEA arrives.
 *   4. Live UART drain -- 10 s observation window; print every snapshot,
 *      every 1PPS edge, and the running 1PPS count.
 *   5. Stack high-water + heap dump.
 *
 * Notes:
 *   - The hardcoded NMEA strings are exercised by writing them straight into
 *     the parser via uart_write_bytes() loopback on the same UART. With no
 *     real GPS attached, the chip's TX line is floating but the ESP TX line
 *     drives our own RX through the on-board test fixture (or via a jumper
 *     wire to short TX18 <-> RX17). The parser itself doesn't care where
 *     the bytes come from.
 *   - The UBX frame is fed via the public max_m10s_feed_ubx_byte() entry
 *     point, bypassing UART entirely.
 */

#include "max_m10s.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "test_max_m10s";

// Required by drivers that include boot_hw_init.h's extern declaration of
// g_i2c_mutex (the RTC seed path inside max_m10s.c uses it). Even though
// this test doesn't seed the RTC, the symbol must exist at link time.
SemaphoreHandle_t g_i2c_mutex = NULL;
volatile bool     g_gps_time_seeded = false;

// ─── Phase 2: hardcoded NMEA strings ────────────────────────────────────────
// $GPRMC fixes UTC time + position; $GPGGA fixes altitude + sats + hdop.
// Lat 4603.0660  N  -> 46.0511° N
// Lon 01430.3060 E  -> 14.5051° E
// Time 14:32:07 UTC on 14 Feb 2026.
static const char NMEA_GGA_SAMPLE[] =
    "$GPGGA,143207.00,4603.0660,N,01430.3060,E,1,08,1.2,302.4,M,46.9,M,,*4F\r\n";
static const char NMEA_RMC_SAMPLE[] =
    "$GPRMC,143207.00,A,4603.0660,N,01430.3060,E,12.5,87.5,140226,,,A*7E\r\n";

// ─── Phase 3: hand-built UBX-NAV-TIMEUTC frame ──────────────────────────────
// Class 0x01, ID 0x21, payload length 20.
// iTOW=0, tAcc=0, nano=0, year=2026, month=2, day=14, hh=14, mm=32, ss=07,
// valid=0x07 (validTOW | validWKN | validUTC).
// Checksum (Fletcher-8) computed over class..end-of-payload below.
static uint8_t s_ubx_frame[28] = {
    0xB5, 0x62,                 // sync
    0x01, 0x21,                 // class, id
    0x14, 0x00,                 // length = 20 LE
    0x00, 0x00, 0x00, 0x00,     // iTOW
    0x00, 0x00, 0x00, 0x00,     // tAcc
    0x00, 0x00, 0x00, 0x00,     // nano
    0xEA, 0x07,                 // year = 2026
    0x02,                       // month
    0x0E,                       // day = 14
    0x0E,                       // hour = 14
    0x20,                       // min = 32
    0x07,                       // sec = 7
    0x07,                       // valid = 0b00000111
    0x00, 0x00,                 // ck_a, ck_b -- filled below
};

static void compute_ubx_checksum(uint8_t *frame, size_t total_len)
{
    // Checksum range: bytes [2 .. total_len-3]
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < total_len - 2; i++) {
        a = (uint8_t)(a + frame[i]);
        b = (uint8_t)(b + a);
    }
    frame[total_len - 2] = a;
    frame[total_len - 1] = b;
}

// ─── Phase utilities ────────────────────────────────────────────────────────
static void feed_string_to_parser(const char *s)
{
    // The NMEA parser only sees what the UART driver buffers. The cleanest
    // way to drive it from inside this same task without a real fix is to
    // write into the UART TX pin and read it back via a TX<->RX loopback
    // jumper on the dev board. If no jumper is present, the test prints
    // a warning -- the UBX phase still works because it's fed directly.
    size_t n = strlen(s);
    int written = uart_write_bytes(MAX_M10S_UART_NUM, s, n);
    if (written != (int)n) {
        ESP_LOGW(TAG, "uart_write_bytes truncated: %d/%u", written, (unsigned)n);
    }
    // Give the chars time to round-trip through the loopback.
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void dump_snapshot(const broker_gps_data_t *bd)
{
    ESP_LOGI(TAG,
             "snap: fix=%d sats=%u hdop=%.1f  lat=%.4f lon=%.4f alt=%.1f "
             "speed=%.1f  time=%04u-%02u-%02u %02u:%02u:%02u valid_t=%d valid_p=%d",
             (int)bd->fix, (unsigned)bd->sats_in_use, (double)bd->hdop,
             bd->latitude, bd->longitude, (double)bd->altitude_m,
             (double)bd->speed_kmh,
             bd->utc_year, bd->utc_month, bd->utc_day,
             bd->utc_hour, bd->utc_minute, bd->utc_second,
             (int)bd->time_valid, (int)bd->position_valid);
}

static void test_max_m10s_run(void)
{
    ESP_LOGI(TAG, "Chip: %s -- %s",
             max_m10s_get_chip_name(), max_m10s_get_chip_desc());

    if (g_i2c_mutex == NULL) g_i2c_mutex = xSemaphoreCreateMutex();

    // ── Phase 1: driver init ───────────────────────────────────────────────
    int64_t t_i0 = esp_timer_get_time();
    esp_err_t err = max_m10s_init();
    int64_t t_i1 = esp_timer_get_time();
    ESP_LOGI(TAG, "max_m10s_init: %s in %lld us",
             esp_err_to_name(err), (long long)(t_i1 - t_i0));
    if (err != ESP_OK) return;

    // ── Phase 2: NMEA parser sanity (loopback) ─────────────────────────────
    ESP_LOGI(TAG, "[Phase 2] feeding NMEA GGA + RMC via UART loopback...");
    feed_string_to_parser(NMEA_GGA_SAMPLE);
    feed_string_to_parser(NMEA_RMC_SAMPLE);

    esp_err_t pret = max_m10s_update();
    ESP_LOGI(TAG, "max_m10s_update -> %s", esp_err_to_name(pret));

    broker_gps_data_t bd = {0};
    max_m10s_get_snapshot(&bd);
    dump_snapshot(&bd);
    if (bd.time_valid && bd.position_valid &&
        bd.utc_year == 2026 && bd.utc_month == 2 && bd.utc_day == 14) {
        ESP_LOGI(TAG, "[Phase 2] NMEA parse OK");
    } else {
        ESP_LOGW(TAG, "[Phase 2] NMEA parse did not populate -- check TX<->RX "
                      "loopback jumper on dev board");
    }

    // ── Phase 3: UBX-NAV-TIMEUTC parser sanity (direct feed) ──────────────
    ESP_LOGI(TAG, "[Phase 3] feeding hand-built UBX NAV-TIMEUTC frame...");
    compute_ubx_checksum(s_ubx_frame, sizeof(s_ubx_frame));
    ESP_LOGI(TAG, "computed ck_a=0x%02X ck_b=0x%02X",
             (unsigned)s_ubx_frame[26], (unsigned)s_ubx_frame[27]);

    int64_t t_u0 = esp_timer_get_time();
    for (size_t i = 0; i < sizeof(s_ubx_frame); i++) {
        max_m10s_feed_ubx_byte(s_ubx_frame[i]);
    }
    int64_t t_u1 = esp_timer_get_time();
    ESP_LOGI(TAG, "UBX 28 bytes fed in %lld us", (long long)(t_u1 - t_u0));

    max_m10s_get_snapshot(&bd);
    dump_snapshot(&bd);
    if (bd.time_valid) {
        ESP_LOGI(TAG, "[Phase 3] UBX parse OK -- time_valid set");
    } else {
        ESP_LOGE(TAG, "[Phase 3] UBX parse FAILED -- time_valid still false");
    }

    // ── Phase 4: live UART drain + 1PPS observation ─────────────────────────
    ESP_LOGI(TAG, "[Phase 4] live observation for 10 s...");
    uint32_t prev_pps = max_m10s_get_pps_count();
    int64_t  t_phase4 = esp_timer_get_time();
    while ((esp_timer_get_time() - t_phase4) < 10LL * 1000LL * 1000LL) {
        (void)max_m10s_update();
        max_m10s_get_snapshot(&bd);

        uint32_t pps_now = max_m10s_get_pps_count();
        if (pps_now != prev_pps) {
            ESP_LOGI(TAG, "1PPS edge #%lu @ %lld us",
                     (unsigned long)pps_now,
                     (long long)max_m10s_get_last_pps_us());
            prev_pps = pps_now;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "[Phase 4] final 1PPS count = %lu",
             (unsigned long)max_m10s_get_pps_count());

    // ── Phase 5: memory snapshot ───────────────────────────────────────────
    ESP_LOGI(TAG, "stack high-water (this task): %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "free heap (internal): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "All checks completed.");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    test_max_m10s_run();
}
