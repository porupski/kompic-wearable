/*
 * 13_adv_heartrate_monitor.ino  --  Kompic Mk I dual-channel HR (PPG + BCG)
 *
 * Successor to sketch 10 (MAX30101 wrist-PPG diagnostic) that runs the
 * MAX30101 (green PPG) and the LSM6DSV16X (BCG via accel magnitude) in
 * parallel and applies the *same* signal-processing pipeline to both
 * channels ("twins"). One session = one button press = a continuous
 * LED_PA sweep across the MAX30101 green LED, with everything streamed
 * to SD as an interleaved CSV plus a per-session JSON summary that
 * picks the winning LED_PA by composite quality.
 *
 * WHY: sketch 10 confirmed that on the wrist the raw green PPG on a
 * MAX30101 sits at DC counts ~900..9k across the LED_PA ladder (well
 * clear of the 18-bit ADC ceiling of 262k), so the ceiling here is
 * *noise floor* not *DC clip* -- the "squeeze" campaign wants a
 * quantitative sweep of AC/DC (perfusion index) vs LED current at
 * rest, then per-user selection of "best current for still wrist".
 * The BCG channel comes along for free: it's a fully independent
 * heartbeat estimate, and lets us cross-check PPG at rest. Same
 * pipeline for both makes them directly comparable.
 *
 * ── Session flow ────────────────────────────────────────────────────────
 *   idle       : RGB slow cyan breathe, waits for button
 *   single-click: PINK 3 s countdown ("get still"), then sweep begins
 *   sweep      : LED_PA ramps linearly from LED_PA_MIN..LED_PA_MAX over
 *                SWEEP_DURATION_S seconds in SWEEP_STEPS discrete steps.
 *                Signal-chain state is NOT reset between steps -- both
 *                channels roll forward so you see a *gradual hill* of
 *                amplitude vs LED current instead of hard jumps.
 *   end        : winner LED_PA (best composite quality) printed + written
 *                to JSON; RGB latches to winner color; back to idle
 *   long-hold  : ship mode (BQ25619 BATFET off) -- always available
 *
 * ── Signal chain (identical for MAX-green and LSM-BCG) ─────────────────
 *   1. Baseline EMA  : dc estimate, fast 0.10 for first 3 s, slow 0.02 after
 *   2. Bandpass IIR  : 2-pole Butterworth
 *                        MAX: 0.5..4 Hz (30..240 BPM cardiac fundamental)
 *                        LSM: 0.8..15 Hz (BCG fundamental + harmonics)
 *   3. Motion flag   : |dAC/dt| > k * running envelope -> mark motion,
 *                      drop beats within motion window
 *   4. Peak detect   : adaptive threshold = k * leaky-max(|bp|),
 *                      refractory 250 ms
 *   5. Autocorr      : every 2 s over last 5 s of bp signal -> independent
 *                      BPM estimate for cross-check (peak-detect vs autocorr)
 *   6. Quality       : geometric mean of (PI_norm * IBI_regularity *
 *                      motion_cleanliness * autocorr_agreement) in [0..100]
 *
 * ── Output modes (set OUT_MODE at top) ─────────────────────────────────
 *   OUT_MODE_PLOTTER  : 8 tab-separated traces per line for Serial Plotter
 *                        max_raw, max_baseline, max_ac, max_bp,
 *                        max_motion*1000, lsm_bcg_raw*1e5, lsm_bp*1e5,
 *                        lsm_motion*1000
 *   OUT_MODE_SERIAL   : 1 Hz human-readable status block
 *
 * ── Files (under /data/sk13_data/) ─────────────────────────────────────
 *   sk13_NNNN_YYYYMMDD_HHMMSS.csv   per-sample interleaved log, both channels
 *   sk13_NNNN_YYYYMMDD_HHMMSS.json  session metadata + per-step summary + winner
 *   NNNN  = session index scanned from SD at boot (1 + max existing on card).
 *          Monotonic across reboots so filenames sort by run order.
 *   YYYYMMDD_HHMMSS = PCF85063A RTC (OS bit is ignored; year-sanity check
 *          gates use of the value).
 *   If RTC is dead / not sane: falls back to sk13_NNNN_bootMsXXXXXXX.csv
 *
 * Board: ESP32-S3, Arduino IDE, USB Mode = "Hardware CDC and JTAG".
 */

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <math.h>
#include "types.h"     // chan_t, step_summary_t -- MUST be a header, not
                       // inline typedef, so Arduino IDE's auto-prototype
                       // pass sees the types before it hoists prototypes

// ═════════════════════════════════════════════════════════════════════════════
// Compile-time knobs
// ═════════════════════════════════════════════════════════════════════════════
#define OUT_MODE_PLOTTER   0
#define OUT_MODE_SERIAL    1
#define OUT_MODE           OUT_MODE_PLOTTER

#define SWEEP_DURATION_S    120      // 4 x 20 s -- long enough per PA for
                                    // biquad settle + real autocorr window
#define SWEEP_STEPS         4       // discrete PA levels across LED_PA range
#define LED_PA_MIN          0x0F    // ~4 mA typ
#define LED_PA_MAX          0xFF    // ~51 mA typ
#define PRE_STILL_MS        3000    // pink countdown before sweep starts

// Serial-plotter trace clipping (Plotter auto-scales; clipping keeps traces sane)
#define MAX_AC_CLIP         3000
#define MAX_BP_CLIP         3000
#define LSM_BP_CLIP_G       0.03f   // ~30 mg display clip

// ═════════════════════════════════════════════════════════════════════════════
// Pin map (identical to sketches 7-12 on iv7.1)
// ═════════════════════════════════════════════════════════════════════════════
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1     // Wire  -> MAX30101, LSM6DSV16X, PCF85063A
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4     // Wire1 -> BQ25619
#define PIN_SCL_BUS2       5
#define PIN_WS2812_DIN    42
#define PIN_SD_CLK        38
#define PIN_SD_CMD        39
#define PIN_SD_D0         40

// ═════════════════════════════════════════════════════════════════════════════
// I2C addresses
// ═════════════════════════════════════════════════════════════════════════════
#define MAX30101_ADDR     0x57
#define LSM6DSV_ADDR      0x6B
#define PCF85063A_ADDR    0x51
#define BQ25619_ADDR      0x6A

// ═════════════════════════════════════════════════════════════════════════════
// MAX30101 registers (from sketch 10 + datasheet extract 20.11)
// ═════════════════════════════════════════════════════════════════════════════
#define M_REG_FIFO_WR_PTR     0x04
#define M_REG_OVF_CTR         0x05
#define M_REG_FIFO_RD_PTR     0x06
#define M_REG_FIFO_DATA       0x07
#define M_REG_FIFO_CONFIG     0x08
#define M_REG_MODE_CONFIG     0x09
#define M_REG_SPO2_CONFIG     0x0A
#define M_REG_LED1_PA         0x0C
#define M_REG_LED2_PA         0x0D
#define M_REG_LED3_PA         0x0E
#define M_REG_MULTI_LED_1     0x11
#define M_REG_MULTI_LED_2     0x12
#define M_REG_PART_ID         0xFF
#define M_PART_ID_VAL         0x15
#define M_MODE_RESET          0x40
#define M_MODE_MULTI_LED      0x07
#define M_ADC_16384           0x03
#define M_SR_100              0x01
#define M_PW_411              0x03
#define M_SMP_AVE_8           0x03

