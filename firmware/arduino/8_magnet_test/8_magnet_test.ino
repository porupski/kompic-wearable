/*
 * 8_magnet_test.ino  --  Kompic Mk I LIS3MDLTR degauss / bench-monitor
 *
 * Live-only magnetometer read-out sketch. Boots, initialises just what is
 * needed to drive the RGB LED + service the button + talk to the LIS3MDL
 * on bus 1, and then streams magnetometer readings to Serial at ~20 Hz
 * for the operator to watch while manually degaussing the sensor (rotate
 * a magnet over it, then slowly pull it away).
 *
 * Because the iv7.1 prototype has a permanently-attached battery, the
 * double-click ship-mode path is mandatory (see auto-memory
 * feedback_sketches_need_shipmode.md).
 *
 * ── Controls ─────────────────────────────────────────────────────────────
 *   single click : cycle full-scale range   +/-4 -> +/-8 -> +/-16 -> back
 *   double click : BQ ship mode
 *
 * ── RGB status (WS2812B on GPIO42) ───────────────────────────────────────
 *   RED    +/-4 G   (default at boot)
 *   GREEN  +/-8 G
 *   BLUE   +/-16 G
 *
 * ── Serial output (115200 baud) ──────────────────────────────────────────
 *   Header printed every time the range changes.
 *   One data line per read, ~20 Hz:
 *     FS=  4G  raw_x=  -32768  raw_y=    1234  raw_z=    -456   uT: x=  -478.9  y=    18.0  z=    -6.7   |X|>Y>Z
 *   raw_x/y/z at +/-32768 = digital saturation on that axis. Watch for one
 *   axis that stays exactly +/-32768 while the others move -- that is the
 *   "stuck / dead" channel state described in the bench notes.
 *
 * Board: ESP32-S3, Arduino IDE, USB Mode = "Hardware CDC and JTAG"
 *        (do NOT switch to TinyUSB -- see feedback_tinyusb_guard.md).
 */

#include <Arduino.h>
#include <Wire.h>

// ═════════════════════════════════════════════════════════════════════════════
// Pins  (identical to 7_demo_field_capture)
// ═════════════════════════════════════════════════════════════════════════════
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1     // Wire  -- LIS3MDL sits here
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4     // Wire1 -- BQ25619 sits here
#define PIN_SCL_BUS2       5
#define PIN_WS2812_DIN    42

// ═════════════════════════════════════════════════════════════════════════════
// I2C addresses
// ═════════════════════════════════════════════════════════════════════════════
#define LIS3MDL_ADDR      0x1C
#define BQ25619_ADDR      0x6A

// ═════════════════════════════════════════════════════════════════════════════
// LIS3MDL registers
// ═════════════════════════════════════════════════════════════════════════════
#define LIS_REG_WHO          0x0F
#define LIS_WHO_VAL          0x3D
#define LIS_REG_CTRL1        0x20
#define LIS_REG_CTRL2        0x21
#define LIS_REG_CTRL3        0x22
#define LIS_REG_CTRL4        0x23
#define LIS_REG_CTRL5        0x24
#define LIS_REG_OUT_X_L      0x28
#define LIS_AUTO_INC         0x80

// FS field lives in CTRL2 bits 6:5. SOFT_RST = bit 2.
#define LIS_CTRL2_FS_4G      (0x00 << 5)
#define LIS_CTRL2_FS_8G      (0x01 << 5)
#define LIS_CTRL2_FS_16G     (0x03 << 5)
#define LIS_CTRL2_SOFT_RST   (1 << 2)

// ═════════════════════════════════════════════════════════════════════════════
// BQ25619 (only registers used by ship mode)
// ═════════════════════════════════════════════════════════════════════════════
#define BQ_REG_MISC_OP        0x07
#define BQ_REG_STATUS         0x08
#define BQ_STATUS_PG          (1 << 2)
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)

// ═════════════════════════════════════════════════════════════════════════════
// Button + WS2812
// ═════════════════════════════════════════════════════════════════════════════
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS    350
#define WS_LEVEL             26     // per-channel cap (eyes-safe)
#define PRINT_PERIOD_MS      50     // 20 Hz

// ═════════════════════════════════════════════════════════════════════════════
// Range state
// ═════════════════════════════════════════════════════════════════════════════
typedef struct {
    const char *label;
    uint8_t     ctrl2_val;   // FS bits + reserved
    float       ut_per_lsb;  // conversion factor: value_uT = raw_lsb * ut_per_lsb
    uint8_t     r, g, b;     // RGB indicator (level = WS_LEVEL scaled by these bools)
} range_t;

