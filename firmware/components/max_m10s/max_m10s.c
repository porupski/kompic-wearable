/**
 * @file max_m10s.c
 * @brief u-blox MAX-M10S GNSS driver -- UART NMEA + UBX parser, 1PPS ISR.
 *
 * Carries forward from tu10f.c:
 *   - Bulk chunk UART reading (512 bytes at a time)
 *   - Binary-junk byte filter for the NMEA line discipline
 *   - GGA + RMC sentence parsers (with debug sentence capture)
 *   - timegm() RTC-seeding path on first valid time
 *
 * New in this version:
 *   - UBX-NAV-TIMEUTC parser (class 0x01, id 0x21) -- gives atomic UTC time
 *     BEFORE the first position fix is available.
 *   - 1PPS ISR on GPIO46 -- rising-edge count + last-edge timestamp.
 *   - Two independent line disciplines on the same UART stream:
 *       * NMEA (printable ASCII, line-terminated by CR/LF)
 *       * UBX  (binary, 0xB5 0x62 sync, length + checksum framed)
 *
 * Both NMEA and UBX are emitted by the chip on the same UART; we demultiplex
 * each incoming byte: 0xB5 anywhere outside an NMEA line starts a UBX frame,
 * printable ASCII outside a UBX frame extends the NMEA line.
 *
 * Architecture: Blueprint 1 §3, Blueprint 5 §3, Blueprint 7
 */

#include "max_m10s.h"
#include "data_broker.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "cross_driver.h"
#include "pcf85063.h"       // pcf85063_sync_utc() -- auto-RTC-seed

static const char *TAG = "MAX_M10S";

// Shared (defined in boot_hw_init.c once that file lands).
extern SemaphoreHandle_t g_i2c_mutex;

// -- Module config ------------------------------------------------------------
#define MAX_M10S_POLL_MS      200    // Poll UART at 5 Hz; GPS emits at 1 Hz
#define NMEA_BUF_SIZE         128
#define UART_RX_BUF_SIZE      2048
#define BULK_READ_SIZE        512
#define UBX_MAX_PAYLOAD       128
#define NMEA_DEBUG_STALE_MS   5000U

// -- UBX sync bytes & class/id used today -------------------------------------
#define UBX_SYNC1             0xB5
#define UBX_SYNC2             0x62
#define UBX_CLASS_NAV         0x01
#define UBX_ID_NAV_TIMEUTC    0x21

// -- Internal state -----------------------------------------------------------
typedef struct {
    double         latitude;
    double         longitude;
    float          altitude;
    float          speed_kmh;
    float          course;
    gps_fix_type_t fix;
    uint8_t        sats_in_use;
    float          hdop;
    struct tm      time_utc;
    bool           time_valid;
    bool           position_valid;
    uint32_t       last_update_ms;
    uint32_t       last_nmea_ms;
} max_m10s_internal_t;

static max_m10s_internal_t        s_gps   = {0};
static max_m10s_debug_sentences_t s_debug = {0};
static char                       s_nmea_buf[NMEA_BUF_SIZE];
static uint8_t                    s_nmea_idx = 0;

static bool s_first_fix_sent = false;

// UBX state machine
typedef enum {
    UBX_SYNC_1 = 0,
    UBX_SYNC_2,
    UBX_CLASS,
    UBX_ID,
    UBX_LEN_LO,
    UBX_LEN_HI,
    UBX_PAYLOAD,
    UBX_CK_A,
    UBX_CK_B,
} ubx_state_t;

typedef struct {
    ubx_state_t state;
    uint8_t     cls;
    uint8_t     id;
    uint16_t    len;
    uint16_t    payload_idx;
    uint8_t     payload[UBX_MAX_PAYLOAD];
    uint8_t     ck_a;
    uint8_t     ck_b;
    uint8_t     calc_a;
    uint8_t     calc_b;
} ubx_parser_t;

static ubx_parser_t s_ubx = {0};

// 1PPS state (written from ISR -> read elsewhere)
static volatile uint32_t s_pps_count   = 0;
static volatile int64_t  s_pps_last_us = 0;
static bool              s_pps_installed = false;

