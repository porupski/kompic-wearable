/**
 * 2_smoke_stage2_hotair.ino — Kompic Mk I, post-hot-air stage-2 smoke test
 *
 * Refactored 2026-07-05 as part of the Stage 5 close-out. Renamed from
 * 2_smoke_test_mk1_hotair in the pre-refactor legacy set. See
 * hardware/Reflow_info/Stage_5_Build_Report.md for context.
 *
 * WHEN TO USE: verify Stage-2 population on a freshly-reflowed Mk I board
 * BEFORE the battery is attached. At Stage 2 there is no battery, so this
 * sketch does not include a ship-mode double-click handler. If you flash
 * it to a battery-attached board, either keep USB connected the whole time
 * OR add the ship-mode block from 3_smoke_stage3_full first.
 *
 * Build progression on this board:
 *   Stage 1 (reflow, verified by 1_smoke_stage1_reflow1):
 *     ESP32-S3-WROOM-1U, BQ25619, TPS62840 3.2 V, XC6206 1.8 V,
 *     flashlight LED (GPIO41), button (BQ QON / GPIO16),
 *     PCF85063A RTC + crystal.
 *   Stage 2 (hot air, what this sketch covers):
 *     + USB-C JS16T (bottom-side, prevents further hot-plate cycles)
 *     + VEML6030 ALS         (bus 1 @ 0x10)
 *     + LIS3MDLTR magneto    (bus 1 @ 0x1C)
 *     + LSM6DSV16X IMU       (bus 1 @ 0x6B)
 *     + BME688 environmental (bus 1 @ 0x76)
 *     + MSM261DGT003 PDM mic (I2S: CLK GPIO47, DATA GPIO48)
 *     + WS2812B-2020 RGB     (one-wire DIN @ GPIO42)
 *
 * Sequence:
 *   1. Banner, LEDC/button init, boot flash (3x at 192 duty).
 *   2. Full I2C scan on both buses — flags every ACKing address.
 *   3. Per-chip identity probe (WHO_AM_I / chip ID / config readback).
 *      For BME688, also pulls the calibration block once for compensated
 *      T/P/H readings later.
 *   4. WS2812 colour walk (R / G / B / off, 250 ms each, dimmed).
 *   5. PDM mic init at 48 kHz (gives CLK = 3.072 MHz, in the mic's
 *      Standard Performance Mode window; 16 kHz + default x64 decimation
 *      lands in the 900 kHz–1.1 MHz dead band per the MSM261DGT003
 *      datasheet extract).  Discards the first ~25 ms then checks for
 *      sample-to-sample variation, not just non-zero peak.
 *   6. PASS/FAIL summary.
 *   7. Loop: dwell on each sensor for 3 s, printing one line every 0.5 s
 *      (so 6 lines per sensor visit).  Full cycle through 8 entries
 *      takes ~24 s.  Button (GPIO16) still toggles the flashlight on/off.
 *
 * Brightness tuning vs the previous revision:
 *   - WS2812 dimmed ~60 % (boot walk 64 -> 26, loop colour 48 -> 19).
 *   - Flashlight LEDC breathe lifted ~50 % (sine scaled x1.5, clamped to
 *     8-bit duty; floor 12 -> 18, ceiling 232 -> 255 with a brief plateau
 *     at peak).
 *
 * Arduino-ESP32 v3.x API (ledcAttach, neopixelWrite, ESP_I2S).
 * Board: "ESP32S3 Dev Module", USB CDC On Boot: Enabled.
 * Pins from: hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md
 * Mic config from: firmware/docs/datasheet_extracts/
 *                  20.19_PDM_Microphone_Extract_iv7.1_f0.0_2026-06-19.md
 */

#include <Wire.h>
#include <math.h>
#include "ESP_I2S.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_FLASHLIGHT    41
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1    // east edge — RTC + ALS/IMU/mag/env
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4    // west edge — BQ charger
#define PIN_SCL_BUS2       5
#define PIN_RTC_INT       15
#define PIN_LSM_INT1       8
#define PIN_WS2812_DIN    42
#define PIN_MIC_CLK       47
#define PIN_MIC_DATA      48

// ── I2C addresses (stage-2 expected) ──────────────────────────────────────────
#define VEML6030_ADDR     0x10
#define LIS3MDL_ADDR      0x1C
#define PCF85063A_ADDR    0x51
#define LSM6DSV_ADDR      0x6B
#define BME688_ADDR       0x76
#define BQ25619_ADDR      0x6A

// ── BQ25619 ───────────────────────────────────────────────────────────────────
#define BQ_REG_INPUT_SRC  0x00  // bit6 TS_IGNORE, bit5 BATSNS_DIS, bits4:0 IINDPM
#define BQ_REG_CTRL1      0x05  // bits5:4 WATCHDOG[1:0] (00 = disable)
#define BQ_REG_STATUS     0x08
#define BQ_REG_FAULT      0x09
#define BQ_REG_PART       0x0A
#define BQ_STATUS_PG      (1 << 2)
#define BQ_TS_IGNORE_BIT  (1 << 6)
#define BQ_WD_MASK        (0x03 << 4)

