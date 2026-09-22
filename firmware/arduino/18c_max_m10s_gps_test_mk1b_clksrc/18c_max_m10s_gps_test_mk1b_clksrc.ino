/*
 * 18c_max_m10s_gps_test_mk1b_clksrc -- REWRITTEN 2026-09-20.
 *
 * Bypasses HardwareSerial entirely and drives UART1 through the raw
 * ESP-IDF v4.4 API (this Arduino install is on arduino-esp32 v2.x --
 * uart_set_clock_source() is an IDF v5.0+ symbol and does NOT exist here,
 * hence the previous compile break).
 *
 * What this sketch tests, and why:
 *
 *   1) Peripheral clock source is knob-controllable via
 *      uart_param_config()'s .source_clk field. CLI verb CLK APB|XTAL|REF_TICK
 *      re-initializes the UART with a different clock source without
 *      reflashing. If any of them makes the GPS bytes appear where APB
 *      is currently giving us silence, the arduino-esp32 v2.x
 *      HardwareSerial default clock source is the root cause.
 *
 *   2) On ESP32-S3, GPIO 0-21 are RTC / LP_IO capable. If a previous
 *      ESP-IDF firmware ever enabled digital hold (gpio_hold_en) or RTC
 *      hold (rtc_gpio_hold_en) on the GPS pins -- deliberately or as a
 *      side effect of another driver -- that latch survives NRST and
 *      even esptool erase_flash. It's only cleared by an actual power
 *      cycle of the RTC domain (USB unplug, since this bench Mk1b has
 *      no battery / no BQ), OR by explicit *_hold_dis calls. This
 *      sketch calls the *_hold_dis / rtc_gpio_deinit / gpio_reset_pin
 *      family on both pins at the top of setup() so we don't have to
 *      unplug/replug USB to test the hypothesis.
 *
 *   3) The pin pair is CLI-swappable (PINS <rx> <tx>) so we can move
 *      from 17/18 to 9/10 (or any other free pins) without reflashing.
 *
 * Wiring: identical to 18b.
 *   MAX-M10S TX -> ESP GPIO17  (ESP UART1 RX)
 *   MAX-M10S RX -> ESP GPIO18  (ESP UART1 TX)
 *   MAX-M10S VCC -> 3V3
 *   MAX-M10S GND -> GND
 *   SAFEBOOT_N -> FLOAT (per project_max_m10s_wiring)
 *
 * No ship-mode handler needed for this test rig -- the bench Mk1b is
 * USB-powered with no battery / no BQ / no DRV attached, so
 * feedback_sketches_need_shipmode does not apply here.
 */

#include <Arduino.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

// ════════════════════════════════════════════════════════════════════
//  Config
// ════════════════════════════════════════════════════════════════════
#define UART_PORT         UART_NUM_1
#define GPS_RX_PIN_INIT   18//9//17     // ESP RX <- MAX-M10S TX
#define GPS_TX_PIN_INIT   7//17//10//18     // ESP TX -> MAX-M10S RX
#define GPS_BAUD_INIT   9600
#define RX_RING_BYTES   2048
#define STATS_PERIOD_MS 1000
#define HEARTBEAT_MS    5000

// Runtime state (mutable via CLI)
static int         g_rx_pin = GPS_RX_PIN_INIT;
static int         g_tx_pin = GPS_TX_PIN_INIT;
static uint32_t    g_baud   = GPS_BAUD_INIT;
static uart_sclk_t g_clk    = UART_SCLK_APB;
static bool        g_driver_installed = false;

// Counters
static uint32_t g_bytes_total   = 0;
static uint32_t g_bytes_window  = 0;
static uint32_t g_nmea_count    = 0;
static uint32_t g_ubx_count     = 0;
static uint32_t g_err_count     = 0;
static uint32_t g_last_byte_ms  = 0;
static uint32_t g_last_stats_ms = 0;
static uint32_t g_last_hb_ms    = 0;
static bool     g_hex_mode      = false;
static bool     g_quiet         = false;   // suppress [NMEA] + [?] prints (still counts + still shows UBX + STAT)