// -- Identity -----------------------------------------------------------------
const char *max_m10s_get_chip_name(void) { return "MAX-M10S";        }
const char *max_m10s_get_chip_desc(void) { return "u-blox M10 GNSS"; }

// -- NMEA helpers (verbatim from tu10f.c) -------------------------------------
static double nmea_to_decimal(const char *coord, char dir)
{
    if (!coord || strlen(coord) < 4) return 0.0;
    int deg_digits = (dir == 'N' || dir == 'S') ? 2 : 3;
    char deg_s[4] = {0};
    strncpy(deg_s, coord, deg_digits);
    double degrees = atof(deg_s);
    double minutes = atof(coord + deg_digits);
    double result  = degrees + (minutes / 60.0);
    if (dir == 'S' || dir == 'W') result = -result;
    return result;
}

static bool parse_time_str(const char *s, struct tm *t)
{
    if (!s || strlen(s) < 6) return false;
    char b[3] = {0};
    strncpy(b, s,     2); t->tm_hour = atoi(b);
    strncpy(b, s + 2, 2); t->tm_min  = atoi(b);
    strncpy(b, s + 4, 2); t->tm_sec  = atoi(b);
    return (t->tm_hour < 24 && t->tm_min < 60 && t->tm_sec < 60);
}

static bool parse_date_str(const char *s, struct tm *t)
{
    if (!s || strlen(s) < 6) return false;
    char b[3] = {0};
    strncpy(b, s,     2); t->tm_mday = atoi(b);
    strncpy(b, s + 2, 2); t->tm_mon  = atoi(b) - 1;
    strncpy(b, s + 4, 2); t->tm_year = atoi(b) + 100;
    return true;
}

static void parse_gga(char *sentence)
{
    char *tok  = strtok(sentence, ",");
    int   field = 0;
    char quality_s[4] = {0}, sats_s[4] = {0}, hdop_s[8] = {0}, alt_s[12] = {0};

    while (tok && field <= 9) {
        switch (field) {
            case 6: strncpy(quality_s, tok, sizeof(quality_s) - 1); break;
            case 7: strncpy(sats_s,    tok, sizeof(sats_s)    - 1); break;
            case 8: strncpy(hdop_s,    tok, sizeof(hdop_s)    - 1); break;
            case 9: strncpy(alt_s,     tok, sizeof(alt_s)     - 1); break;
        }
        tok = strtok(NULL, ",");
        field++;
    }

    int quality = atoi(quality_s);
    s_gps.sats_in_use = (uint8_t)atoi(sats_s);
    s_gps.hdop        = atof(hdop_s);
    s_gps.altitude    = atof(alt_s);

    if (quality == 0) {
        s_gps.fix = GPS_FIX_NONE;
    } else {
        s_gps.fix = (s_gps.sats_in_use >= 4) ? GPS_FIX_3D : GPS_FIX_2D;
    }
}