static const char *bq_ntc_name(uint8_t f) {
    switch (f & 0x07) {
        case 0b000: return "normal";
        case 0b010: return "warm";
        case 0b011: return "cool";
        case 0b101: return "cold";
        case 0b110: return "hot";
        default:    return "?";
    }
}

// ── PCF85063A ─────────────────────────────────────────────────────────────────
#define RTC_REG_CTRL1     0x00
#define RTC_REG_SECONDS   0x04
#define RTC_OS_BIT        0x80

// ── VEML6030 ──────────────────────────────────────────────────────────────────
#define VEML_REG_CONF     0x00
#define VEML_REG_ALS      0x04
#define VEML_REG_WHITE    0x05
#define VEML_CONF_RUN     ((uint16_t)((0x03 << 11) | (0x00 << 6)))  // gain 1/4x, IT=100 ms
#define VEML_LX_PER_CT_QUARTER_100MS  0.2304f

// ── LIS3MDLTR ─────────────────────────────────────────────────────────────────
#define LIS_REG_WHO       0x0F
#define LIS_WHO_VAL       0x3D
#define LIS_REG_CTRL1     0x20
#define LIS_REG_CTRL2     0x21
#define LIS_REG_CTRL3     0x22
#define LIS_REG_CTRL4     0x23
#define LIS_REG_CTRL5     0x24
#define LIS_REG_OUT_X_L   0x28
#define LIS_AUTO_INC      0x80
#define LIS_LSB_PER_GAUSS_4G  6842.0f

// ── LSM6DSV16X ────────────────────────────────────────────────────────────────
#define LSM_REG_WHO       0x0F
#define LSM_WHO_VAL       0x70
#define LSM_REG_CTRL1     0x10
#define LSM_REG_CTRL2     0x11
#define LSM_REG_CTRL3     0x12
#define LSM_REG_OUT_TEMP  0x20  // OUT_TEMP_L, gyro XYZ, accel XYZ — 14 bytes

// ── BME688 ────────────────────────────────────────────────────────────────────
#define BME_REG_CHIP_ID      0xD0
#define BME_REG_VARIANT_ID   0xF0
#define BME_REG_RESET        0xE0
#define BME_REG_CTRL_HUM     0x72
#define BME_REG_CTRL_GAS1    0x71
#define BME_REG_CTRL_MEAS    0x74
#define BME_REG_PRESS_ADC    0x1F  // 8 bytes: press[3], temp[3], hum[2]
#define BME_REG_CAL_BLK1     0x8A  // 23 bytes
#define BME_REG_CAL_BLK2     0xE1  // 10 bytes (par_h2/h1 thru par_t1)
#define BME_CHIP_ID_VAL      0x61

// ── PDM mic — datasheet-driven config ────────────────────────────────────────
// 48 kHz @ default x64 decimation -> CLK = 3.072 MHz (Standard Mode 1.1-4.0 MHz).
// Avoids the dead-band at 1.024 MHz that 16 kHz / x64 produces.
#define MIC_SAMPLE_RATE_HZ   48000

// ── LED PWM ───────────────────────────────────────────────────────────────────
#define LED_FREQ_HZ          1000
#define LED_RES_BITS         8

// ── WS2812 levels (dimmed 60 % from previous revision) ───────────────────────
#define WS_BOOT_LEVEL        26
#define WS_LOOP_LEVEL        19

// ── Loop dwell ───────────────────────────────────────────────────────────────
#define SENSOR_DWELL_MS      3000
#define SENSOR_PRINT_MS      500
#define DWELL_PRINTS         (SENSOR_DWELL_MS / SENSOR_PRINT_MS)  // 6
#define N_SENSORS            8

// ── BME688 calibration (filled at boot) ──────────────────────────────────────
typedef struct {
    uint16_t par_t1;
    int16_t  par_t2;
    int8_t   par_t3;
    uint16_t par_p1;
    int16_t  par_p2;
    int8_t   par_p3;
    int16_t  par_p4;
    int16_t  par_p5;
    int8_t   par_p6;
    int8_t   par_p7;
    int16_t  par_p8;
    int16_t  par_p9;
    uint8_t  par_p10;
    uint16_t par_h1;     // 12-bit
    uint16_t par_h2;     // 12-bit
    int8_t   par_h3;
    int8_t   par_h4;
    int8_t   par_h5;
    uint8_t  par_h6;
    int8_t   par_h7;
} bme_cal_t;

// ── State ─────────────────────────────────────────────────────────────────────
static bool bq_ok = false, rtc_ok = false, veml_ok = false, lis_ok = false,
            lsm_ok = false, bme_ok = false, mic_ok = false;
static bool led_on = true;
static uint8_t bme_chip_id = 0, bme_variant = 0;
static bme_cal_t bme_cal = {0};

static I2SClass i2s_mic;

// ── LED helpers ───────────────────────────────────────────────────────────────
static void led_duty(uint8_t d) { ledcWrite(PIN_FLASHLIGHT, d); }
static void led_flash(uint8_t duty, int on_ms, int off_ms, int n) {
    for (int i = 0; i < n; i++) {
        led_duty(duty); delay(on_ms);
        led_duty(0);    delay(off_ms);
    }
}

