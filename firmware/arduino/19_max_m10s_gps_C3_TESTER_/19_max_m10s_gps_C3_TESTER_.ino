/*
 * 19_max_m10s_gps_C3_TESTER_ -- MAX-M10S liveness diagnostic on ESP32-C3 SuperMini Plus.
 *
 * Purpose: Ivan has no S3 to spare, so bench-verify the MAX-M10S GPS
 * module (rescued off the Mk1b, currently drawing ~94 mA on 3V3 vs.
 * bare-C3 baseline of 75-82 mA) with a C3 SuperMini. If ANY NMEA or
 * UBX bytes come out of the module's TX pin, the chip is alive and
 * the wiring on Mk1b is the bug. If nothing comes out for 10+ s across
 * multiple baud rates, the chip is dead.
 *
 * ── Wiring (only 4 wires; power is already there) ────────────────────
 *   C3 3V3   ── MAX-M10S VCC + VIO_SEL float (already done, per Ivan)
 *   C3 GND   ── MAX-M10S GND (already done, per Ivan)
 *   C3 GPIO4 ── MAX-M10S TX  (module talks -> C3 listens)
 *   C3 GPIO5 ── MAX-M10S RX  (C3 talks   -> module listens)
 *
 *   SAFEBOOT_N: leave FLOATING (never tie high; the internal 1k pull
 *   fights TIMEPULSE per [[project_max_m10s_wiring]]).
 *
 * ── Expected output on Serial monitor (115200 baud) ──────────────────
 *   ALIVE, no antenna:
 *     [NMEA] $GNRMC,,V,,,,,,,,,,,N*4D
 *     [NMEA] $GNGGA,,,,,,0,00,99.99,,,,,,*56       ("0" = no fix -- OK)
 *     [UBX ] cls=0x0A id=0x04 len=NN  (MON-VER after PING)
 *     [STAT] 1002 B/s, NMEA=8 UBX=1 err=0
 *
 *   DEAD:
 *     [HB]   silent 5.0 s  (no bytes at all, or only garbage)
 *     [STAT] 0 B/s, NMEA=0 UBX=0 err=0
 *
 * ── CLI (type + Enter over the Serial monitor) ───────────────────────
 *   HELP                list commands
 *   PING                send UBX-MON-VER poll (chip echoes fw/hw)
 *   HW                  send UBX-MON-HW poll  (antenna status too)
 *   MONRF               send UBX-MON-RF poll  (noise/AGC)
 *   NAVPVT              send UBX-NAV-PVT poll (fix telemetry)
 *   RESET WARM|COLD|HOT UBX-CFG-RST
 *   BAUD <n>            reopen UART at <n> (9600 / 38400 / 115200)
 *   SEND <hex...>       raw UBX (auto-checksum): "06 04 04 00 00 00 09 00"
 *   HEX                 toggle raw hex dump vs NMEA/UBX classification
 *   STATS               dump counters
 *   CLEAR               zero counters
 *
 * ── Arduino IDE settings ─────────────────────────────────────────────
 *   Board:            ESP32C3 Dev Module   (or "ESP32C3 SuperMini")
 *   USB CDC On Boot:  Enabled              (needed for Serial over native USB)
 *   Flash Size:       4MB                  (SuperMini default)
 *   Upload Speed:     460800
 *
 * NB: no ship-mode handler in this sketch -- C3 SuperMini has no BQ
 * fuel gate; just USB power. Safe to ignore [[feedback_sketches_need_shipmode]]
 * (that rule targets the iv7.1 / Mk1b BQ25619 flow).
 */

#include <Arduino.h>

// ════════════════════════════════════════════════════════════════════
//  Config
// ════════════════════════════════════════════════════════════════════
#define GPS_RX_PIN     4       // C3 GPIO4 <- MAX-M10S TX
#define GPS_TX_PIN     5       // C3 GPIO5 -> MAX-M10S RX
#define GPS_DEFAULT_BAUD 9600  // MAX-M10S default (per Integration Manual)
#define STATS_PERIOD_MS 1000
#define HEARTBEAT_MS    5000   // "still no data" nag when silent

HardwareSerial GPS(1);         // UART1 on C3

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
static uint32_t g_baud          = GPS_DEFAULT_BAUD;

// ════════════════════════════════════════════════════════════════════
//  NMEA + UBX parser state (very small, single-message granularity)
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