static void parse_rmc(char *sentence)
{
    char *tok  = strtok(sentence, ",");
    int   field = 0;
    char time_s[16] = {0}, lat_s[12] = {0}, lon_s[12] = {0};
    char speed_s[12] = {0}, date_s[8] = {0};
    char status = 'V', lat_dir = 'N', lon_dir = 'E';

    while (tok && field <= 9) {
        switch (field) {
            case 1: strncpy(time_s,  tok, sizeof(time_s)  - 1); break;
            case 2: status  = tok[0]; break;
            case 3: strncpy(lat_s,   tok, sizeof(lat_s)   - 1); break;
            case 4: lat_dir = tok[0]; break;
            case 5: strncpy(lon_s,   tok, sizeof(lon_s)   - 1); break;
            case 6: lon_dir = tok[0]; break;
            case 7: strncpy(speed_s, tok, sizeof(speed_s) - 1); break;
            case 9: strncpy(date_s,  tok, sizeof(date_s)  - 1); break;
        }
        tok = strtok(NULL, ",");
        field++;
    }

    if (strlen(time_s) >= 6 && strlen(date_s) >= 6) {
        struct tm t = {0};
        if (parse_time_str(time_s, &t) && parse_date_str(date_s, &t)) {
            memcpy(&s_gps.time_utc, &t, sizeof(struct tm));
            s_gps.time_valid = true;
        }
    }

    if (status == 'A') {
        s_gps.latitude       = nmea_to_decimal(lat_s, lat_dir);
        s_gps.longitude      = nmea_to_decimal(lon_s, lon_dir);
        s_gps.speed_kmh      = atof(speed_s) * 1.852f;
        s_gps.position_valid = true;
    } else {
        s_gps.position_valid = false;
    }

    s_gps.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void process_nmea_sentence(char *raw)
{
    if (!raw || strlen(raw) < 6) return;
    s_gps.last_nmea_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (strstr(raw, "GGA")) {
        strncpy(s_debug.gga, raw, sizeof(s_debug.gga) - 1);
        s_debug.gga[sizeof(s_debug.gga) - 1] = '\0';
        parse_gga(raw);
    } else if (strstr(raw, "RMC")) {
        strncpy(s_debug.rmc, raw, sizeof(s_debug.rmc) - 1);
        s_debug.rmc[sizeof(s_debug.rmc) - 1] = '\0';
        parse_rmc(raw);
    }
}

// -- UBX-NAV-TIMEUTC parser ---------------------------------------------------
// Frame layout (u-blox UBX protocol spec, M10 receiver, message NAV-TIMEUTC):
//   header (6): B5 62 01 21 14 00
//   payload (20):
//     u32  iTOW    [0..3]
//     u32  tAcc    [4..7]
//     i32  nano    [8..11]
//     u16  year    [12..13]
//     u8   month   [14]
//     u8   day     [15]
//     u8   hour    [16]
//     u8   min     [17]
//     u8   sec     [18]
//     u8   valid   [19]    bit0=validTOW, bit1=validWKN, bit2=validUTC
//   checksum (2): ck_a ck_b   -- Fletcher-8 over class..end of payload
//
// We only commit the UTC fields when bit2 (validUTC) is set.
static void handle_ubx_nav_timeutc(const uint8_t *p, uint16_t len)
{
    if (len < 20) return;
    uint16_t year  = (uint16_t)p[12] | ((uint16_t)p[13] << 8);
    uint8_t  month = p[14];
    uint8_t  day   = p[15];
    uint8_t  hour  = p[16];
    uint8_t  mn    = p[17];
    uint8_t  sec   = p[18];
    uint8_t  flags = p[19];

    if (!(flags & 0x04)) return;   // validUTC bit
    if (year < 2025 || month == 0 || month > 12 || day == 0 || day > 31) return;

    s_gps.time_utc.tm_year = (int)year - 1900;
    s_gps.time_utc.tm_mon  = (int)month - 1;
    s_gps.time_utc.tm_mday = (int)day;
    s_gps.time_utc.tm_hour = (int)hour;
    s_gps.time_utc.tm_min  = (int)mn;
    s_gps.time_utc.tm_sec  = (int)sec;
    s_gps.time_valid       = true;
    s_gps.last_update_ms   = (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void ubx_reset(void)
{
    s_ubx.state       = UBX_SYNC_1;
    s_ubx.payload_idx = 0;
    s_ubx.len         = 0;
    s_ubx.calc_a      = 0;
    s_ubx.calc_b      = 0;
}

static inline void ubx_ck_add(uint8_t b)
{
    s_ubx.calc_a = (uint8_t)(s_ubx.calc_a + b);
    s_ubx.calc_b = (uint8_t)(s_ubx.calc_b + s_ubx.calc_a);
}

void max_m10s_feed_ubx_byte(uint8_t b)
{
    switch (s_ubx.state) {
    case UBX_SYNC_1:
        if (b == UBX_SYNC1) s_ubx.state = UBX_SYNC_2;
        break;
    case UBX_SYNC_2:
        if (b == UBX_SYNC2) { s_ubx.calc_a = 0; s_ubx.calc_b = 0; s_ubx.state = UBX_CLASS; }
        else                 { ubx_reset(); }
        break;
    case UBX_CLASS:
        s_ubx.cls = b; ubx_ck_add(b); s_ubx.state = UBX_ID; break;
    case UBX_ID:
        s_ubx.id  = b; ubx_ck_add(b); s_ubx.state = UBX_LEN_LO; break;
    case UBX_LEN_LO:
        s_ubx.len = b; ubx_ck_add(b); s_ubx.state = UBX_LEN_HI; break;
    case UBX_LEN_HI:
        s_ubx.len |= ((uint16_t)b << 8); ubx_ck_add(b);
        if (s_ubx.len > UBX_MAX_PAYLOAD) { ubx_reset(); }
        else if (s_ubx.len == 0)         { s_ubx.state = UBX_CK_A; }
        else                              { s_ubx.payload_idx = 0; s_ubx.state = UBX_PAYLOAD; }
        break;
    case UBX_PAYLOAD:
        s_ubx.payload[s_ubx.payload_idx++] = b;
        ubx_ck_add(b);
        if (s_ubx.payload_idx >= s_ubx.len) s_ubx.state = UBX_CK_A;
        break;
    case UBX_CK_A:
        s_ubx.ck_a = b; s_ubx.state = UBX_CK_B; break;
    case UBX_CK_B:
        s_ubx.ck_b = b;
        if (s_ubx.ck_a == s_ubx.calc_a && s_ubx.ck_b == s_ubx.calc_b) {
            if (s_ubx.cls == UBX_CLASS_NAV && s_ubx.id == UBX_ID_NAV_TIMEUTC) {
                handle_ubx_nav_timeutc(s_ubx.payload, s_ubx.len);
            }
        }
        ubx_reset();
        break;
    }
}

// -- 1PPS ISR -----------------------------------------------------------------
static void IRAM_ATTR pps_isr(void *arg)
{
    (void)arg;
    s_pps_count++;
    s_pps_last_us = esp_timer_get_time();
}

static esp_err_t pps_install(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << MAX_M10S_PPS_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,    // idle LOW between pulses
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) return ret;

    // gpio_install_isr_service may already be installed by another driver
    // (cst9217, lis3mdl). Calling it twice returns ESP_ERR_INVALID_STATE,
    // which is fine here -- ignore that specific error.
    esp_err_t svc = gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1);
    if (svc != ESP_OK && svc != ESP_ERR_INVALID_STATE) return svc;

    ret = gpio_isr_handler_add(MAX_M10S_PPS_PIN, pps_isr, NULL);
    if (ret != ESP_OK) return ret;
    s_pps_installed = true;
    return ESP_OK;
}

uint32_t max_m10s_get_pps_count(void)   { return s_pps_count;   }
int64_t  max_m10s_get_last_pps_us(void) { return s_pps_last_us; }

// -- Init ---------------------------------------------------------------------
esp_err_t max_m10s_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = MAX_M10S_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_LOGI(TAG, "Init UART%d TX=%d RX=%d 1PPS=%d @ %d baud",
             MAX_M10S_UART_NUM, MAX_M10S_TX_PIN, MAX_M10S_RX_PIN,
             MAX_M10S_PPS_PIN, MAX_M10S_BAUD_RATE);

    esp_err_t ret = uart_driver_install(MAX_M10S_UART_NUM, UART_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "UART install: %s", esp_err_to_name(ret)); return ret; }

    ret = uart_param_config(MAX_M10S_UART_NUM, &cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "UART param: %s", esp_err_to_name(ret)); return ret; }

    ret = uart_set_pin(MAX_M10S_UART_NUM, MAX_M10S_TX_PIN, MAX_M10S_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "UART pin: %s", esp_err_to_name(ret)); return ret; }

    gpio_pullup_en(MAX_M10S_RX_PIN);

    memset(&s_gps, 0, sizeof(s_gps));
    memset(&s_debug, 0, sizeof(s_debug));
    memset(&s_ubx, 0, sizeof(s_ubx));
    s_nmea_idx       = 0;
    s_first_fix_sent = false;
    s_pps_count      = 0;
    s_pps_last_us    = 0;

    ret = pps_install();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "1PPS ISR install failed (%s) -- UART path still functional",
                 esp_err_to_name(ret));
        // Non-fatal: GNSS still works without 1PPS, only RTC discipline is lost.
    }

    ESP_LOGI(TAG, "%s init OK", max_m10s_get_chip_name());
    return ESP_OK;
}

