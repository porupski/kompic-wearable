/*
 * 18_max_m10s_gps_test_mk1b -- u-blox MAX-M10S GNSS UART bring-up sketch
 * Target: Kompic Mk1b iv8.0 (ESP32-S3-WROOM-1U-N16R8)
 *
 * Purpose: minimal proof that the MAX-M10S is soldered correctly and talking
 * on UART1, WITHOUT needing an antenna. On boot, the module should:
 *   1. Auto-emit NMEA sentences at 1 Hz (empty fix fields = "V" void).
 *   2. Reply to a UBX-MON-VER poll with its firmware/hardware version string.
 *
 * If either of those is seen, the chip is alive and UART wiring is fine.
 * If neither is seen after 5-10 s, the module is dead / cold-joint / wired
 * backwards / not powered.
 *
 * ── What to look for ──────────────────────────────────────────────
 *   Success (chip alive, no antenna):
 *     [NMEA] $GNRMC,,V,,,,,,,,,,,N*4D
 *     [NMEA] $GNVTG,,,,,,,,,N*2E
 *     [NMEA] $GNGGA,,,,,,0,00,99.99,,,,,,*56    ← "0" = no fix, expected
 *     [UBX ] class=0x0A id=0x04 len=NN  (MON-VER reply: SW/HW/protocol)
 *     [PPS ] no edges (no fix, no PPS -- expected without antenna)
 *
 *   Failure (chip dead / broken wiring):
 *     No NMEA, no UBX reply, [UART] 0 bytes/sec
 *
 * ── Known Mk1b-specific caveats ───────────────────────────────────
 *   - GPIO18 (GPS RX from ESP's POV) is ALSO wired to the iv7.1-era
 *     vbat_adc override (ADC2_CH7). The main firmware still initialises
 *     that ADC at boot, which briefly reconfigures GPIO18 as an analog
 *     input and can trash UART reception. This sketch does NOT touch the
 *     ADC, so it's a clean UART-only test.
 *   - No antenna on the bench: NMEA will show empty fields, UBX-NAV-*
 *     replies will show fix quality = 0. Chip liveness is confirmed by
 *     presence of any NMEA/UBX bytes, not by fix quality.
 *
 * ── Arduino IDE settings ──────────────────────────────────────────
 *   Board:            ESP32S3 Dev Module
 *   Flash Size:       16MB (128Mb)
 *   Flash Mode:       QIO 80MHz
 *   PSRAM:            OPI PSRAM        (N16R8 module)
 *   USB CDC On Boot:  Enabled
 *   Upload Mode:      UART0
 */

#include <Arduino.h>
#include <Wire.h>

// ════════════════════════════════════════════════════════════════════
//  PINS -- Kompic_Pinout_MASTER_v20 (Mk1b iv8.0)
// ════════════════════════════════════════════════════════════════════

#define GPS_TX_PIN      18   // ESP TX -> MAX-M10S RX
#define GPS_RX_PIN      17   // ESP RX <- MAX-M10S TX
#define GPS_PPS_PIN     46   // MAX-M10S TimePulse
#define GPS_BAUD      9600   // MAX-M10S default

#define BUTTON_PIN      16   // BQ_BUTTON / BQ_/QON
#define SDA_BUS2         4   // I2C1 (BQ25619 lives here for ship-mode)
#define SCL_BUS2         5
#define BQ_ADDR       0x6A

// ── BQ25619 ship-mode registers (per feedback-sketches-need-shipmode) ──
#define BQ_REG_MISC_OP        0x07
#define BQ_REG_STATUS         0x08
#define BQ_STATUS_PG          (1 << 2)
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)

// ── Button state machine timings ──
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS   350

// ── Cadence ──
#define MON_VER_POLL_MS    10000   // Send UBX-MON-VER every 10 s
#define STATS_PRINT_MS      5000   // Byte-rate + PPS-count summary every 5 s

static bool bq_ok = false;

// ════════════════════════════════════════════════════════════════════
//  1PPS ISR
// ════════════════════════════════════════════════════════════════════

static volatile uint32_t pps_count      = 0;
static volatile uint32_t pps_last_us    = 0;

static void IRAM_ATTR pps_isr() {
  pps_count++;
  pps_last_us = micros();
}

// ════════════════════════════════════════════════════════════════════
//  UBX helpers
//  UBX frame:  B5 62  CLASS  ID  LEN_LO LEN_HI  [payload...]  CK_A CK_B
//  Checksum = 8-bit Fletcher over CLASS..payload (exclusive of sync).
// ════════════════════════════════════════════════════════════════════

static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
  uint8_t hdr[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  uint8_t ck_a = 0, ck_b = 0;
  for (int i = 2; i < 6; i++) { ck_a += hdr[i]; ck_b += ck_a; }
  for (uint16_t i = 0; i < len; i++) { ck_a += payload[i]; ck_b += ck_a; }
  Serial1.write(hdr, 6);
  if (payload && len) Serial1.write(payload, len);
  Serial1.write(ck_a);
  Serial1.write(ck_b);
}

