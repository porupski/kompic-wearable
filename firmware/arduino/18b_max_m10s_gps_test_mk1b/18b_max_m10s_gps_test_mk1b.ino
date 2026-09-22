/*
 * 18b_max_m10s_gps_test_mk1b -- MAX-M10S liveness diagnostic on Mk1b (ESP32-S3).
 *
 * Direct port of firmware/arduino/19_max_m10s_gps_C3_TESTER_ (which is
 * bench-proven alive on the same chip), targeted at the Mk1b PCB pinout.
 * No BQ25619, no ship-mode, no PPS ISR, no I2C, no Wire -- just UART + a
 * CLI that pokes the chip.
 *
 * Why 18b: the original 18 sketch had:
 *   (a) ESP TX/RX #defines swapped vs. the PCB (contradicted its own header
 *       comment AND the working ESP-IDF driver at max_m10s.h:57-58)
 *   (b) BQ25619 + Wire1 code that doesn't compile on the C3 core Ivan had
 *       selected and adds nothing to the "is the chip alive?" question.
 *
 * ── Wiring (per firmware/esp-idf/components/max_m10s/max_m10s.h) ─────
 *   ESP GPIO17 -> MAX-M10S RXD (module pin 3)   ESP TX
 *   ESP GPIO18 <- MAX-M10S TXD (module pin 2)   ESP RX
 *   3V3         -> VCC + V_IO;  GND -> GND
 *   EXTINT / V_BCKP / RESET_N / LNA_EN / VIO_SEL / SDA / SCL / SAFEBOOT_N:
 *   ALL FLOAT (per project_max_m10s_wiring + datasheet §3.2.3)
 *
 * ── Expected output ──────────────────────────────────────────────────
 *   [SEND] UBX cls=0x0A id=0x04 len=0  ck=0E 34
 *   [UBX ] cls=0x0A id=0x04 len=190  OK  ...
 *          SW='ROM SPG 5.10 (7b202e)'  HW='000A0000'
 *   [NMEA] $GNRMC,,V,,,,,,,,,,N,V*37
 *   [NMEA] $GNGGA,,,,,,0,00,99.99,,,,,,*56
 *   [STAT] total=~500 B  window=~290 B/s ...
 *
 * ── CLI (type + Enter in Serial monitor at 115200) ───────────────────
 *   HELP                        list commands
 *   PING                        UBX-MON-VER poll (fw/hw string)
 *   HW                          UBX-MON-HW  poll (antenna, RTC)
 *   MONRF                       UBX-MON-RF  poll (noise, AGC, jam)
 *   NAVPVT                      UBX-NAV-PVT poll (fix telemetry)
 *   RESET WARM|COLD|HOT         UBX-CFG-RST
 *   BAUD <n>                    reopen UART @ <n> (9600 / 38400 / 115200)
 *   SEND <cls> <id> <hex...>    raw UBX (auto-checksum)
 *   HEX                         toggle raw hex dump vs NMEA/UBX
 *   STATS                       counters
 *   CLEAR                       zero counters
 *
 * ── Arduino IDE settings ─────────────────────────────────────────────
 *   Board:            ESP32S3 Dev Module
 *   Flash Size:       16MB (128Mb)
 *   PSRAM:            OPI PSRAM
 *   USB CDC On Boot:  Enabled
 *
 *   (If you want to run this on a C3 SuperMini for A/B, flip GPS_RX_PIN=4,
 *   GPS_TX_PIN=5 and select ESP32C3 Dev Module. Nothing else changes.)
 */

#include <Arduino.h>

// ════════════════════════════════════════════════════════════════════
//  Config -- Mk1b iv8.0 (ESP32-S3-WROOM-1U-N16R8)
// ════════════════════════════════════════════════════════════════════
#define GPS_TX_PIN       18    // ESP TX -> MAX-M10S RXD (pin 3)
#define GPS_RX_PIN       17    // ESP RX <- MAX-M10S TXD (pin 2) pin 17 has the squarewave!
#define GPS_BAUD       9600    // MAX-M10S UART default
#define STATS_PERIOD_MS 1000
#define HEARTBEAT_MS    5000

HardwareSerial GPS(1);         // UART1

// ════════════════════════════════════════════════════════════════════
//  Counters
// ════════════════════════════════════════════════════════════════════
static uint32_t g_bytes_total   = 0;
static uint32_t g_bytes_window  = 0;
static uint32_t g_nmea_count    = 0;
static uint32_t g_ubx_count     = 0;
static uint32_t g_err_count     = 0;