// ═════════════════════════════════════════════════════════════════════════════
// LSM6DSV16X registers (from sketch 12)
// ═════════════════════════════════════════════════════════════════════════════
#define LSM_REG_CTRL1             0x10
#define LSM_REG_CTRL2             0x11
#define LSM_REG_CTRL3             0x12
#define LSM_REG_CTRL8             0x17
#define LSM_REG_OUTX_L_G          0x22   // 12B burst: GX,GY,GZ,AX,AY,AZ
#define CTRL1_OP_MODE_HP          (0x00 << 4)
#define CTRL1_ODR_240HZ           (0x07 << 0)
// CTRL8 FS_XL[1:0]: 00=2g, 01=4g, 10=8g, 11=16g. Sketch 12 mislabels 0x02 as
// "4g" (it's actually 8g) -- keep the label meaning here honest so LSB_TO_G
// stays consistent. LSB_TO_G = 1/8192 requires 4g FS.
#define CTRL8_FS_XL_4G            (0x01 << 0)
#define CTRL3_SW_RESET            (1 << 0)
#define CTRL3_IF_INC              (1 << 2)
#define CTRL3_BDU                 (1 << 6)

// ═════════════════════════════════════════════════════════════════════════════
// PCF85063A registers
// ═════════════════════════════════════════════════════════════════════════════
#define RTC_REG_SECONDS      0x04
#define RTC_OS_BIT           0x80

// ═════════════════════════════════════════════════════════════════════════════
// BQ25619 ship mode
// ═════════════════════════════════════════════════════════════════════════════
#define BQ_REG_MISC_OP        0x07
#define BQ_REG_STATUS         0x08
#define BQ_STATUS_PG          (1 << 2)
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)

// ═════════════════════════════════════════════════════════════════════════════
// Button + LED timings
// ═════════════════════════════════════════════════════════════════════════════
#define BTN_DEBOUNCE_MS      30
#define BTN_LONG_HOLD_MS     3000
#define WS_LEVEL             26

// ═════════════════════════════════════════════════════════════════════════════
// Signal chain constants (see docstring pipeline block above)
//
// Sample rates are MEASURED, not target: MAX is throttled by SMP_AVE_8 (FIFO
// updates at 100 Hz / 8 = 12.5 Hz) and LSM by the outer loop cadence (~150 Hz
// wall vs. its 240 Hz internal ODR). Biquad coefficients below assume these
// measured rates -- get them out of sync with reality and the bandpass corner
// migrates and rings on step edges.
// ═════════════════════════════════════════════════════════════════════════════
#define MAX_SR_HZ            12.5f
#define LSM_SR_HZ            150.0f

// MAX order-1 Butterworth bandpass 0.5..3.5 Hz @ 12.5 Hz (single biquad).
// Note: 3.5 Hz upper vs. 4 Hz because Nyquist=6.25 -- 4 Hz is too close to
// the edge for a stable 2nd-order design. Still covers 30..210 BPM.
//   scipy: butter(1, [0.5, 3.5], btype='band', fs=12.5)
#define MAX_BP_B0    0.484287f
#define MAX_BP_B1    0.000000f
#define MAX_BP_B2   -0.484287f
#define MAX_BP_A1   -0.758148f
#define MAX_BP_A2    0.031426f

// LSM order-1 Butterworth bandpass 0.8..15 Hz @ 150 Hz (single biquad).
//   scipy: butter(1, [0.8, 15.0], btype='band', fs=150.0)
#define LSM_BP_B0    0.234593f
#define LSM_BP_B1    0.000000f
#define LSM_BP_B2   -0.234593f
#define LSM_BP_A1   -1.514235f
#define LSM_BP_A2    0.530814f

// Peak-detect + motion thresholds
#define BEAT_REFRACTORY_MS   250     // 240 BPM cap
#define MOTION_HANG_MS       500     // motion flag stays high N ms after trigger
#define QUAL_PI_REF          0.02f   // 2% PI -> PI_norm = 1.0
#define QUAL_HISTORY         8       // last N IBIs for CV computation

// Autocorrelation cross-check
#define AUTOCORR_WINDOW_S    5
#define AUTOCORR_PERIOD_MS   2000
// Sized for measured rates. Static arrays -- integer literals required.
#define MAX_AUTOCORR_N_MAX   64      // 12.5 Hz * 5 s = 62.5, round up
#define LSM_AUTOCORR_N_MAX   750     // 150 Hz * 5 s

// ═════════════════════════════════════════════════════════════════════════════
// I2C primitives (identical shape to sketches 6-12)
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
static uint8_t bcd_to_dec(uint8_t b) { return ((b >> 4) * 10) + (b & 0x0F); }

// ═════════════════════════════════════════════════════════════════════════════
// MAX30101 setup + FIFO drain
// ═════════════════════════════════════════════════════════════════════════════
static bool max_init(uint8_t green_pa) {
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_MODE_CONFIG, M_MODE_RESET);
    uint32_t t0 = millis();
    while ((i2c_read_reg(Wire, MAX30101_ADDR, M_REG_MODE_CONFIG) & M_MODE_RESET) &&
           (millis() - t0 < 200)) delay(2);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_FIFO_CONFIG,
                  (uint8_t)(0x50 | (M_SMP_AVE_8 << 5)));
    uint8_t spo2 = (M_ADC_16384 << 5) | (M_SR_100 << 2) | M_PW_411;
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_SPO2_CONFIG, spo2);
    // Green-only: slot1 = LED3 (green), other slots disabled.
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_MULTI_LED_1, 0x03);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_MULTI_LED_2, 0x00);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_LED1_PA, 0x00);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_LED2_PA, 0x00);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_LED3_PA, green_pa);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_MODE_CONFIG, M_MODE_MULTI_LED);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_FIFO_WR_PTR, 0);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_OVF_CTR,     0);
    i2c_write_reg(Wire, MAX30101_ADDR, M_REG_FIFO_RD_PTR, 0);
    uint8_t rb_mode = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_MODE_CONFIG);
    return rb_mode == M_MODE_MULTI_LED;
}
static bool max_set_green_pa(uint8_t pa) {
    return i2c_write_reg(Wire, MAX30101_ADDR, M_REG_LED3_PA, pa);
}
static uint8_t max_read_fifo(uint32_t *out_buf, uint8_t max_samples) {
    uint8_t wr = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_FIFO_WR_PTR);
    uint8_t rd = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_FIFO_RD_PTR);
    uint8_t avail = (uint8_t)((wr - rd) & 0x1F);
    if (avail == 0) return 0;
    if (avail > max_samples) avail = max_samples;
    uint8_t bytes[96];
    size_t n = (size_t)avail * 3;
    if (!i2c_read_buf(Wire, MAX30101_ADDR, M_REG_FIFO_DATA, bytes, n)) return 0;
    for (uint8_t i = 0; i < avail; i++) {
        uint32_t v = ((uint32_t)bytes[i*3+0] << 16) |
                     ((uint32_t)bytes[i*3+1] <<  8) |
                     ((uint32_t)bytes[i*3+2]);
        v &= 0x0003FFFF;   // 18-bit
        out_buf[i] = v;
    }
    return avail;
}