void max_m10s_deinit(void)
{
    if (s_pps_installed) {
        gpio_isr_handler_remove(MAX_M10S_PPS_PIN);
        s_pps_installed = false;
    }
    uart_driver_delete(MAX_M10S_UART_NUM);
    ESP_LOGI(TAG, "Deinit");
}

// -- Update (drain UART, demux NMEA + UBX) -----------------------------------
esp_err_t max_m10s_update(void)
{
    int avail = 0;
    uart_get_buffered_data_len(MAX_M10S_UART_NUM, (size_t *)&avail);

    bool got_any = false;
    uint8_t chunk[BULK_READ_SIZE];

    while (avail > 0) {
        int to_read = (avail > BULK_READ_SIZE) ? BULK_READ_SIZE : avail;
        int nread = uart_read_bytes(MAX_M10S_UART_NUM, chunk, to_read, pdMS_TO_TICKS(10));
        if (nread <= 0) break;
        avail -= nread;

        for (int i = 0; i < nread; i++) {
            uint8_t byte = chunk[i];

            // Demux: when the UBX state machine is mid-frame, route bytes to it.
            // Otherwise treat 0xB5 as the start of a UBX frame and printable
            // ASCII as NMEA. Non-printable bytes outside UBX reset the NMEA
            // line (matches the binary-junk filter from tu10f.c).
            if (s_ubx.state != UBX_SYNC_1) {
                max_m10s_feed_ubx_byte(byte);
                got_any = true;
                continue;
            }

            if (byte == UBX_SYNC1) {
                max_m10s_feed_ubx_byte(byte);
                continue;
            }

            if (byte == '\n' || byte == '\r') {
                if (s_nmea_idx > 0) {
                    s_nmea_buf[s_nmea_idx] = '\0';
                    process_nmea_sentence(s_nmea_buf);
                    s_nmea_idx = 0;
                    got_any = true;
                }
            } else if (byte >= 0x20 && byte <= 0x7E) {
                if (s_nmea_idx < NMEA_BUF_SIZE - 1) {
                    s_nmea_buf[s_nmea_idx++] = (char)byte;
                } else {
                    s_nmea_idx = 0;  // overflow -- drop the line
                }
            } else {
                if (s_nmea_idx > 0) s_nmea_idx = 0;
            }
        }
    }

    return got_any ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// -- Snapshot -----------------------------------------------------------------
void max_m10s_get_snapshot(broker_gps_data_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(broker_gps_data_t));

    out->latitude       = s_gps.latitude;
    out->longitude      = s_gps.longitude;
    out->altitude_m     = s_gps.altitude;
    out->speed_kmh      = s_gps.speed_kmh;
    out->course_deg     = s_gps.course;
    out->fix            = s_gps.fix;
    out->sats_in_use    = s_gps.sats_in_use;
    out->hdop           = s_gps.hdop;
    out->time_valid     = s_gps.time_valid;
    out->position_valid = s_gps.position_valid;

    if (s_gps.time_valid) {
        out->utc_hour   = (uint8_t) s_gps.time_utc.tm_hour;
        out->utc_minute = (uint8_t) s_gps.time_utc.tm_min;
        out->utc_second = (uint8_t) s_gps.time_utc.tm_sec;
        out->utc_day    = (uint8_t) s_gps.time_utc.tm_mday;
        out->utc_month  = (uint8_t)(s_gps.time_utc.tm_mon + 1);
        out->utc_year   = (uint16_t)(s_gps.time_utc.tm_year + 1900);
    }

    if (s_gps.fix != GPS_FIX_NONE && !s_first_fix_sent) {
        out->first_fix_notified = true;
        s_first_fix_sent = true;
    }
}