static uint32_t g_last_byte_ms  = 0;
static uint32_t g_last_stats_ms = 0;
static uint32_t g_last_hb_ms    = 0;
static bool     g_hex_mode      = false;
static uint32_t g_baud          = GPS_BAUD;

// ════════════════════════════════════════════════════════════════════
//  NMEA + UBX parser state
// ════════════════════════════════════════════════════════════════════
static char nmea_buf[128];
static int  nmea_len = 0;
static bool nmea_active = false;

enum ubx_state_t {
    U_WAIT_B5, U_WAIT_62, U_CLS, U_ID, U_LEN_LO, U_LEN_HI, U_PAY, U_CK_A, U_CK_B
};
static ubx_state_t ubx_st = U_WAIT_B5;
static uint8_t  ubx_cls, ubx_id, ubx_ck_a, ubx_ck_b;
static uint16_t ubx_len, ubx_idx;
static uint8_t  ubx_pay[256];

// ════════════════════════════════════════════════════════════════════
//  UBX helpers
// ════════════════════════════════════════════════════════════════════
static void ubx_checksum(const uint8_t *buf, size_t n, uint8_t &a, uint8_t &b)
{
    a = 0; b = 0;
    for (size_t i = 0; i < n; i++) { a += buf[i]; b += a; }
}

static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *pay, uint16_t payload_len)
{
    uint8_t hdr[4] = { cls, id,
                       (uint8_t)(payload_len & 0xFF),
                       (uint8_t)((payload_len >> 8) & 0xFF) };
    uint8_t buf[256 + 4];
    memcpy(buf, hdr, 4);
    if (payload_len && pay) memcpy(buf + 4, pay, payload_len);
    uint8_t a, b;
    ubx_checksum(buf, 4 + payload_len, a, b);

    GPS.write((uint8_t)0xB5);
    GPS.write((uint8_t)0x62);
    GPS.write(hdr, 4);
    if (payload_len && pay) GPS.write(pay, payload_len);
    GPS.write(a);
    GPS.write(b);
    GPS.flush();

    Serial.printf("[SEND] UBX cls=0x%02X id=0x%02X len=%u  ck=%02X %02X\n",
                  cls, id, payload_len, a, b);
}

