/**
 * 3_smoke_test_mk1_hotair.ino — Kompic Mk I, post-hot-air stage-3 smoke test
 *
 * Build progression on this board:
 *   Stage 1 (reflow, verified by 1_smoke_test_mk1_reflow1.ino):
 *     ESP32-S3-WROOM-1U, BQ25619, TPS62840 3.2 V, XC6206 1.8 V,
 *     flashlight LED (GPIO41), button (BQ QON / GPIO16),
 *     PCF85063A RTC + crystal.
 *   Stage 2 (hot air, 2_smoke_test_mk1_hotair.ino):
 *     + USB-C JS16T, VEML6030, LIS3MDLTR, LSM6DSV16X, BME688,
 *     + MSM261DGT003 PDM mic, WS2812B-2020 RGB.
 *   Stage 3 (this sketch — additional hot-air parts + daughterboard):
 *     + microSD push-push socket    (SDMMC 1-bit: CLK=38 CMD=39 DAT0=40)
 *     + DRV2605L LRA haptic driver  (bus 2 @ 0x5A, EN=GPIO0)
 *     + ALPS EC05E rotary encoder   (A=GPIO21, B=GPIO43)
 *     + MAX30101 PPG  (daughterboard U13, bus 1 @ 0x57, INT=GPIO7)
 *     + TMP117 temp   (daughterboard U15, bus 1 @ 0x48 or 0x49 — probed at boot)
 *   Physical placement (PCB is canonical, schematic hierarchy disagrees):
 *     Bus 1 (east, sensors)  = VEML, LIS, RTC, LSM, BME, MAX, TMP
 *     Bus 2 (west, power)    = BQ (top main), DRV (underside main)
 *   QVAR/ECG electrodes on daughterboard not yet tested; GPS module not fitted.
 *
 * Sequence:
 *   1. Banner, LEDC/button/encoder init, boot flash.
 *   2. Drive DRV_EN (GPIO0) HIGH so the haptic chip leaves shutdown
 *      before any bus 2 traffic. Internal boot pull-up alone does
 *      NOT hold it after the ROM bootloader releases the strap.
 *   3. Full I2C scan on both buses.
 *   4. Per-chip identity probe (stage-2 sensors + DRV2605L).
 *   5. WS2812 colour walk.
 *   6. PDM mic init + variation check.
 *   7. SD card mount (1-bit, GPIO38/39/40). On success:
 *        - append one line to /smoke_log.txt with the boot summary,
 *        - count *.txt and *.TXT files in the root directory.
 *   8. PASS/FAIL summary.
 *   9. Loop: dwell on each device for 3 s, printing one line every
 *      0.5 s. DRV trigger fires once per dwell tick (6 buzzes per
 *      visit). Encoder + button are always responsive (ISR-driven).
 *
 * Encoder notes (datasheet 20.16):
 *   - 6 ms mechanical bounce -> software debounce, 8 ms guard window.
 *   - GPIO43 is also U0TXD: one ~3 ms boot-log blip before user code
 *     runs. We attach the interrupt only after setup() prints its
 *     banner, so the blip is harmless.
 *   - A/B state at detent is undefined; decode on A-edge by sampling
 *     B level.  CW/CCW polarity is a bench guess -- flip
 *     ENC_INVERT below if the printout has the directions swapped.
 *
 * DRV2605L notes (datasheet 20.14):
 *   - ELV1411A parameters (V_rated, f_LRA) are not yet on hand, so
 *     this smoke test skips auto-calibration and trusts the chip's
 *     LRA-library defaults. Effects WILL play but may feel weak; the
 *     intent here is electrical confirmation, not feel tuning.
 *
 * Arduino-ESP32 v3.x API (ledcAttach, neopixelWrite, ESP_I2S, SD_MMC).
 * Board: "ESP32S3 Dev Module", USB CDC On Boot: Enabled.
 * Pins from: hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md
 */

#include <Wire.h>
#include <math.h>
#include "ESP_I2S.h"
#include "FS.h"
#include "SD_MMC.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_FLASHLIGHT    41
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1    // east edge — RTC + ALS/IMU/mag/env
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4    // west edge — BQ charger + DRV2605L
#define PIN_SCL_BUS2       5
#define PIN_RTC_INT       15
#define PIN_LSM_INT1       8
#define PIN_WS2812_DIN    42
#define PIN_MIC_CLK       47
#define PIN_MIC_DATA      48
#define PIN_SD_CLK        38
#define PIN_SD_CMD        39
#define PIN_SD_D0         40
#define PIN_DRV_EN         0    // strap (boot-only pull-up); FW must drive HIGH
#define PIN_ENC_A         21    // RTC wake-capable
#define PIN_ENC_B         43    // U0TXD — boot-log blip before user code runs

// ── I2C addresses ─────────────────────────────────────────────────────────────
#define VEML6030_ADDR     0x10
#define LIS3MDL_ADDR      0x1C
#define PCF85063A_ADDR    0x51
#define LSM6DSV_ADDR      0x6B
#define BME688_ADDR       0x76
#define BQ25619_ADDR      0x6A
#define DRV2605_ADDR      0x5A   // bus 2 — main board UNDERSIDE
#define MAX30101_ADDR     0x57   // bus 1 — daughterboard U13
#define TMP117_ADDR_LO    0x48   // bus 1 — daughterboard U15 if ADD0=GND
#define TMP117_ADDR_HI    0x49   //         if ADD0=3V3 (probe both at startup)
// Physical layout (per Ivan, schematic hierarchy disagrees but PCB is canonical):
//   Bus 1 (east, GPIO1/2):  VEML, LIS, RTC, LSM, BME, MAX, TMP  — sensors
//   Bus 2 (west, GPIO4/5):  BQ (top main), DRV (underside main) — power + haptic

// ── BQ25619 ───────────────────────────────────────────────────────────────────
#define BQ_REG_INPUT_SRC  0x00
#define BQ_REG_CTRL1      0x05
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
#define VEML_CONF_RUN     ((uint16_t)((0x03 << 11) | (0x00 << 6)))
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
#define LSM_REG_OUT_TEMP  0x20