// UBX-MON-VER poll = empty payload; module replies with software/hardware version.
static void ubx_poll_mon_ver() {
  ubx_send(0x0A, 0x04, nullptr, 0);
  Serial.println("  [POLL] -> UBX-MON-VER (class=0x0A id=0x04)");
}

// ════════════════════════════════════════════════════════════════════
//  Frame decoder -- streams bytes from Serial1, prints NMEA lines +
//                   hex-dumps UBX frames.
// ════════════════════════════════════════════════════════════════════

typedef enum {
  RX_IDLE,
  RX_NMEA,
  RX_UBX_SYNC2,
  RX_UBX_HEADER,
  RX_UBX_PAYLOAD,
  RX_UBX_CK_A,
  RX_UBX_CK_B,
} RxState;

static RxState  rx_state       = RX_IDLE;
static char     nmea_buf[128];
static uint16_t nmea_len       = 0;
static uint8_t  ubx_cls        = 0;
static uint8_t  ubx_id         = 0;
static uint16_t ubx_len        = 0;
static uint8_t  ubx_hdr_i      = 0;
static uint8_t  ubx_hdr[4];        // class/id/len_lo/len_hi
static uint8_t  ubx_payload[256];
static uint16_t ubx_payload_i  = 0;

static uint32_t nmea_frames    = 0;
static uint32_t ubx_frames     = 0;
static uint32_t bytes_since_stats = 0;

static void print_ubx_frame() {
  Serial.printf("  [UBX ] class=0x%02X id=0x%02X len=%u  payload=",
                ubx_cls, ubx_id, ubx_len);
  uint16_t n = ubx_len > 32 ? 32 : ubx_len;
  for (uint16_t i = 0; i < n; i++) Serial.printf("%02X ", ubx_payload[i]);
  if (ubx_len > 32) Serial.print("...");
  Serial.println();

  // MON-VER (0x0A 0x04) has readable ASCII strings in the payload -- surface them.
  if (ubx_cls == 0x0A && ubx_id == 0x04 && ubx_len >= 40) {
    ubx_payload[30 - 1] = 0;                       // clamp SW string
    Serial.printf("         SW=\"%s\"\n", (char *)ubx_payload);
    if (ubx_len >= 40) {
      ubx_payload[30 + 10 - 1] = 0;                // clamp HW string
      Serial.printf("         HW=\"%s\"\n", (char *)(ubx_payload + 30));
    }
  }
}

static void rx_feed(uint8_t b) {
  bytes_since_stats++;
  switch (rx_state) {
    case RX_IDLE:
      if (b == '$') {
        nmea_len = 0;
        nmea_buf[nmea_len++] = (char)b;
        rx_state = RX_NMEA;
      } else if (b == 0xB5) {
        rx_state = RX_UBX_SYNC2;
      }
      break;

    case RX_NMEA:
      if (b == '\r' || b == '\n' || nmea_len >= sizeof(nmea_buf) - 1) {
        if (nmea_len > 0) {
          nmea_buf[nmea_len] = 0;
          Serial.printf("  [NMEA] %s\n", nmea_buf);
          nmea_frames++;
        }
        nmea_len = 0;
        rx_state = RX_IDLE;
      } else {
        nmea_buf[nmea_len++] = (char)b;
      }
      break;

    case RX_UBX_SYNC2:
      rx_state = (b == 0x62) ? RX_UBX_HEADER : RX_IDLE;
      ubx_hdr_i = 0;
      break;

    case RX_UBX_HEADER:
      ubx_hdr[ubx_hdr_i++] = b;
      if (ubx_hdr_i >= 4) {
        ubx_cls       = ubx_hdr[0];
        ubx_id        = ubx_hdr[1];
        ubx_len       = ubx_hdr[2] | ((uint16_t)ubx_hdr[3] << 8);
        ubx_payload_i = 0;
        rx_state = (ubx_len == 0) ? RX_UBX_CK_A : RX_UBX_PAYLOAD;
      }
      break;

    case RX_UBX_PAYLOAD:
      if (ubx_payload_i < sizeof(ubx_payload)) ubx_payload[ubx_payload_i] = b;
      ubx_payload_i++;
      if (ubx_payload_i >= ubx_len) rx_state = RX_UBX_CK_A;
      break;

    case RX_UBX_CK_A:
      rx_state = RX_UBX_CK_B;
      break;

    case RX_UBX_CK_B:
      print_ubx_frame();
      ubx_frames++;
      rx_state = RX_IDLE;
      break;
  }
}

// ════════════════════════════════════════════════════════════════════
//  BQ25619 SHIP MODE (per feedback-sketches-need-shipmode)
// ════════════════════════════════════════════════════════════════════

static bool i2c_ping(TwoWire &bus, uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission() == 0;
}

static uint8_t i2c_read_reg(TwoWire &bus, uint8_t addr, uint8_t reg) {
  bus.beginTransmission(addr);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return 0xFF;
  if (bus.requestFrom(addr, (uint8_t)1) != 1) return 0xFF;
  return bus.read();
}