// ════════════════════════════════════════════════════════════════════
//  Input parser
// ════════════════════════════════════════════════════════════════════
static void classify_byte(uint8_t b)
{
    if (g_hex_mode) {
        Serial.printf("%02X ", b);
        if ((g_bytes_total & 15) == 15) Serial.println();
        return;
    }

    switch (ubx_st) {
        case U_WAIT_B5:
            if (b == 0xB5) { ubx_st = U_WAIT_62; return; }
            break;
        case U_WAIT_62:
            if (b == 0x62) { ubx_st = U_CLS; return; }
            ubx_st = U_WAIT_B5;
            if (b == 0xB5) { ubx_st = U_WAIT_62; return; }
            break;
        case U_CLS:    ubx_cls = b; ubx_st = U_ID;      return;
        case U_ID:     ubx_id  = b; ubx_st = U_LEN_LO;  return;
        case U_LEN_LO: ubx_len = b; ubx_st = U_LEN_HI;  return;
        case U_LEN_HI:
            ubx_len |= ((uint16_t)b << 8);
            ubx_idx  = 0;
            if (ubx_len > sizeof(ubx_pay)) {
                Serial.printf("[UBX ] len=%u > %u -- dropping\n",
                              ubx_len, (unsigned)sizeof(ubx_pay));
                g_err_count++;
                ubx_st = U_WAIT_B5;
                return;
            }
            ubx_st = (ubx_len == 0) ? U_CK_A : U_PAY;
            return;
        case U_PAY:
            ubx_pay[ubx_idx++] = b;
            if (ubx_idx >= ubx_len) ubx_st = U_CK_A;
            return;
        case U_CK_A: ubx_ck_a = b; ubx_st = U_CK_B; return;
        case U_CK_B: {
            ubx_ck_b = b;
            uint8_t hdr[4] = { ubx_cls, ubx_id,
                               (uint8_t)(ubx_len & 0xFF),
                               (uint8_t)((ubx_len >> 8) & 0xFF) };
            uint8_t buf[256 + 4];
            memcpy(buf, hdr, 4);
            if (ubx_len) memcpy(buf + 4, ubx_pay, ubx_len);
            uint8_t a, bb;
            ubx_checksum(buf, 4 + ubx_len, a, bb);
            bool ok = (a == ubx_ck_a && bb == ubx_ck_b);
            g_ubx_count++;
            if (!ok) g_err_count++;
            Serial.printf("[UBX ] cls=0x%02X id=0x%02X len=%u  %s",
                          ubx_cls, ubx_id, ubx_len, ok ? "OK" : "BADCK");
            if (ubx_len) {
                Serial.print("  ");
                uint16_t n = ubx_len > 32 ? 32 : ubx_len;
                for (uint16_t i = 0; i < n; i++) Serial.printf("%02X ", ubx_pay[i]);
                if (ubx_len > 32) Serial.print("...");
            }
            if (ok && ubx_cls == 0x0A && ubx_id == 0x04 && ubx_len >= 40) {
                char sw[31] = {0}, hw[11] = {0};
                memcpy(sw, ubx_pay,      30);
                memcpy(hw, ubx_pay + 30, 10);
                Serial.printf("\n       SW='%s'  HW='%s'", sw, hw);
            }
            Serial.println();
            ubx_st = U_WAIT_B5;
            return;
        }
    }

    if (b == '$') {
        nmea_active = true;
        nmea_len    = 0;
        nmea_buf[nmea_len++] = (char)b;
        return;
    }
    if (nmea_active) {
        if (b == '\n' || b == '\r') {
            if (nmea_len > 0) {
                nmea_buf[nmea_len < (int)sizeof(nmea_buf) ? nmea_len : (int)sizeof(nmea_buf)-1] = 0;
                Serial.printf("[NMEA] %s\n", nmea_buf);
                g_nmea_count++;
            }
            nmea_active = false;
            nmea_len    = 0;
            return;
        }
        if (nmea_len < (int)sizeof(nmea_buf) - 1) nmea_buf[nmea_len++] = (char)b;
        else { nmea_active = false; nmea_len = 0; g_err_count++; }
        return;
    }

    if (b != 0x00 && b != 0xFF) Serial.printf("[?] 0x%02X\n", b);
}