bool max_m10s_has_fix(void)
{
    return s_gps.position_valid && s_gps.fix != GPS_FIX_NONE;
}

void max_m10s_flush(void)
{
    uart_flush_input(MAX_M10S_UART_NUM);
    s_nmea_idx = 0;
    ubx_reset();
}

void max_m10s_get_debug_sentences(char *gga_buf, size_t gga_sz,
                                   char *rmc_buf, size_t rmc_sz)
{
    uint32_t now    = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t age_ms = now - s_gps.last_nmea_ms;
    bool     fresh  = (s_gps.last_nmea_ms > 0) && (age_ms < NMEA_DEBUG_STALE_MS);

    if (gga_buf && gga_sz > 0) {
        if (fresh) { strncpy(gga_buf, s_debug.gga, gga_sz - 1); gga_buf[gga_sz - 1] = '\0'; }
        else       { gga_buf[0] = '\0'; }
    }
    if (rmc_buf && rmc_sz > 0) {
        if (fresh) { strncpy(rmc_buf, s_debug.rmc, rmc_sz - 1); rmc_buf[rmc_sz - 1] = '\0'; }
        else       { rmc_buf[0] = '\0'; }
    }
}

esp_err_t max_m10s_get_utc_time(uint16_t *year, uint8_t *month, uint8_t *day,
                                 uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    if (!s_gps.time_valid) return ESP_ERR_INVALID_STATE;
    if (year)   *year   = (uint16_t)(s_gps.time_utc.tm_year + 1900);
    if (month)  *month  = (uint8_t) (s_gps.time_utc.tm_mon  + 1);
    if (day)    *day    = (uint8_t)  s_gps.time_utc.tm_mday;
    if (hour)   *hour   = (uint8_t)  s_gps.time_utc.tm_hour;
    if (minute) *minute = (uint8_t)  s_gps.time_utc.tm_min;
    if (second) *second = (uint8_t)  s_gps.time_utc.tm_sec;
    return ESP_OK;
}

