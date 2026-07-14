/*
 * 9_test_ecg.ino  --  Kompic Mk I LSM6DSV16X Qvar (ECG) diagnostic
 *
 * Streams the AH_QVAR_OUT register of the LSM6DSV16X for two-electrode
 * body-potential capture. Three output modes selectable at compile time
 * via the OUT_MODE define at the top of the file:
 *
 *   OUT_MODE_MONITOR   -> 2 Hz stats to Serial Monitor (default)
 *   OUT_MODE_PLOT_RAW  -> single raw int per line for Serial Plotter
 *   OUT_MODE_PLOT_AC   -> 3 traces per line: raw, baseline, ac (raw-baseline)
 *                         Use this on real body electrodes -- skin galvanic
 *                         DC offset routinely drives the raw near a rail,
 *                         and the AC trace is what shows cardiac / breath.
 *
 * ── Why this sketch exists ─────────────────────────────────────────────────
 * The ESP-IDF field_capture ECG mode read a flat noise floor (~4850 +/-25
 * LSB) with zero response to touching the electrodes. The audit of the
 * LSM6DSV16X datasheet (rev 4, Table 65, p.70) found:
 *
 *   1. AH_QVAR_EN is CTRL7 bit 7 (0x80), not bit 0 (0x01). Every prior
 *      attempt to enable Qvar was actually enabling LPF1_G_EN (a gyro
 *      digital filter). The analog hub / Qvar chain was never on.
 *
 *   2. The datasheet mandates: "Before setting AH_QVAR_EN to 1, the
 *      accelerometer and gyroscope sensors have to be configured in
 *      power-down mode." No prior init did this.
 *
 *   3. The Qvar output rate tracks the accel ODR. After AH_QVAR_EN=1
 *      the accel must be re-enabled in HP mode so the Qvar chain has
 *      a sample clock.
 *
 * This sketch does all three correctly and lets you switch input
 * impedance (2.4 GOhm .. 235 MOhm) live with the button so we can see
 * which setting gives useful signal amplitude for cap-coupled arm
 * electrodes.
 *
 * ── Controls ───────────────────────────────────────────────────────────────
 *   single click : cycle ZIN  2.4G -> 730M -> 300M -> 235M -> back
 *   double click : BQ25619 ship mode
 *
 * ── RGB status (WS2812B) ───────────────────────────────────────────────────
 *   BLUE    2.4 GOhm (default)
 *   GREEN   730 MOhm
 *   YELLOW  300 MOhm
 *   RED     235 MOhm
 *
 * Board: ESP32-S3, Arduino IDE, USB Mode = "Hardware CDC and JTAG".
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// ═════════════════════════════════════════════════════════════════════════════
// Compile-time output-style toggle
//   OUT_MODE_MONITOR      -- 2 Hz stats to Serial Monitor
//   OUT_MODE_PLOT_RAW     -- single raw int per line for Serial Plotter
//   OUT_MODE_PLOT_TRIPLE  -- three traces per line: raw, baseline, ac.
//                            Full picture but plotter can't zoom just the ac.
//   OUT_MODE_PLOT_AC_ONLY -- single ac value per line, clipped to +/-2000.
//                            Serial Plotter auto-scales to just the AC
//                            content -- easiest way to see biopotential
//                            deflections when the raw is pegged near a rail.
//                            Recommended when trying to see cardiac /
//                            respiration on the current DC-coupled electrodes.
// ═════════════════════════════════════════════════════════════════════════════
#define OUT_MODE_MONITOR       0
#define OUT_MODE_PLOT_RAW      1
#define OUT_MODE_PLOT_TRIPLE   2
#define OUT_MODE_PLOT_AC_ONLY  3
#define OUT_MODE               OUT_MODE_PLOT_AC_ONLY

// Clip window for OUT_MODE_PLOT_AC_ONLY. Anything above this is clamped so
// saturation-recovery transients don't blow out the plotter's auto-scale.
#define AC_CLIP  10000

// ═════════════════════════════════════════════════════════════════════════════
// Pins (same as sketches 7 and 8)
// ═════════════════════════════════════════════════════════════════════════════
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1     // Wire   -> LSM6DSV16X
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4     // Wire1  -> BQ25619
#define PIN_SCL_BUS2       5
#define PIN_WS2812_DIN    42

// ═════════════════════════════════════════════════════════════════════════════
// I2C addresses
// ═════════════════════════════════════════════════════════════════════════════
#define LSM6DSV_ADDR      0x6B
#define BQ25619_ADDR      0x6A

// ═════════════════════════════════════════════════════════════════════════════
// LSM6DSV16X registers (only what this sketch needs)
// ═════════════════════════════════════════════════════════════════════════════
#define LSM_REG_WHO           0x0F
#define LSM_WHO_VAL           0x70
#define LSM_REG_CTRL1         0x10   // accel ODR [7:4] + OP_MODE_XL [3:0]
#define LSM_REG_CTRL2         0x11   // gyro  ODR + OP_MODE
#define LSM_REG_CTRL3         0x12   // BDU / IF_INC / SW_RESET / BOOT
#define LSM_REG_CTRL7         0x16   // AH_QVAR chain
#define LSM_REG_CTRL8         0x17   // accel FS  (FS_XL [1:0])
#define LSM_REG_OUT_AH_L      0x3A
#define LSM_REG_OUT_AH_H      0x3B

// CTRL7 bit layout — datasheet Table 65 p.70 (this is the whole point)
#define CTRL7_AH_QVAR_EN         (1 << 7)
#define CTRL7_INT2_DRDY_AH_QVAR  (1 << 6)
#define CTRL7_ZIN_2G4            (0x00 << 4)
#define CTRL7_ZIN_730M           (0x01 << 4)
#define CTRL7_ZIN_300M           (0x02 << 4)
#define CTRL7_ZIN_235M           (0x03 << 4)
#define CTRL7_LPF1_G_EN          (1 << 0)

// CTRL1 -- datasheet §9.14 Table 50 p.65:
//   bits [6:4] = OP_MODE_XL   (000 = HP mode, 111 = normal, LPMs = 100..110)
//   bits [3:0] = ODR_XL       (0111 = 240 Hz, 0110 = 120 Hz, 0000 = power-down)
// The previous version of this file had ODR in bits [7:4] and OP_MODE in
// bits [3:0] -- the *opposite* of the actual layout. Writing 0x70 selected
// OP_MODE=111 (normal) but ODR=0000 (power-down), so the accel never
// clocked, and QVAR read zeros. Fixed value below: OP_MODE=HP (000<<4=0x00)
// and ODR=240 Hz (0111<<0=0x07) -> CTRL1 = 0x07.
#define CTRL1_OP_MODE_HP     (0x00 << 4)   // high-performance mode
#define CTRL1_ODR_240HZ      (0x07 << 0)   // 240 Hz

// CTRL3
#define CTRL3_SW_RESET        (1 << 0)
#define CTRL3_IF_INC          (1 << 2)
#define CTRL3_BDU             (1 << 6)

// ═════════════════════════════════════════════════════════════════════════════
// BQ25619 (ship mode only)
// ═════════════════════════════════════════════════════════════════════════════
#define BQ_REG_MISC_OP        0x07
#define BQ_REG_STATUS         0x08
#define BQ_STATUS_PG          (1 << 2)
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)

// ═════════════════════════════════════════════════════════════════════════════
// Button / WS2812 timing
// ═════════════════════════════════════════════════════════════════════════════
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS    350
#define WS_LEVEL             26

// ═════════════════════════════════════════════════════════════════════════════
// ZIN cycle table
// ═════════════════════════════════════════════════════════════════════════════
typedef struct {
    uint8_t     ctrl7_zin;   // pre-shifted bits 5:4
    const char *label;
    uint8_t     r, g, b;
} zin_t;
static const zin_t ZINS[4] = {
    { CTRL7_ZIN_2G4,   "2.4 GOhm", 0,        0,        WS_LEVEL },
    { CTRL7_ZIN_730M,  "730 MOhm", 0,        WS_LEVEL, 0        },
    { CTRL7_ZIN_300M,  "300 MOhm", WS_LEVEL, WS_LEVEL, 0        },
    { CTRL7_ZIN_235M,  "235 MOhm", WS_LEVEL, 0,        0        },
};
static uint8_t zin_idx = 0;   // start at highest impedance

// ═════════════════════════════════════════════════════════════════════════════
// State
// ═════════════════════════════════════════════════════════════════════════════
static bool     lsm_ok = false;
static bool     bq_ok  = false;

// Window stats (reset every monitor print)
static int32_t  win_sum         = 0;
static int64_t  win_sq_sum      = 0;
static int16_t  win_min         =  32767;
static int16_t  win_max         = -32768;
static uint32_t win_fresh_count = 0;
static uint32_t win_read_count  = 0;
static int16_t  last_raw        = 0;
static bool     last_raw_valid  = false;
static uint32_t win_sat_count   = 0;   // samples pegged near +/- FS

// Beat detector (same shape as sketch 7's PPG detector)
static float    bd_baseline     = 0.0f;
static bool     bd_ready        = false;
static float    bd_ac_peak      = 0.0f;
static float    bd_ac_prev      = 0.0f;
static uint32_t bd_last_beat_ms = 0;
static uint32_t bd_beat_count   = 0;
static float    bd_last_int_ms  = 0.0f;
static float    bd_bpm          = 0.0f;

// 50 Hz IIR biquad notch, f0=50, fs=240, Q=5. Kills mains hum without
// touching the 5-25 Hz QRS band. Coefficients precomputed offline:
//   w0 = 2*pi*50/240 = 1.3090,  cos(w0) = 0.25882,  a = sin(w0)/(2Q) = 0.09659
//   Normalized by a0 = 1 + a:
//     b0 =  0.9119,  b1 = -0.4721,  b2 =  0.9119,  a1 = -0.4721,  a2 =  0.8238
// If you're in a 60 Hz-mains country, recompute with w0 = 2*pi*60/240.
#define NOTCH_B0   0.9119f
#define NOTCH_B1  -0.4721f
#define NOTCH_B2   0.9119f
#define NOTCH_A1  -0.4721f
#define NOTCH_A2   0.8238f
static float notch_x1 = 0.0f, notch_x2 = 0.0f;
static float notch_y1 = 0.0f, notch_y2 = 0.0f;
static inline float apply_notch(float x) {
    float y = NOTCH_B0 * x + NOTCH_B1 * notch_x1 + NOTCH_B2 * notch_x2
            - NOTCH_A1 * notch_y1 - NOTCH_A2 * notch_y2;
    notch_x2 = notch_x1; notch_x1 = x;
    notch_y2 = notch_y1; notch_y1 = y;
    return y;
}

// ═════════════════════════════════════════════════════════════════════════════
// I2C helpers (identical shape to sketch 7)
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
// LSM6DSV16X init for QVAR -- the corrected sequence
// ═════════════════════════════════════════════════════════════════════════════
static bool lsm_init_qvar(uint8_t zin_bits) {
    // 1. Soft reset -- clean slate
    if (!i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, CTRL3_SW_RESET)) return false;
    delay(20);
    // 2. BDU (block data update) + IF_INC (auto-inc reg pointer)
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, CTRL3_BDU | CTRL3_IF_INC);
    // 3. POWER-DOWN accel + gyro *before* touching AH_QVAR_EN. Datasheet §9.20.
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, 0x00);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, 0x00);
    delay(5);
    // 4. Flip AH_QVAR_EN with the desired ZIN.
    uint8_t ctrl7_val = CTRL7_AH_QVAR_EN | zin_bits;
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL7, ctrl7_val);
    delay(5);
    // 5. Re-enable accel in HP mode at 240 Hz so QVAR has a sample clock.
    //    (Gyro left off; QVAR doesn't need it.)
    uint8_t ctrl1_val = CTRL1_OP_MODE_HP | CTRL1_ODR_240HZ;
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, ctrl1_val);
    delay(10);

    // 6. Verify BOTH readbacks -- CTRL7 confirms QVAR bits, CTRL1 confirms
    //    the accel is actually clocking (previous bug wrote 0x70 meaning
    //    "normal-mode, ODR=power-down" -> Qvar never got a clock).
    uint8_t ctrl7_rb = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL7);
    uint8_t ctrl1_rb = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1);
    Serial.printf("[LSM ] CTRL7 wrote 0x%02X read 0x%02X ZIN=%s match=%s | "
                  "CTRL1 wrote 0x%02X read 0x%02X (HP + 240 Hz expected) match=%s\n",
                  ctrl7_val, ctrl7_rb, ZINS[zin_idx].label,
                  ctrl7_rb == ctrl7_val ? "YES" : "NO",
                  ctrl1_val, ctrl1_rb,
                  ctrl1_rb == ctrl1_val ? "YES" : "NO");
    return ctrl7_rb == ctrl7_val && ctrl1_rb == ctrl1_val;
}

// ═════════════════════════════════════════════════════════════════════════════
// RGB status
// ═════════════════════════════════════════════════════════════════════════════
static void rgb_off(void) { neopixelWrite(PIN_WS2812_DIN, 0, 0, 0); }
static void rgb_show_zin(uint8_t idx) {
    const zin_t &z = ZINS[idx];
    neopixelWrite(PIN_WS2812_DIN, z.r, z.g, z.b);
}

// ═════════════════════════════════════════════════════════════════════════════
// Ship mode (verbatim from sketch 7)
// ═════════════════════════════════════════════════════════════════════════════
static void ship_mode_countdown(void) {
    Serial.println("[BTN ] DOUBLE -> ship mode (2 s red countdown, hands off)");
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
        Serial.println("[BTN ] BQ not alive -- ship mode unavailable");
        return;
    }
    ship_mode_countdown();
    if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("      button still LOW at end of countdown -- aborting");
        rgb_off();
        return;
    }
    uint8_t r07 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP);
    uint8_t r07_new = r07 | BQ_BATFET_DIS | BQ_BATFET_RST_WVBUS;
    r07_new &= ~BQ_BATFET_DLY;
    r07_new &= ~BQ_BATFET_RST_EN;
    Serial.printf("      writing REG07 0x%02X -> 0x%02X\n", r07, r07_new);
    Serial.flush();
    i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP, r07_new);
    delay(50);
    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    if (st & BQ_STATUS_PG) {
        Serial.println("      USB present -- BATFET disabled; ship mode fires on unplug");
        rgb_off();
    } else {
        Serial.println("      BATFET off -- expecting power loss now");
        Serial.flush();
        while (1) delay(100);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Stats reset + beat detector reset
// ═════════════════════════════════════════════════════════════════════════════
static void reset_window(void) {
    win_sum         = 0;
    win_sq_sum      = 0;
    win_min         =  32767;
    win_max         = -32768;
    win_fresh_count = 0;
    win_read_count  = 0;
    win_sat_count   = 0;
}
static void reset_beat_detector(void) {
    bd_baseline     = 0.0f;
    bd_ready        = false;
    bd_ac_peak      = 0.0f;
    bd_ac_prev      = 0.0f;
    bd_last_beat_ms = 0;
    bd_beat_count   = 0;
    bd_last_int_ms  = 0.0f;
    bd_bpm          = 0.0f;
    last_raw_valid  = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Sample processing (accumulate stats + run beat detector)
// ═════════════════════════════════════════════════════════════════════════════
// Global so both process_sample and the plotter output can share the
// notch-filtered value.
static float g_notched = 0.0f;

static void process_sample(int16_t raw) {
    win_read_count++;
    if (last_raw_valid && raw == last_raw) return;   // duplicate, no fresh data
    last_raw = raw;
    last_raw_valid = true;
    win_fresh_count++;
    win_sum    += raw;
    win_sq_sum += (int64_t)raw * (int64_t)raw;
    if (raw < win_min) win_min = raw;
    if (raw > win_max) win_max = raw;
    if (raw <= -30000 || raw >= 30000) win_sat_count++;

    // 50 Hz notch on the raw sample. Feeds the beat detector and the
    // "ac-only" plot mode so mains hum doesn't dominate either.
    g_notched = apply_notch((float)raw);

    // Beat detector runs on the NOTCHED signal.
    uint32_t now = millis();
    float x = g_notched;
    if (bd_baseline == 0.0f) bd_baseline = x;
    float alpha = bd_ready ? 0.005f : 0.05f;
    bd_baseline = bd_baseline * (1.0f - alpha) + x * alpha;
    if (!bd_ready && now > 3000) bd_ready = true;
    float ac     = x - bd_baseline;
    float ac_abs = fabsf(ac);
    if (ac_abs > bd_ac_peak) bd_ac_peak = ac_abs;
    else                     bd_ac_peak *= 0.9995f;
    if (bd_ready) {
        float thresh = bd_ac_peak * 0.5f;
        if (thresh < 20.0f) thresh = 20.0f;
        // Reject beats whose AC amplitude is enormous -- those are
        // saturation-recovery transients (raw jumping from +FS to -FS or
        // back), not cardiac events. Cardiac QRS through Qvar caps out
        // around a few hundred LSB in practice; anything above ~10000
        // is artifact.
        bool amplitude_plausible = (bd_ac_prev < 10000.0f);
        if (amplitude_plausible &&
            bd_ac_prev > ac && bd_ac_prev > thresh &&
            (now - bd_last_beat_ms) > 300) {
            if (bd_last_beat_ms > 0) {
                bd_last_int_ms = (float)(now - bd_last_beat_ms);
                if (bd_last_int_ms > 300 && bd_last_int_ms < 1500) {
                    bd_bpm = 60000.0f / bd_last_int_ms;
                }
            }
            bd_last_beat_ms = now;
            bd_beat_count++;
        }
    }
    bd_ac_prev = ac;
}

// ═════════════════════════════════════════════════════════════════════════════
// Monitor mode: print a compact debug line every 500 ms
// ═════════════════════════════════════════════════════════════════════════════
static void print_monitor_line(void) {
    if (win_fresh_count == 0) {
        Serial.printf("[ECG ] ZIN=%s  no fresh samples (reads=%u)\n",
                      ZINS[zin_idx].label, (unsigned)win_read_count);
        reset_window();
        return;
    }
    float mean = (float)win_sum / (float)win_fresh_count;
    float var  = (float)win_sq_sum / (float)win_fresh_count - mean * mean;
    if (var < 0.0f) var = 0.0f;
    float sd = sqrtf(var);
    uint8_t ctrl7_rb = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL7);
    bool contact = (win_max - win_min) > 200;
    uint32_t sat_pct = win_fresh_count ? (100U * win_sat_count) / win_fresh_count : 0;
    Serial.printf("[ECG ] ZIN=%s CTRL7=0x%02X  fresh/read=%u/%u sat=%u%%  min=%d max=%d p2p=%d mean=%.1f sd=%.1f  base=%.1f acpk=%.1f  beats=%u  int=%.0f  bpm=%.0f  contact=%s\n",
                  ZINS[zin_idx].label, ctrl7_rb,
                  (unsigned)win_fresh_count, (unsigned)win_read_count,
                  (unsigned)sat_pct,
                  win_min, win_max, win_max - win_min,
                  mean, sd, bd_baseline, bd_ac_peak,
                  (unsigned)bd_beat_count, bd_last_int_ms, bd_bpm,
                  contact ? "YES" : "no");
    reset_window();
}

// ═════════════════════════════════════════════════════════════════════════════
// Button single/double click (same state machine as sketch 7/8)
// ═════════════════════════════════════════════════════════════════════════════
static void on_single_click(void) {
    zin_idx = (zin_idx + 1) % 4;
    Serial.println();
    Serial.printf("[BTN ] single -> cycle ZIN to %s\n", ZINS[zin_idx].label);
    lsm_init_qvar(ZINS[zin_idx].ctrl7_zin);
    rgb_show_zin(zin_idx);
    reset_window();
    reset_beat_detector();
}
static void on_double_click(void) { enter_ship_mode(); }

typedef enum { BTN_IDLE, BTN_PRESSED, BTN_WAIT_DBL, BTN_PRESSED_2 } btn_state_t;
static void handle_button(void) {
    static btn_state_t state    = BTN_IDLE;
    static uint32_t    last_change = 0;
    static uint32_t    release_ms  = 0;
    static bool        prev_low    = false;
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
        state = BTN_IDLE;
        on_single_click();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Setup + loop
// ═════════════════════════════════════════════════════════════════════════════
void setup(void) {
    Serial.begin(115200);
    for (int i = 0; i < 20 && !Serial; i++) delay(50);

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    Wire.begin (PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 400000);

    Serial.println();
    Serial.println("=== 9_test_ecg boot ===");
    if (i2c_ping(Wire, LSM6DSV_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WHO);
        if (who == LSM_WHO_VAL) {
            lsm_ok = true;
            Serial.printf("  LSM6DSV16X 0x6B : ACK  WHO=0x%02X OK\n", who);
        } else {
            Serial.printf("  LSM6DSV16X 0x6B : ACK  WHO=0x%02X (unexpected)\n", who);
        }
    } else {
        Serial.println("  LSM6DSV16X 0x6B : NO ACK -- sketch useless");
    }
    if (i2c_ping(Wire1, BQ25619_ADDR)) {
        bq_ok = true;
        Serial.println("  BQ25619    0x6A : ACK  (ship mode armed)");
    } else {
        Serial.println("  BQ25619    0x6A : NO ACK  (ship mode unavailable)");
    }
    if (!lsm_ok) {
        while (1) {
            neopixelWrite(PIN_WS2812_DIN, WS_LEVEL, 0, 0); delay(200);
            neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);        delay(200);
        }
    }

    lsm_init_qvar(ZINS[zin_idx].ctrl7_zin);
    rgb_show_zin(zin_idx);
    reset_window();
    reset_beat_detector();

    Serial.println();
    Serial.println("controls: single-click = cycle ZIN, double-click = ship mode");
    Serial.printf ("output  : %s\n",
                   OUT_MODE == OUT_MODE_MONITOR      ? "Serial MONITOR (2 Hz stats)" :
                   OUT_MODE == OUT_MODE_PLOT_RAW     ? "Serial PLOTTER (raw only)" :
                   OUT_MODE == OUT_MODE_PLOT_TRIPLE  ? "Serial PLOTTER (raw baseline ac -- 3 traces)" :
                                                      "Serial PLOTTER (ac only, clipped +/-2000)");
    Serial.println();
}

void loop(void) {
    handle_button();

    // Read one AH_QVAR sample.
    uint8_t b[2] = {0};
    if (!i2c_read_buf(Wire, LSM6DSV_ADDR, LSM_REG_OUT_AH_L, b, 2)) {
        delay(2);
        return;
    }
    int16_t raw = (int16_t)((uint16_t)b[1] << 8 | b[0]);
    process_sample(raw);

    // Detect fresh sample once per loop so all output modes share it.
    static int16_t plot_last = 0;
    static bool    plot_first = true;
    bool fresh = plot_first || raw != plot_last;
    plot_last  = raw;
    plot_first = false;

#if OUT_MODE == OUT_MODE_PLOT_RAW
    if (fresh) Serial.println(raw);
#elif OUT_MODE == OUT_MODE_PLOT_TRIPLE
    if (fresh) {
        // Three traces per line: raw, notched (post-50 Hz filter), ac
        // (notched minus tracked baseline). The notched trace is what
        // shows cardiac shape when mains hum was dominating raw.
        int32_t notched = (int32_t)g_notched;
        int32_t base    = (int32_t)bd_baseline;
        int32_t ac      = notched - base;
        Serial.printf("%d %d %d\n", (int)raw, (int)notched, (int)ac);
    }
#elif OUT_MODE == OUT_MODE_PLOT_AC_ONLY
    if (fresh) {
        // AC of the notched signal, clipped. Serial Plotter auto-scales
        // to +/-AC_CLIP -- this is the trace most likely to show a QRS.
        int32_t ac = (int32_t)g_notched - (int32_t)bd_baseline;
        if (ac >  AC_CLIP) ac =  AC_CLIP;
        if (ac < -AC_CLIP) ac = -AC_CLIP;
        Serial.println((int)ac);
    }
#else   /* OUT_MODE_MONITOR */
    (void)fresh;
    static uint32_t last_print = 0;
    uint32_t now_ms = millis();
    if ((now_ms - last_print) >= 500) {
        last_print = now_ms;
        print_monitor_line();
    }
#endif

    // Poll a hair faster than the 240 Hz output so we don't miss samples.
    delay(2);
}
