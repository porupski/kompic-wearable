/*
 * 5_demo_sensor_logger  --  Multi-sensor rolling data logger + PDM WAV recorder
 *                 for power profiling and sanity-check bring-up on
 *                 Kompic Mk I (iv7.1)
 *
 * Refactored 2026-07-05 as part of the Stage 5 close-out. Renamed from
 * 7_demo_mk1 in the pre-refactor legacy set. See
 * hardware/Reflow_info/Stage_5_Build_Report.md for context.
 *
 * Simpler successor to the pair-testing version (which showed no meaningful
 * current delta between solo and pair phases -- pairs dropped 2026-07-05).
 * Now: 10 s idle baseline, then each sensor solo for 10 s, printing live
 * data samples to Serial while writing to SD. Ammeter markers on stage
 * boundaries let Ivan correlate USB current draw to the active sensor.
 *
 * Coverage today (Stage 5): LIS, LSM, BME, MIC, VEML. MAX30101 and TMP117
 * are exercised by 3_smoke_stage3_full but not yet wired into this rolling
 * logger. Extension planned in a follow-up field-data-collection sketch.
 *
 * Phase list (~65 s total):
 *   P00 idle_baseline   10 s  -- all sensors parked, ammeter floor
 *   P01 lis             10 s  -- LIS3MDLTR       (mag, 40 Hz)
 *   P02 lsm             10 s  -- LSM6DSV16X      (IMU 6-axis + T, 100 Hz)
 *   P03 bme             10 s  -- BME688          (T/P/H forced, 2 Hz)
 *   P04 mic             10 s  -- MSM261DGT003    (PDM 48 kHz mono WAV, 16 dB gain)
 *   P05 veml            10 s  -- VEML6030        (ALS, 5 Hz)
 *   P06 idle_after       5 s  -- return-to-baseline check
 *
 * Files:
 *   /data/lis /s<seq>_P01_lis.csv
 *   /data/lsm /s<seq>_P02_lsm.csv
 *   /data/bme /s<seq>_P03_bme.csv
 *   /data/mic /s<seq>_P04_mic.wav   (+ .txt sidecar for provenance)
 *   /data/veml/s<seq>_P05_veml.csv
 *
 * Where <seq> is a persistent boot counter in NVS (namespace "demo7",
 * key "boot_seq"). Each CSV has a header-comment block naming sketch,
 * sensor, phase, RTC start time (from PCF85063A), and sample rate.
 *
 * During each phase, a heartbeat line is printed to Serial roughly once
 * per second showing the most recent live values, so Ivan can eyeball
 * the sensor sanity without opening files.
 *
 * MIC GAIN: each 16-bit sample is multiplied by MIC_GAIN_MULT (default 16
 * = +24 dB) with saturation before being written to the WAV. Noise floor
 * scales with the signal; if silence sounds noisy you can drop the gain.
 *
 * MANDATORY: double-click on the button (GPIO16 == BQ25619 /QON) enters
 * BQ ship mode. All open files are flushed and closed (WAV header patched
 * with actual bytes written) before the BATFET write.
 */

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <FS.h>
#include <Preferences.h>
#include "ESP_I2S.h"
#include "driver/gpio.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_SD_CLK        38
#define PIN_SD_CMD        39
#define PIN_SD_D0         40
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4
#define PIN_SCL_BUS2       5
#define PIN_MIC_CLK       47
#define PIN_MIC_DATA      48

// ── I2C addresses ────────────────────────────────────────────────────────────
#define VEML6030_ADDR     0x10
#define LIS3MDL_ADDR      0x1C
#define PCF85063A_ADDR    0x51
#define LSM6DSV_ADDR      0x6B
#define BME688_ADDR       0x76
#define BQ25619_ADDR      0x6A

// ── LIS3MDLTR (40 Hz, ultra-high-perf, temp on) ─────────────────────────────
#define LIS_REG_WHO       0x0F
#define LIS_WHO_VAL       0x3D
#define LIS_REG_CTRL1     0x20
#define LIS_REG_CTRL2     0x21
#define LIS_REG_CTRL3     0x22
#define LIS_REG_CTRL4     0x23
#define LIS_REG_CTRL5     0x24
#define LIS_REG_OUT_X_L   0x28
#define LIS_AUTO_INC      0x80
#define LIS_CTRL1_RUN     0xF8    // TEMP_EN | OM=UHP | DO=40 Hz
#define LIS_CTRL3_PWRDN   0x03

// ── LSM6DSV16X (240 Hz ODR, sampled at 100 Hz) ──────────────────────────────
#define LSM_REG_WHO         0x0F
#define LSM_WHO_VAL         0x70
#define LSM_REG_CTRL1       0x10
#define LSM_REG_CTRL2       0x11
#define LSM_REG_CTRL3       0x12
#define LSM_REG_OUT_TEMP    0x20
#define LSM_ODR_240HZ       0x07