// ════════════════════════════════════════════════════════════════════
//  Parser state (NMEA + UBX)  -- identical to 18b
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
//  Helpers
// ════════════════════════════════════════════════════════════════════
static const char* clk_name(uart_sclk_t c)
{
    // NB: ESP32-S3 UART only exposes APB / XTAL / RTC as source clocks.
    // REF_TICK is a C3/older-SoC option and is not defined for the S3.
    switch (c) {
        case UART_SCLK_APB:  return "APB";
        case UART_SCLK_XTAL: return "XTAL";
        case UART_SCLK_RTC:  return "RTC";
        default:             return "?";
    }
}

static void ubx_checksum(const uint8_t *buf, size_t n, uint8_t &a, uint8_t &b)
{
    a = 0; b = 0;
    for (size_t i = 0; i < n; i++) { a += buf[i]; b += a; }
}

static void uart_tx_raw(const uint8_t *buf, size_t n)
{
    if (!g_driver_installed) return;
    uart_write_bytes(UART_PORT, (const char*)buf, n);
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(100));
}

static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *pay, uint16_t payload_len)
{
    uint8_t hdr[4] = { cls, id,
                       (uint8_t)(payload_len & 0xFF),
                       (uint8_t)((payload_len >> 8) & 0xFF) };
    uint8_t chkbuf[256 + 4];
    memcpy(chkbuf, hdr, 4);
    if (payload_len && pay) memcpy(chkbuf + 4, pay, payload_len);
    uint8_t a, b;
    ubx_checksum(chkbuf, 4 + payload_len, a, b);

    uint8_t sync[2] = { 0xB5, 0x62 };
    uart_tx_raw(sync, 2);
    uart_tx_raw(hdr,  4);
    if (payload_len && pay) uart_tx_raw(pay, payload_len);
    uint8_t ck[2] = { a, b };
    uart_tx_raw(ck, 2);

    Serial.printf("[SEND] UBX cls=0x%02X id=0x%02X len=%u  ck=%02X %02X\n",
                  cls, id, payload_len, a, b);
}

// ════════════════════════════════════════════════════════════════════
//  Pin-hold nuke -- call before any uart_set_pin() on the GPS pins.
//  Clears every latching mechanism that could disconnect a pin from
//  the GPIO matrix while leaving the external waveform visible.
// ════════════════════════════════════════════════════════════════════
static void nuke_pin_state(int pin)
{
    gpio_num_t g = (gpio_num_t)pin;
    // Digital-domain hold (survives sleep + software reset)
    gpio_hold_dis(g);
    // RTC-domain hold (survives *hardware* reset, only cleared by RTC power cycle)
    if (rtc_gpio_is_valid_gpio(g)) {
        rtc_gpio_hold_dis(g);
        rtc_gpio_deinit(g);           // detach from RTC IO subsystem
    }
    // Default the pin (disables output, disables pu/pd, restores GPIO function)
    gpio_reset_pin(g);
}

// ════════════════════════════════════════════════════════════════════
//  UART init / reinit
// ════════════════════════════════════════════════════════════════════
static bool uart_init(int rx, int tx, uint32_t baud, uart_sclk_t clk)
{
    // Uninstall any prior driver install so we can re-configure freely
    if (g_driver_installed) {
        uart_driver_delete(UART_PORT);
        g_driver_installed = false;
    }

    // Nuke any lingering pin state on both pins
    gpio_deep_sleep_hold_dis();       // global belt-and-suspenders
    nuke_pin_state(rx);
    nuke_pin_state(tx);

    uart_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.baud_rate  = (int)baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.rx_flow_ctrl_thresh = 0;
    cfg.source_clk = clk;

    esp_err_t e;
    e = uart_param_config(UART_PORT, &cfg);
    if (e != ESP_OK) {
        Serial.printf("[UART] param_config err=%s\n", esp_err_to_name(e));
        return false;
    }
    // uart_set_pin(port, tx, rx, rts, cts)
    e = uart_set_pin(UART_PORT, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) {
        Serial.printf("[UART] set_pin err=%s\n", esp_err_to_name(e));
        return false;
    }
    e = uart_driver_install(UART_PORT, RX_RING_BYTES, 0, 0, NULL, 0);
    if (e != ESP_OK) {
        Serial.printf("[UART] driver_install err=%s\n", esp_err_to_name(e));
        return false;
    }
    // Match the IDF driver's usual gentle input pullup so a floating pin
    // idles high instead of flapping.
    gpio_pullup_en((gpio_num_t)rx);

    g_driver_installed = true;
    g_rx_pin = rx; g_tx_pin = tx; g_baud = baud; g_clk = clk;

    Serial.printf("[UART] INIT ok  rx=GPIO%d  tx=GPIO%d  baud=%u  clk=%s\n",
                  rx, tx, (unsigned)baud, clk_name(clk));
    return true;
}