// ── I2C helpers ───────────────────────────────────────────────────────────────
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
static bool i2c_read_buf(TwoWire &bus, uint8_t addr, uint8_t reg,
                         uint8_t *buf, size_t n) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return false;
    bus.requestFrom(addr, (uint8_t)n);
    for (size_t i = 0; i < n; i++) {
        if (!bus.available()) return false;
        buf[i] = bus.read();
    }
    return true;
}
static bool i2c_write_reg(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t val) {
    bus.beginTransmission(addr);
    bus.write(reg);
    bus.write(val);
    return bus.endTransmission() == 0;
}

// VEML6030 word access (little-endian on the wire)
static bool veml_read_word(uint8_t reg, uint16_t *val) {
    Wire.beginTransmission(VEML6030_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)VEML6030_ADDR, 2) != 2) return false;
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    *val = ((uint16_t)hi << 8) | lo;
    return true;
}
static bool veml_write_word(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(VEML6030_ADDR);
    Wire.write(reg);
    Wire.write(val & 0xFF);
    Wire.write(val >> 8);
    return Wire.endTransmission() == 0;
}

// ── BME688 cal parse ─────────────────────────────────────────────────────────
static bool bme_read_calibration(void) {
    uint8_t b1[23] = {0};
    uint8_t b2[10] = {0};
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_CAL_BLK1, b1, sizeof(b1))) return false;
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_CAL_BLK2, b2, sizeof(b2))) return false;

    // Block 1 (regs 0x8A..0xA0)
    bme_cal.par_t2  = (int16_t)((uint16_t)b1[1]  << 8 | b1[0]);   // 0x8A,0x8B
    bme_cal.par_t3  = (int8_t)b1[2];                              // 0x8C
    bme_cal.par_p1  = (uint16_t)((uint16_t)b1[5]  << 8 | b1[4]);  // 0x8E,0x8F
    bme_cal.par_p2  = (int16_t)((uint16_t)b1[7]  << 8 | b1[6]);   // 0x90,0x91
    bme_cal.par_p3  = (int8_t)b1[8];                              // 0x92
    bme_cal.par_p4  = (int16_t)((uint16_t)b1[11] << 8 | b1[10]);  // 0x94,0x95
    bme_cal.par_p5  = (int16_t)((uint16_t)b1[13] << 8 | b1[12]);  // 0x96,0x97
    bme_cal.par_p7  = (int8_t)b1[14];                             // 0x98
    bme_cal.par_p6  = (int8_t)b1[15];                             // 0x99
    bme_cal.par_p8  = (int16_t)((uint16_t)b1[19] << 8 | b1[18]);  // 0x9C,0x9D
    bme_cal.par_p9  = (int16_t)((uint16_t)b1[21] << 8 | b1[20]);  // 0x9E,0x9F
    bme_cal.par_p10 = b1[22];                                     // 0xA0

    // Block 2 (regs 0xE1..0xEA)
    bme_cal.par_h2  = ((uint16_t)b2[0] << 4) | (b2[1] >> 4);      // 0xE1, 0xE2[7:4]
    bme_cal.par_h1  = ((uint16_t)b2[2] << 4) | (b2[1] & 0x0F);    // 0xE3, 0xE2[3:0]
    bme_cal.par_h3  = (int8_t)b2[3];                              // 0xE4
    bme_cal.par_h4  = (int8_t)b2[4];                              // 0xE5
    bme_cal.par_h5  = (int8_t)b2[5];                              // 0xE6
    bme_cal.par_h6  = b2[6];                                      // 0xE7
    bme_cal.par_h7  = (int8_t)b2[7];                              // 0xE8
    bme_cal.par_t1  = (uint16_t)((uint16_t)b2[9] << 8 | b2[8]);   // 0xE9,0xEA
    return true;
}

