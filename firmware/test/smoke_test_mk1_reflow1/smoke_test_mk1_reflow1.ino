/**
 * smoke_test_mk1.ino — Kompic Mk I, first board smoke test
 *
 * Population at this stage:
 *   ESP32-S3-WROOM-1U, BQ25619 charger, TPS62840 3.2 V buck,
 *   XC6206 1.8 V LDO, flashlight LED (GPIO41), button (GPIO16 / BQ QON),
 *   PCF85063A RTC + crystal.  USB-C power only, no battery.
 *
 * Sequence:
 *   1. Serial up, brief startup banner.
 *   2. LEDC + button init.
 *   3. 2× LED flash at 50% — boot confirmation.
 *   4. Probe BQ25619 on I2C bus 2 (GPIO4/5, 0x6A) — REG_PART + REG_STATUS.
 *   5. Probe PCF85063A on I2C bus 1 (GPIO1/2, 0x51) — Control_1 + Seconds OS bit.
 *   6. PASS/FAIL summary.
 *   7. Breathe loop — slow sine PWM on LED.
 *      Button (GPIO16) toggles LED on / off.
 *
 * Arduino-ESP32 v3.x API (ledcAttach / ledcWrite).
 * Board: "ESP32S3 Dev Module", USB CDC On Boot: Enabled.
 * Pins from: 0_Kompic_Pinout_MASTER_v20_iv7.1.md
 */

#include <Wire.h>
#include <math.h>

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_FLASHLIGHT   41   // LED via FET; ext pull-down keeps it off at boot
#define PIN_BUTTON       16   // BQ25619 QON dual-wire; LOW = pressed
#define PIN_SDA_BUS1      1   // I2C bus 1 — RTC  (east edge)
#define PIN_SCL_BUS1      2
#define PIN_SDA_BUS2      4   // I2C bus 2 — BQ charger (west edge)
#define PIN_SCL_BUS2      5
#define PIN_RTC_INT      15   // PCF85063A open-drain INT — just input, not used here

// ── I2C addresses ─────────────────────────────────────────────────────────────
#define BQ25619_ADDR     0x6A
#define PCF85063A_ADDR   0x51

// ── BQ25619 registers (from bq25619.h) ───────────────────────────────────────
#define BQ_REG_STATUS    0x08  // VBUS_STAT, CHRG_STAT, PG_STAT
#define BQ_REG_FAULT     0x09
#define BQ_REG_PART      0x0A  // WHO_AM_I / part number
#define BQ_STATUS_PG     (1 << 2)

// ── PCF85063A registers ───────────────────────────────────────────────────────
#define RTC_REG_CTRL1    0x00
#define RTC_REG_SECONDS  0x04  // bit 7 = OS (oscillator stop flag)
#define RTC_OS_BIT       0x80

// ── LEDC ──────────────────────────────────────────────────────────────────────
#define LED_FREQ_HZ      1000
#define LED_RES_BITS     8     // 0–255

// ── State ─────────────────────────────────────────────────────────────────────
static bool bq_ok  = false;
static bool rtc_ok = false;
static bool led_on = true;

// ── LED helpers ───────────────────────────────────────────────────────────────
static void led_duty(uint8_t d) { ledcWrite(PIN_FLASHLIGHT, d); }

static void led_flash(uint8_t duty, int on_ms, int off_ms, int n) {
    for (int i = 0; i < n; i++) {
        led_duty(duty);  delay(on_ms);
        led_duty(0);     delay(off_ms);
    }
}

// ── I2C helpers ──────────────────────────────────────────────────────────────
static bool i2c_ping(TwoWire &bus, uint8_t addr) {
    bus.beginTransmission(addr);
    return bus.endTransmission() == 0;
}