// ── BME688 (forced mode, 2 Hz) ──────────────────────────────────────────────
#define BME_REG_CHIP_ID      0xD0
#define BME_REG_RESET        0xE0
#define BME_REG_CTRL_HUM     0x72
#define BME_REG_CTRL_GAS1    0x71
#define BME_REG_CTRL_MEAS    0x74
#define BME_REG_PRESS_ADC    0x1F
#define BME_REG_CAL_BLK1     0x8A
#define BME_REG_CAL_BLK2     0xE1
#define BME_CHIP_ID_VAL      0x61
#define BME_CTRL_MEAS_FORCED ((0x02 << 5) | (0x05 << 2) | 0x01)
#define BME_CTRL_MEAS_SLEEP  0x00

// ── VEML6030 (5 Hz) ─────────────────────────────────────────────────────────
#define VEML_REG_CONF        0x00
#define VEML_REG_ALS         0x04
#define VEML_REG_WHITE       0x05
#define VEML_CONF_RUN        ((uint16_t)((0x03 << 11) | (0x00 << 6)))
#define VEML_CONF_SHUTDOWN   ((uint16_t)0x0001)
#define VEML_LX_PER_CT       0.2304f

// ── PCF85063A RTC ────────────────────────────────────────────────────────────
#define RTC_REG_CTRL1        0x00
#define RTC_REG_SECONDS      0x04
#define RTC_OS_BIT           0x80

// ── BQ25619 ship mode ───────────────────────────────────────────────────────
#define BQ_REG_MISC_OP        0x07
#define BQ_REG_STATUS         0x08
#define BQ_STATUS_VBUS_MASK   0xE0
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)

// ── Mic ──────────────────────────────────────────────────────────────────────
#define MIC_SAMPLE_HZ         48000
#define MIC_CHUNK_SAMPLES     480      // ~10 ms per read
#define MIC_GAIN_MULT         8       // +18 dB digital gain, minimal noise floor

// ── Sensor bitmask (single-sensor phases -- one bit set at most) ────────────
#define S_LIS   (1u << 0)
#define S_LSM   (1u << 1)
#define S_BME   (1u << 2)
#define S_MIC   (1u << 3)
#define S_VEML  (1u << 4)

// ── Per-sensor sample intervals (ms) ────────────────────────────────────────
#define LIS_INTERVAL_MS     25    // 40 Hz
#define LSM_INTERVAL_MS     10    // 100 Hz
#define BME_INTERVAL_MS     500   // 2 Hz (forced)
#define VEML_INTERVAL_MS    200   // 5 Hz

// ── Phase table ──────────────────────────────────────────────────────────────
typedef struct {
    const char *label;
    uint32_t    duration_ms;
    uint16_t    sensor;    // exactly one bit, or 0 for idle
} phase_t;

static const phase_t PHASES[] = {
    { "P00_idle_baseline",  10000, 0      },
    { "P01_lis",            10000, S_LIS  },
    { "P02_lsm",            10000, S_LSM  },
    { "P03_bme",            10000, S_BME  },
    { "P04_mic",            10000, S_MIC  },
    { "P05_veml",           10000, S_VEML },
    { "P06_idle_after",      5000, 0      },
};
static const size_t N_PHASES = sizeof(PHASES) / sizeof(PHASES[0]);

// ── BME688 calibration struct ───────────────────────────────────────────────
typedef struct {
    uint16_t par_t1; int16_t par_t2; int8_t par_t3;
    uint16_t par_p1; int16_t par_p2; int8_t par_p3;
    int16_t par_p4; int16_t par_p5; int8_t par_p6; int8_t par_p7;
    int16_t par_p8; int16_t par_p9; uint8_t par_p10;
    uint16_t par_h1; uint16_t par_h2; int8_t par_h3;
    int8_t par_h4; int8_t par_h5; uint8_t par_h6; int8_t par_h7;
} bme_cal_t;

// ── Globals ──────────────────────────────────────────────────────────────────
static bme_cal_t   g_bme_cal = {0};
static bool        g_lis_ok = false, g_lsm_ok = false, g_bme_ok = false;
static bool        g_veml_ok = false, g_mic_ok = false, g_rtc_ok = false, g_sd_ok = false;
static Preferences g_prefs;
static I2SClass    g_i2s_mic;
static uint32_t    g_boot_seq = 0;
static char        g_rtc_start[32] = "unset";
static uint32_t    g_ms_boot_start = 0;

// Current-phase file + WAV byte count.
static File     g_file;
static uint32_t g_mic_bytes_written = 0;

// ── I2C helpers ─────────────────────────────────────────────────────────────
static bool i2c_write_reg(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t val) {
    bus.beginTransmission(addr);
    bus.write(reg);
    bus.write(val);
    return bus.endTransmission() == 0;
}
static uint8_t i2c_read_reg(TwoWire &bus, uint8_t addr, uint8_t reg) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return 0xFF;
    if (bus.requestFrom((int)addr, 1) != 1) return 0xFF;
    return bus.read();
}
static bool i2c_read_buf(TwoWire &bus, uint8_t addr, uint8_t reg,
                         uint8_t *out, size_t n) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return false;
    if (bus.requestFrom((int)addr, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; i++) out[i] = bus.read();
    return true;
}
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
    Wire.write((uint8_t)(val & 0xFF));
    Wire.write((uint8_t)((val >> 8) & 0xFF));
    return Wire.endTransmission() == 0;
}