// ════════════════════════════════════════════════════════════════════
//  CLI
// ════════════════════════════════════════════════════════════════════
static void cli_help()
{
    Serial.println(F(
        "Commands:\n"
        "  HELP                        this list\n"
        "  PING                        UBX-MON-VER poll (fw/hw string)\n"
        "  HW                          UBX-MON-HW  poll (antenna, RTC)\n"
        "  MONRF                       UBX-MON-RF  poll (noise, AGC, jam)\n"
        "  NAVPVT                      UBX-NAV-PVT poll (fix telemetry)\n"
        "  RESET WARM|COLD|HOT         UBX-CFG-RST\n"
        "  BAUD <n>                    reopen UART @ <n> (9600/38400/115200)\n"
        "  SEND <cls> <id> <hex...>    raw UBX (checksum auto-added)\n"
        "  HEX                         toggle raw hex dump vs NMEA/UBX\n"
        "  STATS                       counters\n"
        "  CLEAR                       zero counters\n"
    ));
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, uint8_t *out, size_t cap)
{
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        int hi = hex_nibble(*s++); if (hi < 0) return -1;
        int lo = hex_nibble(*s++); if (lo < 0) return -1;
        if ((size_t)n >= cap) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static void cli_baud(const char *arg)
{
    uint32_t b = strtoul(arg, nullptr, 10);
    if (b < 300 || b > 921600) { Serial.println("baud out of range"); return; }
    GPS.end();
    delay(50);
    GPS.begin(b, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    pinMode(GPS_RX_PIN, INPUT_PULLUP);
    g_baud = b;
    Serial.printf("[UART] reopened at %u baud\n", (unsigned)b);
}

static void cli_reset(const char *arg)
{
    uint16_t mask = 0x0000;
    if      (!strcasecmp(arg, "WARM")) mask = 0x0001;
    else if (!strcasecmp(arg, "COLD")) mask = 0xFFFF;
    else if (!strcasecmp(arg, "HOT"))  mask = 0x0000;
    else { Serial.println("RESET WARM|COLD|HOT"); return; }
    uint8_t pay[4] = { (uint8_t)(mask & 0xFF), (uint8_t)(mask >> 8), 0x02, 0x00 };
    ubx_send(0x06, 0x04, pay, sizeof(pay));
}

static void cli_send(const char *arg)
{
    uint8_t bytes[64];
    int n = parse_hex(arg, bytes, sizeof(bytes));
    if (n < 2) { Serial.println("SEND <cls> <id> [<hex payload...>]"); return; }
    ubx_send(bytes[0], bytes[1], (n > 2) ? &bytes[2] : nullptr, (uint16_t)(n - 2));
}

static void cli_stats()
{
    uint32_t now = millis();
    uint32_t idle = now - g_last_byte_ms;
    Serial.printf("[STAT] total=%u B  window=%u B/s (last %u ms)  "
                  "NMEA=%u UBX=%u err=%u  idle=%u ms  baud=%u  hex=%d\n",
                  g_bytes_total, g_bytes_window, STATS_PERIOD_MS,
                  g_nmea_count, g_ubx_count, g_err_count,
                  idle, (unsigned)g_baud, (int)g_hex_mode);
}

static void cli_dispatch(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;
    char *sp = strchr(line, ' ');
    const char *args = "";
    if (sp) { *sp = 0; args = sp + 1; while (*args == ' ') args++; }

    if      (!strcasecmp(line, "HELP"))    cli_help();
    else if (!strcasecmp(line, "PING"))    ubx_send(0x0A, 0x04, nullptr, 0);
    else if (!strcasecmp(line, "HW"))      ubx_send(0x0A, 0x09, nullptr, 0);
    else if (!strcasecmp(line, "MONRF"))   ubx_send(0x0A, 0x38, nullptr, 0);
    else if (!strcasecmp(line, "NAVPVT"))  ubx_send(0x01, 0x07, nullptr, 0);
    else if (!strcasecmp(line, "RESET"))   cli_reset(args);
    else if (!strcasecmp(line, "BAUD"))    cli_baud(args);
    else if (!strcasecmp(line, "SEND"))    cli_send(args);
    else if (!strcasecmp(line, "HEX"))     { g_hex_mode = !g_hex_mode;
                                             Serial.printf("hex=%d\n", (int)g_hex_mode); }
    else if (!strcasecmp(line, "STATS"))   cli_stats();
    else if (!strcasecmp(line, "CLEAR"))   { g_bytes_total = g_nmea_count = g_ubx_count = g_err_count = 0;
                                             Serial.println("counters cleared"); }
    else Serial.printf("? unknown '%s' (HELP)\n", line);
}

// ════════════════════════════════════════════════════════════════════
//  setup / loop
// ════════════════════════════════════════════════════════════════════
void setup()
{
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println(F("========================================================"));
    Serial.println(F("  MAX-M10S liveness diagnostic (Mk1b iv8.0, ESP32-S3)"));
    Serial.println(F("========================================================"));
    Serial.printf ("  UART1: RX=GPIO%u  TX=GPIO%u  baud=%u\n",
                   GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);
    Serial.println(F("  Type HELP for CLI commands."));
    Serial.println(F("========================================================"));

    GPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    pinMode(GPS_RX_PIN, INPUT_PULLUP);   // match IDF driver's gpio_pullup_en on RX

    g_last_byte_ms  = millis();
    g_last_stats_ms = millis();
    g_last_hb_ms    = millis();

    delay(200);
    ubx_send(0x0A, 0x04, nullptr, 0);   // kick a MON-VER on boot
}

void loop()
{
    while (GPS.available()) {
        uint8_t b = (uint8_t)GPS.read();
        g_bytes_total++;
        g_bytes_window++;
        g_last_byte_ms = millis();
        classify_byte(b);
    }

    static char   line[96];
    static size_t line_len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[line_len] = 0;
            if (line_len > 0) cli_dispatch(line);
            line_len = 0;
            continue;
        }
        if (line_len < sizeof(line) - 1) line[line_len++] = c;
    }

    uint32_t now = millis();
    if (now - g_last_stats_ms >= STATS_PERIOD_MS) {
        cli_stats();
        g_bytes_window  = 0;
        g_last_stats_ms = now;
    }

    if (g_bytes_total == 0 && (now - g_last_hb_ms) >= HEARTBEAT_MS) {
        Serial.printf("[HB]   silent %.1f s -- try PING, BAUD 38400, "
                      "check TX/RX not swapped\n",
                      (double)now / 1000.0);
        g_last_hb_ms = now;
    }
}