// -- Task ---------------------------------------------------------------------
void task_gps_fn(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(MAX_M10S_POLL_MS);
    TickType_t       last   = xTaskGetTickCount();

    ESP_LOGI(TAG, "Task started on Core %d", xPortGetCoreID());

    static bool s_had_fix = false;

    while (1) {
        if (!broker_gps_hw_alive() || !broker_gps_get_enabled()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        esp_err_t ret = max_m10s_update();

        if (ret == ESP_OK) {
            broker_gps_data_t bd = {0};
            max_m10s_get_snapshot(&bd);
            bd.enabled = broker_gps_get_enabled();
            broker_gps_write(&bd);

            if (bd.time_valid && !g_gps_time_seeded) {
                cross_driver_fire(XD_EVENT_GPS_TIME_VALID, &bd);
            }
            if (bd.position_valid) {
                cross_driver_fire(XD_EVENT_GPS_FIX_VALID, &bd);
            }
            if (s_had_fix && !bd.position_valid) {
                cross_driver_fire(XD_EVENT_GPS_FIX_LOST, NULL);
            }
            s_had_fix = bd.position_valid;

            // Seed ESP32 system clock + RTC from UBX-TIMEUTC (preferred) or
            // NMEA RMC (fallback). UBX flips time_valid before the position
            // fix arrives, so this fires earlier than the old NMEA-only path.
            if (bd.time_valid && !g_gps_time_seeded) {
                struct tm utc_tm = {
                    .tm_sec  = bd.utc_second,
                    .tm_min  = bd.utc_minute,
                    .tm_hour = bd.utc_hour,
                    .tm_mday = bd.utc_day,
                    .tm_mon  = bd.utc_month - 1,
                    .tm_year = bd.utc_year - 1900,
                };
                setenv("TZ", "UTC0", 1);
                tzset();
                time_t epoch = timegm(&utc_tm);
                struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
                settimeofday(&tv, NULL);

                if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    pcf85063_sync_utc(I2C_NUM_0,
                                      bd.utc_hour, bd.utc_minute, bd.utc_second,
                                      bd.utc_day,  bd.utc_month,  bd.utc_year);
                    xSemaphoreGive(g_i2c_mutex);
                    ESP_LOGI(TAG, "RTC auto-seeded from GNSS UTC");
                } else {
                    ESP_LOGW(TAG, "RTC auto-seed: I2C mutex timeout");
                }

                g_gps_time_seeded = true;
                ESP_LOGW(TAG, "System clock seeded from %s: "
                              "%04u-%02u-%02u %02u:%02u:%02u UTC",
                         bd.position_valid ? "NMEA RMC" : "UBX-NAV-TIMEUTC",
                         bd.utc_year, bd.utc_month, bd.utc_day,
                         bd.utc_hour, bd.utc_minute, bd.utc_second);
            }
        }

        vTaskDelayUntil(&last, period);
    }
}