// ── Sensor probe / park / enable / read ─────────────────────────────────────
static bool lis_probe(void) {
    if (i2c_read_reg(Wire, LIS3MDL_ADDR, LIS_REG_WHO) != LIS_WHO_VAL) return false;
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL2, 0x00);
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL4, (0x03 << 2));
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL5, 0x40);
    return true;
}
static void lis_park(void)   { i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL3, LIS_CTRL3_PWRDN); }
static void lis_enable(void) {
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL1, LIS_CTRL1_RUN);
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL3, 0x00);
}
static bool lis_read(int16_t *x, int16_t *y, int16_t *z) {
    uint8_t b[6];
    if (!i2c_read_buf(Wire, LIS3MDL_ADDR, LIS_AUTO_INC | LIS_REG_OUT_X_L, b, 6)) return false;
    *x = (int16_t)((b[1] << 8) | b[0]);
    *y = (int16_t)((b[3] << 8) | b[2]);
    *z = (int16_t)((b[5] << 8) | b[4]);
    return true;
}

static bool lsm_probe(void) {
    if (i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WHO) != LSM_WHO_VAL) return false;
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x01);
    delay(20);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x44);
    return true;
}
static void lsm_park(void) {
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, 0x00);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, 0x00);
}
static void lsm_enable(void) {
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, LSM_ODR_240HZ);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, LSM_ODR_240HZ);
}
static bool lsm_read(int16_t *tc, int16_t *gx, int16_t *gy, int16_t *gz,
                                  int16_t *ax, int16_t *ay, int16_t *az) {
    uint8_t b[14];
    if (!i2c_read_buf(Wire, LSM6DSV_ADDR, LSM_REG_OUT_TEMP, b, 14)) return false;
    *tc = (int16_t)((b[1]  << 8) | b[0]);
    *gx = (int16_t)((b[3]  << 8) | b[2]);
    *gy = (int16_t)((b[5]  << 8) | b[4]);
    *gz = (int16_t)((b[7]  << 8) | b[6]);
    *ax = (int16_t)((b[9]  << 8) | b[8]);
    *ay = (int16_t)((b[11] << 8) | b[10]);
    *az = (int16_t)((b[13] << 8) | b[12]);
    return true;
}