// ── BME688 ────────────────────────────────────────────────────────────────────
#define BME_REG_CHIP_ID      0xD0
#define BME_REG_VARIANT_ID   0xF0
#define BME_REG_RESET        0xE0
#define BME_REG_CTRL_HUM     0x72
#define BME_REG_CTRL_GAS1    0x71
#define BME_REG_CTRL_MEAS    0x74
#define BME_REG_PRESS_ADC    0x1F
#define BME_REG_CAL_BLK1     0x8A
#define BME_REG_CAL_BLK2     0xE1
#define BME_CHIP_ID_VAL      0x61

// ── DRV2605L ──────────────────────────────────────────────────────────────────
#define DRV_REG_STATUS       0x00
#define DRV_REG_MODE         0x01
#define DRV_REG_LIBRARY      0x03
#define DRV_REG_WAVSEQ1      0x04
#define DRV_REG_WAVSEQ2      0x05
#define DRV_REG_GO           0x0C
#define DRV_REG_RATED_VOLT   0x16
#define DRV_REG_OD_CLAMP     0x17
#define DRV_REG_A_CAL_COMP   0x18
#define DRV_REG_A_CAL_BEMF   0x19
#define DRV_REG_FEEDBACK     0x1A
#define DRV_REG_CTRL1        0x1B   // DRIVE_TIME (initial f_LRA guess)
#define DRV_REG_CTRL4        0x1E   // AUTO_CAL_TIME
#define DRV_DEVICE_ID_VAL    0x07   // top 3 bits of STATUS
#define DRV_MODE_INTERNAL    0x00   // STANDBY=0, MODE=000 (internal trigger / GO)
#define DRV_MODE_AUTO_CAL    0x07   // STANDBY=0, MODE=111 (auto-cal)
#define DRV_LIB_LRA          0x06

// ── ELV1411A LRA motor specs (per Ivan, no datasheet on file yet) ────────────
//   V_rated  = 2.0 V_RMS       (max steady drive)
//   f_LRA    = 150 ± 5 Hz      (mechanical resonance)
//   I_max    = 150 mA          (absolute current limit)
// User wants this to rattle hard — drive at rated voltage. Auto-cal will
// confirm back-EMF is in range; if it fails the chip falls back to open-loop
// and effects still play (per 20.14 datasheet note).
#define DRV_V_RATED_RMS      2.0f
#define DRV_F_LRA_HZ         150
#define DRV_I_MAX_MA         150

// RATED_VOLTAGE = V_rated_RMS / 20.58e-3                 (simplified, p.44)
// Spec is 2.0 V_RMS but auto-cal tripped OC_DETECT at 0x61/0x8D — the chip's
// open-loop probe pulses near OD_CLAMP into ~10 Ω DCR exceed the OC threshold.
// Step the targets down so cal can complete: 1.5 V_RMS / 2.0 V peak.
//   1.5 V_RMS / 0.02058 ≈ 73 → 0x49
#define DRV_RATED_VOLTAGE    0x49

// OD_CLAMP_V = 21.22e-3 × OD_CLAMP                       (peak, p.44)
// Must remain > RATED_VOLTAGE register-wise.
//   2.0 V peak / 0.02122 ≈ 94 → 0x60
#define DRV_OD_CLAMP         0x60

// CTRL1 bit7=STARTUP_BOOST (default keep), bits4:0=DRIVE_TIME.
// t_drive(ms) ≈ 0.5/f_LRA → 0.5/150 = 3.33 ms → DRIVE_TIME = (3.33-0.5)/0.1 = 28 = 0x1C.
// CTRL1 = STARTUP_BOOST | DRIVE_TIME = 0x80 | 0x1C = 0x9C.
#define DRV_CTRL1_DRIVE_TIME 0x9C

// FEEDBACK_CTRL (datasheet auto-cal example value).
//   bit7   N_ERM_LRA      = 1  (LRA)
//   6:4    FB_BRAKE_FACTOR= 3  (4×)
//   3:2    LOOP_GAIN      = 1  (medium)
//   1:0    BEMF_GAIN      = 2  (15× LRA, recommended)
#define DRV_FEEDBACK_LRA     0xB6

// CTRL4: AUTO_CAL_TIME = 3 (1000–1200 ms, most accurate). Other fields = 0.
#define DRV_AUTO_CAL_TIME    0x30

// ── MAX30101 (PPG, daughterboard U13) ────────────────────────────────────────
#define MAX_REG_INT_STATUS_1  0x00
#define MAX_REG_FIFO_WR_PTR   0x04
#define MAX_REG_OVERFLOW_CTR  0x05
#define MAX_REG_FIFO_RD_PTR   0x06
#define MAX_REG_FIFO_DATA     0x07
#define MAX_REG_MODE_CONFIG   0x09
#define MAX_REG_LED1_PA       0x0C  // Red LED
#define MAX_REG_REV_ID        0xFE
#define MAX_REG_PART_ID       0xFF
#define MAX_PART_ID_VAL       0x15
#define MAX_MODE_RESET        0x40
#define MAX_MODE_HR           0x02  // heart-rate, Red LED only
#define MAX_LED_PA_SMOKE      0x1F  // ~6.2 mA per LED — modest, no skin needed

// ── TMP117 (precision temperature, daughterboard U15) ────────────────────────
#define TMP_REG_TEMP_RESULT   0x00  // 16-bit signed, 7.8125 m°C / LSB
#define TMP_REG_CONFIG        0x01
#define TMP_REG_DEVICE_ID     0x0F  // bottom 12 bits == 0x117
#define TMP_DEVICE_ID_LOW12   0x0117
#define TMP_LSB_C             (7.8125e-3f)