static uint8_t i2c_read_reg(TwoWire &bus, uint8_t addr, uint8_t reg) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return 0xFF;
    bus.requestFrom(addr, (uint8_t)1);
    return bus.available() ? bus.read() : 0xFF;
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(600);  // let USB-CDC enumerate

    Serial.println("\n========================================");
    Serial.println("  Kompic Mk I — First Board Smoke Test");
    Serial.println("  ESP32-S3 | USB-C power | no battery");
    Serial.println("========================================\n");

    // LEDC for flashlight (v3.x API: single call, pin-addressed)
    ledcAttach(PIN_FLASHLIGHT, LED_FREQ_HZ, LED_RES_BITS);
    Serial.println("[LED] LEDC init: GPIO41, 1 kHz, 8-bit");

    // Button: BQ holds QON high internally; ESP pull-up for belt-and-braces
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    Serial.println("[BTN] GPIO16 INPUT_PULLUP");

    // RTC INT: open-drain, active-low — passive input for now
    pinMode(PIN_RTC_INT, INPUT);
    Serial.println("[RTC] GPIO15 INT: INPUT\n");

    // ── Step 1: boot flash ────────────────────────────────────────────────────
    Serial.println("[LED] Boot flash: 2× 50% ...");
    led_flash(128, 200, 250, 2);
    Serial.println("[LED] Done.\n");

    // ── Step 2: BQ25619 on I2C bus 2 ─────────────────────────────────────────
    Serial.println("[BQ] Bus 2: SDA=GPIO4, SCL=GPIO5, 400 kHz");
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 400000);
    delay(20);

    Serial.printf("[BQ] Pinging 0x%02X ... ", BQ25619_ADDR);
    if (i2c_ping(Wire1, BQ25619_ADDR)) {
        uint8_t part   = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_PART);
        uint8_t status = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
        uint8_t fault  = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_FAULT);
        bool    pg     = status & BQ_STATUS_PG;
        bq_ok = true;
        Serial.println("ACK");
        Serial.printf("[BQ] REG_PART=0x%02X  REG_STATUS=0x%02X (PG=%s)  REG_FAULT=0x%02X\n",
                      part, status, pg ? "OK" : "NO", fault);
        if (fault)
            Serial.printf("[BQ] WARN: fault register non-zero — check TS pin / NTC / VBUS\n");
        Serial.println("[BQ] PASS\n");
    } else {
        Serial.println("NO ACK");
        Serial.println("[BQ] FAIL — check BQ25619 solder, I2C lines, 3V3 rail\n");
    }

    // ── Step 3: PCF85063A on I2C bus 1 ───────────────────────────────────────
    Serial.println("[RTC] Bus 1: SDA=GPIO1, SCL=GPIO2, 400 kHz");
    Wire.begin(PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    delay(20);

    Serial.printf("[RTC] Pinging 0x%02X ... ", PCF85063A_ADDR);
    if (i2c_ping(Wire, PCF85063A_ADDR)) {
        uint8_t ctrl1   = i2c_read_reg(Wire, PCF85063A_ADDR, RTC_REG_CTRL1);
        uint8_t seconds = i2c_read_reg(Wire, PCF85063A_ADDR, RTC_REG_SECONDS);
        bool    os      = seconds & RTC_OS_BIT;
        rtc_ok = true;
        Serial.println("ACK");
        Serial.printf("[RTC] Control_1=0x%02X  Seconds_reg=0x%02X  OS=%s\n",
                      ctrl1, seconds,
                      os ? "SET (crystal stopped — check load caps / solder)"
                         : "clear (clock running)");
        Serial.println("[RTC] PASS\n");
    } else {
        Serial.println("NO ACK");
        Serial.println("[RTC] FAIL — check PCF85063A solder, crystal, I2C pull-ups, 1V8 rail\n");
    }

    // ── Summary ───────────────────────────────────────────────────────────────
    Serial.println("── Summary ──────────────────────────────");
    Serial.printf("   BQ25619   (charger): %s\n", bq_ok  ? "PASS" : "FAIL");
    Serial.printf("   PCF85063A (RTC)    : %s\n", rtc_ok ? "PASS" : "FAIL");
    Serial.println("─────────────────────────────────────────");

    if (bq_ok && rtc_ok)
        Serial.println("   All devices found — entering breathe loop.");
    else
        Serial.println("   One or more devices missing — fix and reboot.");

    Serial.println("\n[BTN] Press GPIO16 button to toggle LED on / off\n");
}

// ── loop — breathe + button ───────────────────────────────────────────────────
void loop() {
    // Button: debounced toggle
    static uint32_t btn_last_ms = 0;
    if (digitalRead(PIN_BUTTON) == LOW) {
        uint32_t now = millis();
        if (now - btn_last_ms > 300) {
            led_on = !led_on;
            btn_last_ms = now;
            if (!led_on) led_duty(0);
            Serial.printf("[BTN] Pressed → LED %s\n", led_on ? "ON" : "OFF");
        }
    }

    if (!led_on) {
        delay(20);
        return;
    }

    // Sine breathe: period ≈ 2.1 s, floor ≈ 5%, ceiling ≈ 91%
    static float phase = 0.0f;
    phase += 0.03f;
    if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    float   s = (sinf(phase) + 1.0f) * 0.5f;   // 0.0–1.0
    uint8_t d = (uint8_t)(s * 220.0f + 12.0f); // 12–232
    led_duty(d);
    delay(10);
}