static bool bme_read_calibration(void) {
    uint8_t b1[23], b2[14];
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_CAL_BLK1, b1, sizeof(b1))) return false;
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_CAL_BLK2, b2, sizeof(b2))) return false;
    g_bme_cal.par_t2  = (int16_t)((uint16_t)b1[1]  << 8 | b1[0]);
    g_bme_cal.par_t3  = (int8_t)b1[2];
    g_bme_cal.par_p1  = (uint16_t)((uint16_t)b1[5]  << 8 | b1[4]);
    g_bme_cal.par_p2  = (int16_t)((uint16_t)b1[7]  << 8 | b1[6]);
    g_bme_cal.par_p3  = (int8_t)b1[8];
    g_bme_cal.par_p4  = (int16_t)((uint16_t)b1[11] << 8 | b1[10]);
    g_bme_cal.par_p5  = (int16_t)((uint16_t)b1[13] << 8 | b1[12]);
    g_bme_cal.par_p7  = (int8_t)b1[14];
    g_bme_cal.par_p6  = (int8_t)b1[15];
    g_bme_cal.par_p8  = (int16_t)((uint16_t)b1[19] << 8 | b1[18]);
    g_bme_cal.par_p9  = (int16_t)((uint16_t)b1[21] << 8 | b1[20]);
    g_bme_cal.par_p10 = b1[22];
    g_bme_cal.par_h2  = ((uint16_t)b2[0] << 4) | (b2[1] >> 4);
    g_bme_cal.par_h1  = ((uint16_t)b2[2] << 4) | (b2[1] & 0x0F);
    g_bme_cal.par_h3  = (int8_t)b2[3];
    g_bme_cal.par_h4  = (int8_t)b2[4];
    g_bme_cal.par_h5  = (int8_t)b2[5];
    g_bme_cal.par_h6  = b2[6];
    g_bme_cal.par_h7  = (int8_t)b2[7];
    g_bme_cal.par_t1  = (uint16_t)((uint16_t)b2[9] << 8 | b2[8]);
    return true;
}
static bool bme_probe(void) {
    if (i2c_read_reg(Wire, BME688_ADDR, BME_REG_CHIP_ID) != BME_CHIP_ID_VAL) return false;
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_RESET, 0xB6);
    delay(10);
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_GAS1, 0x00);
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_HUM,  0x01);
    return bme_read_calibration();
}
static void bme_park(void)   { i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS, BME_CTRL_MEAS_SLEEP); }
static void bme_enable(void) { /* per-read trigger */ }
static bool bme_read(float *T_C, float *P_hPa, float *H_RH) {
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS, BME_CTRL_MEAS_FORCED);
    delay(45);
    uint8_t d[8];
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_PRESS_ADC, d, sizeof(d))) return false;
    uint32_t p_adc = ((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4);
    uint32_t t_adc = ((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4);
    uint16_t h_adc = ((uint16_t)d[6] << 8)  | d[7];

    float v1 = ((float)t_adc / 16384.0f - (float)g_bme_cal.par_t1 / 1024.0f)
             * (float)g_bme_cal.par_t2;
    float v2 = (((float)t_adc / 131072.0f - (float)g_bme_cal.par_t1 / 8192.0f)
              * ((float)t_adc / 131072.0f - (float)g_bme_cal.par_t1 / 8192.0f))
             * ((float)g_bme_cal.par_t3 * 16.0f);
    float t_fine = v1 + v2;
    *T_C = t_fine / 5120.0f;

    v1 = (t_fine / 2.0f) - 64000.0f;
    v2 = v1 * v1 * ((float)g_bme_cal.par_p6 / 131072.0f);
    v2 = v2 + (v1 * (float)g_bme_cal.par_p5 * 2.0f);
    v2 = (v2 / 4.0f) + ((float)g_bme_cal.par_p4 * 65536.0f);
    v1 = ((((float)g_bme_cal.par_p3 * v1 * v1) / 16384.0f)
        + ((float)g_bme_cal.par_p2 * v1)) / 524288.0f;
    v1 = (1.0f + (v1 / 32768.0f)) * (float)g_bme_cal.par_p1;
    float P = 1048576.0f - (float)p_adc;
    if (v1 != 0.0f) {
        P = ((P - (v2 / 4096.0f)) * 6250.0f) / v1;
        v1 = ((float)g_bme_cal.par_p9 * P * P) / 2147483648.0f;
        v2 = P * ((float)g_bme_cal.par_p8 / 32768.0f);
        float v3 = (P / 256.0f) * (P / 256.0f) * (P / 256.0f)
                 * ((float)g_bme_cal.par_p10 / 131072.0f);
        P = P + (v1 + v2 + v3 + ((float)g_bme_cal.par_p7 * 128.0f)) / 16.0f;
    }
    *P_hPa = P / 100.0f;

    float tc = t_fine / 5120.0f;
    v1 = (float)h_adc - (((float)g_bme_cal.par_h1 * 16.0f)
                       + (((float)g_bme_cal.par_h3 / 2.0f) * tc));
    v2 = v1 * (((float)g_bme_cal.par_h2 / 262144.0f)
       * (1.0f + (((float)g_bme_cal.par_h4 / 16384.0f) * tc)
              + (((float)g_bme_cal.par_h5 / 1048576.0f) * tc * tc)));
    float h3f = (float)g_bme_cal.par_h6 / 16384.0f;
    float h4f = (float)g_bme_cal.par_h7 / 2097152.0f;
    float H = v2 + ((h3f + (h4f * tc)) * v2 * v2);
    if (H > 100.0f) H = 100.0f;
    if (H <   0.0f) H =   0.0f;
    *H_RH = H;
    return true;
}

static bool veml_probe(void) {
    if (!veml_write_word(VEML_REG_CONF, VEML_CONF_RUN)) return false;
    delay(120);
    uint16_t rb = 0;
    if (!veml_read_word(VEML_REG_CONF, &rb)) return false;
    return (rb == VEML_CONF_RUN);
}
static void veml_park(void)   { veml_write_word(VEML_REG_CONF, VEML_CONF_SHUTDOWN); }
static void veml_enable(void) { veml_write_word(VEML_REG_CONF, VEML_CONF_RUN); }
static bool veml_read(uint16_t *als, uint16_t *white) {
    if (!veml_read_word(VEML_REG_ALS,   als))   return false;
    if (!veml_read_word(VEML_REG_WHITE, white)) return false;
    return true;
}

static bool mic_probe(void) {
    g_i2s_mic.setPinsPdmRx(PIN_MIC_CLK, PIN_MIC_DATA);
    if (!g_i2s_mic.begin(I2S_MODE_PDM_RX, MIC_SAMPLE_HZ,
                         I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) return false;
    g_i2s_mic.setTimeout(50);
    int16_t drop[128];
    g_i2s_mic.readBytes((char *)drop, sizeof(drop));
    int16_t buf[512];
    size_t got = g_i2s_mic.readBytes((char *)buf, sizeof(buf));
    size_t n = got / 2;
    int16_t mn = INT16_MAX, mx = INT16_MIN;
    for (size_t i = 0; i < n; i++) { if (buf[i] < mn) mn = buf[i]; if (buf[i] > mx) mx = buf[i]; }
    return (n > 0) && ((mx - mn) > 200);
}

// Saturating gain multiplier for a single sample.
static inline int16_t mic_gain(int16_t s) {
    int32_t v = (int32_t)s * MIC_GAIN_MULT;
    if (v >  INT16_MAX) v = INT16_MAX;
    if (v <  INT16_MIN) v = INT16_MIN;
    return (int16_t)v;
}

// ── RTC read → string ───────────────────────────────────────────────────────
static void rtc_read_to_string(char *out, size_t n) {
    if (!g_rtc_ok) { snprintf(out, n, "unset"); return; }
    uint8_t b[7];
    if (!i2c_read_buf(Wire, PCF85063A_ADDR, RTC_REG_SECONDS, b, sizeof(b))) {
        snprintf(out, n, "read_fail");
        return;
    }
    if (b[0] & RTC_OS_BIT) { snprintf(out, n, "oscstop"); return; }
    uint8_t sec = ((b[0] >> 4) & 0x07) * 10 + (b[0] & 0x0F);
    uint8_t min = ((b[1] >> 4) & 0x07) * 10 + (b[1] & 0x0F);
    uint8_t hr  = ((b[2] >> 4) & 0x03) * 10 + (b[2] & 0x0F);
    uint8_t day = ((b[3] >> 4) & 0x03) * 10 + (b[3] & 0x0F);
    uint8_t mon = ((b[5] >> 4) & 0x01) * 10 + (b[5] & 0x0F);
    uint8_t yr2 = ((b[6] >> 4) & 0x0F) * 10 + (b[6] & 0x0F);
    snprintf(out, n, "20%02u-%02u-%02uT%02u:%02u:%02u",
             (unsigned)yr2, (unsigned)mon, (unsigned)day,
             (unsigned)hr,  (unsigned)min, (unsigned)sec);
}

// ── WAV header helpers ──────────────────────────────────────────────────────
static void wav_write_header(File &f, uint32_t sample_hz, uint16_t bits,
                             uint16_t chans, uint32_t data_bytes) {
    uint16_t block_align  = chans * (bits / 8);
    uint32_t byte_rate    = sample_hz * block_align;
    uint32_t chunk_size   = 36 + data_bytes;
    uint32_t subchunk1_sz = 16;
    uint16_t audio_fmt    = 1;

    f.write((const uint8_t *)"RIFF", 4);
    f.write((uint8_t *)&chunk_size, 4);
    f.write((const uint8_t *)"WAVE", 4);
    f.write((const uint8_t *)"fmt ", 4);
    f.write((uint8_t *)&subchunk1_sz, 4);
    f.write((uint8_t *)&audio_fmt, 2);
    f.write((uint8_t *)&chans, 2);
    f.write((uint8_t *)&sample_hz, 4);
    f.write((uint8_t *)&byte_rate, 4);
    f.write((uint8_t *)&block_align, 2);
    f.write((uint8_t *)&bits, 2);
    f.write((const uint8_t *)"data", 4);
    f.write((uint8_t *)&data_bytes, 4);
}
static void wav_patch_size(File &f, uint32_t data_bytes) {
    uint32_t chunk_size = 36 + data_bytes;
    f.seek(4);   f.write((uint8_t *)&chunk_size, 4);
    f.seek(40);  f.write((uint8_t *)&data_bytes, 4);
    f.seek(f.size());
}

// ── SD helpers ──────────────────────────────────────────────────────────────
static void sd_mkdirs(void) {
    SD_MMC.mkdir("/data");
    SD_MMC.mkdir("/data/lis");
    SD_MMC.mkdir("/data/lsm");
    SD_MMC.mkdir("/data/bme");
    SD_MMC.mkdir("/data/mic");
    SD_MMC.mkdir("/data/veml");
}
static void csv_write_header(File &f, const char *sensor, const char *phase,
                             const char *csv_columns, uint32_t rate_hz) {
    f.printf("# sketch=7_demo_mk1\n");
    f.printf("# sensor=%s\n", sensor);
    f.printf("# phase=%s\n", phase);
    f.printf("# rtc_start=%s\n", g_rtc_start);
    f.printf("# ms_boot_start=%lu\n", (unsigned long)g_ms_boot_start);
    f.printf("# rate_hz=%lu\n", (unsigned long)rate_hz);
    f.printf("%s\n", csv_columns);
    f.flush();
}
static void mic_wav_open_header(File &f, const char *phase, uint32_t est_bytes) {
    wav_write_header(f, MIC_SAMPLE_HZ, 16, 1, est_bytes);
    // Sidecar .txt with provenance (RTC start, phase, boot seq).
    char sidecar[80];
    snprintf(sidecar, sizeof(sidecar), "%s", f.path());
    size_t slen = strlen(sidecar);
    if (slen > 4 && strcmp(sidecar + slen - 4, ".wav") == 0) {
        strcpy(sidecar + slen - 4, ".txt");
    }
    File sf = SD_MMC.open(sidecar, FILE_WRITE);
    if (sf) {
        sf.printf("sketch=7_demo_mk1\nsensor=msm261dgt003\nphase=%s\n"
                  "rtc_start=%s\nms_boot_start=%lu\nsample_hz=%u\n"
                  "bits=16\nchannels=1\ngain_mult=%d\n",
                  phase, g_rtc_start, (unsigned long)g_ms_boot_start,
                  (unsigned)MIC_SAMPLE_HZ, MIC_GAIN_MULT);
        sf.close();
    }
}

// ── Ship-mode + button state machine ─────────────────────────────────────────
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS    350
typedef enum { BTN_IDLE, BTN_PRESSED, BTN_WAIT_DBL, BTN_PRESSED_2 } btn_state_t;

static void close_current_file(void) {
    if (!g_file) return;
    // Detect WAV files by extension so we patch the header before closing.
    const char *p = g_file.path();
    size_t n = strlen(p);
    if (n > 4 && strcmp(p + n - 4, ".wav") == 0) {
        wav_patch_size(g_file, g_mic_bytes_written);
    }
    g_file.flush();
    g_file.close();
}
static void enter_ship_mode(void) {
    Serial.println("[BTN ] DOUBLE -> ship mode requested");
    close_current_file();
    while (digitalRead(PIN_BUTTON) == LOW) delay(5);
    delay(30);

    uint8_t r07 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP);
    uint8_t r07_new = r07;
    r07_new |=  BQ_BATFET_DIS;
    r07_new |=  BQ_BATFET_RST_WVBUS;
    r07_new &= ~BQ_BATFET_DLY;
    r07_new &= ~BQ_BATFET_RST_EN;
    Serial.printf("       writing REG07 0x%02X -> 0x%02X\n", r07, r07_new);
    i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP, r07_new);

    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    if ((st & BQ_STATUS_VBUS_MASK) != 0) {
        Serial.println("       USB present -- BATFET off but chip stays awake.");
    } else {
        Serial.println("       BATFET off -- expecting power loss now.");
    }
}
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
            if (state == BTN_PRESSED)        { release_ms = now; state = BTN_WAIT_DBL; }
            else if (state == BTN_PRESSED_2) { state = BTN_IDLE; enter_ship_mode(); }
        }
    }
    if (state == BTN_WAIT_DBL && (now - release_ms) > BTN_DOUBLE_GAP_MS) state = BTN_IDLE;
}