// LRA effect IDs to play during the DRV dwell (one per 500-ms tick, 6 total).
static const uint8_t drv_effects[6] = {
    1,    // Strong Click 100 %
    4,    // Sharp Click 100 %
    10,   // Double Click 100 %
    14,   // Strong Buzz 100 %
    47,   // Pulse 100 %
    16,   // 750 ms Alert 100 %
};

// ── PDM mic ──────────────────────────────────────────────────────────────────
#define MIC_SAMPLE_RATE_HZ   48000

// ── LED PWM ───────────────────────────────────────────────────────────────────
#define LED_FREQ_HZ          1000
#define LED_RES_BITS         8

// Flashlight current cap. The iv7.1 PCB has 2 × 47 Ω in series with the LED
// (R25 + R26 = 94 Ω). R26 was lost during bench rework, so only R25 (47 Ω)
// is in circuit — doubling the LED current at full duty (~36 mA, over the
// 30 mA abs-max). Clamp the LEDC duty to 25 % (= 64 / 255) until R26 is
// back in place. Keeps the LED at ~9 mA average / 18 mA peak — well cool.
#define LED_MAX_DUTY         64

// ── WS2812 levels ────────────────────────────────────────────────────────────
#define WS_BOOT_LEVEL        26
#define WS_LOOP_LEVEL        19

// ── Loop dwell ───────────────────────────────────────────────────────────────
#define SENSOR_DWELL_MS      3000
#define SENSOR_PRINT_MS      500
#define DWELL_PRINTS         (SENSOR_DWELL_MS / SENSOR_PRINT_MS)   // 6
#define N_SENSORS            12                                    // + SD + DRV + MAX + TMP

// ── Encoder ──────────────────────────────────────────────────────────────────
#define ENC_DEBOUNCE_MS      8        // 6 ms bounce + 2 ms margin (20.16 W-01)
#define ENC_INVERT           0        // flip if printed direction is wrong

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
    uint16_t par_h1;
    uint16_t par_h2;
    int8_t   par_h3;
    int8_t   par_h4;
    int8_t   par_h5;
    uint8_t  par_h6;
    int8_t   par_h7;
} bme_cal_t;

// ── State ─────────────────────────────────────────────────────────────────────
static bool bq_ok = false, rtc_ok = false, veml_ok = false, lis_ok = false,
            lsm_ok = false, bme_ok = false, mic_ok = false,
            drv_ok = false, sd_ok = false, max_ok = false, tmp_ok = false;
static bool drv_cal_ok = false;
static bool led_on = true;
static uint8_t bme_chip_id = 0, bme_variant = 0;
static bme_cal_t bme_cal = {0};
static uint8_t drv_device_id_raw = 0;
static uint8_t drv_cal_status = 0, drv_cal_comp = 0, drv_cal_bemf = 0;
static uint32_t drv_cal_ms = 0;
static uint64_t sd_card_size_mb = 0;
static int sd_txt_count = -1;
static const char *sd_card_kind = "?";
static uint8_t  max_part_id = 0, max_rev_id = 0;
static uint8_t  tmp_addr_used = 0;
static uint16_t tmp_device_id_raw = 0;

static I2SClass i2s_mic;

// Encoder ISR scratch (ints are 32-bit atomic on ESP32-S3).
static volatile int32_t enc_delta   = 0;   // accumulated +CW / -CCW
static volatile uint32_t enc_last_us = 0;

// ── LED helpers ───────────────────────────────────────────────────────────────
static void led_duty(uint8_t d) { ledcWrite(PIN_FLASHLIGHT, d); }

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

// TMP117 word read (big-endian on the wire: high byte first). Bus 1.
static bool tmp_read_word(uint8_t addr, uint8_t reg, uint16_t *val) {
    uint8_t b[2];
    if (!i2c_read_buf(Wire, addr, reg, b, 2)) return false;
    *val = ((uint16_t)b[0] << 8) | b[1];
    return true;
}

// VEML6030 word access
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

    bme_cal.par_t2  = (int16_t)((uint16_t)b1[1]  << 8 | b1[0]);
    bme_cal.par_t3  = (int8_t)b1[2];
    bme_cal.par_p1  = (uint16_t)((uint16_t)b1[5]  << 8 | b1[4]);
    bme_cal.par_p2  = (int16_t)((uint16_t)b1[7]  << 8 | b1[6]);
    bme_cal.par_p3  = (int8_t)b1[8];
    bme_cal.par_p4  = (int16_t)((uint16_t)b1[11] << 8 | b1[10]);
    bme_cal.par_p5  = (int16_t)((uint16_t)b1[13] << 8 | b1[12]);
    bme_cal.par_p7  = (int8_t)b1[14];
    bme_cal.par_p6  = (int8_t)b1[15];
    bme_cal.par_p8  = (int16_t)((uint16_t)b1[19] << 8 | b1[18]);
    bme_cal.par_p9  = (int16_t)((uint16_t)b1[21] << 8 | b1[20]);
    bme_cal.par_p10 = b1[22];

    bme_cal.par_h2  = ((uint16_t)b2[0] << 4) | (b2[1] >> 4);
    bme_cal.par_h1  = ((uint16_t)b2[2] << 4) | (b2[1] & 0x0F);
    bme_cal.par_h3  = (int8_t)b2[3];
    bme_cal.par_h4  = (int8_t)b2[4];
    bme_cal.par_h5  = (int8_t)b2[5];
    bme_cal.par_h6  = b2[6];
    bme_cal.par_h7  = (int8_t)b2[7];
    bme_cal.par_t1  = (uint16_t)((uint16_t)b2[9] << 8 | b2[8]);
    return true;
}

// ── DRV2605L helpers ─────────────────────────────────────────────────────────
static bool drv_trigger(uint8_t effect_id) {
    if (!drv_ok) return false;
    if (!i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_WAVSEQ1, effect_id)) return false;
    if (!i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_WAVSEQ2, 0x00))      return false;
    return i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_GO, 0x01);
}