// ════════════════════════════════════════════════════════════════════
//  Input parser -- identical to 18b (per-byte classifier)
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
                if (!g_quiet) Serial.printf("[NMEA] %s\n", nmea_buf);
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

    if (!g_quiet && b != 0x00 && b != 0xFF) Serial.printf("[?] 0x%02X\n", b);
}

// ════════════════════════════════════════════════════════════════════
//  CLI
// ════════════════════════════════════════════════════════════════════
static void cli_help()
{
    Serial.println(F(
        "Commands:\n"
        "  HELP                         this list\n"
        "  PING                         UBX-MON-VER poll (fw/hw string)\n"
        "  HW                           UBX-MON-HW  poll (antenna, RTC)\n"
        "  MONRF                        UBX-MON-RF  poll (noise, AGC, jam)\n"
        "  NAVPVT                       UBX-NAV-PVT poll (fix telemetry)\n"
        "  RESET WARM|COLD|HOT          UBX-CFG-RST\n"
        "  BAUD <n>                     reinit UART @ <n> (300..921600)\n"
        "  CLK APB|XTAL|REF_TICK|RTC    reinit UART with clock source\n"
        "  PINS <rx> <tx>               reinit UART on new pin pair\n"
        "  PROBE                        read raw digital level of RX pin (10 ms sample)\n"
        "  SEND <cls> <id> <hex...>     raw UBX (checksum auto-added)\n"
        "  HEX                          toggle raw hex dump vs NMEA/UBX\n"
        "  QUIET                        toggle: silence [NMEA] + [?] spam (UBX + STAT still print)\n"
        "  STATS                        counters\n"
        "  CLEAR                        zero counters\n"
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
    uart_init(g_rx_pin, g_tx_pin, b, g_clk);
}

static void cli_clk(const char *arg)
{
    uart_sclk_t c;
    if      (!strcasecmp(arg, "APB"))  c = UART_SCLK_APB;
    else if (!strcasecmp(arg, "XTAL")) c = UART_SCLK_XTAL;
    else if (!strcasecmp(arg, "RTC"))  c = UART_SCLK_RTC;
    else { Serial.println("CLK APB|XTAL|RTC  (S3 has no REF_TICK on UART)"); return; }
    uart_init(g_rx_pin, g_tx_pin, g_baud, c);
}

static void cli_pins(const char *arg)
{
    int rx = -1, tx = -1;
    if (sscanf(arg, "%d %d", &rx, &tx) != 2 || rx < 0 || tx < 0) {
        Serial.println("PINS <rx> <tx>");
        return;
    }
    uart_init(rx, tx, g_baud, g_clk);
}

static void cli_probe()
{
    // Temporarily read the current RX pin as a plain GPIO input to
    // confirm the incoming waveform is present at the pin level even
    // when the UART peripheral sees nothing.
    // NB: this briefly detaches the pin from UART; we re-attach at the end.
    gpio_num_t g = (gpio_num_t)g_rx_pin;
    Serial.printf("[PROBE] sampling GPIO%d @ ~100kHz for 10ms ...\n", g_rx_pin);
    gpio_reset_pin(g);
    pinMode(g_rx_pin, INPUT);
    int lo = 0, hi = 0, edges = 0;
    int prev = digitalRead(g_rx_pin);
    uint32_t t_end = micros() + 10000;
    while ((int32_t)(t_end - micros()) > 0) {
        int v = digitalRead(g_rx_pin);
        if (v) hi++; else lo++;
        if (v != prev) { edges++; prev = v; }
    }
    Serial.printf("[PROBE] samples: hi=%d lo=%d edges=%d  (edges>0 => signal is on the pin)\n",
                  hi, lo, edges);
    // Reattach the pin to the UART with current config
    uart_init(g_rx_pin, g_tx_pin, g_baud, g_clk);
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
                  "NMEA=%u UBX=%u err=%u  idle=%u ms  "
                  "rx=GPIO%d tx=GPIO%d baud=%u clk=%s hex=%d\n",
                  g_bytes_total, g_bytes_window, STATS_PERIOD_MS,
                  g_nmea_count, g_ubx_count, g_err_count,
                  idle, g_rx_pin, g_tx_pin, (unsigned)g_baud,
                  clk_name(g_clk), (int)g_hex_mode);
}

static void cli_dispatch(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;
    char *sp = strchr(line, ' ');
    const char *args = "";
    if (sp) { *sp = 0; args = sp + 1; while (*args == ' ') args++; }

    if      (!strcasecmp(line, "HELP"))   cli_help();
    else if (!strcasecmp(line, "PING"))   ubx_send(0x0A, 0x04, nullptr, 0);
    else if (!strcasecmp(line, "HW"))     ubx_send(0x0A, 0x09, nullptr, 0);
    else if (!strcasecmp(line, "MONRF"))  ubx_send(0x0A, 0x38, nullptr, 0);
    else if (!strcasecmp(line, "NAVPVT")) ubx_send(0x01, 0x07, nullptr, 0);
    else if (!strcasecmp(line, "RESET"))  cli_reset(args);
    else if (!strcasecmp(line, "BAUD"))   cli_baud(args);
    else if (!strcasecmp(line, "CLK"))    cli_clk(args);
    else if (!strcasecmp(line, "PINS"))   cli_pins(args);
    else if (!strcasecmp(line, "PROBE"))  cli_probe();
    else if (!strcasecmp(line, "SEND"))   cli_send(args);
    else if (!strcasecmp(line, "HEX"))    { g_hex_mode = !g_hex_mode;
                                            Serial.printf("hex=%d\n", (int)g_hex_mode); }
    else if (!strcasecmp(line, "QUIET"))  { g_quiet = !g_quiet;
                                            Serial.printf("quiet=%d (NMEA+[?] %s, UBX+STAT stay)\n",
                                                          (int)g_quiet, g_quiet ? "SILENCED" : "on"); }
    else if (!strcasecmp(line, "STATS"))  cli_stats();
    else if (!strcasecmp(line, "CLEAR"))  { g_bytes_total = g_nmea_count = g_ubx_count = g_err_count = 0;
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
    Serial.println(F("  MAX-M10S liveness diagnostic  (Mk1b iv8.0, ESP32-S3)"));
    Serial.println(F("  18c: raw IDF UART (no HardwareSerial), CLK/PINS knobs"));
    Serial.println(F("========================================================"));
    Serial.printf ("  Default: UART1  RX=GPIO%d  TX=GPIO%d  baud=%u  clk=APB\n",
                   GPS_RX_PIN_INIT, GPS_TX_PIN_INIT, GPS_BAUD_INIT);
    Serial.println(F("  Try:  PING   CLK XTAL   CLK REF_TICK   PINS 9 10   PROBE"));
    Serial.println(F("========================================================"));

    if (!uart_init(GPS_RX_PIN_INIT, GPS_TX_PIN_INIT, GPS_BAUD_INIT, UART_SCLK_APB)) {
        Serial.println("[UART] INITIAL INIT FAILED -- see error above");
    }

    g_last_byte_ms  = millis();
    g_last_stats_ms = millis();
    g_last_hb_ms    = millis();

    delay(200);
    ubx_send(0x0A, 0x04, nullptr, 0);   // MON-VER poll at boot
}

void loop()
{
    // Drain UART RX ring
    if (g_driver_installed) {
        size_t avail = 0;
        uart_get_buffered_data_len(UART_PORT, &avail);
        if (avail > 0) {
            uint8_t buf[128];
            size_t take = avail > sizeof(buf) ? sizeof(buf) : avail;
            int n = uart_read_bytes(UART_PORT, buf, take, 0);
            for (int i = 0; i < n; i++) {
                g_bytes_total++;
                g_bytes_window++;
                g_last_byte_ms = millis();
                classify_byte(buf[i]);
            }
        }
    }

    // CLI line buffer
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
        Serial.printf("[HB]   silent %.1f s -- try:  PING  CLK XTAL  CLK REF_TICK  PROBE\n",
                      (double)now / 1000.0);
        g_last_hb_ms = now;
    }
}