static void i2c_write_reg(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t val) {
  bus.beginTransmission(addr);
  bus.write(reg);
  bus.write(val);
  bus.endTransmission();
}

static void enter_ship_mode() {
  if (!bq_ok) {
    Serial.println("[BTN ] double-click -> ship mode, but BQ not alive; aborting");
    return;
  }
  Serial.println("[BTN ] double-click -> ship mode (BQ25619 BATFET off)");
  uint8_t r07 = i2c_read_reg(Wire1, BQ_ADDR, BQ_REG_MISC_OP);
  uint8_t r07_new = r07 | BQ_BATFET_DIS | BQ_BATFET_RST_WVBUS;
  r07_new &= ~BQ_BATFET_DLY;
  r07_new &= ~BQ_BATFET_RST_EN;
  Serial.printf("      REG07 0x%02X -> 0x%02X\n", r07, r07_new);
  Serial.flush();
  i2c_write_reg(Wire1, BQ_ADDR, BQ_REG_MISC_OP, r07_new);
  delay(50);
  uint8_t st = i2c_read_reg(Wire1, BQ_ADDR, BQ_REG_STATUS);
  if (st & BQ_STATUS_PG) {
    Serial.println("      USB present -- BATFET disabled; ship mode fires on unplug");
  } else {
    Serial.println("      BATFET off -- power gone");
    Serial.flush();
    while (1) delay(100);
  }
}

typedef enum {
  BTN_IDLE, BTN_PRESSED, BTN_WAIT_DBL, BTN_PRESSED_2
} BtnState;

static void button_poll() {
  static BtnState state    = BTN_IDLE;
  static uint32_t press_ms = 0;
  static uint32_t rel_ms   = 0;
  bool low = (digitalRead(BUTTON_PIN) == LOW);
  uint32_t now = millis();

  switch (state) {
    case BTN_IDLE:
      if (low) { press_ms = now; state = BTN_PRESSED; }
      break;
    case BTN_PRESSED:
      if (!low && (now - press_ms) >= BTN_DEBOUNCE_MS) {
        rel_ms = now; state = BTN_WAIT_DBL;
      }
      break;
    case BTN_WAIT_DBL:
      if (low) { press_ms = now; state = BTN_PRESSED_2; }
      else if ((now - rel_ms) > BTN_DOUBLE_GAP_MS) {
        Serial.println("[BTN ] single (ignored in test sketch)");
        state = BTN_IDLE;
      }
      break;
    case BTN_PRESSED_2:
      if (!low && (now - press_ms) >= BTN_DEBOUNCE_MS) {
        state = BTN_IDLE;
        enter_ship_mode();
      }
      break;
  }
}

// ════════════════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== 18_max_m10s_gps_test_mk1b (Mk1b iv8.0) ===");
  Serial.printf("  UART1: TX=GPIO%d  RX=GPIO%d  @ %d baud\n",
                GPS_TX_PIN, GPS_RX_PIN, GPS_BAUD);
  Serial.printf("  1PPS : GPIO%d (rising-edge counter, no antenna = no edges)\n",
                GPS_PPS_PIN);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire1.begin(SDA_BUS2, SCL_BUS2, 400000);
  if (i2c_ping(Wire1, BQ_ADDR)) {
    bq_ok = true;
    Serial.println("  BQ25619 0x6A : ACK  (ship mode armed on double-click)");
  } else {
    Serial.println("  BQ25619 0x6A : NO ACK  (ship mode UNAVAILABLE)");
  }

  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  pinMode(GPS_PPS_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(GPS_PPS_PIN), pps_isr, RISING);

  delay(200);
  ubx_poll_mon_ver();

  Serial.println();
  Serial.println("Streaming everything received on Serial1. NMEA is line-buffered; UBX is frame-decoded.");
  Serial.println("Byte-rate + PPS summary every 5 s. Double-click GPIO16 for ship mode.");
  Serial.println();
}

void loop() {
  button_poll();

  // Drain everything the GPS has to say.
  while (Serial1.available()) {
    rx_feed((uint8_t)Serial1.read());
  }

  // Periodic MON-VER poll -- forces a UBX reply even if NMEA is disabled.
  static uint32_t last_poll_ms = 0;
  uint32_t now = millis();
  if (now - last_poll_ms >= MON_VER_POLL_MS) {
    last_poll_ms = now;
    ubx_poll_mon_ver();
  }

  // Byte-rate + frame-count + PPS-count summary.
  static uint32_t last_stats_ms = 0;
  if (now - last_stats_ms >= STATS_PRINT_MS) {
    uint32_t elapsed = now - last_stats_ms;
    last_stats_ms = now;
    Serial.printf("  [STAT] %lu ms: %u B  (%u NMEA, %u UBX)   PPS edges=%lu\n",
                  (unsigned long)elapsed,
                  bytes_since_stats, nmea_frames, ubx_frames,
                  (unsigned long)pps_count);
    bytes_since_stats = 0;
    nmea_frames = 0;
    ubx_frames  = 0;
  }

  delay(2);
}