// ═════════════════════════════════════════════════════════════════════════════
// LSM6DSV16X: HP mode, 240 Hz, +/-4g, accel-only. Same knobs as sketch 12.
// ═════════════════════════════════════════════════════════════════════════════
static bool lsm_init(void) {
    if (!i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, CTRL3_SW_RESET)) return false;
    delay(20);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, CTRL3_BDU | CTRL3_IF_INC);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL8,  CTRL8_FS_XL_4G);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1,  CTRL1_OP_MODE_HP | CTRL1_ODR_240HZ);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2,  0x00);   // gyro off
    delay(10);
    return true;
}
static bool lsm_read_accel(float *ax, float *ay, float *az) {
    uint8_t b[12] = {0};
    if (!i2c_read_buf(Wire, LSM6DSV_ADDR, LSM_REG_OUTX_L_G, b, 12)) return false;
    int16_t xr = (int16_t)((uint16_t)b[6]  | ((uint16_t)b[7]  << 8));
    int16_t yr = (int16_t)((uint16_t)b[8]  | ((uint16_t)b[9]  << 8));
    int16_t zr = (int16_t)((uint16_t)b[10] | ((uint16_t)b[11] << 8));
    const float LSB_TO_G = 1.0f / 8192.0f;   // 0.122 mg/LSB at +/-4g FS
    *ax = xr * LSB_TO_G;
    *ay = yr * LSB_TO_G;
    *az = zr * LSB_TO_G;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// PCF85063A RTC read (ISO string) -- for filenames
//
// The OS ("oscillator stopped") flag in Seconds bit 7 latches to 1 on any
// power interruption and stays set until explicitly cleared by a fresh
// SET_TIME. In practice the RTC keeps running fine off the coin cell, so
// the ESP-IDF firmware ignores this bit and uses the time anyway. Do the
// same here: validate year is sane instead of trusting OS.
// ═════════════════════════════════════════════════════════════════════════════
static bool rtc_read(char *iso_date, char *iso_time) {
    // iso_date: "YYYYMMDD" (8+null),  iso_time: "HHMMSS" (6+null)
    uint8_t r[7] = {0};
    if (!i2c_read_buf(Wire, PCF85063A_ADDR, RTC_REG_SECONDS, r, 7)) return false;
    uint8_t sec = bcd_to_dec(r[0] & 0x7F);
    uint8_t min = bcd_to_dec(r[1] & 0x7F);
    uint8_t hr  = bcd_to_dec(r[2] & 0x3F);
    uint8_t day = bcd_to_dec(r[3] & 0x3F);
    uint8_t mon = bcd_to_dec(r[5] & 0x1F);
    uint16_t yr = 2000 + bcd_to_dec(r[6]);
    bool sane = (yr >= 2024 && yr <= 2099 &&
                 mon >= 1  && mon <= 12 &&
                 day >= 1  && day <= 31 &&
                 hr  <= 23 && min <= 59 && sec <= 59);
    if (!sane) return false;
    snprintf(iso_date, 9, "%04u%02u%02u", yr, mon, day);
    snprintf(iso_time, 7, "%02u%02u%02u", hr, min, sec);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// RGB helpers + ship mode (same shape as sketch 12)
// ═════════════════════════════════════════════════════════════════════════════
static void rgb(uint8_t r, uint8_t g, uint8_t b) {
    neopixelWrite(PIN_WS2812_DIN, r, g, b);
}
static void rgb_off(void) { rgb(0, 0, 0); }
static void rgb_pa_color(uint8_t pa) {
    // Same palette as sketch 10: blue->green->yellow->orange->red as PA rises.
    // Interpolated so a continuous ramp looks like a rainbow, not steps.
    float t = (float)(pa - LED_PA_MIN) / (float)(LED_PA_MAX - LED_PA_MIN);
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    // 5-stop palette, piecewise linear
    struct Stop { float t; uint8_t r, g, b; };
    static const Stop stops[] = {
        { 0.00f, 0,        0,        WS_LEVEL },   // blue   (dim)
        { 0.25f, 0,        WS_LEVEL, 0        },   // green
        { 0.50f, WS_LEVEL, WS_LEVEL, 0        },   // yellow
        { 0.75f, WS_LEVEL, WS_LEVEL/2, 0      },   // orange
        { 1.00f, WS_LEVEL, 0,        0        },   // red    (max)
    };
    const int n = 5;
    for (int i = 0; i < n - 1; i++) {
        if (t >= stops[i].t && t <= stops[i+1].t) {
            float u = (t - stops[i].t) / (stops[i+1].t - stops[i].t);
            uint8_t r = (uint8_t)(stops[i].r + u * (stops[i+1].r - stops[i].r));
            uint8_t g = (uint8_t)(stops[i].g + u * (stops[i+1].g - stops[i].g));
            uint8_t b = (uint8_t)(stops[i].b + u * (stops[i+1].b - stops[i].b));
            rgb(r, g, b);
            return;
        }
    }
    rgb(stops[n-1].r, stops[n-1].g, stops[n-1].b);
}
static void rgb_idle_breathe(void) {
    float phase = ((float)(millis() % 3000)) / 3000.0f * 2.0f * (float)M_PI;
    uint8_t v = (uint8_t)(WS_LEVEL/2 * (0.5f + 0.5f * sinf(phase)));
    rgb(0, v/2, v);   // dim cyan breathe
}
static void rgb_pink_flash(void) {
    bool on = ((millis() / 150) % 2) == 0;
    rgb(on ? WS_LEVEL : 0, 0, on ? (WS_LEVEL/2) : 0);
}
static bool bq_ok = false;
static void enter_ship_mode(void) {
    if (!bq_ok) { Serial.println("[SHIP] BQ dead -- skipping"); return; }
    Serial.println("[SHIP] long-hold -> BATFET off (2 s red countdown)");
    for (uint32_t i = 0; i < 50; i++) {
        float s = 0.5f * (1.0f - cosf((float)i / 15.9155f * (float)M_PI));
        rgb((uint8_t)(s * WS_LEVEL), 0, 0);
        delay(40);
    }
    rgb(WS_LEVEL, 0, 0);
    if (digitalRead(PIN_BUTTON) == LOW) { Serial.println("[SHIP] still held -> abort"); rgb_off(); return; }
    uint8_t r07 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP);
    uint8_t r07_new = r07 | BQ_BATFET_DIS | BQ_BATFET_RST_WVBUS;
    r07_new &= ~BQ_BATFET_DLY;
    r07_new &= ~BQ_BATFET_RST_EN;
    i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP, r07_new);
    delay(50);
    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    if (st & BQ_STATUS_PG) { Serial.println("[SHIP] USB present -- fires on unplug"); rgb_off(); }
    else                   { Serial.println("[SHIP] BATFET off"); Serial.flush(); while (1) delay(100); }
}