// ── Encoder ISR ──────────────────────────────────────────────────────────────
// A-edge sample of B: at rest A/B are unspecified, but during a transition
// the sign of (A XOR B_prev) reflects rotation direction. With pure rising-A
// triggering, "B==LOW means CW" or "B==HIGH means CW" depends on body
// orientation -- flip ENC_INVERT if wrong on bench.
static void IRAM_ATTR enc_isr_a(void) {
    uint32_t now = micros();
    if ((now - enc_last_us) < (uint32_t)ENC_DEBOUNCE_MS * 1000u) return;
    enc_last_us = now;
    int dir = (digitalRead(PIN_ENC_B) == LOW) ? +1 : -1;
    if (ENC_INVERT) dir = -dir;
    enc_delta += dir;
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

// ── SD helpers ───────────────────────────────────────────────────────────────
static int sd_count_txt_files(void) {
    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) return -1;
    int n = 0;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (!f.isDirectory()) {
            const char *name = f.name();
            size_t len = strlen(name);
            if (len >= 4) {
                const char *ext = name + len - 4;
                if ((ext[0] == '.') &&
                    (ext[1] == 't' || ext[1] == 'T') &&
                    (ext[2] == 'x' || ext[2] == 'X') &&
                    (ext[3] == 't' || ext[3] == 'T')) {
                    n++;
                }
            }
        }
        f.close();
    }
    root.close();
    return n;
}