// Sensitivity per datasheet:
//   +/-4  G : 6842 LSB/gauss -> 68.42 LSB/uT -> 1/68.42 ~ 0.01462 uT/LSB
//   +/-8  G : 3421 LSB/gauss -> 34.21 LSB/uT -> 1/34.21 ~ 0.02923 uT/LSB
//   +/-16 G : 1711 LSB/gauss -> 17.11 LSB/uT -> 1/17.11 ~ 0.05845 uT/LSB
// (+/-12 G exists but is skipped -- the operator asked for 4/8/16 only.)
static const range_t RANGES[3] = {
    { "  4G", LIS_CTRL2_FS_4G,   1.0f / 68.42f,   WS_LEVEL, 0,        0        },  // Red
    { "  8G", LIS_CTRL2_FS_8G,   1.0f / 34.21f,   0,        WS_LEVEL, 0        },  // Green
    { " 16G", LIS_CTRL2_FS_16G,  1.0f / 17.11f,   0,        0,        WS_LEVEL },  // Blue
};
static uint8_t range_idx = 0;   // starts at +/-4 G

static bool lis_ok = false;
static bool bq_ok  = false;

// ═════════════════════════════════════════════════════════════════════════════
// I2C helpers (same shape as 7_demo_field_capture)
// ═════════════════════════════════════════════════════════════════════════════
static bool i2c_ping(TwoWire &bus, uint8_t addr) {
    bus.beginTransmission(addr);
    return bus.endTransmission() == 0;
}
static uint8_t i2c_read_reg(TwoWire &bus, uint8_t addr, uint8_t reg) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return 0xFF;
    if (bus.requestFrom((int)addr, 1) != 1) return 0xFF;
    return bus.read();
}
static bool i2c_write_reg(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t val) {
    bus.beginTransmission(addr);
    bus.write(reg);
    bus.write(val);
    return bus.endTransmission() == 0;
}
static bool i2c_read_buf(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t *dst, size_t n) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return false;
    if (bus.requestFrom((int)addr, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; i++) dst[i] = bus.read();
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// LIS3MDL config for the currently-selected range
// ═════════════════════════════════════════════════════════════════════════════
static void lis_apply_range(uint8_t idx) {
    if (!lis_ok) return;
    const range_t &r = RANGES[idx];

    // Soft-reset before every range change wipes any latched saturation
    // state -- the whole point of this sketch is to give the chip a chance
    // to recover.
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL2, LIS_CTRL2_SOFT_RST);
    delay(10);

    // CTRL1: TEMP_EN=0, OM=high perf, DO=40 Hz (0x50)  -- same as sketch 7
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL1, 0x50);
    // CTRL2: FS per range
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL2, r.ctrl2_val);
    // CTRL3: continuous conversion (MD=00)
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL3, 0x00);
    // CTRL4: OMZ=high perf, LE=0 (0x00)
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL4, 0x00);
    // CTRL5: BDU=1 (0x40)
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL5, 0x40);

    Serial.printf("[LIS] range now +/-%s (CTRL2=0x%02X, %.4f uT/LSB)\n",
                  r.label, r.ctrl2_val, r.ut_per_lsb);
}

