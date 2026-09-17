/*
 * 17_max17048_fuelgauge_mk1b -- MAX17048 fuel-gauge presence + read sketch
 * Target: Kompic Mk1b iv8.0 (ESP32-S3-WROOM-1U-N16R8)
 *
 * Purpose: minimal bare-bones proof that the MAX17048 fuel gauge is soldered
 * correctly on Mk1b (I2C bus 2 @ 0x36, alongside BQ25619 at 0x6A). No
 * bench work should be required to answer:
 *   1. Does the chip ACK on I2C1?
 *   2. Does VERSION (reg 0x08) look right (upper nibble 0x1 for MAX17048)?
 *   3. Does VCELL (reg 0x02) return a plausible non-zero value?
 *   4. Does SOC (reg 0x04) return a plausible value?
 *
 * The sketch assumes a battery WILL be attached eventually. Without a cell
 * on BAT+/BAT-, the MAX17048 still ACKs and reads VERSION correctly, but
 * VCELL will float low (~0.0-0.3 V) and SOC will be garbage. That's the
 * expected state for a freshly-soldered Mk1b -- Ivan just needs to see:
 *   MAX17048 0x36 : ACK   VERSION=0x001x   VCELL=<some value>   SOC=<some value>
 * to confirm the chip is talking.
 *
 * ── Arduino IDE settings ────────────────────────────────────────────
 *   Board:            ESP32S3 Dev Module
 *   Flash Size:       16MB (128Mb)
 *   Flash Mode:       QIO 80MHz
 *   PSRAM:            OPI PSRAM        (N16R8 module)
 *   USB CDC On Boot:  Enabled
 *   Upload Mode:      UART0
 *
 * ── MAX17048 register map (from datasheet, MSB-first, 16-bit) ───────
 *   0x02  VCELL      raw * 78.125 uV/LSB  -> volts
 *   0x04  SOC        MSB = integer %, LSB = 1/256 %
 *   0x08  VERSION    upper nibble = 0x1 for MAX17048 family
 *   0x0A  HIBRT      hibernate control (not used here)
 *   0x0C  CONFIG     alerts + sleep bits (not touched here)
 *   0xFE  CMD        write 0x5400 for a fresh IC POR (not used here)
 */

#include <Arduino.h>
#include <Wire.h>

// ════════════════════════════════════════════════════════════════════
//  PINS -- Kompic_Pinout_MASTER_v20 (Mk1b iv8.0)
// ════════════════════════════════════════════════════════════════════

#define SDA_BUS2      4   // I2C1 -- BQ25619 + MAX17048 live here on Mk1b
#define SCL_BUS2      5
#define BUTTON_PIN   16   // BQ_BUTTON, doubles as BQ /QON

#define MAX17048_ADDR  0x36
#define BQ_ADDR        0x6A

// ── MAX17048 registers ──
#define MAX_REG_VCELL   0x02
#define MAX_REG_SOC     0x04
#define MAX_REG_VERSION 0x08

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

// ── Sample cadence ──
#define SAMPLE_INTERVAL_MS  1000

static bool bq_ok  = false;
static bool max_ok = false;

// ════════════════════════════════════════════════════════════════════
//  I2C HELPERS
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

// MAX17048 registers are 16-bit, MSB-first. Return true on success.
static bool max17048_read16(uint8_t reg, uint16_t *out) {
  Wire1.beginTransmission(MAX17048_ADDR);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((uint8_t)MAX17048_ADDR, (uint8_t)2) != 2) return false;
  uint8_t hi = Wire1.read();
  uint8_t lo = Wire1.read();
  *out = ((uint16_t)hi << 8) | lo;
  return true;
}

// ════════════════════════════════════════════════════════════════════
//  BQ25619 SHIP MODE (per feedback-sketches-need-shipmode)
//  On Mk1b (no battery yet) this is a no-op — USB stays up regardless.
// ════════════════════════════════════════════════════════════════════

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
    Serial.println("      USB present -- BATFET disabled; ship mode fires on unplug (no-op without battery)");
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
//  MAX17048 SAMPLING
// ════════════════════════════════════════════════════════════════════

static void print_fuel_sample() {
  uint16_t vcell_raw = 0, soc_raw = 0;

  if (!max17048_read16(MAX_REG_VCELL, &vcell_raw)) {
    Serial.println("  [FUEL] VCELL read FAILED");
    return;
  }
  if (!max17048_read16(MAX_REG_SOC, &soc_raw)) {
    Serial.println("  [FUEL] SOC read FAILED");
    return;
  }

  // VCELL: 78.125 uV per LSB (16-bit register; lower 4 bits are noise
  //        floor per datasheet -- treated as part of the value here).
  float vcell_v = vcell_raw * 78.125e-6f;
  // SOC: high byte = integer %, low byte = 1/256 %.
  float soc_pct = (soc_raw >> 8) + ((soc_raw & 0xFF) / 256.0f);

  Serial.printf("  [FUEL] VCELL=0x%04X (%.3f V)   SOC=0x%04X (%.2f %%)\n",
                vcell_raw, vcell_v, soc_raw, soc_pct);
}

// ════════════════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== 17_max17048_fuelgauge_mk1b (Mk1b iv8.0) ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire1.begin(SDA_BUS2, SCL_BUS2, 400000);

  // BQ25619 -- for ship-mode arming
  if (i2c_ping(Wire1, BQ_ADDR)) {
    bq_ok = true;
    Serial.println("  BQ25619   0x6A : ACK  (ship mode armed on double-click)");
  } else {
    Serial.println("  BQ25619   0x6A : NO ACK  (ship mode UNAVAILABLE)");
  }

  // MAX17048 -- the actual subject of this sketch
  if (i2c_ping(Wire1, MAX17048_ADDR)) {
    max_ok = true;
    Serial.println("  MAX17048  0x36 : ACK");
  } else {
    Serial.println("  MAX17048  0x36 : NO ACK  (fuel gauge absent or bus fault)");
  }

  if (max_ok) {
    uint16_t version = 0;
    if (max17048_read16(MAX_REG_VERSION, &version)) {
      Serial.printf("  MAX17048  VERSION = 0x%04X", version);
      uint8_t nibble = (version >> 12) & 0x0F;
      if (nibble == 0x1) {
        Serial.println("   (upper nibble 0x1 -- MAX17048 family, PASS)");
      } else {
        Serial.printf("   (upper nibble 0x%X -- unexpected, family mismatch?)\n", nibble);
      }
    } else {
      Serial.println("  MAX17048  VERSION read FAILED");
    }
  }

  Serial.println();
  Serial.println("Streaming VCELL + SOC every 1 s. Values are noise until a cell is attached.");
  Serial.println("Double-click button (GPIO16) to enter ship mode (no-op without a battery).");
  Serial.println();
}

void loop() {
  button_poll();

  static uint32_t last_ms = 0;
  uint32_t now = millis();
  if (max_ok && (now - last_ms) >= SAMPLE_INTERVAL_MS) {
    last_ms = now;
    print_fuel_sample();
  }

  delay(10);
}