static void sd_log_boot_summary(void) {
    File f = SD_MMC.open("/smoke_log.txt", FILE_APPEND);
    if (!f) {
        Serial.println("       could not open /smoke_log.txt for append");
        return;
    }
    f.printf("boot %lu ms  BQ=%d RTC=%d VEML=%d LIS=%d LSM=%d BME=%d MIC=%d "
             "DRV=%d (id=0x%02X)  cardSize=%llu MB\n",
             (unsigned long)millis(),
             (int)bq_ok, (int)rtc_ok, (int)veml_ok, (int)lis_ok,
             (int)lsm_ok, (int)bme_ok, (int)mic_ok,
             (int)drv_ok, drv_device_id_raw,
             (unsigned long long)sd_card_size_mb);
    f.close();
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(700);

    Serial.println("\n========================================");
    Serial.println("  Kompic Mk I -- Stage 3 (hot-air+) Smoke");
    Serial.println("  ESP32-S3 | USB-C power | no battery");
    Serial.println("  + SD card, DRV2605L, ALPS encoder");
    Serial.println("========================================\n");

    ledcAttach(PIN_FLASHLIGHT, LED_FREQ_HZ, LED_RES_BITS);
    pinMode(PIN_BUTTON,   INPUT_PULLUP);
    pinMode(PIN_RTC_INT,  INPUT);
    pinMode(PIN_LSM_INT1, INPUT);
    pinMode(PIN_ENC_A,    INPUT_PULLUP);
    pinMode(PIN_ENC_B,    INPUT_PULLUP);
    Serial.println("[GPIO] flashlight LEDC, button, RTC-INT, LSM-INT1, EC_A/B init");

    // DRV_EN: GPIO0 strap pull-up releases after ROM. Drive HIGH explicitly so
    // the DRV2605L is awake before bus 2 probes (20.14 W-01).
    pinMode(PIN_DRV_EN, OUTPUT);
    digitalWrite(PIN_DRV_EN, HIGH);
    delay(5);   // ≥1.5 ms device-ready per datasheet
    Serial.println("[DRV ] EN driven HIGH on GPIO0");

    // Brightness sweep (same as stage 2).
    Serial.printf("[LED ] Brightness sweep -- 50%% / 75%% / 100%% of capped duty %u "
                  "(1.0 s each, 0.3 s off between)\n", (unsigned)LED_MAX_DUTY);
    led_duty(LED_MAX_DUTY / 2);    delay(1000); led_duty(0); delay(300);
    led_duty(LED_MAX_DUTY * 3 / 4); delay(1000); led_duty(0); delay(300);
    led_duty(LED_MAX_DUTY);        delay(1000); led_duty(0); delay(300);
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
    Serial.println("       Stage-3 expected:");
    Serial.println("         Bus1 (east, sensors) = 0x10, 0x1C, 0x48/0x49 (TMP),");
    Serial.println("                                0x51, 0x57 (MAX), 0x6B, 0x76");
    Serial.println("         Bus2 (west, power)   = 0x5A (DRV), 0x6A (BQ)");
    Serial.println();

    // ── BQ25619 (bus 2) ──────────────────────────────────────────────────────
    Serial.printf("[BQ  ] Ping 0x%02X ... ", BQ25619_ADDR);
    if (i2c_ping(Wire1, BQ25619_ADDR)) {
        uint8_t part  = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_PART);
        uint8_t reg00 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_INPUT_SRC);
        uint8_t reg05 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_CTRL1);
        i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_INPUT_SRC, reg00 | BQ_TS_IGNORE_BIT);
        i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_CTRL1,     reg05 & ~BQ_WD_MASK);
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
        if (fault & 0x80) Serial.println("       FAULT bit7 = WATCHDOG (latched)");
        if (fault & 0x40) Serial.println("       FAULT bit6 = BOOST_FAULT");
        if (fault & 0x30) Serial.printf( "       FAULT bits5:4 = CHRG_FAULT %u\n",
                                         (unsigned)((fault >> 4) & 0x03));
        if (fault & 0x08) Serial.println("       FAULT bit3 = BAT_FAULT");
        if (fault & 0x07) Serial.printf( "       FAULT bits2:0 = NTC %s\n",
                                         bq_ntc_name(fault));
    } else Serial.println("NO ACK -- check charger solder / 3V3 rail");

    // ── DRV2605L (bus 2) — identity + ELV1411A auto-calibration ─────────────
    Serial.printf("[DRV ] Ping 0x%02X ... ", DRV2605_ADDR);
    if (i2c_ping(Wire1, DRV2605_ADDR)) {
        uint8_t st = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
        drv_device_id_raw = st;
        uint8_t devid = (st >> 5) & 0x07;
        bool id_ok = (devid == DRV_DEVICE_ID_VAL);
        Serial.printf("%s  STATUS=0x%02X  DEVICE_ID=%u (expect %u = DRV2605L)\n",
                      id_ok ? "ACK " : "BAD ID", st, devid, DRV_DEVICE_ID_VAL);
        if (id_ok) {
            Serial.printf("       ELV1411A target: V_rated=%.2f Vrms  f_LRA=%u Hz  I_max=%u mA\n",
                          (double)DRV_V_RATED_RMS, (unsigned)DRV_F_LRA_HZ, (unsigned)DRV_I_MAX_MA);
            Serial.printf("       writing  RATED=0x%02X  OD_CLAMP=0x%02X  CTRL1=0x%02X "
                          "FEEDBACK=0x%02X  CTRL4=0x%02X\n",
                          DRV_RATED_VOLTAGE, DRV_OD_CLAMP, DRV_CTRL1_DRIVE_TIME,
                          DRV_FEEDBACK_LRA, DRV_AUTO_CAL_TIME);

            // Enter calibration mode: STANDBY=0, MODE=7. Motor profile must be
            // written BEFORE GO=1; the chip latches it at the start of cal.
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_MODE,       DRV_MODE_AUTO_CAL);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_FEEDBACK,   DRV_FEEDBACK_LRA);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_RATED_VOLT, DRV_RATED_VOLTAGE);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_OD_CLAMP,   DRV_OD_CLAMP);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL4,      DRV_AUTO_CAL_TIME);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL1,      DRV_CTRL1_DRIVE_TIME);

            Serial.print("       auto-calibrating");
            uint32_t t0 = millis();
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_GO, 0x01);
            // Poll GO until self-clear (typ ~1000-1200 ms, datasheet); 1500 ms ceiling.
            // The motor briefly buzzes during cal — that's the back-EMF probe sweep.
            uint8_t go = 1;
            while (go && (millis() - t0) < 1500) {
                delay(50);
                Serial.print('.');
                go = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_GO) & 0x01;
            }
            drv_cal_ms     = millis() - t0;
            drv_cal_status = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
            drv_cal_comp   = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_A_CAL_COMP);
            drv_cal_bemf   = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_A_CAL_BEMF);
            bool diag_pass = !(drv_cal_status & 0x08);   // bit 3 DIAG_RESULT: 0=pass
            bool finished  = (go == 0);
            drv_cal_ok = finished && diag_pass;
            Serial.printf(" %s  (%u ms)  STATUS=0x%02X  A_CAL_COMP=0x%02X  A_CAL_BEMF=0x%02X\n",
                          drv_cal_ok ? "PASS" :
                          finished   ? "FAIL (DIAG_RESULT=1, motor unloaded or out-of-range?)" :
                                       "TIMEOUT",
                          (unsigned)drv_cal_ms, drv_cal_status, drv_cal_comp, drv_cal_bemf);
            if (drv_cal_status & 0x02) Serial.println("       STATUS OVER_TEMP latched");
            if (drv_cal_status & 0x01) Serial.println("       STATUS OC_DETECT latched");

            // Leave calibration mode — internal trigger, LRA library.
            // Even if cal failed, the chip falls back to open-loop drive and
            // library effects still play; that's the desired "rattle anyway"
            // behaviour for this smoke test.
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_MODE,    DRV_MODE_INTERNAL);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_LIBRARY, DRV_LIB_LRA);
            drv_ok = true;
        }
    } else Serial.println("NO ACK -- check DRV solder / EN line / 3V3");

    // ── RTC PCF85063A (bus 1) ────────────────────────────────────────────────
    Serial.printf("[RTC ] Ping 0x%02X ... ", PCF85063A_ADDR);
    if (i2c_ping(Wire, PCF85063A_ADDR)) {
        uint8_t ctrl1   = i2c_read_reg(Wire, PCF85063A_ADDR, RTC_REG_CTRL1);
        uint8_t seconds = i2c_read_reg(Wire, PCF85063A_ADDR, RTC_REG_SECONDS);
        rtc_ok = true;
        Serial.printf("ACK  CTRL1=0x%02X SEC=0x%02X OS=%s\n", ctrl1, seconds,
                      (seconds & RTC_OS_BIT) ? "set" : "clr");
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
    } else Serial.println("NO ACK -- check VEML solder / bus1 pull-ups");

    // ── LIS3MDLTR (bus 1) ────────────────────────────────────────────────────
    Serial.printf("[LIS ] Ping 0x%02X ... ", LIS3MDL_ADDR);
    if (i2c_ping(Wire, LIS3MDL_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LIS3MDL_ADDR, LIS_REG_WHO);
        lis_ok = (who == LIS_WHO_VAL);
        Serial.printf("%s  WHO_AM_I=0x%02X (expect 0x%02X)\n",
                      lis_ok ? "ACK " : "BAD ID", who, LIS_WHO_VAL);
        if (lis_ok) {
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL1, 0x80 | (0x03 << 5) | (0x04 << 2));
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL2, 0x00);
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL3, 0x00);
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL4, (0x03 << 2));
            i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL5, 0x40);
        }
    } else Serial.println("NO ACK -- check LIS solder / bus1");

    // ── LSM6DSV16X (bus 1) ───────────────────────────────────────────────────
    Serial.printf("[LSM ] Ping 0x%02X ... ", LSM6DSV_ADDR);
    if (i2c_ping(Wire, LSM6DSV_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WHO);
        lsm_ok = (who == LSM_WHO_VAL);
        Serial.printf("%s  WHO_AM_I=0x%02X (expect 0x%02X)\n",
                      lsm_ok ? "ACK " : "BAD ID", who, LSM_WHO_VAL);
        if (lsm_ok) {
            i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x01);
            delay(50);
            i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x44);
            i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, 0x07);
            i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, 0x07);
        }
    } else Serial.println("NO ACK -- check LSM solder / bus1");

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
            i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_GAS1, 0x00);
            i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_HUM,  0x01);
            if (bme_read_calibration()) {
                bme_ok = true;
                Serial.printf("       cal loaded: T1=%u T2=%d T3=%d  P1=%u  H1=%u H2=%u\n",
                              bme_cal.par_t1, bme_cal.par_t2, bme_cal.par_t3,
                              bme_cal.par_p1, bme_cal.par_h1, bme_cal.par_h2);
            } else {
                Serial.println("       FAIL -- could not read calibration block");
            }
        }
    } else Serial.println("NO ACK -- check BME solder / bus1");

    // ── MAX30101 (bus 1, daughterboard U13) ──────────────────────────────────
    Serial.printf("[MAX ] Ping 0x%02X ... ", MAX30101_ADDR);
    if (i2c_ping(Wire, MAX30101_ADDR)) {
        max_part_id = i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_PART_ID);
        max_rev_id  = i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_REV_ID);
        bool id_ok = (max_part_id == MAX_PART_ID_VAL);
        Serial.printf("%s  PART_ID=0x%02X (expect 0x%02X)  REV_ID=0x%02X\n",
                      id_ok ? "ACK " : "BAD ID",
                      max_part_id, MAX_PART_ID_VAL, max_rev_id);
        if (id_ok) {
            // Soft reset, wait for RESET bit to self-clear.
            i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_MODE_CONFIG, MAX_MODE_RESET);
            uint32_t t0 = millis();
            while ((i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_MODE_CONFIG) & MAX_MODE_RESET)
                   && (millis() - t0) < 100) delay(2);
            // Enter HR mode (Red LED only), modest LED current — no skin needed
            // to confirm the FIFO write pointer is incrementing during dwell.
            i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_MODE_CONFIG, MAX_MODE_HR);
            i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_LED1_PA,    MAX_LED_PA_SMOKE);
            max_ok = true;
            Serial.println("       configured HR mode (Red LED ~6 mA) for dwell-loop sampling");
        }
    } else Serial.println("NO ACK -- check MAX solder / daughterboard connector / bus1");

    // ── TMP117 (bus 1, daughterboard U15) — probe both ADD0 strap options ────
    Serial.print("[TMP ] Ping 0x48 / 0x49 ... ");
    if (i2c_ping(Wire, TMP117_ADDR_LO))        tmp_addr_used = TMP117_ADDR_LO;
    else if (i2c_ping(Wire, TMP117_ADDR_HI))   tmp_addr_used = TMP117_ADDR_HI;
    if (tmp_addr_used) {
        if (tmp_read_word(tmp_addr_used, TMP_REG_DEVICE_ID, &tmp_device_id_raw)) {
            uint16_t id12 = tmp_device_id_raw & 0x0FFF;
            tmp_ok = (id12 == TMP_DEVICE_ID_LOW12);
            Serial.printf("%s @ 0x%02X  DEVICE_ID=0x%04X (low12 expect 0x%04X)\n",
                          tmp_ok ? "ACK " : "BAD ID",
                          tmp_addr_used, tmp_device_id_raw, TMP_DEVICE_ID_LOW12);
        } else {
            Serial.printf("ACK @ 0x%02X  but DEVICE_ID read FAILED\n", tmp_addr_used);
        }
    } else {
        Serial.println("NO ACK on either address -- check TMP solder / daughterboard / bus1");
    }
    Serial.println();

    // ── WS2812 colour walk ───────────────────────────────────────────────────
    Serial.printf("[WS  ] Colour walk on GPIO%d (R / G / B / off @ %u, 250 ms each)\n",
                  PIN_WS2812_DIN, (unsigned)WS_BOOT_LEVEL);
    neopixelWrite(PIN_WS2812_DIN, WS_BOOT_LEVEL, 0, 0); delay(250);
    neopixelWrite(PIN_WS2812_DIN, 0, WS_BOOT_LEVEL, 0); delay(250);
    neopixelWrite(PIN_WS2812_DIN, 0, 0, WS_BOOT_LEVEL); delay(250);
    neopixelWrite(PIN_WS2812_DIN, 0, 0, 0); delay(50);
    Serial.println("       Done -- visual confirmation only (no readback)\n");

    // ── PDM mic ──────────────────────────────────────────────────────────────
    Serial.printf("[MIC ] PDM init: CLK=GPIO%d DATA=GPIO%d  %u Hz / 16-bit / mono\n",
                  PIN_MIC_CLK, PIN_MIC_DATA, (unsigned)MIC_SAMPLE_RATE_HZ);
    i2s_mic.setPinsPdmRx(PIN_MIC_CLK, PIN_MIC_DATA);
    if (i2s_mic.begin(I2S_MODE_PDM_RX, MIC_SAMPLE_RATE_HZ,
                      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        i2s_mic.setTimeout(100);
        delay(30);
        const size_t N = 1024;
        int16_t drop[256];
        i2s_mic.readBytes((char *)drop, sizeof(drop));
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
                      mic_ok ? "OK   (samples varying)" : "FAIL (samples stuck)",
                      (unsigned)samples, mins, maxs, (long)spread, (double)rms);
    } else {
        Serial.println("       BEGIN FAILED -- check mic solder / clock / 3V3");
    }
    Serial.println();

    // ── Summary ──────────────────────────────────────────────────────────────
    Serial.println("---- Stage-3 Summary ----------------------");
    Serial.printf("  BQ25619    (charger,  bus2 0x6A) : %s\n", bq_ok   ? "PASS" : "FAIL");
    Serial.printf("  DRV2605L   (haptic,   bus2 0x5A) : %s  cal=%s  COMP=0x%02X BEMF=0x%02X (%u ms)\n",
                  drv_ok ? "PASS" : "FAIL",
                  drv_cal_ok ? "PASS" : "FAIL",
                  drv_cal_comp, drv_cal_bemf, (unsigned)drv_cal_ms);
    Serial.printf("  PCF85063A  (RTC,      bus1 0x51) : %s\n", rtc_ok  ? "PASS" : "FAIL");
    Serial.printf("  VEML6030   (ALS,      bus1 0x10) : %s\n", veml_ok ? "PASS" : "FAIL");
    Serial.printf("  LIS3MDLTR  (mag,      bus1 0x1C) : %s\n", lis_ok  ? "PASS" : "FAIL");
    Serial.printf("  LSM6DSV16X (IMU,      bus1 0x6B) : %s\n", lsm_ok  ? "PASS" : "FAIL");
    Serial.printf("  BME688     (env,      bus1 0x76) : %s\n", bme_ok  ? "PASS" : "FAIL");
    Serial.printf("  WS2812B    (RGB,      GPIO42)    : DRIVEN (visual only)\n");
    Serial.printf("  MSM261DGT  (mic, PDM 47/48)      : %s\n", mic_ok  ? "PASS" : "FAIL");
    Serial.printf("  MAX30101   (PPG,      bus1 0x57) : %s  PART=0x%02X REV=0x%02X\n",
                  max_ok  ? "PASS" : "FAIL", max_part_id, max_rev_id);
    Serial.printf("  TMP117     (temp,     bus1 0x%02X) : %s  DEVID=0x%04X\n",
                  tmp_addr_used ? tmp_addr_used : 0x48,
                  tmp_ok  ? "PASS" : "FAIL", tmp_device_id_raw);
    Serial.printf("  SDMMC      (CLK/CMD/D0 38/39/40) : %s", sd_ok   ? "PASS" : "FAIL");
    if (sd_ok) Serial.printf("  (%s, %llu MB, %d .txt)",
                             sd_card_kind, (unsigned long long)sd_card_size_mb, sd_txt_count);
    Serial.println();
    Serial.printf("  ALPS Enc   (A=21 B=43)           : DRIVEN (ISR, rotate to test)\n");
    Serial.println("-------------------------------------------");

    int fails = !bq_ok + !rtc_ok + !veml_ok + !lis_ok + !lsm_ok + !bme_ok
              + !mic_ok + !drv_ok + !sd_ok + !max_ok + !tmp_ok;
    if (fails == 0)
        Serial.println("  All electrical checks PASS -- entering 3-s dwell loop.");
    else
        Serial.printf("  %d device(s) FAIL -- inspect solder / bridges / pull-ups\n",
                      fails);

    // Attach encoder ISR AFTER setup banner: GPIO43 == U0TXD, and the ~3 ms
    // boot-log blip already happened before we got here (20.16 W-05).
    enc_last_us = micros();
    attachInterrupt(PIN_ENC_A, enc_isr_a, RISING);

    Serial.println("\n[BTN ] Press GPIO16 to toggle flashlight");
    Serial.println("[ENC ] Rotate crown -- CW / CCW will print as you turn\n");

    // ── SD card (1-bit SDMMC) — DEFERRED to the very end of setup ───────────
    // The previous boot panicked with "Stack canary watchpoint triggered (ipc1)"
    // *during* the summary print, several lines after SD_MMC.begin() returned
    // 0x107. The SDMMC driver schedules cleanup callbacks via the cross-core
    // IPC task (ipc1, ~1 kB stack on arduino-esp32 default), and on a failed
    // init those callbacks blow past the canary asynchronously. By running
    // SD last — *after* the summary, ISR attach, and user-facing prompts —
    // a crash here can't take out anything we care about. Reboot prompt and
    // loop() will still come up cleanly thanks to ipc1 catching it locally.
    // Stage 3 hardware note: CLK pull-up was missing on iv7.1 — bench-hacked
    // 10 kΩ to 3V2. 400 kHz is the SD identification clock; anything faster
    // previously failed signal integrity past CMD9 / ACMD51 / ACMD41.
    Serial.printf("[SD  ] SDMMC 1-bit init: CLK=GPIO%d CMD=GPIO%d D0=GPIO%d  @ 400 kHz\n",
                  PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    if (!SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0)) {
        Serial.println("       setPins FAILED -- pin config rejected by SD_MMC");
    } else if (!SD_MMC.begin("/sdcard", true /*mode1bit*/, false /*format_if_fail*/,
                             400 /*sdmmc_frequency, kHz*/)) {
        Serial.println("       mount FAILED -- card / wiring / signal integrity.");
        // Force synchronous teardown so the IPC cleanup doesn't run later
        // while we're already printing other stuff.
        SD_MMC.end();
    } else {
        sdcard_type_t t = SD_MMC.cardType();
        switch (t) {
            case CARD_MMC:  sd_card_kind = "MMC";   break;
            case CARD_SD:   sd_card_kind = "SDSC";  break;
            case CARD_SDHC: sd_card_kind = "SDHC";  break;
            case CARD_NONE: sd_card_kind = "NONE";  break;
            default:        sd_card_kind = "UNKN";  break;
        }
        sd_card_size_mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
        sd_ok = (t != CARD_NONE);
        Serial.printf("       mount OK  type=%s  size=%llu MB\n",
                      sd_card_kind, (unsigned long long)sd_card_size_mb);
        if (sd_ok) {
            sd_log_boot_summary();
            sd_txt_count = sd_count_txt_files();
            Serial.printf("       wrote /smoke_log.txt  total .txt files=%d\n",
                          sd_txt_count);
        }
    }
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
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS,
                  (0x02 << 5) | (0x05 << 2) | 0x01);
    delay(40);
    uint8_t d[8];
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_PRESS_ADC, d, sizeof(d))) {
        Serial.println("[BME ] read FAIL");
        return;
    }
    uint32_t press_adc = ((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4);
    uint32_t temp_adc  = ((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4);
    uint16_t hum_adc   = ((uint16_t)d[6] << 8) | d[7];

    float v1, v2, t_fine, T;
    v1 = ((float)temp_adc / 16384.0f - (float)bme_cal.par_t1 / 1024.0f)
         * (float)bme_cal.par_t2;
    v2 = (((float)temp_adc / 131072.0f - (float)bme_cal.par_t1 / 8192.0f)
        * ((float)temp_adc / 131072.0f - (float)bme_cal.par_t1 / 8192.0f))
        * ((float)bme_cal.par_t3 * 16.0f);
    t_fine = v1 + v2;
    T = t_fine / 5120.0f;

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
static void stream_sd(uint8_t tick) {
    // First tick of the dwell re-counts files (cheap: root only); other ticks
    // just echo cached size + status so we can see the card stays alive.
    if (tick == 0) {
        sd_txt_count = sd_count_txt_files();
    }
    Serial.printf("[SD  ] type=%s  size=%llu MB  used=%llu MB  .txt=%d\n",
                  sd_card_kind,
                  (unsigned long long)sd_card_size_mb,
                  (unsigned long long)(SD_MMC.usedBytes() / (1024ULL * 1024ULL)),
                  sd_txt_count);
}
static void stream_drv(uint8_t tick) {
    uint8_t id = tick < (sizeof(drv_effects) / sizeof(drv_effects[0]))
                 ? drv_effects[tick] : drv_effects[0];
    bool ok = drv_trigger(id);
    uint8_t st = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
    Serial.printf("[DRV ] play effect %3u %s  STATUS=0x%02X%s%s\n",
                  id, ok ? "GO" : "FAIL", st,
                  (st & 0x02) ? " OVER_TEMP" : "",
                  (st & 0x01) ? " OC_DETECT" : "");
}

static void stream_max(uint8_t /*tick*/) {
    // FIFO pointers tell us whether the AFE is actually sampling.
    // wr - rd (modulo 32) = unread samples. In HR mode with Red LED on, the
    // pointer should advance several counts between 500-ms dwell ticks.
    uint8_t wr   = i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_FIFO_WR_PTR) & 0x1F;
    uint8_t rd   = i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_FIFO_RD_PTR) & 0x1F;
    uint8_t ovf  = i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_OVERFLOW_CTR);
    uint8_t ints = i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_INT_STATUS_1);
    // Read one FIFO sample (3 bytes, 18-bit Red channel) just to look at the level.
    uint8_t s[3] = {0};
    i2c_read_buf(Wire, MAX30101_ADDR, MAX_REG_FIFO_DATA, s, 3);
    uint32_t red = ((uint32_t)(s[0] & 0x03) << 16) | ((uint32_t)s[1] << 8) | s[2];
    uint8_t unread = (uint8_t)((wr - rd) & 0x1F);
    Serial.printf("[MAX ] WR=%u RD=%u unread=%u OVF=%u INT=0x%02X  Red=%lu\n",
                  wr, rd, unread, ovf, ints, (unsigned long)red);
}
static void stream_tmp(uint8_t /*tick*/) {
    uint16_t raw = 0;
    if (!tmp_read_word(tmp_addr_used, TMP_REG_TEMP_RESULT, &raw)) {
        Serial.println("[TMP ] read FAIL");
        return;
    }
    int16_t s = (int16_t)raw;
    float tc = (float)s * TMP_LSB_C;
    Serial.printf("[TMP ] @ 0x%02X  raw=0x%04X  T=%6.3fC\n",
                  tmp_addr_used, raw, (double)tc);
}