static bool lis_read(int16_t &rx, int16_t &ry, int16_t &rz) {
    if (!lis_ok) return false;
    uint8_t b[6];
    if (!i2c_read_buf(Wire, LIS3MDL_ADDR, LIS_REG_OUT_X_L | LIS_AUTO_INC, b, 6)) return false;
    rx = (int16_t)((uint16_t)b[1] << 8 | b[0]);
    ry = (int16_t)((uint16_t)b[3] << 8 | b[2]);
    rz = (int16_t)((uint16_t)b[5] << 8 | b[4]);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// RGB (WS2812B on GPIO42) -- neopixelWrite lives in the Arduino-ESP32 core
// ═════════════════════════════════════════════════════════════════════════════
static void rgb_off(void) { neopixelWrite(PIN_WS2812_DIN, 0, 0, 0); }
static void rgb_show_range(uint8_t idx) {
    const range_t &r = RANGES[idx];
    neopixelWrite(PIN_WS2812_DIN, r.r, r.g, r.b);
}

// ═════════════════════════════════════════════════════════════════════════════
// Ship mode (double-click GPIO16) -- ported from sketch 7
// ═════════════════════════════════════════════════════════════════════════════
static void ship_mode_countdown(void) {
    Serial.println("[BTN] DOUBLE -> ship mode (2 s red countdown -- hands off)");
    const uint32_t step_ms = 40;
    const uint32_t steps   = 2000 / step_ms;
    for (uint32_t i = 0; i < steps; i++) {
        float phase = (float)i / 12.5f;
        float s = 0.5f * (1.0f - cosf(phase * (float)M_PI));
        uint8_t r = (uint8_t)(s * (float)WS_LEVEL);
        neopixelWrite(PIN_WS2812_DIN, r, 0, 0);
        delay(step_ms);
    }
    neopixelWrite(PIN_WS2812_DIN, WS_LEVEL, 0, 0);
}
static void enter_ship_mode(void) {
    if (!bq_ok) {
        Serial.println("[BTN] BQ not alive -- ship mode unavailable");
        return;
    }
    ship_mode_countdown();
    if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("      button still LOW at end of countdown -- aborting");
        rgb_off();
        return;
    }
    uint8_t r07 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP);
    uint8_t r07_new = r07;
    r07_new |=  BQ_BATFET_DIS;
    r07_new |=  BQ_BATFET_RST_WVBUS;
    r07_new &= ~BQ_BATFET_DLY;
    r07_new &= ~BQ_BATFET_RST_EN;
    Serial.printf("      writing REG07 0x%02X -> 0x%02X\n", r07, r07_new);
    Serial.flush();
    i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP, r07_new);
    delay(50);
    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    if (st & BQ_STATUS_PG) {
        Serial.println("      USB present -- BATFET disabled; ship mode fires on USB unplug");
        rgb_off();
    } else {
        Serial.println("      BATFET off -- expecting power loss now");
        Serial.flush();
        while (1) { delay(100); }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Button state machine (single vs double click) -- ported from sketch 7
// ═════════════════════════════════════════════════════════════════════════════
static void on_single_click(void) {
    range_idx = (range_idx + 1) % 3;
    lis_apply_range(range_idx);
    rgb_show_range(range_idx);
}
static void on_double_click(void) { enter_ship_mode(); }

typedef enum { BTN_IDLE, BTN_PRESSED, BTN_WAIT_DBL, BTN_PRESSED_2 } btn_state_t;
static void handle_button(void) {
    static btn_state_t state    = BTN_IDLE;
    static uint32_t last_change = 0;
    static uint32_t release_ms  = 0;
    static bool     prev_low    = false;
    uint32_t now = millis();
    bool low = (digitalRead(PIN_BUTTON) == LOW);
    if (low != prev_low && (now - last_change) >= BTN_DEBOUNCE_MS) {
        last_change = now;
        prev_low = low;
        if (low) {
            state = (state == BTN_WAIT_DBL) ? BTN_PRESSED_2 : BTN_PRESSED;
        } else {
            if (state == BTN_PRESSED) {
                release_ms = now;
                state = BTN_WAIT_DBL;
            } else if (state == BTN_PRESSED_2) {
                state = BTN_IDLE;
                on_double_click();
            }
        }
    }
    if (state == BTN_WAIT_DBL && (now - release_ms) > BTN_DOUBLE_GAP_MS) {
        on_single_click();
        state = BTN_IDLE;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Boot check + main loop
// ═════════════════════════════════════════════════════════════════════════════
static void boot_check(void) {
    Serial.println();
    Serial.println("=== 8_magnet_test boot check ===");

    // Bus 1 (LIS3MDL lives here)
    Wire.begin(PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    if (i2c_ping(Wire, LIS3MDL_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LIS3MDL_ADDR, LIS_REG_WHO);
        if (who == LIS_WHO_VAL) {
            lis_ok = true;
            Serial.printf("  LIS3MDL   bus1 0x1C : ACK  WHO_AM_I=0x%02X OK\n", who);
        } else {
            Serial.printf("  LIS3MDL   bus1 0x1C : ACK  WHO_AM_I=0x%02X (unexpected)\n", who);
        }
    } else {
        Serial.println("  LIS3MDL   bus1 0x1C : NO ACK -- sketch is now useless, aborting");
    }

    // Bus 2 (BQ25619 for ship mode)
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 400000);
    if (i2c_ping(Wire1, BQ25619_ADDR)) {
        bq_ok = true;
        Serial.println("  BQ25619   bus2 0x6A : ACK  (ship mode armed)");
    } else {
        Serial.println("  BQ25619   bus2 0x6A : NO ACK -- ship mode unavailable");
    }

    Serial.println("=================================");
}

void setup(void) {
    Serial.begin(115200);
    // Give USB CDC a moment before printing.
    for (int i = 0; i < 20 && !Serial; i++) delay(50);

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    boot_check();
    if (!lis_ok) {
        // Nothing more we can do; sit and blink red.
        while (1) {
            neopixelWrite(PIN_WS2812_DIN, WS_LEVEL, 0, 0);
            delay(200);
            neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
            delay(200);
        }
    }

    lis_apply_range(range_idx);
    rgb_show_range(range_idx);

    Serial.println();
    Serial.println("Controls:  single click = cycle range   double click = ship mode");
    Serial.println("           range LED:  RED=+/-4G   GREEN=+/-8G   BLUE=+/-16G");
    Serial.println();
}

void loop(void) {
    handle_button();

    static uint32_t last_print = 0;
    uint32_t now = millis();
    if ((now - last_print) < PRINT_PERIOD_MS) return;
    last_print = now;

    int16_t rx, ry, rz;
    if (!lis_read(rx, ry, rz)) {
        Serial.println("[LIS] read failed");
        return;
    }

    const range_t &r = RANGES[range_idx];
    float ux = (float)rx * r.ut_per_lsb;
    float uy = (float)ry * r.ut_per_lsb;
    float uz = (float)rz * r.ut_per_lsb;

    // Mark saturated axes so the "stuck" state is obvious.
    // int16 rails are +32767 / -32768.
    const char *sx = (rx == 32767 || rx == -32768) ? "*" : " ";
    const char *sy = (ry == 32767 || ry == -32768) ? "*" : " ";
    const char *sz = (rz == 32767 || rz == -32768) ? "*" : " ";

    Serial.printf("FS=%s  raw:%s%7d%s%7d%s%7d   uT:%8.1f %8.1f %8.1f\n",
                  r.label, sx, rx, sy, ry, sz, rz, ux, uy, uz);
}