// ── Bus scan ──────────────────────────────────────────────────────────────────
static void scan_bus(TwoWire &bus, const char *name) {
    Serial.printf("[SCAN] %s: ", name);
    int count = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_ping(bus, a)) {
            Serial.printf("0x%02X ", a);
            count++;
        }
    }
    if (count == 0) Serial.print("(no devices)");
    Serial.printf(" -> %d device(s)\n", count);
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(700);

    Serial.println("\n========================================");
    Serial.println("  Kompic Mk I -- Stage 2 (hot-air) Smoke");
    Serial.println("  ESP32-S3 | USB-C power | no battery");
    Serial.println("========================================\n");

    ledcAttach(PIN_FLASHLIGHT, LED_FREQ_HZ, LED_RES_BITS);
    pinMode(PIN_BUTTON,   INPUT_PULLUP);
    pinMode(PIN_RTC_INT,  INPUT);
    pinMode(PIN_LSM_INT1, INPUT);
    Serial.println("[GPIO] flashlight LEDC, button, RTC-INT, LSM-INT1 init");

    // Brightness calibration sweep -- hold each level long enough to eyeball.
    // If 255 looks much brighter than 128 -> FET / LED current-limit OK,
    // breathe waveform just needs to spend more time near peak.
    // If 255 ~= 192 ~= 128 -> hardware ceiling (FET partial-enhancement or
    // series resistor too large); firmware can't push it harder.
    Serial.println("[LED ] Brightness sweep -- 50 % / 75 % / 100 % "
                   "(1.0 s each, 0.3 s off between)");
    led_duty(128); delay(1000); led_duty(0); delay(300);
    led_duty(192); delay(1000); led_duty(0); delay(300);
    led_duty(255); delay(1000); led_duty(0); delay(300);
    Serial.println();

    // ── I2C buses ────────────────────────────────────────────────────────────
    Serial.printf("[I2C ] Bus1 SDA=%d SCL=%d  Bus2 SDA=%d SCL=%d  @ 400 kHz\n",
                  PIN_SDA_BUS1, PIN_SCL_BUS1, PIN_SDA_BUS2, PIN_SCL_BUS2);
    Wire.begin(PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 400000);
    delay(20);

    Serial.println();
    scan_bus(Wire,  "Bus1");
    scan_bus(Wire1, "Bus2");
    Serial.println("       Stage-2 expected:");
    Serial.println("         Bus1 = 0x10, 0x1C, 0x51, 0x6B, 0x76");
    Serial.println("         Bus2 = 0x6A");
    Serial.println();

    // ── BQ25619 (bus 2) ──────────────────────────────────────────────────────
    // Silence two chronic FAULT bits seen on this board:
    //   - WATCHDOG (bit 7): sketch never pets WD -> disable WD in REG05[5:4]=00
    //   - NTC (bits 2:0): TS divider on v7.2 is 10k/10k (wrong per TI app note,
    //     see datasheet extract line 42) -> set TS_IGNORE in REG00[6]=1.
    // Once a battery + correct NTC network is fitted, both can be re-enabled.
    Serial.printf("[BQ  ] Ping 0x%02X ... ", BQ25619_ADDR);
    if (i2c_ping(Wire1, BQ25619_ADDR)) {
        uint8_t part      = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_PART);
        uint8_t reg00     = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_INPUT_SRC);
        uint8_t reg05     = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_CTRL1);
        i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_INPUT_SRC, reg00 | BQ_TS_IGNORE_BIT);
        i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_CTRL1,     reg05 & ~BQ_WD_MASK);
        // Re-read FAULT after silencing -- it latches, so do one more read to
        // give it a chance to clear, then capture.
        i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_FAULT);
        delay(5);
        uint8_t status = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
        uint8_t fault  = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_FAULT);
        bq_ok = true;
        Serial.printf("ACK  PART=0x%02X STATUS=0x%02X (PG=%s) FAULT=0x%02X\n",
                      part, status, (status & BQ_STATUS_PG) ? "OK" : "NO", fault);
        Serial.printf("       silenced: TS_IGNORE=1, WATCHDOG=disabled "
                      "(REG00 0x%02X -> 0x%02X, REG05 0x%02X -> 0x%02X)\n",
                      reg00, reg00 | BQ_TS_IGNORE_BIT,
                      reg05, reg05 & ~BQ_WD_MASK);
        if (fault & 0x80) Serial.println("       FAULT bit7 still set = WATCHDOG (latched -- will clear next boot)");
        if (fault & 0x40) Serial.println("       FAULT bit6 = BOOST_FAULT");
        if (fault & 0x30) Serial.printf( "       FAULT bits5:4 = CHRG_FAULT %u (01=input 10=therm 11=safety-timer)\n",
                                         (unsigned)((fault >> 4) & 0x03));
        if (fault & 0x08) Serial.println("       FAULT bit3 = BAT_FAULT (VBAT > 104%% VREG)");
        if (fault & 0x07) Serial.printf( "       FAULT bits2:0 = NTC %s (latched -- will clear next boot)\n",
                                         bq_ntc_name(fault));
    } else Serial.println("NO ACK -- check charger solder / 3V3 rail");

    // ── RTC PCF85063A (bus 1) ────────────────────────────────────────────────
    Serial.printf("[RTC ] Ping 0x%02X ... ", PCF85063A_ADDR);
    if (i2c_ping(Wire, PCF85063A_ADDR)) {
        uint8_t ctrl1   = i2c_read_reg(Wire, PCF85063A_ADDR, RTC_REG_CTRL1);
        uint8_t seconds = i2c_read_reg(Wire, PCF85063A_ADDR, RTC_REG_SECONDS);
        rtc_ok = true;
        Serial.printf("ACK  CTRL1=0x%02X SEC=0x%02X OS=%s\n", ctrl1, seconds,
                      (seconds & RTC_OS_BIT)
                          ? "set (sticky cold-boot flag -- clock still ticking)"
                          : "clr");
    } else Serial.println("NO ACK -- check RTC solder / crystal / 1V8 rail");

    // ── VEML6030 (bus 1) ─────────────────────────────────────────────────────
    Serial.printf("[VEML] Ping 0x%02X ... ", VEML6030_ADDR);
    if (i2c_ping(Wire, VEML6030_ADDR)) {
        veml_write_word(VEML_REG_CONF, VEML_CONF_RUN);
        delay(120);
        uint16_t cfg = 0;
        veml_read_word(VEML_REG_CONF, &cfg);
        veml_ok = (cfg == VEML_CONF_RUN);
        Serial.printf("%s  ALS_CONF=0x%04X (expect 0x%04X)\n",
                      veml_ok ? "ACK " : "BAD READ-BACK",
                      cfg, VEML_CONF_RUN);
    } else Serial.println("NO ACK -- check VEML6030 solder / bus1 pull-ups");

    // ── LIS3MDLTR (bus 1) ────────────────────────────────────────────────────
    Serial.printf("[LIS ] Ping 0x%02X ... ", LIS3MDL_ADDR);
    if (i2c_ping(Wire, LIS3MDL_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LIS3MDL_ADDR, LIS_REG_WHO);
        lis_ok = (who == LIS_WHO_VAL);
        Serial.printf("%s  WHO_AM_I=0x%02X (expect 0x%02X)\n",
                      lis_ok ? "ACK " : "BAD ID",
                      who, LIS_WHO_VAL);
        if (lis_ok) {
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL1, 0x80 | (0x03 << 5) | (0x04 << 2));
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL2, 0x00);
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL3, 0x00);
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL4, (0x03 << 2));
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL5, 0x40);
        }
    } else Serial.println("NO ACK -- check LIS3MDL solder / bus1");

    // ── LSM6DSV16X (bus 1) ───────────────────────────────────────────────────
    Serial.printf("[LSM ] Ping 0x%02X ... ", LSM6DSV_ADDR);
    if (i2c_ping(Wire, LSM6DSV_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WHO);
        lsm_ok = (who == LSM_WHO_VAL);
        Serial.printf("%s  WHO_AM_I=0x%02X (expect 0x%02X)\n",
                      lsm_ok ? "ACK " : "BAD ID",
                      who, LSM_WHO_VAL);
        if (lsm_ok) {
            i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x01);
            delay(50);
            // CTRL3 layout: bit7 BOOT, bit6 BDU, bit2 IF_INC, bit0 SW_RESET.
            // IF_INC is at bit 2 (not bit 1) -- writing 0x42 cleared IF_INC
            // and made every burst-read return the same register seven times.
            i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x44);
            i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, 0x07);
            i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, 0x07);
        }
    } else Serial.println("NO ACK -- check LSM6DSV16X solder / bus1");

    // ── BME688 (bus 1) ───────────────────────────────────────────────────────
    Serial.printf("[BME ] Ping 0x%02X ... ", BME688_ADDR);
    if (i2c_ping(Wire, BME688_ADDR)) {
        bme_chip_id = i2c_read_reg(Wire, BME688_ADDR, BME_REG_CHIP_ID);
        bme_variant = i2c_read_reg(Wire, BME688_ADDR, BME_REG_VARIANT_ID);
        bool id_ok = (bme_chip_id == BME_CHIP_ID_VAL);
        Serial.printf("%s  CHIP_ID=0x%02X (expect 0x%02X) VARIANT=0x%02X\n",
                      id_ok ? "ACK " : "BAD ID",
                      bme_chip_id, BME_CHIP_ID_VAL, bme_variant);
        if (id_ok) {
            i2c_write_reg(Wire, BME688_ADDR, BME_REG_RESET, 0xB6);
            delay(10);
            i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_GAS1, 0x00);  // no gas
            i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_HUM,  0x01);  // osrs_h x1
            if (bme_read_calibration()) {
                bme_ok = true;
                Serial.printf("       cal loaded: T1=%u T2=%d T3=%d  P1=%u  H1=%u H2=%u\n",
                              bme_cal.par_t1, bme_cal.par_t2, bme_cal.par_t3,
                              bme_cal.par_p1, bme_cal.par_h1, bme_cal.par_h2);
            } else {
                Serial.println("       FAIL -- could not read calibration block");
            }
        }
    } else Serial.println("NO ACK -- check BME688 solder / bus1");
    Serial.println();

    // ── WS2812 colour walk ───────────────────────────────────────────────────
    Serial.printf("[WS  ] Colour walk on GPIO%d (R / G / B / off @ %u, 250 ms each)\n",
                  PIN_WS2812_DIN, (unsigned)WS_BOOT_LEVEL);
    neopixelWrite(PIN_WS2812_DIN, WS_BOOT_LEVEL, 0, 0); delay(250);
    neopixelWrite(PIN_WS2812_DIN, 0, WS_BOOT_LEVEL, 0); delay(250);
    neopixelWrite(PIN_WS2812_DIN, 0, 0, WS_BOOT_LEVEL); delay(250);
    neopixelWrite(PIN_WS2812_DIN, 0, 0, 0); delay(50);
    Serial.println("       Done -- visual confirmation only (no readback)\n");

    // ── PDM mic — 48 kHz / x64 -> CLK 3.072 MHz (Standard Mode) ──────────────
    Serial.printf("[MIC ] PDM init: CLK=GPIO%d DATA=GPIO%d  %u Hz / 16-bit / mono\n",
                  PIN_MIC_CLK, PIN_MIC_DATA, (unsigned)MIC_SAMPLE_RATE_HZ);
    Serial.println("       (16 kHz x64 -> 1.024 MHz lands in MSM261DGT003 dead band; "
                   "48 kHz x64 -> 3.072 MHz Standard Mode)");
    i2s_mic.setPinsPdmRx(PIN_MIC_CLK, PIN_MIC_DATA);
    if (i2s_mic.begin(I2S_MODE_PDM_RX, MIC_SAMPLE_RATE_HZ,
                      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        i2s_mic.setTimeout(100);
        delay(30);                       // datasheet power-up time up to 20 ms
        const size_t N = 1024;
        int16_t drop[256];
        i2s_mic.readBytes((char *)drop, sizeof(drop));  // discard transient
        int16_t buf[N];
        size_t got = i2s_mic.readBytes((char *)buf, sizeof(buf));
        size_t samples = got / 2;
        int16_t maxs = INT16_MIN, mins = INT16_MAX;
        int64_t sumsq = 0;
        for (size_t i = 0; i < samples; i++) {
            int16_t s = buf[i];
            if (s > maxs) maxs = s;
            if (s < mins) mins = s;
            sumsq += (int64_t)s * s;
        }
        int32_t spread = (int32_t)maxs - (int32_t)mins;
        float rms = samples ? sqrtf((float)sumsq / samples) : 0.0f;
        mic_ok = (samples > 0 && spread > 200);
        Serial.printf("       %s  samples=%u  min=%d  max=%d  spread=%ld  rms=%.1f\n",
                      mic_ok ? "OK   (samples varying -- PDM bitstream live)"
                             : "FAIL (samples stuck -- check solder / VDD / L_R pad / port)",
                      (unsigned)samples, mins, maxs, (long)spread, (double)rms);
    } else {
        Serial.println("       BEGIN FAILED -- check mic solder / clock pin / 3V3");
    }
    Serial.println();

    // ── Summary ──────────────────────────────────────────────────────────────
    Serial.println("---- Stage-2 Summary ----------------------");
    Serial.printf("  BQ25619    (charger,  bus2 0x6A) : %s\n", bq_ok   ? "PASS" : "FAIL");
    Serial.printf("  PCF85063A  (RTC,      bus1 0x51) : %s\n", rtc_ok  ? "PASS" : "FAIL");
    Serial.printf("  VEML6030   (ALS,      bus1 0x10) : %s\n", veml_ok ? "PASS" : "FAIL");
    Serial.printf("  LIS3MDLTR  (mag,      bus1 0x1C) : %s\n", lis_ok  ? "PASS" : "FAIL");
    Serial.printf("  LSM6DSV16X (IMU,      bus1 0x6B) : %s\n", lsm_ok  ? "PASS" : "FAIL");
    Serial.printf("  BME688     (env,      bus1 0x76) : %s\n", bme_ok  ? "PASS" : "FAIL");
    Serial.printf("  WS2812B    (RGB,      GPIO42)    : DRIVEN (visual only)\n");
    Serial.printf("  MSM261DGT  (mic, PDM 47/48)      : %s\n", mic_ok  ? "PASS" : "FAIL");
    Serial.println("-------------------------------------------");

    int fails = !bq_ok + !rtc_ok + !veml_ok + !lis_ok + !lsm_ok + !bme_ok + !mic_ok;
    if (fails == 0)
        Serial.println("  All electrical checks PASS -- entering 3-s dwell loop.");
    else
        Serial.printf("  %d device(s) FAIL -- inspect solder / bridges / pull-ups\n",
                      fails);

    Serial.println("\n[BTN ] Press GPIO16 to toggle flashlight on/off\n");
}