static const char *sensor_name(uint8_t idx) {
    switch (idx) {
        case 0:  return "RTC";
        case 1:  return "BQ";
        case 2:  return "VEML";
        case 3:  return "LIS";
        case 4:  return "LSM";
        case 5:  return "BME";
        case 6:  return "MIC";
        case 7:  return "WS";
        case 8:  return "SD";
        case 9:  return "DRV";
        case 10: return "MAX";
        case 11: return "TMP";
        default: return "?";
    }
}
static bool sensor_ok(uint8_t idx) {
    switch (idx) {
        case 0:  return rtc_ok;
        case 1:  return bq_ok;
        case 2:  return veml_ok;
        case 3:  return lis_ok;
        case 4:  return lsm_ok;
        case 5:  return bme_ok;
        case 6:  return mic_ok;
        case 7:  return true;   // WS no probe
        case 8:  return sd_ok;
        case 9:  return drv_ok;
        case 10: return max_ok;
        case 11: return tmp_ok;
        default: return false;
    }
}
static void sensor_dispatch(uint8_t idx, uint8_t tick) {
    if (!sensor_ok(idx)) {
        Serial.printf("[%s] skipped (FAIL)\n", sensor_name(idx));
        return;
    }
    switch (idx) {
        case 0:  stream_rtc (tick); break;
        case 1:  stream_bq  (tick); break;
        case 2:  stream_veml(tick); break;
        case 3:  stream_lis (tick); break;
        case 4:  stream_lsm (tick); break;
        case 5:  stream_bme (tick); break;
        case 6:  stream_mic (tick); break;
        case 7:  stream_ws  (tick); break;
        case 8:  stream_sd  (tick); break;
        case 9:  stream_drv (tick); break;
        case 10: stream_max (tick); break;
        case 11: stream_tmp (tick); break;
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

    // Encoder: drain accumulated delta from ISR, print one line per detent.
    int32_t d = enc_delta;
    if (d != 0) {
        enc_delta = 0;          // race-OK: any new ticks come right back next loop
        while (d > 0) { Serial.println("[ENC ] CW  ->"); d--; }
        while (d < 0) { Serial.println("[ENC ] CCW <-"); d++; }
    }

    // Dwell-based per-device streaming
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
            if (active != 7) neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
        }
    }

    // Sine breathe on flashlight, clamped to LED_MAX_DUTY (current cap until
    // R26 is back in place — see top-of-file note).
    if (!led_on) { delay(10); return; }
    static float phase = 0.0f;
    phase += 0.03f;
    if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    float s = (sinf(phase) + 1.0f) * 0.5f;                       // 0.0 .. 1.0
    uint8_t duty = (uint8_t)(s * (float)(LED_MAX_DUTY - 4) + 4); // 4 .. LED_MAX_DUTY
    led_duty(duty);
    delay(10);
}
