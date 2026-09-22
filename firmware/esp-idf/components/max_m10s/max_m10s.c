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
#include "ui_subjects.h"       // Stage 31.4: g_gps_q for UI drain path
#include "driver/gpio.h"
#include "driver/rtc_io.h"     // rtc_gpio_hold_dis / _deinit / _is_valid_gpio
#include "esp_rom_gpio.h"      // esp_rom_gpio_connect_out_signal (manual TX route)
#include "soc/gpio_sig_map.h"  // U1TXD_OUT_IDX / U1RXD_IN_IDX
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

// UI-driven "resync PCF85063 from next GPS PPS" request flag. Set by
// gps_tile.c's ATOMIC SYNC button, read (eventually) by task_gps_fn. Task is
// held out today because the GPS module is offline on iv7.1 -- flag is present
// so the tile links and stays UI-testable in force mode.
volatile bool g_gps_sync_requested = false;

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
#define UBX_CLASS_MON         0x0A
#define UBX_CLASS_ACK         0x05
#define UBX_ID_NAV_TIMEUTC    0x21
#define UBX_ID_MON_RF         0x38
#define UBX_ID_ACK_ACK        0x01
#define UBX_ID_ACK_NAK        0x00

// -- Stage 31.4b: CFG-VALSET key IDs (from M10 Interface Description) ---------
#define UBX_KEY_NAVSPG_DYNMODEL          0x20110021u
#define UBX_KEY_MSGOUT_UBX_MON_RF_UART1  0x2091035au
#define UBX_KEY_MSGOUT_UBX_NAV_PVT_UART1 0x20910007u
#define UBX_LAYER_RAM  0x01

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

// ─── NMEA GSV parser -> rolling SNR history for signal-strength bar ─────────
// GSV format:
//   $--GSV,total_sentences,sentence_num,sats_in_view,
//         {prn,elev,azim,snr,}[up to 4 per sentence]*checksum
// SNR is CN0 in dBHz (0..99). Multiple constellations (GP/GA/GB/GQ) each
// emit their own GSV cycle every second; we merge across all of them.
//
// Storage: rolling buffer of the most recent SNR observations with a
// timestamp. Anything older than SNR_MAX_AGE_MS is ignored when computing
// the summary, so a sat that drops out stops contributing within ~2.5s.
#define SNR_HISTORY_SIZE  40
#define SNR_MAX_AGE_MS    2500U

static struct { uint8_t snr; uint32_t rx_ms; } s_snr_hist[SNR_HISTORY_SIZE];
static uint8_t s_snr_hist_idx = 0;

static void snr_push(uint8_t snr)
{
    s_snr_hist[s_snr_hist_idx].snr   = snr;
    s_snr_hist[s_snr_hist_idx].rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_snr_hist_idx = (uint8_t)((s_snr_hist_idx + 1) % SNR_HISTORY_SIZE);
}

static void parse_gsv(char *sentence)
{
    // strtok mutates -- caller must be OK with that (matches parse_gga/rmc).
    // We only care about fields 4, 8, 12, 16 (snr for each of up to 4 sats).
    // Field indices (0-based): 0='$--GSV', 1=total, 2=num, 3=sats_in_view,
    //   4=prn1, 5=elev1, 6=azim1, 7=snr1,
    //   8=prn2, 9=elev2, 10=azim2, 11=snr2, ...
    char *tok  = strtok(sentence, ",");
    int   field = 0;
    while (tok && field < 20) {
        // snr fields are at positions 7, 11, 15, 19 (every 4th from 7)
        if (field >= 7 && ((field - 7) % 4) == 0) {
            // strip optional trailing "*CS" checksum (only on last snr in
            // a sentence with fewer than 4 sats -- but strtok on '*' would
            // complicate; atoi stops at non-digit so we're fine).
            uint8_t snr = (uint8_t)atoi(tok);
            if (snr > 0 && snr < 100) snr_push(snr);
        }
        tok = strtok(NULL, ",");
        field++;
    }
}