// ── Live-stream readers ──────────────────────────────────────────────────────
static void stream_rtc(uint8_t /*tick*/) {
    uint8_t r[3] = {0};
    if (i2c_read_buf(Wire, PCF85063A_ADDR, RTC_REG_SECONDS, r, 3)) {
        uint8_t s10 = (r[0] & 0x70) >> 4, s1 = r[0] & 0x0F;
        uint8_t m10 = (r[1] & 0x70) >> 4, m1 = r[1] & 0x0F;
        uint8_t h10 = (r[2] & 0x30) >> 4, h1 = r[2] & 0x0F;
        Serial.printf("[RTC ] time = %u%u:%u%u:%u%u  OS=%s\n",
                      h10, h1, m10, m1, s10, s1,
                      (r[0] & RTC_OS_BIT) ? "set" : "clr");
    } else Serial.println("[RTC ] read FAIL");
}
static void stream_bq(uint8_t /*tick*/) {
    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    uint8_t fa = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_FAULT);
    Serial.printf("[BQ  ] STATUS=0x%02X FAULT=0x%02X  VBUS=%u CHRG=%u PG=%u "
                  "WD=%u NTC=%s\n",
                  st, fa,
                  (st >> 5) & 0x07, (st >> 3) & 0x03, (st >> 2) & 1,
                  (fa >> 7) & 1, bq_ntc_name(fa));
}
static void stream_veml(uint8_t /*tick*/) {
    uint16_t als = 0, white = 0;
    veml_read_word(VEML_REG_ALS,   &als);
    veml_read_word(VEML_REG_WHITE, &white);
    float lux = als * VEML_LX_PER_CT_QUARTER_100MS;
    Serial.printf("[VEML] ALS=%5u WHITE=%5u  ~%.1f lux\n",
                  als, white, (double)lux);
}
static void stream_lis(uint8_t /*tick*/) {
    uint8_t b[6];
    if (i2c_read_buf(Wire, LIS3MDL_ADDR, LIS_AUTO_INC | LIS_REG_OUT_X_L, b, 6)) {
        int16_t x = (int16_t)((b[1] << 8) | b[0]);
        int16_t y = (int16_t)((b[3] << 8) | b[2]);
        int16_t z = (int16_t)((b[5] << 8) | b[4]);
        Serial.printf("[LIS ] MAG  X=%+6d Y=%+6d Z=%+6d raw  "
                      "(%+5.2f %+5.2f %+5.2f G)\n",
                      x, y, z,
                      x / LIS_LSB_PER_GAUSS_4G,
                      y / LIS_LSB_PER_GAUSS_4G,
                      z / LIS_LSB_PER_GAUSS_4G);
    } else Serial.println("[LIS ] read FAIL");
}
static void stream_lsm(uint8_t /*tick*/) {
    uint8_t b[14];
    if (i2c_read_buf(Wire, LSM6DSV_ADDR, LSM_REG_OUT_TEMP, b, 14)) {
        int16_t t  = (int16_t)((b[1]  << 8) | b[0]);
        int16_t gx = (int16_t)((b[3]  << 8) | b[2]);
        int16_t gy = (int16_t)((b[5]  << 8) | b[4]);
        int16_t gz = (int16_t)((b[7]  << 8) | b[6]);
        int16_t ax = (int16_t)((b[9]  << 8) | b[8]);
        int16_t ay = (int16_t)((b[11] << 8) | b[10]);
        int16_t az = (int16_t)((b[13] << 8) | b[12]);
        float tempC = t / 256.0f + 25.0f;
        float ax_g = ax * (2.0f / 32768.0f);
        float ay_g = ay * (2.0f / 32768.0f);
        float az_g = az * (2.0f / 32768.0f);
        float gx_d = gx * (250.0f / 32768.0f);
        float gy_d = gy * (250.0f / 32768.0f);
        float gz_d = gz * (250.0f / 32768.0f);
        Serial.printf("[LSM ] T=%4.1fC  A=(%+5.2f,%+5.2f,%+5.2f)g  "
                      "G=(%+6.1f,%+6.1f,%+6.1f)dps\n",
                      (double)tempC,
                      (double)ax_g, (double)ay_g, (double)az_g,
                      (double)gx_d, (double)gy_d, (double)gz_d);
    } else Serial.println("[LSM ] read FAIL");
}
static void stream_bme(uint8_t /*tick*/) {
    // Force a single T+P+H measurement (no gas).
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS,
                  (0x02 << 5) | (0x05 << 2) | 0x01);  // osrs_t x2, osrs_p x16, forced
    delay(40);
    uint8_t d[8];
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_PRESS_ADC, d, sizeof(d))) {
        Serial.println("[BME ] read FAIL");
        return;
    }
    uint32_t press_adc = ((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4);
    uint32_t temp_adc  = ((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4);
    uint16_t hum_adc   = ((uint16_t)d[6] << 8) | d[7];

    // Temperature (Bosch float compensation)
    float v1, v2, t_fine, T;
    v1 = ((float)temp_adc / 16384.0f - (float)bme_cal.par_t1 / 1024.0f)
         * (float)bme_cal.par_t2;
    v2 = (((float)temp_adc / 131072.0f - (float)bme_cal.par_t1 / 8192.0f)
        * ((float)temp_adc / 131072.0f - (float)bme_cal.par_t1 / 8192.0f))
        * ((float)bme_cal.par_t3 * 16.0f);
    t_fine = v1 + v2;
    T = t_fine / 5120.0f;

    // Pressure (returns Pa)
    v1 = (t_fine / 2.0f) - 64000.0f;
    v2 = v1 * v1 * ((float)bme_cal.par_p6 / 131072.0f);
    v2 = v2 + (v1 * (float)bme_cal.par_p5 * 2.0f);
    v2 = (v2 / 4.0f) + ((float)bme_cal.par_p4 * 65536.0f);
    v1 = ((((float)bme_cal.par_p3 * v1 * v1) / 16384.0f)
        + ((float)bme_cal.par_p2 * v1)) / 524288.0f;
    v1 = (1.0f + (v1 / 32768.0f)) * (float)bme_cal.par_p1;
    float P = 1048576.0f - (float)press_adc;
    if (v1 != 0.0f) {
        P = ((P - (v2 / 4096.0f)) * 6250.0f) / v1;
        v1 = ((float)bme_cal.par_p9 * P * P) / 2147483648.0f;
        v2 = P * ((float)bme_cal.par_p8 / 32768.0f);
        float v3 = (P / 256.0f) * (P / 256.0f) * (P / 256.0f)
                 * ((float)bme_cal.par_p10 / 131072.0f);
        P = P + (v1 + v2 + v3 + ((float)bme_cal.par_p7 * 128.0f)) / 16.0f;
    }

    // Humidity (returns %RH)
    float tc = t_fine / 5120.0f;
    v1 = (float)hum_adc
       - (((float)bme_cal.par_h1 * 16.0f)
       + (((float)bme_cal.par_h3 / 2.0f) * tc));
    v2 = v1 * (((float)bme_cal.par_h2 / 262144.0f)
       * (1.0f + (((float)bme_cal.par_h4 / 16384.0f) * tc)
       + (((float)bme_cal.par_h5 / 1048576.0f) * tc * tc)));
    float h3f = (float)bme_cal.par_h6 / 16384.0f;
    float h4f = (float)bme_cal.par_h7 / 2097152.0f;
    float H = v2 + ((h3f + (h4f * tc)) * v2 * v2);
    if (H > 100.0f) H = 100.0f;
    if (H < 0.0f)   H = 0.0f;

    Serial.printf("[BME ] T=%5.2fC  P=%7.2f hPa  H=%5.2f %%RH\n",
                  (double)T, (double)(P / 100.0f), (double)H);
}
static void stream_mic(uint8_t /*tick*/) {
    const size_t N = 256;
    int16_t buf[N];
    size_t got = i2s_mic.readBytes((char *)buf, sizeof(buf));
    size_t samples = got / 2;
    int16_t maxs = INT16_MIN, mins = INT16_MAX;
    int64_t sumsq = 0;
    for (size_t i = 0; i < samples; i++) {
        int16_t s = buf[i];
        if (s > maxs) maxs = s;
        if (s < mins) mins = s;
        sumsq += (int64_t)s * s;
    }
    int32_t spread = (int32_t)maxs - (int32_t)mins;
    float rms = samples ? sqrtf((float)sumsq / samples) : 0.0f;
    Serial.printf("[MIC ] samples=%u min=%d max=%d spread=%ld rms=%.0f\n",
                  (unsigned)samples, mins, maxs, (long)spread, (double)rms);
}
static void stream_ws(uint8_t tick) {
    // 6 hues across the 3-s dwell, so the operator sees the LED cycling
    // through distinct colours rather than just R/G/B/off.
    uint8_t v = WS_LOOP_LEVEL;
    switch (tick % 6) {
        case 0: neopixelWrite(PIN_WS2812_DIN, v, 0, 0); Serial.println("[WS  ] RED");     break;
        case 1: neopixelWrite(PIN_WS2812_DIN, v, v, 0); Serial.println("[WS  ] YELLOW");  break;
        case 2: neopixelWrite(PIN_WS2812_DIN, 0, v, 0); Serial.println("[WS  ] GREEN");   break;
        case 3: neopixelWrite(PIN_WS2812_DIN, 0, v, v); Serial.println("[WS  ] CYAN");    break;
        case 4: neopixelWrite(PIN_WS2812_DIN, 0, 0, v); Serial.println("[WS  ] BLUE");    break;
        case 5: neopixelWrite(PIN_WS2812_DIN, v, 0, v); Serial.println("[WS  ] MAGENTA"); break;
    }
}

static const char *sensor_name(uint8_t idx) {
    switch (idx) {
        case 0: return "RTC";
        case 1: return "BQ";
        case 2: return "VEML";
        case 3: return "LIS";
        case 4: return "LSM";
        case 5: return "BME";
        case 6: return "MIC";
        case 7: return "WS";
        default: return "?";
    }
}
static bool sensor_ok(uint8_t idx) {
    switch (idx) {
        case 0: return rtc_ok;
        case 1: return bq_ok;
        case 2: return veml_ok;
        case 3: return lis_ok;
        case 4: return lsm_ok;
        case 5: return bme_ok;
        case 6: return mic_ok;
        case 7: return true;   // WS has no probe, always drive
        default: return false;
    }
}
static void sensor_dispatch(uint8_t idx, uint8_t tick) {
    if (!sensor_ok(idx)) {
        Serial.printf("[%s] skipped (FAIL)\n", sensor_name(idx));
        return;
    }
    switch (idx) {
        case 0: stream_rtc (tick); break;
        case 1: stream_bq  (tick); break;
        case 2: stream_veml(tick); break;
        case 3: stream_lis (tick); break;
        case 4: stream_lsm (tick); break;
        case 5: stream_bme (tick); break;
        case 6: stream_mic (tick); break;
        case 7: stream_ws  (tick); break;
    }
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    // Button: debounced toggle
    static uint32_t btn_last_ms = 0;
    if (digitalRead(PIN_BUTTON) == LOW) {
        uint32_t now = millis();
        if (now - btn_last_ms > 300) {
            led_on = !led_on;
            btn_last_ms = now;
            if (!led_on) led_duty(0);
            Serial.printf("[BTN ] press -> LED %s\n", led_on ? "ON" : "OFF");
        }
    }

    // Dwell-based per-sensor streaming
    static uint8_t  active     = 0;
    static uint8_t  print_idx  = 0;
    static uint32_t next_ms    = 0;
    if ((int32_t)(millis() - next_ms) >= 0) {
        next_ms = millis() + SENSOR_PRINT_MS;
        if (print_idx == 0)
            Serial.printf("\n--- %s dwell (%u s) ---\n",
                          sensor_name(active), (unsigned)(SENSOR_DWELL_MS / 1000));
        sensor_dispatch(active, print_idx);
        if (++print_idx >= DWELL_PRINTS) {
            print_idx = 0;
            active = (active + 1) % N_SENSORS;
            // Leave the WS pixel off whenever we're not in its dwell.
            if (active != 7) neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
        }
    }

    // Sine breathe on flashlight (50 % lifted, clamped to 8-bit)
    if (!led_on) { delay(10); return; }
    static float phase = 0.0f;
    phase += 0.03f;
    if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    float s = (sinf(phase) + 1.0f) * 0.5f;             // 0.0 .. 1.0
    float raw = (s * 220.0f + 12.0f) * 1.5f;           // 18 .. 348 (was 12 .. 232)
    uint8_t d = raw > 255.0f ? 255 : (uint8_t)raw;
    led_duty(d);
    delay(10);
}