// ── Phase runner ────────────────────────────────────────────────────────────
static void run_phase(const phase_t &ph) {
    Serial.println();
    Serial.printf("== STAGE %s  (duration %lu ms)  t=%lu ms ==\n",
                  ph.label, (unsigned long)ph.duration_ms, (unsigned long)millis());

    // Configure sensor + open file.
    char path[80];
    switch (ph.sensor) {
    case S_LIS:
        if (!g_lis_ok) { Serial.println("   LIS unavailable, skipping"); break; }
        lis_enable();
        snprintf(path, sizeof(path), "/data/lis/s%04lu_%s.csv",
                 (unsigned long)g_boot_seq, ph.label);
        g_file = SD_MMC.open(path, FILE_WRITE);
        if (g_file) csv_write_header(g_file, "lis3mdl", ph.label, "ms,x,y,z", 1000/LIS_INTERVAL_MS);
        break;
    case S_LSM:
        if (!g_lsm_ok) { Serial.println("   LSM unavailable, skipping"); break; }
        lsm_enable();
        snprintf(path, sizeof(path), "/data/lsm/s%04lu_%s.csv",
                 (unsigned long)g_boot_seq, ph.label);
        g_file = SD_MMC.open(path, FILE_WRITE);
        if (g_file) csv_write_header(g_file, "lsm6dsv16x", ph.label, "ms,ax,ay,az,gx,gy,gz,tc", 1000/LSM_INTERVAL_MS);
        break;
    case S_BME:
        if (!g_bme_ok) { Serial.println("   BME unavailable, skipping"); break; }
        bme_enable();
        snprintf(path, sizeof(path), "/data/bme/s%04lu_%s.csv",
                 (unsigned long)g_boot_seq, ph.label);
        g_file = SD_MMC.open(path, FILE_WRITE);
        if (g_file) csv_write_header(g_file, "bme688", ph.label, "ms,tC,pHPa,hRH", 1000/BME_INTERVAL_MS);
        break;
    case S_VEML:
        if (!g_veml_ok) { Serial.println("   VEML unavailable, skipping"); break; }
        veml_enable();
        snprintf(path, sizeof(path), "/data/veml/s%04lu_%s.csv",
                 (unsigned long)g_boot_seq, ph.label);
        g_file = SD_MMC.open(path, FILE_WRITE);
        if (g_file) csv_write_header(g_file, "veml6030", ph.label, "ms,als,white", 1000/VEML_INTERVAL_MS);
        break;
    case S_MIC:
        if (!g_mic_ok) { Serial.println("   MIC unavailable, skipping"); break; }
        snprintf(path, sizeof(path), "/data/mic/s%04lu_%s.wav",
                 (unsigned long)g_boot_seq, ph.label);
        g_file = SD_MMC.open(path, FILE_WRITE);
        g_mic_bytes_written = 0;
        if (g_file) {
            uint32_t est = MIC_SAMPLE_HZ * 2 * (ph.duration_ms / 1000);
            mic_wav_open_header(g_file, ph.label, est);
        }
        break;
    case 0: default:
        // idle phase -- no file, no sensor enable
        break;
    }
    if (g_file) Serial.printf("   -> writing %s\n", path);
    else if (ph.sensor != 0) Serial.println("   (no file open)");

    // Sample loop.
    uint32_t t_start   = millis();
    uint32_t t_end     = t_start + ph.duration_ms;
    uint32_t next_lis  = t_start, next_lsm = t_start;
    uint32_t next_bme  = t_start, next_veml = t_start;
    uint32_t next_hb   = t_start + 1000;   // heartbeat every 1 s
    static int16_t mic_buf[MIC_CHUNK_SAMPLES];
    uint32_t sample_count = 0;

    // Cache the last read for the heartbeat print.
    int16_t last_x=0, last_y=0, last_z=0;
    int16_t last_ax=0, last_ay=0, last_az=0, last_gx=0, last_gy=0, last_gz=0, last_tc=0;
    float   last_T=0, last_P=0, last_H=0;
    uint16_t last_als=0, last_white=0;
    int16_t  last_mic_mn=0, last_mic_mx=0;

    while (millis() < t_end) {
        handle_button();
        uint32_t now = millis();

        if (ph.sensor == S_LSM && g_lsm_ok && (int32_t)(now - next_lsm) >= 0) {
            if (lsm_read(&last_tc, &last_gx, &last_gy, &last_gz,
                         &last_ax, &last_ay, &last_az) && g_file) {
                g_file.printf("%lu,%d,%d,%d,%d,%d,%d,%d\n",
                              (unsigned long)now, last_ax, last_ay, last_az,
                              last_gx, last_gy, last_gz, last_tc);
                sample_count++;
                if ((sample_count % 100) == 0) g_file.flush();
            }
            next_lsm = now + LSM_INTERVAL_MS;
        }
        if (ph.sensor == S_LIS && g_lis_ok && (int32_t)(now - next_lis) >= 0) {
            if (lis_read(&last_x, &last_y, &last_z) && g_file) {
                g_file.printf("%lu,%d,%d,%d\n",
                              (unsigned long)now, last_x, last_y, last_z);
                sample_count++;
                if ((sample_count % 40) == 0) g_file.flush();
            }
            next_lis = now + LIS_INTERVAL_MS;
        }
        if (ph.sensor == S_VEML && g_veml_ok && (int32_t)(now - next_veml) >= 0) {
            if (veml_read(&last_als, &last_white) && g_file) {
                g_file.printf("%lu,%u,%u\n",
                              (unsigned long)now, last_als, last_white);
                sample_count++;
                g_file.flush();
            }
            next_veml = now + VEML_INTERVAL_MS;
        }
        if (ph.sensor == S_BME && g_bme_ok && (int32_t)(now - next_bme) >= 0) {
            if (bme_read(&last_T, &last_P, &last_H) && g_file) {
                g_file.printf("%lu,%.3f,%.3f,%.3f\n",
                              (unsigned long)now, last_T, last_P, last_H);
                sample_count++;
                g_file.flush();
            }
            next_bme = now + BME_INTERVAL_MS;
        }
        if (ph.sensor == S_MIC && g_mic_ok && g_file) {
            size_t got = g_i2s_mic.readBytes((char *)mic_buf, sizeof(mic_buf));
            size_t n_samples = got / 2;
            last_mic_mn = INT16_MAX;
            last_mic_mx = INT16_MIN;
            for (size_t i = 0; i < n_samples; i++) {
                int16_t s = mic_gain(mic_buf[i]);
                mic_buf[i] = s;
                if (s < last_mic_mn) last_mic_mn = s;
                if (s > last_mic_mx) last_mic_mx = s;
            }
            if (got > 0) {
                g_file.write((uint8_t *)mic_buf, got);
                g_mic_bytes_written += got;
                sample_count += n_samples;
            }
        } else if (ph.sensor != S_MIC) {
            delayMicroseconds(500);
        }

        // Heartbeat serial print once a second.
        if ((int32_t)(now - next_hb) >= 0) {
            switch (ph.sensor) {
            case S_LIS:
                Serial.printf("   [LIS ]  X=%+6d Y=%+6d Z=%+6d  (%+5.2f %+5.2f %+5.2f Gauss)\n",
                              last_x, last_y, last_z,
                              (double)(last_x / 6842.0f),
                              (double)(last_y / 6842.0f),
                              (double)(last_z / 6842.0f));
                break;
            case S_LSM:
                Serial.printf("   [LSM ]  T=%4.1fC  A=(%+5.2f,%+5.2f,%+5.2f)g  G=(%+6.1f,%+6.1f,%+6.1f)dps\n",
                              (double)(last_tc / 256.0f + 25.0f),
                              (double)(last_ax * (2.0f / 32768.0f)),
                              (double)(last_ay * (2.0f / 32768.0f)),
                              (double)(last_az * (2.0f / 32768.0f)),
                              (double)(last_gx * (250.0f / 32768.0f)),
                              (double)(last_gy * (250.0f / 32768.0f)),
                              (double)(last_gz * (250.0f / 32768.0f)));
                break;
            case S_BME:
                Serial.printf("   [BME ]  T=%5.2fC  P=%7.2f hPa  H=%5.2f %%RH\n",
                              (double)last_T, (double)last_P, (double)last_H);
                break;
            case S_VEML:
                Serial.printf("   [VEML]  ALS=%5u  WHITE=%5u  ~%.1f lux\n",
                              last_als, last_white,
                              (double)(last_als * VEML_LX_PER_CT));
                break;
            case S_MIC:
                Serial.printf("   [MIC ]  bytes=%lu  last-chunk min=%d max=%d spread=%d (gain x%d)\n",
                              (unsigned long)g_mic_bytes_written,
                              last_mic_mn, last_mic_mx,
                              (int)(last_mic_mx - last_mic_mn),
                              MIC_GAIN_MULT);
                break;
            default:
                Serial.printf("   [IDLE]  t=%lu ms\n", (unsigned long)now);
                break;
            }
            next_hb = now + 1000;
        }
    }

    // Close file + park sensor.
    if (g_file) {
        close_current_file();
        Serial.printf("   %s wrote %lu samples\n",
                      ph.label, (unsigned long)sample_count);
    }
    switch (ph.sensor) {
    case S_LIS:  if (g_lis_ok)  lis_park();  break;
    case S_LSM:  if (g_lsm_ok)  lsm_park();  break;
    case S_BME:  if (g_bme_ok)  bme_park();  break;
    case S_VEML: if (g_veml_ok) veml_park(); break;
    // MIC driver stays up; not reading == no additional cost beyond driver overhead.
    default: break;
    }
    Serial.printf("   %s DONE t=%lu ms\n", ph.label, (unsigned long)millis());
}