void max_m10s_get_snr_summary(max_m10s_snr_summary_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint8_t  fresh[SNR_HISTORY_SIZE];
    uint8_t  nfresh = 0;

    for (uint8_t i = 0; i < SNR_HISTORY_SIZE; i++) {
        uint8_t snr = s_snr_hist[i].snr;
        if (snr == 0) continue;
        if ((now - s_snr_hist[i].rx_ms) > SNR_MAX_AGE_MS) continue;
        fresh[nfresh++] = snr;
        if (snr > out->max_cn0) out->max_cn0 = snr;
    }
    out->sats_with_snr = nfresh;
    out->sats_in_view  = nfresh;   // best proxy we have from GSV alone
    out->last_update_ms = now;

    // Top-4 average.
    if (nfresh == 0) return;
    // Simple selection: repeatedly find max, sum, remove.
    uint32_t sum = 0;
    uint8_t  taken = 0;
    for (uint8_t k = 0; k < 4 && k < nfresh; k++) {
        uint8_t best = 0, best_i = 0;
        for (uint8_t i = 0; i < nfresh; i++) {
            if (fresh[i] > best) { best = fresh[i]; best_i = i; }
        }
        if (best == 0) break;
        sum += best;
        taken++;
        fresh[best_i] = 0;
    }
    if (taken) out->top4_avg_cn0 = (uint8_t)(sum / taken);
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
    } else if (strstr(raw, "GSV")) {
        // Working buffer so we don't strtok-mutate s_debug/state.
        char tmp[NMEA_BUF_SIZE];
        strncpy(tmp, raw, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        parse_gsv(tmp);
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

// ─── Stage 31.4b: UBX-MON-RF (0x0A 0x38) parser + snapshot ─────────────────
// Frame: 4 B header (version, nBlocks, reserved0[2]) + nBlocks * 24 B blocks.
// Per manual, block offsets: 0 blockId, 1 flags, 2 antStatus, 3 antPower,
// 4 postStatus U4, 8 reserved1 U1[4], 12 noisePerMS U2, 14 agcCnt U2.
// We only look at block 0 (single-RF-block modules like MAX-M10S).
static max_m10s_monrf_t s_monrf = {0};
static SemaphoreHandle_t s_monrf_lock = NULL;

static void handle_ubx_mon_rf(const uint8_t *p, uint16_t len)
{
    if (len < 4 + 24) return;
    uint8_t nBlocks = p[1];
    if (nBlocks < 1) return;
    const uint8_t *blk = p + 4;   // first block starts here
    uint8_t  ant_status   = blk[2];
    uint8_t  ant_power    = blk[3];
    uint16_t noise_per_ms = (uint16_t)blk[12] | ((uint16_t)blk[13] << 8);
    uint16_t agc_cnt      = (uint16_t)blk[14] | ((uint16_t)blk[15] << 8);

    if (s_monrf_lock && xSemaphoreTake(s_monrf_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_monrf.valid          = true;
        s_monrf.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        s_monrf.ant_status     = ant_status;
        s_monrf.ant_power      = ant_power;
        s_monrf.noise_per_ms   = noise_per_ms;
        s_monrf.agc_cnt        = agc_cnt;
        xSemaphoreGive(s_monrf_lock);
    }
    ESP_LOGD(TAG, "MON-RF: ant=%s(%u) pwr=%u noise=%u agc=%u",
             max_m10s_ant_status_name(ant_status), ant_status,
             ant_power, noise_per_ms, agc_cnt);
}

void max_m10s_get_monrf(max_m10s_monrf_t *out)
{
    if (!out) return;
    if (s_monrf_lock && xSemaphoreTake(s_monrf_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        *out = s_monrf;
        xSemaphoreGive(s_monrf_lock);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

const char *max_m10s_ant_status_name(uint8_t ant_status)
{
    switch (ant_status) {
        case 0: return "INIT";
        case 1: return "DONTKNOW";
        case 2: return "OK";
        case 3: return "SHORT";
        case 4: return "OPEN";
        default: return "?";
    }
}

// ─── Stage 31.4b: UBX-CFG-VALSET frame builder ─────────────────────────────
// Fletcher-8 checksum over CLASS+ID+LEN_lo+LEN_hi+payload bytes.
static void ubx_fletcher(const uint8_t *buf, size_t n, uint8_t *ck_a, uint8_t *ck_b)
{
    uint8_t a = 0, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (uint8_t)(a + buf[i]);
        b = (uint8_t)(b + a);
    }
    *ck_a = a; *ck_b = b;
}

// Build + send a single-key VALSET (U1/E1 value). Buffered UART write.
static esp_err_t ubx_send_valset_u1(uint8_t layers, uint32_t key_id, uint8_t value)
{
    // Payload: version(1) + layers(1) + reserved0(2) + keyID(4) + value(1) = 9 B
    uint8_t frame[6 + 9 + 2];
    frame[0] = UBX_SYNC1;
    frame[1] = UBX_SYNC2;
    frame[2] = 0x06;                 // CLASS = CFG
    frame[3] = 0x8A;                 // ID    = VALSET
    frame[4] = 9;                    // LEN lo
    frame[5] = 0;                    // LEN hi
    frame[6] = 0x00;                 // version
    frame[7] = layers;               // layers bitmask
    frame[8] = 0x00;                 // reserved0[0]
    frame[9] = 0x00;                 // reserved0[1]
    frame[10] = (uint8_t)(key_id >>  0);
    frame[11] = (uint8_t)(key_id >>  8);
    frame[12] = (uint8_t)(key_id >> 16);
    frame[13] = (uint8_t)(key_id >> 24);
    frame[14] = value;
    ubx_fletcher(&frame[2], 4 + 9, &frame[15], &frame[16]);   // CLASS..value

    int w = uart_write_bytes(MAX_M10S_UART_NUM, frame, sizeof(frame));
    if (w != (int)sizeof(frame)) {
        ESP_LOGW(TAG, "ubx_send_valset_u1: uart_write short (%d/%zu)",
                 w, sizeof(frame));
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "ubx_send_valset_u1: key=0x%08lx val=%u layers=0x%02x",
             (unsigned long)key_id, value, layers);
    return ESP_OK;
}

// -- Public API ---------------------------------------------------------------

static max_m10s_dynmodel_t s_dynmodel_last = UBX_DYNMODEL_PORTABLE;   // chip default

const char *max_m10s_dynmodel_name(max_m10s_dynmodel_t mode)
{
    switch (mode) {
        case UBX_DYNMODEL_PORTABLE:   return "PORTABLE";
        case UBX_DYNMODEL_STATIONARY: return "STATIONARY";
        case UBX_DYNMODEL_PEDESTRIAN: return "PEDESTRIAN";
        case UBX_DYNMODEL_AUTOMOTIVE: return "AUTOMOTIVE";
        case UBX_DYNMODEL_SEA:        return "SEA";
        case UBX_DYNMODEL_AIR1G:      return "AIR<1G";
        case UBX_DYNMODEL_AIR2G:      return "AIR<2G";
        case UBX_DYNMODEL_AIR4G:      return "AIR<4G";
        case UBX_DYNMODEL_WRIST:      return "WRIST";
        case UBX_DYNMODEL_BIKE:       return "BIKE";
        default:                      return "?";
    }
}

esp_err_t max_m10s_set_dynmodel(max_m10s_dynmodel_t mode)
{
    esp_err_t r = ubx_send_valset_u1(UBX_LAYER_RAM, UBX_KEY_NAVSPG_DYNMODEL,
                                     (uint8_t)mode);
    if (r == ESP_OK) {
        s_dynmodel_last = mode;
        ESP_LOGI(TAG, "DYNMODEL -> %s (%u) queued to UART",
                 max_m10s_dynmodel_name(mode), mode);
    }
    return r;
}

max_m10s_dynmodel_t max_m10s_get_dynmodel(void) { return s_dynmodel_last; }

esp_err_t max_m10s_enable_monrf(uint8_t rate)
{
    esp_err_t r = ubx_send_valset_u1(UBX_LAYER_RAM,
                                     UBX_KEY_MSGOUT_UBX_MON_RF_UART1, rate);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "MON-RF output rate -> %u", rate);
    }
    return r;
}

// -- UBX diagnostics (TX-alive probe + per-class receive counters) -----------
static max_m10s_ubx_counters_t s_ubx_ctr = {0};

void max_m10s_get_ubx_counters(max_m10s_ubx_counters_t *out)
{
    if (!out) return;
    *out = s_ubx_ctr;
}

// UBX-MON-VER poll: 6-byte header only, no payload. Chip always answers
// if it's alive and hears the request. Use to prove ESP->GPS TX works.
esp_err_t max_m10s_ping(void)
{
    static const uint8_t frame[8] = {
        0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00,
        0x0E, 0x34,   // pre-computed Fletcher for (0A 04 00 00)
    };
    int w = uart_write_bytes(MAX_M10S_UART_NUM, frame, sizeof(frame));
    if (w != (int)sizeof(frame)) {
        ESP_LOGW(TAG, "ping: uart_write short (%d/%zu)", w, sizeof(frame));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PING -> UBX-MON-VER poll queued");
    return ESP_OK;
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
            s_ubx_ctr.total++;
            if (s_ubx.cls == UBX_CLASS_NAV && s_ubx.id == UBX_ID_NAV_TIMEUTC) {
                s_ubx_ctr.nav_timeutc++;
                handle_ubx_nav_timeutc(s_ubx.payload, s_ubx.len);
            }
            else if (s_ubx.cls == UBX_CLASS_MON && s_ubx.id == UBX_ID_MON_RF) {
                s_ubx_ctr.mon_rf++;
                handle_ubx_mon_rf(s_ubx.payload, s_ubx.len);
            }
            else if (s_ubx.cls == UBX_CLASS_MON && s_ubx.id == 0x04) {
                // UBX-MON-VER: response to PING. Log SW/HW strings if long
                // enough, so bench can see chip identity + confirm TX path.
                s_ubx_ctr.mon_ver++;
                if (s_ubx.len >= 40) {
                    char sw[31] = {0}, hw[11] = {0};
                    memcpy(sw, s_ubx.payload,      30);
                    memcpy(hw, s_ubx.payload + 30, 10);
                    ESP_LOGI(TAG, "MON-VER SW='%s' HW='%s' (TX path OK)", sw, hw);
                } else {
                    ESP_LOGI(TAG, "MON-VER (short, %u B) received", s_ubx.len);
                }
            }
            else if (s_ubx.cls == UBX_CLASS_ACK && s_ubx.len >= 2) {
                bool ack = (s_ubx.id == UBX_ID_ACK_ACK);
                if (ack) s_ubx_ctr.ack_ack++; else s_ubx_ctr.ack_nak++;
                // Log ACK/NAK so bench sees whether the chip accepted our
                // CFG-VALSET frames. payload[0] = ack'd class, [1] = ack'd id.
                ESP_LOGD(TAG, "UBX-ACK-%s cls=0x%02x id=0x%02x",
                         ack ? "ACK" : "NAK",
                         s_ubx.payload[0], s_ubx.payload[1]);
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
    // Clear every latching mechanism that could hold either GPS pin in a
    // stale state from a prior firmware image (digital hold, RTC hold,
    // deep-sleep hold, RTC-IO subsystem attachment). Cheap belt-and-
    // suspenders: without this, on WROOMs that had older IDF fw the pin
    // can look electrically fine on a scope but stay disconnected from
    // the UART matrix. Diagnosed 2026-09-20 via Arduino sketch 18c.
    const gpio_num_t gps_pins[2] = { MAX_M10S_TX_PIN, MAX_M10S_RX_PIN };
    gpio_deep_sleep_hold_dis();
    for (int i = 0; i < 2; i++) {
        gpio_num_t g = gps_pins[i];
        gpio_hold_dis(g);
        if (rtc_gpio_is_valid_gpio(g)) {
            rtc_gpio_hold_dis(g);
            rtc_gpio_deinit(g);
        }
        gpio_reset_pin(g);
    }

    // Explicit APB clock source -- matches the bench-verified sketch.
    // UART_SCLK_DEFAULT on ESP32-S3 usually resolves to APB too, but
    // pinning it removes ambiguity and any future IDF default change.
    uart_config_t cfg = {
        .baud_rate  = MAX_M10S_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_LOGI(TAG, "Init UART%d TX=GPIO%d RX=GPIO%d 1PPS=GPIO%d @ %d baud, clk=APB",
             MAX_M10S_UART_NUM, MAX_M10S_TX_PIN, MAX_M10S_RX_PIN,
             MAX_M10S_PPS_PIN, MAX_M10S_BAUD_RATE);

    // IDF v5.x order: install -> param_config -> set_pin. Calling
    // param_config/set_pin before install worked in the Arduino sketch
    // (IDF v4.4) but on v5.x install can reset peripheral state, silently
    // losing the earlier config -- symptom is RX-works / TX-queues-but-
    // never-clocks-out. Diagnosed 2026-09-21 vs. sketch 18c on same pins.
    esp_err_t ret = uart_driver_install(MAX_M10S_UART_NUM, UART_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "UART install: %s", esp_err_to_name(ret)); return ret; }

    ret = uart_param_config(MAX_M10S_UART_NUM, &cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "UART param: %s", esp_err_to_name(ret)); return ret; }

    ret = uart_set_pin(MAX_M10S_UART_NUM, MAX_M10S_TX_PIN, MAX_M10S_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "UART pin: %s", esp_err_to_name(ret)); return ret; }

    // Belt-and-suspenders: on IDF v5.5, uart_set_pin for a non-IOMUX pin
    // (GPIO7 is not U1TXD's IOMUX default -- only GPIO17 is) can silently
    // fail to route the output signal, leaving TX bytes queued but never
    // clocked out on the pin. Force it by hand:
    //   1) select GPIO function on the pin (undo any prior peripheral IOMUX)
    //   2) enable output + idle-high (UART idle state)
    //   3) route U1TXD_OUT_IDX through the matrix to this pin
    //   4) same for RX side (input) for symmetry
    gpio_num_t tx_pin = (gpio_num_t)MAX_M10S_TX_PIN;
    gpio_num_t rx_pin = (gpio_num_t)MAX_M10S_RX_PIN;
    esp_rom_gpio_pad_select_gpio(tx_pin);
    gpio_set_direction(tx_pin, GPIO_MODE_OUTPUT);
    gpio_set_level    (tx_pin, 1);
    esp_rom_gpio_connect_out_signal(tx_pin, U1TXD_OUT_IDX, false, false);

    esp_rom_gpio_pad_select_gpio(rx_pin);
    gpio_set_direction(rx_pin, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_in_signal (rx_pin, U1RXD_IN_IDX,  false);
    ESP_LOGI(TAG, "GPIO matrix forced: U1TXD->GPIO%d, U1RXD<-GPIO%d",
             MAX_M10S_TX_PIN, MAX_M10S_RX_PIN);

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

    // Stage 31.4b: MON-RF snapshot mutex + queue config sends to the chip.
    // Chip startup is fast per §2.1.1 of the Integration Manual; brief delay
    // gives it a beat to finish its own boot before we hit it with CFG.
    if (s_monrf_lock == NULL) s_monrf_lock = xSemaphoreCreateMutex();
    vTaskDelay(pdMS_TO_TICKS(200));

    // Dynamic model: PEDESTRIAN is the sanest default for a wrist device
    // (WRIST enum value 9 is not guaranteed on M10 -- start conservative,
    // Ivan can flip via GPS_DYNMODEL CLI verb once bench-verified).
    (void)max_m10s_set_dynmodel(UBX_DYNMODEL_PEDESTRIAN);

    // Enable UBX-MON-RF at 1 Hz -- primary "why no fix" diagnostic
    // (antenna status + noise + AGC).
    (void)max_m10s_enable_monrf(1);

    ESP_LOGI(TAG, "%s init OK (dynmodel=PED, MON-RF on)",
             max_m10s_get_chip_name());
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
        if (!broker_gps_hw_alive()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!broker_gps_get_enabled()) {
            // Stage 31.4: push a "disabled" snapshot so the UI tile
            // reflects the state transition without waiting for a
            // real sample (which will never arrive while disabled).
            if (g_gps_q) {
                broker_gps_data_t bd = {0};
                max_m10s_get_snapshot(&bd);
                bd.enabled = false;
                (void)xQueueOverwrite(g_gps_q, &bd);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        esp_err_t ret = max_m10s_update();

        if (ret == ESP_OK) {
            broker_gps_data_t bd = {0};
            max_m10s_get_snapshot(&bd);
            bd.enabled = broker_gps_get_enabled();
            broker_gps_write(&bd);

            // Stage 31.4: publish to UI drain queue in addition to the
            // broker write above. Overwrite semantics; ui_subjects drain
            // formats + pushes into subjects on the LVGL task every 200 ms.
            if (g_gps_q) {
                (void)xQueueOverwrite(g_gps_q, &bd);
            }

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
                // ESP-IDF newlib does not expose timegm(). With TZ=UTC0
                // set + tzset() called above, local-time == UTC, so
                // mktime() yields the same epoch value as timegm() would.
                time_t epoch = mktime(&utc_tm);
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