// Send a UBX poll/write. `pay` may be NULL when payload_len == 0.
static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *pay, uint16_t payload_len)
{
    uint8_t hdr[4] = { cls, id, (uint8_t)(payload_len & 0xFF),
                                (uint8_t)((payload_len >> 8) & 0xFF) };
    uint8_t a, b;
    // Checksum covers cls+id+len+payload
    uint8_t buf[256 + 4];
    memcpy(buf, hdr, 4);
    if (payload_len && pay) memcpy(buf + 4, pay, payload_len);
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
//  Input parser -- classify streaming bytes into NMEA / UBX / other
// ════════════════════════════════════════════════════════════════════
static void classify_byte(uint8_t b)
{
    if (g_hex_mode) {
        Serial.printf("%02X ", b);
        if ((g_bytes_total & 15) == 15) Serial.println();
        return;
    }

    // ── UBX (0xB5 0x62 ...) has priority ─────────────────────────────
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
            // Print first up-to-32 bytes of payload as hex
            if (ubx_len) {
                Serial.print("  ");
                uint16_t n = ubx_len > 32 ? 32 : ubx_len;
                for (uint16_t i = 0; i < n; i++)
                    Serial.printf("%02X ", ubx_pay[i]);
                if (ubx_len > 32) Serial.print("...");
            }
            // MON-VER (0x0A 0x04): first 30 bytes = SW ver, next 10 = HW ver.
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

    // ── NMEA ('$ ... \r\n', ASCII) ───────────────────────────────────
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

    // Garbage / framing byte -- only log if it's not obviously a null.
    if (b != 0x00 && b != 0xFF) {
        Serial.printf("[?] 0x%02X\n", b);
    }
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
        "                              e.g. SEND 0A 04       (MON-VER poll)\n"
        "  HEX                         toggle raw hex dump vs NMEA/UBX\n"
        "  STATS                       dump counters\n"
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

// Parse "AB CD 12 34" into bytes. Returns count, -1 on parse error.
static int parse_hex(const char *s, uint8_t *out, size_t cap)
{
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        int hi = hex_nibble(*s++);
        if (hi < 0) return -1;
        int lo = hex_nibble(*s++);
        if (lo < 0) return -1;
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
    g_baud = b;
    Serial.printf("[UART] reopened at %u baud\n", (unsigned)b);
}

static void cli_reset(const char *arg)
{
    // UBX-CFG-RST: navBbrMask(2) startType(1) reserved(1)
    // navBbrMask: 0x0000 = hot, 0x0001 = warm, 0xFFFF = cold
    uint16_t mask = 0x0000;
    if (!strcasecmp(arg, "WARM")) mask = 0x0001;
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
    uint8_t cls = bytes[0], id = bytes[1];
    ubx_send(cls, id, (n > 2) ? &bytes[2] : nullptr, (uint16_t)(n - 2));
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
    // Split verb / args
    char *sp = strchr(line, ' ');
    const char *args = "";
    if (sp) { *sp = 0; args = sp + 1; while (*args == ' ') args++; }

    if (!strcasecmp(line, "HELP"))         { cli_help(); }
    else if (!strcasecmp(line, "PING"))    { ubx_send(0x0A, 0x04, nullptr, 0); }
    else if (!strcasecmp(line, "HW"))      { ubx_send(0x0A, 0x09, nullptr, 0); }
    else if (!strcasecmp(line, "MONRF"))   { ubx_send(0x0A, 0x38, nullptr, 0); }
    else if (!strcasecmp(line, "NAVPVT"))  { ubx_send(0x01, 0x07, nullptr, 0); }
    else if (!strcasecmp(line, "RESET"))   { cli_reset(args); }
    else if (!strcasecmp(line, "BAUD"))    { cli_baud(args); }
    else if (!strcasecmp(line, "SEND"))    { cli_send(args); }
    else if (!strcasecmp(line, "HEX"))     { g_hex_mode = !g_hex_mode;
                                             Serial.printf("hex=%d\n", (int)g_hex_mode); }
    else if (!strcasecmp(line, "STATS"))   { cli_stats(); }
    else if (!strcasecmp(line, "CLEAR"))   { g_bytes_total = g_nmea_count = g_ubx_count = g_err_count = 0;
                                             Serial.println("counters cleared"); }
    else                                   { Serial.printf("? unknown '%s' (HELP)\n", line); }
}

// ════════════════════════════════════════════════════════════════════
//  setup / loop
// ════════════════════════════════════════════════════════════════════
void setup()
{
    Serial.begin(115200);
    delay(400);   // let CDC come up before first print
    Serial.println();
    Serial.println(F("========================================================"));
    Serial.println(F("  MAX-M10S liveness diagnostic (ESP32-C3 SuperMini)"));
    Serial.println(F("========================================================"));
    Serial.printf ("  UART1: RX=GPIO%u  TX=GPIO%u  baud=%u\n",
                   GPS_RX_PIN, GPS_TX_PIN, GPS_DEFAULT_BAUD);
    Serial.println(F("  Wire: C3.3V3 -> M10.VCC, C3.GND -> M10.GND"));
    Serial.println(F("        C3.GPIO4 -> M10.TX,  C3.GPIO5 -> M10.RX"));
    Serial.println(F("  SAFEBOOT_N: leave FLOATING (do NOT tie high)"));
    Serial.println(F("  Type HELP for CLI commands."));
    Serial.println(F("========================================================"));

    GPS.begin(GPS_DEFAULT_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    g_last_byte_ms  = millis();
    g_last_stats_ms = millis();
    g_last_hb_ms    = millis();

    // Kick a MON-VER poll so we don't have to wait for the first NMEA burst.
    delay(200);
    ubx_send(0x0A, 0x04, nullptr, 0);
}

void loop()
{
    // ── Drain the GPS UART ────────────────────────────────────────────
    while (GPS.available()) {
        uint8_t b = (uint8_t)GPS.read();
        g_bytes_total++;
        g_bytes_window++;
        g_last_byte_ms = millis();
        classify_byte(b);
    }

    // ── CLI (line-buffered on Serial) ─────────────────────────────────
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

    // ── 1 Hz stats line ───────────────────────────────────────────────
    uint32_t now = millis();
    if (now - g_last_stats_ms >= STATS_PERIOD_MS) {
        cli_stats();
        g_bytes_window  = 0;
        g_last_stats_ms = now;
    }

    // ── Heartbeat when totally silent (chip-dead nag) ─────────────────
    if (g_bytes_total == 0 && (now - g_last_hb_ms) >= HEARTBEAT_MS) {
        Serial.printf("[HB]   silent %.1f s -- try PING, BAUD 38400, "
                      "check TX/RX not swapped\n",
                      (double)(now - 0) / 1000.0);
        g_last_hb_ms = now;
    }
}