// ── Setup / loop ────────────────────────────────────────────────────────────
void setup(void) {
    Serial.begin(115200);
    delay(200);

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    Wire .begin(PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 100000);
    delay(50);

    Serial.println();
    Serial.println("=== 7_demo_mk1 -- Kompic Mk I (iv7.1) rolling sensor logger ===");
    Serial.println("(single-sensor phases, 10 s each; ammeter markers on stage boundaries)");

    g_lis_ok = lis_probe();
    Serial.printf("[LIS ] probe: %s\n", g_lis_ok ? "OK" : "FAIL");
    if (g_lis_ok) lis_park();

    g_lsm_ok = lsm_probe();
    Serial.printf("[LSM ] probe: %s\n", g_lsm_ok ? "OK" : "FAIL");
    if (g_lsm_ok) lsm_park();

    g_bme_ok = bme_probe();
    Serial.printf("[BME ] probe: %s\n", g_bme_ok ? "OK" : "FAIL");
    if (g_bme_ok) bme_park();

    g_veml_ok = veml_probe();
    Serial.printf("[VEML] probe: %s\n", g_veml_ok ? "OK" : "FAIL");
    if (g_veml_ok) veml_park();

    g_mic_ok = mic_probe();
    Serial.printf("[MIC ] probe: %s  (gain x%d = +24 dB)\n",
                  g_mic_ok ? "OK" : "FAIL", MIC_GAIN_MULT);

    Wire.beginTransmission(PCF85063A_ADDR);
    g_rtc_ok = (Wire.endTransmission() == 0);
    Serial.printf("[RTC ] probe: %s\n", g_rtc_ok ? "OK" : "FAIL");
    rtc_read_to_string(g_rtc_start, sizeof(g_rtc_start));
    Serial.printf("[RTC ] start-of-run: %s\n", g_rtc_start);

    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    g_sd_ok = SD_MMC.begin("/sdcard", true, false, 4000);
    Serial.printf("[SD  ] mount: %s\n", g_sd_ok ? "OK" : "FAIL");
    if (!g_sd_ok) { Serial.println("[SD  ] halting."); return; }
    sd_mkdirs();

    g_prefs.begin("demo7", false);
    g_boot_seq = g_prefs.getUInt("boot_seq", 0) + 1;
    g_prefs.putUInt("boot_seq", g_boot_seq);
    g_prefs.end();
    g_ms_boot_start = millis();
    Serial.printf("[LOG ] boot_seq=%lu   files under /data/{lis,lsm,bme,mic,veml}/\n",
                  (unsigned long)g_boot_seq);

    Serial.println();
    Serial.println("Double-click button = ship mode (files closed cleanly first).");
    Serial.println("Ammeter: watch USB current between stage markers below.");
    Serial.println();

    for (size_t i = 0; i < N_PHASES; i++) {
        run_phase(PHASES[i]);
    }

    Serial.println();
    Serial.printf("== ALL PHASES DONE ==  files: /data/*/s%04lu_*\n",
                  (unsigned long)g_boot_seq);
    Serial.println("Idling.  Double-click to ship-mode.");
}

void loop(void) {
    handle_button();
    delay(10);
}