// ═════════════════════════════════════════════════════════════════════════════
// Button state machine  (single + long-hold; no double-click needed here)
// ═════════════════════════════════════════════════════════════════════════════
typedef enum { BTN_IDLE, BTN_PRESSED, BTN_LONG_FIRED } btn_state_t;
static btn_state_t g_btn_state = BTN_IDLE;
static uint32_t    g_btn_last_change = 0;
static uint32_t    g_btn_press_ms    = 0;
static bool        g_btn_prev_low    = false;
static bool        g_btn_single_pending = false;

static void handle_button(void) {
    uint32_t now = millis();
    bool low = (digitalRead(PIN_BUTTON) == LOW);
    if (low != g_btn_prev_low && (now - g_btn_last_change) >= BTN_DEBOUNCE_MS) {
        g_btn_last_change = now;
        g_btn_prev_low = low;
        if (low) {
            g_btn_state = BTN_PRESSED;
            g_btn_press_ms = now;
        } else {
            if (g_btn_state == BTN_PRESSED) {
                g_btn_single_pending = true;      // consumed by main loop
            }
            g_btn_state = BTN_IDLE;
        }
    }
    if (g_btn_state == BTN_PRESSED && (now - g_btn_press_ms) >= BTN_LONG_HOLD_MS) {
        g_btn_state = BTN_LONG_FIRED;
        enter_ship_mode();
    }
}
static bool consume_single_click(void) {
    if (g_btn_single_pending) { g_btn_single_pending = false; return true; }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Per-channel state -- chan_t declared in types.h so Arduino auto-prototype
// pass sees it. QUAL_HISTORY is baked as literal `8` in types.h; keep the
// two in sync if you change it here.
// ═════════════════════════════════════════════════════════════════════════════

// Backing storage for the two autocorrelation ring buffers.
// MAX: 5 s @ 100 Hz = 500 floats. LSM: 5 s @ 240 Hz = 1200 floats.
// These are the largest static allocations in the sketch.
static float g_max_ac_ring[MAX_AUTOCORR_N_MAX];
static float g_lsm_ac_ring[LSM_AUTOCORR_N_MAX];

static chan_t g_max_ch;
static chan_t g_lsm_ch;

static void chan_init(chan_t *c, float b0, float b1, float b2, float a1, float a2,
                       float *ring, int ring_cap)
{
    memset(c, 0, sizeof(*c));
    c->baseline_start_ms = millis();
    c->bp_b0 = b0; c->bp_b1 = b1; c->bp_b2 = b2;
    c->bp_a1 = a1; c->bp_a2 = a2;
    c->ac_ring = ring;
    c->ac_ring_cap = ring_cap;
    c->last_autocorr_ms = millis();
}

static void chan_reset_step_aggregate(chan_t *c) {
    c->step_samples = 0;
    c->step_dc_sum = 0.0f;
    c->step_ac_absmax = 0.0f;
    c->step_motion_samples = 0;
    c->step_beats = 0;
}

// Autocorrelation over the ring buffer (search lag range mapped to 30..240 BPM).
// Cost: O(N * lag_range). At MAX N=500, lag_range~=250, ~125k mul-adds per call
// once per 2 s -- easily fits on ESP32-S3 core 0.
static void chan_autocorr(chan_t *c, float sr_hz) {
    if (c->ac_ring_n < c->ac_ring_cap / 2) return;   // not enough data yet
    int n = c->ac_ring_n;
    int lag_min = (int)(sr_hz * 60.0f / 240.0f);
    int lag_max = (int)(sr_hz * 60.0f / 30.0f);
    if (lag_max > n / 2) lag_max = n / 2;
    if (lag_min < 2) lag_min = 2;
    // Zero-mean the buffer
    float mean = 0.0f;
    for (int i = 0; i < n; i++) {
        int idx = (c->ac_ring_head + i) % c->ac_ring_cap;
        mean += c->ac_ring[idx];
    }
    mean /= (float)n;
    // R(0)
    float r0 = 0.0f;
    for (int i = 0; i < n; i++) {
        int idx = (c->ac_ring_head + i) % c->ac_ring_cap;
        float v = c->ac_ring[idx] - mean;
        r0 += v * v;
    }
    if (r0 < 1e-9f) return;
    // Scan lags
    float best_r = -1.0f;
    int   best_lag = 0;
    for (int lag = lag_min; lag <= lag_max; lag++) {
        float r = 0.0f;
        int m = n - lag;
        for (int i = 0; i < m; i++) {
            int ia = (c->ac_ring_head + i) % c->ac_ring_cap;
            int ib = (c->ac_ring_head + i + lag) % c->ac_ring_cap;
            r += (c->ac_ring[ia] - mean) * (c->ac_ring[ib] - mean);
        }
        r /= r0;
        if (r > best_r) { best_r = r; best_lag = lag; }
    }
    if (best_lag > 0 && best_r > 0.0f) {
        c->autocorr_bpm = 60.0f * sr_hz / (float)best_lag;
        c->autocorr_peak_ratio = best_r;   // typical values: 0.2..0.9
    }
}

// Feed one sample into a channel's pipeline. Returns the AC value.
// motion_slope_thresh is a multiplier (e.g. 4.0) on the running slope envelope.
// Baseline EMA + bandpass + motion gate + peak detect all happen here.
static float chan_process(chan_t *c, float x, float sr_hz, uint32_t now_ms,
                          float motion_slope_thresh)
{
    // 1. Baseline EMA (DC follower)
    if (c->baseline == 0.0f && !c->baseline_ready) c->baseline = x;
    uint32_t elapsed = now_ms - c->baseline_start_ms;
    float alpha = (elapsed < 3000) ? 0.10f : 0.02f;
    c->baseline = c->baseline * (1.0f - alpha) + x * alpha;
    if (elapsed >= 3000) c->baseline_ready = true;

    float ac = x - c->baseline;

    // 2. Bandpass (Butterworth biquad, Direct-Form I on ac)
    float bp = c->bp_b0 * ac + c->bp_b1 * c->bp_x1 + c->bp_b2 * c->bp_x2
              - c->bp_a1 * c->bp_y1 - c->bp_a2 * c->bp_y2;
    c->bp_x2 = c->bp_x1; c->bp_x1 = ac;
    c->bp_y2 = c->bp_y1; c->bp_y1 = bp;
    c->bp_out = bp;

    // 3. Envelope of |bp| (leaky max)
    float bp_abs = fabsf(bp);
    if (bp_abs > c->env) c->env = bp_abs;
    else                 c->env *= 0.9995f;

    // 4. Motion gate on d(ac)/dt vs its running envelope
    float d_ac = fabsf(ac - c->ac_prev);
    if (d_ac > c->slope_env) c->slope_env = d_ac;
    else                     c->slope_env *= 0.995f;
    float slope_floor = (c->env * 0.5f > 0.0f) ? c->env * 0.5f : 0.0f;
    float slope_thresh = motion_slope_thresh * (c->slope_env > slope_floor ? c->slope_env : slope_floor);
    if (d_ac > slope_thresh && slope_thresh > 0.0f && c->baseline_ready) {
        c->motion_until_ms = now_ms + MOTION_HANG_MS;
    }
    c->ac_prev = ac;

    // 5. Adaptive-threshold peak detector on bp
    float thresh = c->env * 0.30f;
    if (thresh < 1e-9f) thresh = 1e-9f;
    bool in_motion = (now_ms < c->motion_until_ms);
    if (c->baseline_ready && !in_motion) {
        if (c->peak_prev > bp && c->peak_prev > thresh &&
            (now_ms - c->last_beat_ms) > BEAT_REFRACTORY_MS) {
            if (c->last_beat_ms > 0) {
                float ibi = (float)(now_ms - c->last_beat_ms);
                if (ibi > 250.0f && ibi < 2000.0f) {
                    float bpm_i = 60000.0f / ibi;
                    c->bpm_pk = (c->bpm_pk == 0.0f) ? bpm_i
                                : c->bpm_pk * 0.7f + bpm_i * 0.3f;
                    c->ibi_hist[c->ibi_hist_i] = ibi;
                    c->ibi_hist_i = (c->ibi_hist_i + 1) % QUAL_HISTORY;
                    if (c->ibi_hist_n < QUAL_HISTORY) c->ibi_hist_n++;
                    c->beat_count_valid++;
                }
            }
            c->last_beat_ms = now_ms;
            c->beat_count++;
            c->step_beats++;
        }
    }
    c->peak_prev = bp;

    // 6. Autocorr buffer push (ring)
    c->ac_ring[c->ac_ring_head] = bp;
    c->ac_ring_head = (c->ac_ring_head + 1) % c->ac_ring_cap;
    if (c->ac_ring_n < c->ac_ring_cap) c->ac_ring_n++;

    // 7. Per-step aggregate
    c->step_samples++;
    c->step_dc_sum += x;
    if (bp_abs > c->step_ac_absmax) c->step_ac_absmax = bp_abs;
    if (in_motion) c->step_motion_samples++;

    return ac;
}

// Compose the quality metric [0..100] from PI, IBI regularity, motion cleanliness,
// autocorr agreement. Geometric mean punishes any single-axis failure.
static void chan_update_quality(chan_t *c) {
    // PI (perfusion index) proxy: env/|baseline|. Note: for LSM 'baseline' is
    // ~1.0 g (gravity magnitude) so PI is dimensionally still "AC over DC" and
    // the QUAL_PI_REF constant just needs to be tuned to expected ranges.
    float dc = fabsf(c->baseline);
    float pi = (dc > 1e-9f) ? (c->env / dc) : 0.0f;
    float pi_norm = pi / QUAL_PI_REF;
    if (pi_norm > 1.0f) pi_norm = 1.0f;

    // IBI CV (coefficient of variation over history)
    float reg = 0.0f;
    if (c->ibi_hist_n >= 3) {
        float mean = 0.0f;
        for (uint8_t i = 0; i < c->ibi_hist_n; i++) mean += c->ibi_hist[i];
        mean /= (float)c->ibi_hist_n;
        float var = 0.0f;
        for (uint8_t i = 0; i < c->ibi_hist_n; i++) {
            float d = c->ibi_hist[i] - mean;
            var += d * d;
        }
        float sd = sqrtf(var / (float)c->ibi_hist_n);
        float cv = (mean > 1.0f) ? sd / mean : 1.0f;
        reg = 1.0f / (1.0f + cv);
    }

    // Motion cleanliness on the current step aggregate
    float motion_frac = (c->step_samples > 0)
        ? ((float)c->step_motion_samples / (float)c->step_samples) : 0.0f;
    float mc = 1.0f - motion_frac;
    if (mc < 0.0f) mc = 0.0f;

    // Autocorr agreement with peak-detect BPM
    float agr = 0.0f;
    if (c->bpm_pk > 0.0f && c->autocorr_bpm > 0.0f) {
        float d = fabsf(c->bpm_pk - c->autocorr_bpm);
        agr = 1.0f - (d / 60.0f);   // 60 BPM disagreement = 0 agreement
        if (agr < 0.0f) agr = 0.0f;
    }

    float prod = pi_norm * reg * mc * agr;
    if (prod < 0.0f) prod = 0.0f;
    c->quality = 100.0f * powf(prod, 0.25f);
}

// ═════════════════════════════════════════════════════════════════════════════
// SD card + files
// ═════════════════════════════════════════════════════════════════════════════
static bool     g_sd_ok = false;
static File     g_csv_file;
static bool     g_csv_open = false;
static char     g_session_stem[64];   // e.g. "sk13_20260809_182311_0"
static char     g_csv_path[96];
static char     g_json_path[96];

// SD path constants -- data lives under /data/sk13_data/ from now on.
static const char *SK13_DIR = "/data/sk13_data";

static uint16_t g_next_session_n = 0;   // 1 + max sk13_NNNN_* on card at boot

// Scan SK13_DIR for existing sk13_NNNN_*.csv, return highest N + 1 (or 0 empty).
static uint16_t sd_scan_next_index(void) {
    File d = SD_MMC.open(SK13_DIR);
    if (!d || !d.isDirectory()) { if (d) d.close(); return 0; }
    uint16_t maxN = 0;
    bool found = false;
    File f;
    while ((f = d.openNextFile())) {
        String name = String(f.name());
        f.close();
        int slash = name.lastIndexOf('/');
        String base = (slash >= 0) ? name.substring(slash + 1) : name;
        // Expect prefix "sk13_" then N (digits) then "_"
        if (!base.startsWith("sk13_")) continue;
        int under = base.indexOf('_', 5);
        if (under < 5) continue;
        String num_str = base.substring(5, under);
        long n = num_str.toInt();
        // toInt returns 0 on non-numeric -- guard by verifying at least one digit
        if (num_str.length() == 0 || (num_str[0] < '0' || num_str[0] > '9')) continue;
        if (n > (long)maxN) { maxN = (uint16_t)n; }
        found = true;
    }
    d.close();
    return found ? (uint16_t)(maxN + 1) : 0;
}

static bool sd_mount(void) {
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    if (!SD_MMC.begin("/sdcard", true, false, 20000)) {
        Serial.println("[SD  ] mount FAILED");
        return false;
    }
    Serial.printf("[SD  ] mounted, card size %llu MB\n",
                  (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
    SD_MMC.mkdir("/data");        // parent
    SD_MMC.mkdir(SK13_DIR);
    g_next_session_n = sd_scan_next_index();
    Serial.printf("[SD  ] %s: next session index = %u\n",
                  SK13_DIR, (unsigned)g_next_session_n);
    return true;
}

// Filename format:  sk13_NNNN_YYYYMMDD_HHMMSS.csv     (RTC available)
//              or:  sk13_NNNN_bootMSxxxxxxx.csv       (RTC dead)
// NNNN = monotonic session index scanned from SD at boot, ++ per session.
// This makes filenames sort by run order across reboots and gives you a real
// timestamp for the ones where the RTC is up.
static void build_session_paths(void) {
    char d[9], t[7];
    if (rtc_read(d, t)) {
        snprintf(g_session_stem, sizeof(g_session_stem),
                 "sk13_%04u_%s_%s", (unsigned)g_next_session_n, d, t);
    } else {
        snprintf(g_session_stem, sizeof(g_session_stem),
                 "sk13_%04u_bootMs%lu",
                 (unsigned)g_next_session_n,
                 (unsigned long)millis());
    }
    g_next_session_n++;
    snprintf(g_csv_path,  sizeof(g_csv_path),  "%s/%s.csv",  SK13_DIR, g_session_stem);
    snprintf(g_json_path, sizeof(g_json_path), "%s/%s.json", SK13_DIR, g_session_stem);
}

// CSV: interleaved by source. `src` = 'M' for MAX row, 'L' for LSM row.
// Empty (dash) for fields that don't apply to the row's source, so pandas
// (or any csv reader) still sees a consistent column count.
//
// t_ms, src, led_pa, raw, baseline, ac, bp, motion, beat, bpm_pk, bpm_ac, quality
static bool csv_open_session(void) {
    if (!g_sd_ok) return false;
    g_csv_file = SD_MMC.open(g_csv_path, FILE_WRITE);
    if (!g_csv_file) return false;
    g_csv_file.println("t_ms,src,led_pa,raw,baseline,ac,bp,motion,beat,bpm_pk,bpm_ac,quality");
    g_csv_open = true;
    return true;
}
static void csv_close_session(void) {
    if (g_csv_open) {
        g_csv_file.flush();
        g_csv_file.close();
        g_csv_open = false;
    }
}
static void csv_row_max(uint32_t t_ms, uint8_t pa, uint32_t raw, chan_t *c, bool beat_flag) {
    if (!g_csv_open) return;
    g_csv_file.printf("%lu,M,%u,%lu,%.1f,%.1f,%.2f,%d,%d,%.1f,%.1f,%.1f\n",
                      (unsigned long)t_ms, (unsigned)pa, (unsigned long)raw,
                      c->baseline, (float)raw - c->baseline, c->bp_out,
                      (millis() < c->motion_until_ms) ? 1 : 0,
                      beat_flag ? 1 : 0,
                      c->bpm_pk, c->autocorr_bpm, c->quality);
}
static void csv_row_lsm(uint32_t t_ms, float mag_g, chan_t *c, bool beat_flag) {
    if (!g_csv_open) return;
    g_csv_file.printf("%lu,L,-,%.5f,%.5f,%.5f,%.5f,%d,%d,%.1f,%.1f,%.1f\n",
                      (unsigned long)t_ms, mag_g,
                      c->baseline, mag_g - c->baseline, c->bp_out,
                      (millis() < c->motion_until_ms) ? 1 : 0,
                      beat_flag ? 1 : 0,
                      c->bpm_pk, c->autocorr_bpm, c->quality);
}
static void csv_flush(void) { if (g_csv_open) g_csv_file.flush(); }

// step_summary_t declared in types.h (see chan_t comment above).
static step_summary_t g_steps[SWEEP_STEPS];
static int g_step_count = 0;

static void snapshot_step_from_channels(step_summary_t *s, uint8_t pa,
                                        uint32_t t_start, uint32_t t_end)
{
    s->led_pa = pa;
    s->t_start_ms = t_start;
    s->t_end_ms = t_end;
    // MAX
    float max_dc = (g_max_ch.step_samples > 0)
        ? (g_max_ch.step_dc_sum / (float)g_max_ch.step_samples) : 0.0f;
    s->max_dc_mean = max_dc;
    s->max_pi = (max_dc > 1e-9f) ? (g_max_ch.step_ac_absmax / max_dc) : 0.0f;
    s->max_bpm_pk = g_max_ch.bpm_pk;
    s->max_bpm_ac = g_max_ch.autocorr_bpm;
    s->max_motion_pct = (g_max_ch.step_samples > 0)
        ? (100.0f * (float)g_max_ch.step_motion_samples / (float)g_max_ch.step_samples) : 0.0f;
    s->max_quality = g_max_ch.quality;
    s->max_beats = g_max_ch.step_beats;
    // LSM
    s->lsm_bpm_pk = g_lsm_ch.bpm_pk;
    s->lsm_bpm_ac = g_lsm_ch.autocorr_bpm;
    s->lsm_motion_pct = (g_lsm_ch.step_samples > 0)
        ? (100.0f * (float)g_lsm_ch.step_motion_samples / (float)g_lsm_ch.step_samples) : 0.0f;
    s->lsm_quality = g_lsm_ch.quality;
    s->lsm_beats = g_lsm_ch.step_beats;
}

static void json_write_session(uint8_t winner_pa, float winner_q,
                                const char *rtc_iso_at_start)
{
    if (!g_sd_ok) return;
    File f = SD_MMC.open(g_json_path, FILE_WRITE);
    if (!f) return;
    f.printf("{\n");
    f.printf("  \"sketch\": \"13_adv_heartrate_monitor\",\n");
    f.printf("  \"version\": \"0.1.0\",\n");
    f.printf("  \"session_id\": \"%s\",\n", g_session_stem);
    f.printf("  \"started_at\": \"%s\",\n", rtc_iso_at_start);
    f.printf("  \"sweep\": { \"duration_s\": %d, \"steps\": %d, "
             "\"pa_min\": %d, \"pa_max\": %d },\n",
             SWEEP_DURATION_S, SWEEP_STEPS, LED_PA_MIN, LED_PA_MAX);
    f.printf("  \"steps\": [\n");
    for (int i = 0; i < g_step_count; i++) {
        const step_summary_t *s = &g_steps[i];
        f.printf("    { \"i\": %d, \"led_pa\": %u, \"t_start_ms\": %lu, \"t_end_ms\": %lu,\n",
                 i, (unsigned)s->led_pa,
                 (unsigned long)s->t_start_ms, (unsigned long)s->t_end_ms);
        f.printf("      \"max\": { \"bpm_pk\": %.1f, \"bpm_ac\": %.1f, "
                 "\"pi\": %.5f, \"motion_pct\": %.1f, \"quality\": %.1f, "
                 "\"dc_mean\": %.1f, \"beats\": %lu },\n",
                 s->max_bpm_pk, s->max_bpm_ac, s->max_pi,
                 s->max_motion_pct, s->max_quality,
                 s->max_dc_mean, (unsigned long)s->max_beats);
        f.printf("      \"lsm\": { \"bpm_pk\": %.1f, \"bpm_ac\": %.1f, "
                 "\"motion_pct\": %.1f, \"quality\": %.1f, \"beats\": %lu } }%s\n",
                 s->lsm_bpm_pk, s->lsm_bpm_ac,
                 s->lsm_motion_pct, s->lsm_quality, (unsigned long)s->lsm_beats,
                 (i == g_step_count - 1) ? "" : ",");
    }
    f.printf("  ],\n");
    f.printf("  \"winner_pa\": %u,\n", (unsigned)winner_pa);
    f.printf("  \"winner_quality\": %.1f\n", winner_q);
    f.printf("}\n");
    f.flush();
    f.close();
}

// ═════════════════════════════════════════════════════════════════════════════
// Output helpers (plotter / serial mode)
// ═════════════════════════════════════════════════════════════════════════════
#if OUT_MODE == OUT_MODE_PLOTTER
static void plot_dual(uint32_t max_raw, chan_t *m, float lsm_mag_g, chan_t *l) {
    // MAX slice: raw, baseline, ac_clipped, bp_clipped, motion*1000
    float m_ac = (float)max_raw - m->baseline;
    float m_bp = m->bp_out;
    if (m_ac >  MAX_AC_CLIP) m_ac =  MAX_AC_CLIP;
    if (m_ac < -MAX_AC_CLIP) m_ac = -MAX_AC_CLIP;
    if (m_bp >  MAX_BP_CLIP) m_bp =  MAX_BP_CLIP;
    if (m_bp < -MAX_BP_CLIP) m_bp = -MAX_BP_CLIP;
    int m_motion = (millis() < m->motion_until_ms) ? 1000 : 0;
    // LSM slice: bcg_raw*1e5, bp*1e5 (mg-scale), motion*1000
    float l_disp = (lsm_mag_g - l->baseline) * 1e5f;   // ~ug scale
    float l_bp_disp = l->bp_out * 1e5f;
    if (l_bp_disp >  LSM_BP_CLIP_G * 1e5f) l_bp_disp =  LSM_BP_CLIP_G * 1e5f;
    if (l_bp_disp < -LSM_BP_CLIP_G * 1e5f) l_bp_disp = -LSM_BP_CLIP_G * 1e5f;
    int l_motion = (millis() < l->motion_until_ms) ? 1000 : 0;
    Serial.printf("%lu\t%.1f\t%.1f\t%.1f\t%d\t%.1f\t%.1f\t%d\n",
                  (unsigned long)max_raw, m->baseline, m_ac, m_bp, m_motion,
                  l_disp, l_bp_disp, l_motion);
}
#else
static void serial_status_1hz(uint32_t sess_t_ms, uint8_t pa, int step,
                              uint32_t max_raw)
{
    static uint32_t last = 0;
    uint32_t now = millis();
    if ((now - last) < 1000) return;
    last = now;
    Serial.printf("[t=%5.1fs @LED=0x%02X step=%d]\n"
                  "  MAX  BPM=%.0f (ac=%.0f) PI=%.4f q=%.0f motion=%.0f%% base=%.0f raw=%lu\n"
                  "  LSM  BPM=%.0f (ac=%.0f) q=%.0f motion=%.0f%% env=%.5f\n",
                  (double)sess_t_ms / 1000.0, (unsigned)pa, step,
                  g_max_ch.bpm_pk, g_max_ch.autocorr_bpm,
                  (g_max_ch.baseline > 0 ? g_max_ch.env / g_max_ch.baseline : 0.0f),
                  g_max_ch.quality,
                  g_max_ch.step_samples > 0
                    ? 100.0f * (float)g_max_ch.step_motion_samples / (float)g_max_ch.step_samples : 0.0f,
                  g_max_ch.baseline, (unsigned long)max_raw,
                  g_lsm_ch.bpm_pk, g_lsm_ch.autocorr_bpm, g_lsm_ch.quality,
                  g_lsm_ch.step_samples > 0
                    ? 100.0f * (float)g_lsm_ch.step_motion_samples / (float)g_lsm_ch.step_samples : 0.0f,
                  g_lsm_ch.env);
}
#endif

// ═════════════════════════════════════════════════════════════════════════════
// The session (button-triggered)
// ═════════════════════════════════════════════════════════════════════════════
static void run_session(void) {
    // Countdown
    Serial.println("[SESS] countdown 3 s -- hold still");
    uint32_t t_cd = millis();
    while ((millis() - t_cd) < PRE_STILL_MS) {
        rgb_pink_flash();
        handle_button();
        delay(10);
    }

    // Session paths + files
    build_session_paths();
    char rtc_iso[24];
    { char d[9], t[7];
      if (rtc_read(d, t)) snprintf(rtc_iso, sizeof(rtc_iso),
                                    "%c%c%c%c-%c%c-%c%cT%c%c:%c%c:%c%c",
                                    d[0],d[1],d[2],d[3], d[4],d[5], d[6],d[7],
                                    t[0],t[1], t[2],t[3], t[4],t[5]);
      else snprintf(rtc_iso, sizeof(rtc_iso), "boot+%lums", (unsigned long)millis());
    }
    if (!csv_open_session()) {
        Serial.printf("[SD  ] failed to open %s -- session aborted\n", g_csv_path);
        rgb(WS_LEVEL, 0, 0);
        delay(1000);
        return;
    }
    Serial.printf("[SESS] logging -> %s\n", g_csv_path);

#if OUT_MODE == OUT_MODE_PLOTTER
    // Serial Plotter (Arduino IDE 1.x + 2.x) treats the first non-numeric
    // line as trace labels when values are tab-separated. Print it once
    // per session so the traces get named instead of showing as color-
    // only lines. Order must match plot_dual() below.
    Serial.println("max_raw\tmax_baseline\tmax_ac\tmax_bp\tmax_motion_x1000\t"
                   "lsm_ac_uG\tlsm_bp_uG\tlsm_motion_x1000");
#endif

    // Reset per-channel state (start fresh baselines etc. -- previous session
    // may have been minutes ago). Signal chain state resets HERE, at the START
    // of the session; it does NOT reset between sweep steps (rolling).
    chan_init(&g_max_ch,
              MAX_BP_B0, MAX_BP_B1, MAX_BP_B2, MAX_BP_A1, MAX_BP_A2,
              g_max_ac_ring, MAX_AUTOCORR_N_MAX);
    chan_init(&g_lsm_ch,
              LSM_BP_B0, LSM_BP_B1, LSM_BP_B2, LSM_BP_A1, LSM_BP_A2,
              g_lsm_ac_ring, LSM_AUTOCORR_N_MAX);
    chan_reset_step_aggregate(&g_max_ch);
    chan_reset_step_aggregate(&g_lsm_ch);

    // Sweep loop
    g_step_count = 0;
    const uint32_t step_dur_ms = (uint32_t)((SWEEP_DURATION_S * 1000UL) / SWEEP_STEPS);
    const uint8_t  step_pa_span = (uint8_t)((LED_PA_MAX - LED_PA_MIN) / SWEEP_STEPS);
    uint32_t session_start_ms = millis();

    for (int step = 0; step < SWEEP_STEPS; step++) {
        uint8_t pa = LED_PA_MIN + step * step_pa_span;
        max_set_green_pa(pa);
        rgb_pa_color(pa);
        chan_reset_step_aggregate(&g_max_ch);
        chan_reset_step_aggregate(&g_lsm_ch);
        uint32_t step_start_ms = millis();
        uint32_t step_end_ms   = step_start_ms + step_dur_ms;

        while (millis() < step_end_ms) {
            handle_button();

            // Drain MAX FIFO (0..N samples). Each sample -> pipeline + CSV row.
            uint32_t max_buf[16];
            uint8_t n_max = max_read_fifo(max_buf, 16);
            for (uint8_t i = 0; i < n_max; i++) {
                uint32_t now_ms = millis();
                uint32_t before_beats = g_max_ch.beat_count;
                (void)chan_process(&g_max_ch, (float)max_buf[i], MAX_SR_HZ, now_ms, 4.0f);
                bool beat_flag = (g_max_ch.beat_count != before_beats);
                csv_row_max(now_ms - session_start_ms, pa, max_buf[i], &g_max_ch, beat_flag);
#if OUT_MODE == OUT_MODE_PLOTTER
                float lsm_dummy = g_lsm_ch.baseline;   // hold last LSM value in plot
                plot_dual(max_buf[i], &g_max_ch, lsm_dummy, &g_lsm_ch);
#endif
            }

            // Read LSM once per outer tick. At 240 Hz internal ODR the accel
            // updates every ~4 ms; we read every ~5-10 ms so we oversample
            // slightly which is fine for the biquad.
            float ax, ay, az;
            if (lsm_read_accel(&ax, &ay, &az)) {
                float mag = sqrtf(ax*ax + ay*ay + az*az);   // ~1 g at rest
                uint32_t now_ms = millis();
                uint32_t before_beats = g_lsm_ch.beat_count;
                (void)chan_process(&g_lsm_ch, mag, LSM_SR_HZ, now_ms, 4.0f);
                bool beat_flag = (g_lsm_ch.beat_count != before_beats);
                csv_row_lsm(now_ms - session_start_ms, mag, &g_lsm_ch, beat_flag);
            }

            // Periodic autocorr for both channels
            uint32_t now = millis();
            if ((now - g_max_ch.last_autocorr_ms) >= AUTOCORR_PERIOD_MS) {
                chan_autocorr(&g_max_ch, MAX_SR_HZ);
                g_max_ch.last_autocorr_ms = now;
            }
            if ((now - g_lsm_ch.last_autocorr_ms) >= AUTOCORR_PERIOD_MS) {
                chan_autocorr(&g_lsm_ch, LSM_SR_HZ);
                g_lsm_ch.last_autocorr_ms = now;
            }
            chan_update_quality(&g_max_ch);
            chan_update_quality(&g_lsm_ch);

#if OUT_MODE == OUT_MODE_SERIAL
            uint32_t max_last = max_buf[n_max > 0 ? n_max - 1 : 0];
            serial_status_1hz(now - session_start_ms, pa, step, max_last);
#endif

            // Flush CSV every second-ish (durable per-row protection)
            static uint32_t last_flush = 0;
            if ((now - last_flush) >= 1000) { csv_flush(); last_flush = now; }

            delay(5);
        }

        // Step done -- snapshot summary
        if (g_step_count < SWEEP_STEPS) {
            snapshot_step_from_channels(&g_steps[g_step_count], pa,
                                        step_start_ms - session_start_ms,
                                        millis() - session_start_ms);
            g_step_count++;
        }
    }

    // Pick winner: highest MAX quality
    uint8_t winner_pa = LED_PA_MIN;
    float   winner_q  = -1.0f;
    for (int i = 0; i < g_step_count; i++) {
        if (g_steps[i].max_quality > winner_q) {
            winner_q  = g_steps[i].max_quality;
            winner_pa = g_steps[i].led_pa;
        }
    }
    csv_close_session();
    json_write_session(winner_pa, winner_q, rtc_iso);

    Serial.printf("[SESS] done. winner LED_PA=0x%02X quality=%.1f. wrote %s\n",
                  (unsigned)winner_pa, winner_q, g_json_path);
    rgb_pa_color(winner_pa);
}

// ═════════════════════════════════════════════════════════════════════════════
// setup / loop
// ═════════════════════════════════════════════════════════════════════════════
void setup(void) {
    Serial.begin(115200);
    delay(50);
    Serial.println();
    Serial.println("=== 13_adv_heartrate_monitor v0.1 ===");
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    Wire.begin (PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 400000);

    // Sensor bring-up
    if (!i2c_ping(Wire, MAX30101_ADDR)) { Serial.println("[MAX ] NO ACK"); }
    else {
        uint8_t part = i2c_read_reg(Wire, MAX30101_ADDR, M_REG_PART_ID);
        Serial.printf("[MAX ] PART_ID=0x%02X\n", part);
        max_init(LED_PA_MIN);
    }
    if (!i2c_ping(Wire, LSM6DSV_ADDR)) { Serial.println("[LSM ] NO ACK"); }
    else                                { lsm_init(); Serial.println("[LSM ] init ok"); }

    bq_ok = i2c_ping(Wire1, BQ25619_ADDR);
    Serial.printf("[BQ  ] %s\n", bq_ok ? "ACK" : "NO ACK");

    // RTC probe (not fatal -- filenames just fall back to boot-ms)
    { char d[9], t[7];
      if (rtc_read(d, t)) Serial.printf("[RTC ] %s %s\n", d, t);
      else                Serial.println("[RTC ] oscillator stopped or NACK -- boot-ms filenames"); }

    // SD mount
    g_sd_ok = sd_mount();

    Serial.printf("[CFG ] OUT_MODE=%s  SWEEP=%ds/%dsteps  LED_PA %02X..%02X\n",
                  OUT_MODE == OUT_MODE_PLOTTER ? "PLOTTER" : "SERIAL",
                  SWEEP_DURATION_S, SWEEP_STEPS, LED_PA_MIN, LED_PA_MAX);
    Serial.println("[MAIN] idle. single-click = start sweep. hold >=3s = ship mode.");
    rgb_off();
}

void loop(void) {
    handle_button();
    if (consume_single_click()) {
        run_session();
        Serial.println("[MAIN] back to idle.");
    } else {
        rgb_idle_breathe();
    }
    delay(20);
}
