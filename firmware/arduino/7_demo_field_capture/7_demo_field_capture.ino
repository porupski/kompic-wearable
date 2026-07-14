/*
 * 7_demo_field_capture.ino  --  Kompic Mk I field-data-collection demo
 *
 * Encoder-driven mode selector, single-button trigger, RGB status indicator,
 * DRV haptic feedback, voice annotation, and SD-backed sensor logging.
 * Written 2026-07-05 for iv7.1 (post-Stage-5 all-systems-go). No screen.
 *
 * ── Modes (encoder cycles 1 detent = 1 step) ────────────────────────────────
 *   Red    MIC        mic-only 30 s capture (48 kHz mono WAV)
 *   Green  ENV        BME688 (T/P/H + gas) + VEML6030
 *   Yellow MOTION     LIS3MDLTR + LSM6DSV16X
 *   Pink   SKIN       MAX30101 PPG (Red + IR + Green) + TMP117 temp + live BPM
 *   White  FLASHLIGHT flashlight toggle; encoder = brightness while ON
 *   Purple ALARM      DRV wake-up alarm test (3 rotating patterns @ ~15 s each)
 *
 * ── UI (per mode) ───────────────────────────────────────────────────────────
 *   STANDBY       RGB slow-pulses mode colour (5 s peak-to-peak).
 *                   encoder rotate = step mode (+ DRV click per detent)
 *                   single-click   = enter action for current mode
 *                   double-click   = BQ ship mode (any state, any time)
 *
 *   VOICE ANNOT   (all sensor modes except MIC and FLASHLIGHT/ALARM)
 *                 Enters on single-click. RGB blinks red 3 times in sync with
 *                 3 DRV clicks (recording-imminent warning). Then 5 s of mic
 *                 capture, RGB blinking red at 1 Hz. A single DRV click at
 *                 the end signals hand-off to the sensor recording.
 *
 *   RECORDING    RGB pulses mode colour at 1 Hz for 30 s. Sensor writes to
 *                 SD. Long DRV fire at end. Encoder locked; single-click
 *                 during recording = end early + save what's captured.
 *
 *   FLASHLIGHT ON  Flashlight LED on at level; RGB solid white; encoder
 *                    steps brightness; single-click = toggle off.
 *
 *   ALARM FIRING   Purple LED solid; current pattern plays out; single-click
 *                    interrupts and returns to STANDBY. Next click advances
 *                    to the next pattern in the rotation.
 *
 * ── File layout on SD ───────────────────────────────────────────────────────
 *   /data/<mode>/s<boot>_r<seq>.csv     -- sensor CSV (mode ≠ mic)
 *   /data/mic/s<boot>_r<seq>_annot.wav  -- voice annotation (paired with r<seq>)
 *   /data/mic/s<boot>_r<seq>.wav        -- MIC-mode 30 s recording (no annot)
 *
 * ── NVS ─────────────────────────────────────────────────────────────────────
 *   Namespace "field", key "mode" = current_mode (last-used, persists across
 *   boot). Namespace "field", key "boot_seq" = monotonic boot counter for
 *   filename uniqueness.
 *
 * ── RTC ─────────────────────────────────────────────────────────────────────
 *   Serial commands (case-insensitive):
 *     SET_TIME 2026-07-05T14:30:00      -- set PCF85063A + clear OS bit
 *     GET_TIME                          -- read back
 *   Timestamp of each recording start is stored in the CSV header comment /
 *   WAV .txt sidecar as `rtc_start=<ISO>` when RTC is set, else `oscstop`.
 *
 * ── Ship mode ───────────────────────────────────────────────────────────────
 *   Double-click GPIO16 any time. 2 s red RGB countdown, then REG07 BATFET
 *   drop (with RST_WVBUS=1 for USB-attached path). If a recording is in
 *   progress at the moment ship mode fires, the file is flushed and closed
 *   (WAV header patched) before BATFET drops.
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <SD_MMC.h>
#include <FS.h>
#include <Preferences.h>
#include "ESP_I2S.h"
#include "driver/gpio.h"

// Note: USB Mass Storage was investigated (see auto-memory
// `feedback_tinyusb_guard.md` and Stage_5_Build_Report.md § 2.7). It requires
// switching the Arduino IDE USB Mode to "USB-OTG (TinyUSB)" at compile time,
// which disables the ESP32-S3's built-in USB-Serial-JTAG peripheral -- the
// same peripheral esptool relies on for reset-into-bootloader. Result: MSC
// works but every subsequent upload needs a manual timing-window hack. Not
// worth the operational cost for a field prototype, so USB-MSC is not built
// in. Data extraction from SD is via serial-monitor stream or physical card
// removal.

// ═════════════════════════════════════════════════════════════════════════════
// Pins
// ═════════════════════════════════════════════════════════════════════════════
#define PIN_FLASHLIGHT    41
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4
#define PIN_SCL_BUS2       5
#define PIN_WS2812_DIN    42
#define PIN_DRV_EN         0
#define PIN_ENC_A         21
#define PIN_ENC_B         43
#define PIN_MIC_CLK       47
#define PIN_MIC_DATA      48
#define PIN_SD_CLK        38
#define PIN_SD_CMD        39
#define PIN_SD_D0         40

// ═════════════════════════════════════════════════════════════════════════════
// I2C addresses
// ═════════════════════════════════════════════════════════════════════════════
#define VEML6030_ADDR     0x10
#define LIS3MDL_ADDR      0x1C
#define PCF85063A_ADDR    0x51
#define LSM6DSV_ADDR      0x6B
#define BME688_ADDR       0x76
#define BQ25619_ADDR      0x6A
#define DRV2605_ADDR      0x5A
#define MAX30101_ADDR     0x57
#define TMP117_ADDR_LO    0x48
#define TMP117_ADDR_HI    0x49

// ═════════════════════════════════════════════════════════════════════════════
// Registers (concise -- see legacy 3_smoke for full commentary)
// ═════════════════════════════════════════════════════════════════════════════
// BQ25619
#define BQ_REG_INPUT_SRC  0x00
#define BQ_REG_CTRL1      0x05
#define BQ_REG_MISC_OP    0x07
#define BQ_REG_STATUS     0x08
#define BQ_REG_PART       0x0A
#define BQ_TS_IGNORE_BIT      (1 << 6)
#define BQ_WD_MASK            (0x03 << 4)
#define BQ_STATUS_PG          (1 << 2)
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)

// DRV2605L
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
#define DRV_REG_CTRL1        0x1B
#define DRV_REG_CTRL4        0x1E
#define DRV_MODE_INTERNAL    0x00
#define DRV_MODE_AUTO_CAL    0x07
#define DRV_LIB_LRA          0x06
// ELV1411A profile
#define DRV_RATED_VOLTAGE    0x49
#define DRV_OD_CLAMP_REG     0x60
#define DRV_CTRL1_DRIVE_TIME 0x9C
#define DRV_FEEDBACK_LRA     0xB6
#define DRV_AUTO_CAL_TIME    0x30
// Effect IDs used
#define DRV_STRONG_CLICK       1
#define DRV_STRONG_CLICK_60    2
#define DRV_STRONG_CLICK_30    3
#define DRV_SHARP_CLICK        4
#define DRV_SHARP_CLICK_30     6
#define DRV_DOUBLE_CLICK      10
#define DRV_TRIPLE_CLICK      12
#define DRV_STRONG_BUZZ       14
#define DRV_ALERT_250         15
#define DRV_ALERT_750         16
#define DRV_MEDIUM_CLICK      20
#define DRV_LONG_BUZZ         47   // 5 s continuous LRA buzz

// PCF85063A RTC
#define RTC_REG_CTRL1        0x00
#define RTC_REG_SECONDS      0x04    // bit7 = OS
#define RTC_REG_MINUTES      0x05
#define RTC_REG_HOURS        0x06
#define RTC_REG_DAYS         0x07
#define RTC_REG_WEEKDAYS     0x08
#define RTC_REG_MONTHS       0x09
#define RTC_REG_YEARS        0x0A
#define RTC_OS_BIT           0x80

// VEML6030
#define VEML_REG_CONF        0x00
#define VEML_REG_ALS         0x04
#define VEML_REG_WHITE       0x05
#define VEML_CONF_RUN        ((uint16_t)((0x03 << 11) | (0x00 << 6)))    // gain 1/8, IT 100 ms
#define VEML_CONF_SHUTDOWN   ((uint16_t)0x0001)
#define VEML_LX_PER_CT       0.2304f

// LIS3MDLTR
#define LIS_REG_WHO          0x0F
#define LIS_WHO_VAL          0x3D
#define LIS_REG_CTRL1        0x20
#define LIS_REG_CTRL2        0x21
#define LIS_REG_CTRL3        0x22
#define LIS_REG_CTRL4        0x23
#define LIS_REG_CTRL5        0x24
#define LIS_REG_OUT_X_L      0x28
#define LIS_AUTO_INC         0x80

// LSM6DSV16X
#define LSM_REG_WHO          0x0F
#define LSM_WHO_VAL          0x70
#define LSM_REG_CTRL1        0x10
#define LSM_REG_CTRL2        0x11
#define LSM_REG_CTRL3        0x12
#define LSM_REG_OUT_TEMP     0x20

// BME688
#define BME_REG_CHIP_ID      0xD0
#define BME_CHIP_ID_VAL      0x61
#define BME_REG_CTRL_HUM     0x72
#define BME_REG_CTRL_GAS1    0x71
#define BME_REG_CTRL_GAS0    0x70
#define BME_REG_CTRL_MEAS    0x74
#define BME_REG_CAL_BLK1     0x8A
#define BME_REG_CAL_BLK2     0xE1
#define BME_REG_PRESS_ADC    0x1F
#define BME_REG_RES_HEAT_0   0x5A
#define BME_REG_IDAC_HEAT_0  0x50
#define BME_REG_GAS_WAIT_0   0x64
// BME688 Field-0 gas ADC is at 0x2C/0x2D (BME680 used 0x2A/0x2B -- verified
// against doc 20.12 § 132/144). My original 0x2A/0x2B code was reading garbage
// which is why gas_r stayed 0 despite v=1 stab=1.
#define BME_REG_GAS_R_MSB    0x2C
#define BME_REG_GAS_R_LSB    0x2D
#define BME_REG_RES_HEAT_VAL 0x00
#define BME_REG_RES_HEAT_RNG 0x02

// MAX30101
#define MAX_REG_INT_STATUS1  0x00
#define MAX_REG_FIFO_WR_PTR  0x04
#define MAX_REG_OVF_CTR      0x05
#define MAX_REG_FIFO_RD_PTR  0x06
#define MAX_REG_FIFO_DATA    0x07
#define MAX_REG_FIFO_CFG     0x08
#define MAX_REG_MODE_CFG     0x09
#define MAX_REG_SPO2_CFG     0x0A
#define MAX_REG_LED1_PA      0x0C
#define MAX_REG_LED2_PA      0x0D
#define MAX_REG_LED3_PA      0x0E
#define MAX_REG_PART_ID      0xFF
#define MAX_MODE_RESET       0x40
#define MAX_MODE_HR          0x02
#define MAX_MODE_SPO2        0x03
#define MAX_MODE_MULTI       0x07
#define MAX_LED_PA_FIELD     0x24    // ~7.2 mA per LED (finger-on-sensor sweet spot)

// TMP117
#define TMP_REG_TEMP         0x00
#define TMP_REG_CONFIG       0x01
#define TMP_REG_DEVICE_ID    0x0F

// ═════════════════════════════════════════════════════════════════════════════
// Button + WS2812 timing
// ═════════════════════════════════════════════════════════════════════════════
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS    350
#define WS_MAX_LEVEL         26    // per-channel cap
#define WS_BLIP_LEVEL        26

// ═════════════════════════════════════════════════════════════════════════════
// Recording session tuning
// ═════════════════════════════════════════════════════════════════════════════
#define RECORDING_MS         30000    // 30 s per session
#define VOICE_ANNOT_MS       5000     // 5 s pre-record voice tag
#define BEEP_COUNT           3        // number of red flashes + DRV clicks before annot
#define BEEP_ON_MS           140
#define BEEP_OFF_MS          260

// Flashlight
#define FL_LEVELS            24
#define FL_MAX_DUTY          80
#define FL_INIT_LEVEL        12

// Encoder -- polled detent-rest state machine (from legacy 5_smoke_test_mk1).
// The ISR-on-A-rising approach with a debounce window did NOT solve the
// double-detent problem on this board: mechanical settle bounces past the
// debounce with B in the opposite state, giving +1 immediately followed by
// -1. Instead we sample A + B on every loop pass and only emit ONE click
// event per detent-rest → detent-rest cycle. Direction is latched from the
// FIRST line that goes LOW out of rest. See doc 20.16 C-08.
#define ENC_DETENT_REST_MS   10    // both A + B HIGH continuously to confirm rest
#define ENC_INVERT           0

// Standby vs recording pulse periods
#define PULSE_STANDBY_MS     5000
#define PULSE_RECORD_MS      1000
// Solid RGB (no pulse dimming) for this long after a rotation or entering
// standby, so the current mode's colour is unambiguous. After this window
// the standby sine breathe kicks in.
#define SOLID_ON_ACTIVITY_MS 2000

// ═════════════════════════════════════════════════════════════════════════════
// Modes
// ═════════════════════════════════════════════════════════════════════════════
typedef enum {
    MODE_MIC = 0,
    MODE_ENV,
    MODE_MOTION,
    MODE_SKIN,
    MODE_FLASHLIGHT,
    MODE_ALARM,
    MODE_COUNT
} field_mode_t;

typedef struct {
    const char *name;
    uint8_t r, g, b;      // max colour (scales down with pulse)
} mode_info_t;

// Perceptual palette. WS2812 red is much brighter than green at the same PWM
// value so any raw R+G mixture reads warmer than intended. Yellow gets a
// green-dominant mix so it doesn't look orange; orange gets a red-heavy mix
// with just a hint of green so it doesn't look pure red; purple leans blue
// for the same reason (raw R+B reads magenta).
static const mode_info_t MODE_INFO[MODE_COUNT] = {
    { "mic",    26,  0,  0 },   // Red
    { "env",     0, 26,  0 },   // Green
    { "mot",    14, 26,  0 },   // Yellow -- green-dominant
    { "skin",   26,  0, 12 },   // Pink -- red-blue mix (was orange -- looked too red)
    { "fl",     18, 18, 18 },   // White -- slightly dimmed
    { "alarm",   8,  0, 26 },   // Purple -- blue-dominant
};

// ═════════════════════════════════════════════════════════════════════════════
// App states
// ═════════════════════════════════════════════════════════════════════════════
typedef enum {
    ST_STANDBY = 0,
    ST_FL_ON,
    ST_VOICE_ANNOT,
    ST_RECORDING,
    ST_ALARM_FIRING,
} app_state_t;

// ═════════════════════════════════════════════════════════════════════════════
// Alarm patterns (Purple mode) -- (offset_ms, DRV effect_id)
// ═════════════════════════════════════════════════════════════════════════════
typedef struct { uint32_t ms; uint8_t effect; } alarm_step_t;

// A: ramp up -- rapid soft-clicks tightening into a sustained hard buzz
// filling the second half of the window. High duty throughout.
static const alarm_step_t ALARM_A[] = {
    {     0, DRV_STRONG_CLICK_30 },
    {   200, DRV_STRONG_CLICK_30 },
    {   400, DRV_STRONG_CLICK_30 },
    {   700, DRV_SHARP_CLICK     },
    {   950, DRV_SHARP_CLICK     },
    {  1200, DRV_SHARP_CLICK     },
    {  1450, DRV_MEDIUM_CLICK    },
    {  1650, DRV_MEDIUM_CLICK    },
    {  1850, DRV_STRONG_CLICK    },
    {  2050, DRV_STRONG_CLICK    },
    {  2250, DRV_DOUBLE_CLICK    },
    {  2450, DRV_TRIPLE_CLICK    },
    {  2700, DRV_STRONG_BUZZ     },   // 1 s
    {  3800, DRV_STRONG_BUZZ     },   // 1 s
    {  4900, DRV_LONG_BUZZ       },   // 5 s -- covers 4.9 to 9.9
    {  9900, DRV_LONG_BUZZ       },   // 5 s -- covers 9.9 to 14.9
    { 14900, DRV_ALERT_750       },
};

// B: strong open, trough mid-run, crescendo back to sustained buzz.
static const alarm_step_t ALARM_B[] = {
    {     0, DRV_LONG_BUZZ       },   // 5 s hard open (0..5)
    {  5000, DRV_STRONG_CLICK    },
    {  5250, DRV_MEDIUM_CLICK    },
    {  5500, DRV_MEDIUM_CLICK    },
    {  5800, DRV_SHARP_CLICK_30  },
    {  6100, DRV_SHARP_CLICK_30  },
    {  6400, DRV_STRONG_CLICK_30 },   // trough
    {  6700, DRV_STRONG_CLICK_30 },
    {  7000, DRV_SHARP_CLICK_30  },
    {  7300, DRV_MEDIUM_CLICK    },
    {  7600, DRV_MEDIUM_CLICK    },
    {  7900, DRV_STRONG_CLICK    },
    {  8200, DRV_STRONG_CLICK    },
    {  8500, DRV_DOUBLE_CLICK    },
    {  8800, DRV_TRIPLE_CLICK    },
    {  9200, DRV_STRONG_BUZZ     },
    { 10300, DRV_LONG_BUZZ       },   // 5 s final buzz (10.3..15.3)
};

// C: surprise -- brief silence, then a sudden long buzz hit, brief silence,
// then rapid crescendo burst all the way to end.
static const alarm_step_t ALARM_C[] = {
    {  2500, DRV_SHARP_CLICK     },   // small tell that something is coming
    {  3000, DRV_LONG_BUZZ       },   // 5 s SUDDEN hit (3..8)
    {  8500, DRV_TRIPLE_CLICK    },
    {  8700, DRV_TRIPLE_CLICK    },
    {  8900, DRV_TRIPLE_CLICK    },
    {  9200, DRV_STRONG_CLICK    },
    {  9400, DRV_STRONG_CLICK    },
    {  9600, DRV_DOUBLE_CLICK    },
    {  9900, DRV_STRONG_BUZZ     },
    { 11000, DRV_STRONG_BUZZ     },
    { 12100, DRV_ALERT_750       },
    { 13000, DRV_LONG_BUZZ       },   // fill to end
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
static const alarm_step_t *const ALARM_TABLES[3] = { ALARM_A, ALARM_B, ALARM_C };
static const size_t             ALARM_LENS  [3] = { ARRAY_LEN(ALARM_A), ARRAY_LEN(ALARM_B), ARRAY_LEN(ALARM_C) };
#define ALARM_DURATION_MS  15000

// ═════════════════════════════════════════════════════════════════════════════
// Globals
// ═════════════════════════════════════════════════════════════════════════════
static field_mode_t current_mode      = MODE_ENV;
static app_state_t  app_state         = ST_STANDBY;
static uint32_t     state_enter_ms    = 0;
static uint32_t     last_activity_ms  = 0;   // last encoder rotate / mode-change / standby entry
static uint32_t     boot_seq          = 0;
static uint32_t     rec_seq           = 0;
static uint8_t      alarm_idx         = 0;   // 0..2 rotating pattern picker

static Preferences  nvs;

typedef struct {
    bool     in_motion;        // set true the moment we leave both-HIGH rest
    int8_t   latched_dir;      // 0, +1 (CW), or -1 (CCW); assigned at leave-rest
    uint32_t rest_since_ms;    // when both lines last became HIGH (0 = not yet)
} enc_state_t;
static enc_state_t enc = { false, 0, 0 };

static bool bq_ok = false, drv_ok = false, drv_cal_ok = false;
static bool rtc_running = false;     // true once OS bit cleared

// Flashlight
static uint8_t fl_level   = FL_INIT_LEVEL;

// I2S mic (single instance for both annot and MIC-mode capture)
static I2SClass I2S;
static bool     i2s_up = false;
#define MIC_GAIN_MULT   8    // Ivan's sweet spot from Stage 4 § 7.2

// TMP117 detected address (0x48 or 0x49)
static uint8_t tmp_addr = 0;

// Currently-open recording file handles
static File     f_csv;
static File     f_wav;
static uint32_t wav_bytes_written = 0;
static bool     recording_early_end = false;


// ═════════════════════════════════════════════════════════════════════════════
// I2C helpers
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
static bool veml_write_word(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(VEML6030_ADDR);
    Wire.write(reg);
    Wire.write(val & 0xFF);
    Wire.write((val >> 8) & 0xFF);
    return Wire.endTransmission() == 0;
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

// ═════════════════════════════════════════════════════════════════════════════
// BCD helpers (RTC)
// ═════════════════════════════════════════════════════════════════════════════
static uint8_t dec_to_bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }
static uint8_t bcd_to_dec(uint8_t b) { return ((b >> 4) * 10) + (b & 0x0F); }

// ═════════════════════════════════════════════════════════════════════════════
// DRV trigger (fire-and-forget)
// ═════════════════════════════════════════════════════════════════════════════
static bool drv_trigger(uint8_t effect_id) {
    if (!drv_ok) return false;
    if (!i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_WAVSEQ1, effect_id)) return false;
    if (!i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_WAVSEQ2, 0x00))       return false;
    return i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_GO, 0x01);
}

// ═════════════════════════════════════════════════════════════════════════════
// Encoder (polled detent-rest state machine)
// ═════════════════════════════════════════════════════════════════════════════
// Called once per detent-rest → detent-rest cycle with the resolved direction.
// Set to a forward declaration; the actual body lives further down alongside
// the mode-selection / brightness-adjust logic.
static void on_encoder_click(int8_t dir);

// Run on every loop() pass. Detects one click per full detent by tracking:
//   * "at rest"     : both A and B HIGH continuously for ENC_DETENT_REST_MS.
//   * "in motion"   : at least one line LOW.
// At the leave-rest transition we latch direction from whichever line went
// LOW first. Bounces during the click cycle don't affect the latched dir.
// At the confirmed return-to-rest we emit exactly one on_encoder_click().
static void handle_encoder_poll(void) {
    uint8_t a = digitalRead(PIN_ENC_A);
    uint8_t b = digitalRead(PIN_ENC_B);
    bool at_rest = (a == HIGH) && (b == HIGH);
    uint32_t now = millis();

    if (at_rest) {
        if (enc.in_motion) {
            if (enc.rest_since_ms == 0) {
                enc.rest_since_ms = now;
            } else if ((now - enc.rest_since_ms) >= ENC_DETENT_REST_MS) {
                if (enc.latched_dir != 0) {
                    int8_t d = enc.latched_dir;
                    if (ENC_INVERT) d = -d;
                    on_encoder_click(d);
                }
                enc.latched_dir = 0;
                enc.in_motion   = false;
            }
        }
    } else {
        enc.rest_since_ms = 0;
        if (!enc.in_motion) {
            // Just left rest -- latch direction from which line went LOW first.
            if (a == LOW && b == HIGH)      enc.latched_dir = +1;
            else if (a == HIGH && b == LOW) enc.latched_dir = -1;
            // (both LOW: don't latch; wait for next click cycle)
            enc.in_motion = true;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Ship-mode + button
// ═════════════════════════════════════════════════════════════════════════════
static void abort_recording_and_flush(void);   // forward decl

static void ship_mode_countdown(void) {
    Serial.println("       2 s red countdown -- hands off the button.");
    const uint32_t step_ms = 40;
    const uint32_t steps   = 2000 / step_ms;
    for (uint32_t i = 0; i < steps; i++) {
        float phase = (float)i / 12.5f;
        float s = 0.5f * (1.0f - cosf(phase * (float)M_PI));
        uint8_t r = (uint8_t)(s * (float)WS_BLIP_LEVEL);
        neopixelWrite(PIN_WS2812_DIN, r, 0, 0);
        delay(step_ms);
    }
    neopixelWrite(PIN_WS2812_DIN, WS_BLIP_LEVEL, 0, 0);
}

static void enter_ship_mode(void) {
    Serial.println("[BTN ] DOUBLE -> ship mode requested");
    // Flush + close any open files before the BATFET write. Ship mode from
    // mid-recording is legal; we just don't want a truncated WAV or CSV.
    abort_recording_and_flush();
    if (!bq_ok) {
        Serial.println("       BQ not ok -- aborting (no ship mode)");
        return;
    }
    ship_mode_countdown();
    if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("       button LOW at end of countdown -- aborting");
        neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
        return;
    }
    uint8_t r07 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP);
    uint8_t r07_new = r07;
    r07_new |=  BQ_BATFET_DIS;
    r07_new |=  BQ_BATFET_RST_WVBUS;
    r07_new &= ~BQ_BATFET_DLY;
    r07_new &= ~BQ_BATFET_RST_EN;
    Serial.printf("       writing REG07 0x%02X -> 0x%02X\n", r07, r07_new);
    Serial.flush();
    i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP, r07_new);
    delay(50);
    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    if (st & BQ_STATUS_PG) {
        Serial.println("       USB still present -- BATFET disabled; ship mode on USB unplug.");
        neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
    } else {
        Serial.println("       BATFET off -- expecting power loss now.");
        Serial.flush();
        while (1) { delay(100); }
    }
}

static void on_single_click(void);   // forward decl
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
// RGB pulse animator
// ═════════════════════════════════════════════════════════════════════════════
static void rgb_off(void) { neopixelWrite(PIN_WS2812_DIN, 0, 0, 0); }
static void rgb_set_max(field_mode_t m) {
    const mode_info_t &mi = MODE_INFO[m];
    neopixelWrite(PIN_WS2812_DIN, mi.r, mi.g, mi.b);
}
static void rgb_pulse(field_mode_t m, uint32_t period_ms, uint32_t now_ms) {
    // Sine breathe: 0..1..0 across period_ms.
    float phase = (float)(now_ms % period_ms) / (float)period_ms;
    float s = 0.5f * (1.0f - cosf(phase * 2.0f * (float)M_PI));
    const mode_info_t &mi = MODE_INFO[m];
    uint8_t r = (uint8_t)(s * (float)mi.r);
    uint8_t g = (uint8_t)(s * (float)mi.g);
    uint8_t b = (uint8_t)(s * (float)mi.b);
    neopixelWrite(PIN_WS2812_DIN, r, g, b);
}
static void rgb_pulse_red(uint32_t period_ms, uint32_t now_ms) {
    float phase = (float)(now_ms % period_ms) / (float)period_ms;
    float s = 0.5f * (1.0f - cosf(phase * 2.0f * (float)M_PI));
    uint8_t r = (uint8_t)(s * (float)WS_MAX_LEVEL);
    neopixelWrite(PIN_WS2812_DIN, r, 0, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// NVS
// ═════════════════════════════════════════════════════════════════════════════
static void nvs_load(void) {
    nvs.begin("field", true);
    current_mode = (field_mode_t)nvs.getUChar("mode", (uint8_t)MODE_ENV);
    if (current_mode >= MODE_COUNT) current_mode = MODE_ENV;
    boot_seq = nvs.getUInt("boot_seq", 0);
    nvs.end();
    nvs.begin("field", false);
    nvs.putUInt("boot_seq", boot_seq + 1);
    nvs.end();
    boot_seq++;
    Serial.printf("[NVS ] boot_seq=%u  restored mode=%s\n",
                  (unsigned)boot_seq, MODE_INFO[current_mode].name);
}
static void nvs_save_mode(void) {
    nvs.begin("field", false);
    nvs.putUChar("mode", (uint8_t)current_mode);
    nvs.end();
}

// ═════════════════════════════════════════════════════════════════════════════
// RTC read + set (with serial parsing)
// ═════════════════════════════════════════════════════════════════════════════
static void rtc_read_iso(char *out, size_t n) {
    uint8_t r[7] = {0};
    if (!i2c_read_buf(Wire, PCF85063A_ADDR, RTC_REG_SECONDS, r, 7)) {
        snprintf(out, n, "rtc_read_fail");
        return;
    }
    if (r[0] & RTC_OS_BIT) { snprintf(out, n, "oscstop"); return; }
    uint8_t sec = bcd_to_dec(r[0] & 0x7F);
    uint8_t min = bcd_to_dec(r[1] & 0x7F);
    uint8_t hr  = bcd_to_dec(r[2] & 0x3F);
    uint8_t day = bcd_to_dec(r[3] & 0x3F);
    uint8_t mon = bcd_to_dec(r[5] & 0x1F);
    uint16_t yr = 2000 + bcd_to_dec(r[6]);
    snprintf(out, n, "%04u-%02u-%02uT%02u:%02u:%02u",
             yr, mon, day, hr, min, sec);
}

// Parses "YYYY-MM-DDTHH:MM:SS" and writes RTC. Returns true on success.
static bool rtc_set_from_iso(const char *iso) {
    int yr, mon, day, hr, min, sec;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &yr, &mon, &day, &hr, &min, &sec) != 6) return false;
    if (yr < 2000 || yr > 2099) return false;
    if (mon < 1 || mon > 12 || day < 1 || day > 31) return false;
    if (hr > 23 || min > 59 || sec > 59) return false;
    uint8_t v[7];
    v[0] = dec_to_bcd(sec) & 0x7F;       // bit 7 = OS clear
    v[1] = dec_to_bcd(min) & 0x7F;
    v[2] = dec_to_bcd(hr)  & 0x3F;       // 24h mode
    v[3] = dec_to_bcd(day) & 0x3F;
    v[4] = 0;                            // weekday (unused)
    v[5] = dec_to_bcd(mon) & 0x1F;
    v[6] = dec_to_bcd(yr - 2000);
    Wire.beginTransmission(PCF85063A_ADDR);
    Wire.write(RTC_REG_SECONDS);
    for (int i = 0; i < 7; i++) Wire.write(v[i]);
    if (Wire.endTransmission() != 0) return false;
    rtc_running = true;
    return true;
}

static void handle_serial_command(void) {
    static char buf[80];
    static size_t len = 0;
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n' || len >= sizeof(buf) - 1) {
            buf[len] = 0;
            if (strncasecmp(buf, "SET_TIME ", 9) == 0) {
                if (rtc_set_from_iso(buf + 9)) {
                    char now[32]; rtc_read_iso(now, sizeof(now));
                    Serial.printf("[RTC ] set OK -> %s\n", now);
                } else {
                    Serial.println("[RTC ] SET_TIME parse failed. Expected: SET_TIME YYYY-MM-DDTHH:MM:SS");
                }
            } else if (strncasecmp(buf, "GET_TIME", 8) == 0) {
                char now[32]; rtc_read_iso(now, sizeof(now));
                Serial.printf("[RTC ] %s\n", now);
            } else if (len > 0) {
                Serial.printf("[CMD ] unknown: %s\n", buf);
                Serial.println("       commands: SET_TIME YYYY-MM-DDTHH:MM:SS  |  GET_TIME");
            }
            len = 0;
            continue;
        }
        buf[len++] = (char)c;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// BME688 calibration + compensation (ported from 3_smoke_stage3_full)
// ═════════════════════════════════════════════════════════════════════════════
typedef struct {
    uint16_t par_t1;  int16_t  par_t2;  int8_t  par_t3;
    uint16_t par_p1;  int16_t  par_p2;  int8_t  par_p3;
    int16_t  par_p4;  int16_t  par_p5;  int8_t  par_p6;
    int8_t   par_p7;  int16_t  par_p8;  int16_t par_p9;
    uint8_t  par_p10;
    uint16_t par_h1;  uint16_t par_h2;  int8_t  par_h3;
    int8_t   par_h4;  int8_t   par_h5;  uint8_t par_h6; int8_t par_h7;
    // Gas heater cal (needed to compute res_heat_x for a target temp).
    int8_t   par_g1;  int16_t  par_g2;  int8_t  par_g3;
    uint8_t  res_heat_range;
    int8_t   res_heat_val;
} bme_cal_t;
static bme_cal_t bme_cal = {0};
static bool      bme_cal_loaded = false;

static bool bme_read_calibration(void) {
    uint8_t b1[23] = {0};
    uint8_t b2[10] = {0};
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_CAL_BLK1, b1, sizeof(b1))) return false;
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_CAL_BLK2, b2, sizeof(b2))) return false;
    bme_cal.par_t2  = (int16_t)((uint16_t)b1[1] << 8 | b1[0]);
    bme_cal.par_t3  = (int8_t)b1[2];
    bme_cal.par_p1  = (uint16_t)((uint16_t)b1[5] << 8 | b1[4]);
    bme_cal.par_p2  = (int16_t)((uint16_t)b1[7] << 8 | b1[6]);
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
    // Gas heater cal -- separate single-byte reads (par_g2 spans 0xEB/0xEC,
    // par_g1 at 0xED, par_g3 at 0xEE, res_heat_val at 0x00, res_heat_range
    // at 0x02[5:4]. Verified against doc 20.12 § 167-171).
    uint8_t gh_lo = i2c_read_reg(Wire, BME688_ADDR, 0xEB);
    uint8_t gh_hi = i2c_read_reg(Wire, BME688_ADDR, 0xEC);
    bme_cal.par_g2         = (int16_t)(((uint16_t)gh_hi << 8) | gh_lo);
    bme_cal.par_g1         = (int8_t)i2c_read_reg(Wire, BME688_ADDR, 0xED);
    bme_cal.par_g3         = (int8_t)i2c_read_reg(Wire, BME688_ADDR, 0xEE);
    bme_cal.res_heat_val   = (int8_t)i2c_read_reg(Wire, BME688_ADDR, BME_REG_RES_HEAT_VAL);
    bme_cal.res_heat_range = (i2c_read_reg(Wire, BME688_ADDR, BME_REG_RES_HEAT_RNG) >> 4) & 0x03;
    bme_cal_loaded = true;
    return true;
}

// Compute the res_heat_x code that programs a target heater temp given an
// ambient. Formula from BME688 datasheet p. 27 (floating-point form). Doc
// 20.12 doesn't spell it out verbatim; this is Bosch's canonical impl.
static uint8_t bme_res_heat_code(float target_temp_c, float amb_temp_c) {
    if (!bme_cal_loaded) return 0x64;   // safe-ish fallback if cal not loaded
    float var1 = ((float)bme_cal.par_g1 / 16.0f) + 49.0f;
    float var2 = (((float)bme_cal.par_g2 / 32768.0f) * 0.0005f) + 0.00235f;
    float var3 = (float)bme_cal.par_g3 / 1024.0f;
    float var4 = var1 * (1.0f + (var2 * target_temp_c));
    float var5 = var4 + (var3 * amb_temp_c);
    float rh   = 3.4f *
                 ((var5 * (4.0f / (4.0f + (float)bme_cal.res_heat_range))
                        * (1.0f / (1.0f + ((float)bme_cal.res_heat_val * 0.002f)))) - 25.0f);
    if (rh < 0.0f)   rh = 0.0f;
    if (rh > 255.0f) rh = 255.0f;
    return (uint8_t)rh;
}

// Return true if compensation succeeded; writes T (°C), P (Pa), H (%RH).
static bool bme_compensate(uint32_t t_raw, uint32_t p_raw, uint16_t h_raw,
                           float *T_out, float *P_out, float *H_out) {
    if (!bme_cal_loaded) return false;
    float v1, v2, t_fine, T;
    v1 = ((float)t_raw / 16384.0f - (float)bme_cal.par_t1 / 1024.0f)
         * (float)bme_cal.par_t2;
    v2 = (((float)t_raw / 131072.0f - (float)bme_cal.par_t1 / 8192.0f)
        * ((float)t_raw / 131072.0f - (float)bme_cal.par_t1 / 8192.0f))
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
    float P = 1048576.0f - (float)p_raw;
    if (v1 != 0.0f) {
        P = ((P - (v2 / 4096.0f)) * 6250.0f) / v1;
        v1 = ((float)bme_cal.par_p9 * P * P) / 2147483648.0f;
        v2 = P * ((float)bme_cal.par_p8 / 32768.0f);
        float v3 = (P / 256.0f) * (P / 256.0f) * (P / 256.0f)
                 * ((float)bme_cal.par_p10 / 131072.0f);
        P = P + (v1 + v2 + v3 + ((float)bme_cal.par_p7 * 128.0f)) / 16.0f;
    }
    v1 = (float)h_raw - (((float)bme_cal.par_h1 * 16.0f)
                       + (((float)bme_cal.par_h3 / 2.0f) * T));
    v2 = v1 * (((float)bme_cal.par_h2 / 262144.0f)
       * (1.0f + (((float)bme_cal.par_h4 / 16384.0f) * T)
       + (((float)bme_cal.par_h5 / 1048576.0f) * T * T)));
    float h3f = (float)bme_cal.par_h6 / 16384.0f;
    float h4f = (float)bme_cal.par_h7 / 2097152.0f;
    float H = v2 + ((h3f + (h4f * T)) * v2 * v2);
    if (H > 100.0f) H = 100.0f;
    if (H < 0.0f)   H = 0.0f;
    *T_out = T;
    *P_out = P;
    *H_out = H;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Sensor init
// ═════════════════════════════════════════════════════════════════════════════
static bool veml_init(void) {
    return veml_write_word(VEML_REG_CONF, VEML_CONF_RUN);
}
static void veml_park(void) {
    veml_write_word(VEML_REG_CONF, VEML_CONF_SHUTDOWN);
}
static bool lis_init(void) {
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL2, 0x04);   // SOFT_RST
    delay(5);
    // 40 Hz, ±16 G, continuous. ±16 G is coarse but survives the LRA magnet
    // sitting a few mm away in the case -- ±4 G was pinning Y at -32768 the
    // whole time. Sensitivity is 1711 LSB/G at this range.
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL1, 0x50);   // TEMP_EN=0, OM=high perf, DO=40 Hz
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL2, 0x60);   // FS=±16 G (bits 6:5 = 11)
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL3, 0x00);   // continuous
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL4, 0x00);
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL5, 0x40);   // BDU=1
    return true;
}
static void lis_park(void) {
    i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL3, 0x03);   // power-down
}
static bool lsm_init(void) {
    // LSM6DSV16X CTRL1 layout: bits 6:4 = OP_MODE_XL, bits 3:0 = ODR_XL.
    // My earlier 0x70 = OP_MODE=111 (normal) + ODR=0000 (OFF) -- silent!
    // 0x07 = OP_MODE=000 (high-perf) + ODR=0111 (240 Hz). Same for CTRL2 (gyro).
    // Soft-reset + BDU + IF_INC + then enable both channels.
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x01);   // SW_RESET
    delay(10);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x44);   // BDU + IF_INC
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, 0x07);   // XL high-perf @ 240 Hz
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, 0x07);   // G  high-perf @ 240 Hz
    return true;
}
static void lsm_park(void) {
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, 0x00);   // ODR=0 both
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, 0x00);
}

// BME688 with gas heater enabled at profile 0.
// res_heat_0 / gas_wait_0 use "reasonable defaults" -- proper Bosch calibration
// requires the calib coefficient blocks; here we log raw gas_r + status bits.
static bool bme_init_with_gas(void) {
    // Read the per-chip calibration coefficients once so env_read_and_write
    // can output real T/P/H, and so the heater target is achievable.
    if (!bme_cal_loaded) bme_read_calibration();
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_HUM,  0x01);              // hum ×1
    // Compute res_heat_0 for target 250 °C at 25 °C ambient. Lower target
    // (was 320 °C) is easier to reach from cold in the forced-mode hold
    // window; 250 °C is still well inside the MOX reaction range for VOC
    // detection. Bench observed v=0/stab=0 at 320 °C with 100 ms hold ->
    // heater never plateaued. Doc 20.12 § 296-303.
    uint8_t rh0 = bme_res_heat_code(250.0f, 25.0f);
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_RES_HEAT_0, rh0);
    // Seed the heater current DAC (idac_heat_0) to shorten heater plateau
    // time. The chip's control loop overrides this once conversion starts.
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_IDAC_HEAT_0, 0x80);
    // gas_wait_0 = 4× factor (bits 7:6 = 01), value 50 -> 200 ms heater hold.
    // Was 100 ms (0x59); doubled to give more margin above the 20-30 ms
    // settle time per doc line 201, so heat_stab has a chance to latch.
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_GAS_WAIT_0, 0x72);
    // CTRL_GAS_1: run_gas=1 (bit 4), nb_conv=0 (bits 3:0)
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_GAS1, 0x10);
    // CTRL_GAS_0: heat_off=0 (bit 3 clear = heater on)
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_GAS0, 0x00);
    // CTRL_MEAS: T=×2, P=×16, forced mode
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS, (0x02 << 5) | (0x05 << 2) | 0x01);
    Serial.printf("[BME ] heater cal: par_g1=%d par_g2=%d par_g3=%d range=%u val=%d  res_heat_0=0x%02X for target 250C  gas_wait_0=200ms\n",
                  bme_cal.par_g1, bme_cal.par_g2, bme_cal.par_g3,
                  bme_cal.res_heat_range, bme_cal.res_heat_val, rh0);
    return true;
}
static void bme_trigger_forced(void) {
    // Re-issue mode=forced to trigger a fresh conversion.
    uint8_t cm = i2c_read_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS);
    cm = (cm & ~0x03) | 0x01;
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS, cm);
}
static void bme_park(void) {
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS, 0x00);
    i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_GAS1, 0x00);
}

static bool max_init_all_leds(void) {
    // Multi-LED mode: 3 slots, one per LED (Red / IR / Green).
    //   MODE_CFG = 0x07 (multi-LED)
    //   Slot layout registers (0x11, 0x12) select which LED fires in each slot:
    //     0x11 bits 2:0 = slot1 (001 = Red LED1)
    //     0x11 bits 6:4 = slot2 (010 = IR   LED2)
    //     0x12 bits 2:0 = slot3 (011 = Green LED3)
    //     0x12 bits 6:4 = slot4 (000 = disabled)
    //   Sample size in FIFO = 3 slots × 3 bytes = 9 bytes per sample.
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_MODE_CFG, MAX_MODE_RESET);
    uint32_t t0 = millis();
    while ((i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_MODE_CFG) & MAX_MODE_RESET)
           && millis() - t0 < 200) delay(2);
    // FIFO_CFG: sample avg=4 (bits 7:5=010), FIFO rollover enable=1, almost-full=0
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_FIFO_CFG, 0x50);
    // SPO2_CFG: ADC_RGE=full (11), SR=100 Hz (001), LED_PW=411us/18bit (11).
    // With 3 slots × 411us ≈ 1.24 ms per sample cycle -- 100 Hz is comfortable.
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_SPO2_CFG, (0x03 << 5) | (0x01 << 2) | 0x03);
    // Multi-LED slot config: slot1=Red, slot2=IR, slot3=Green, slot4=off.
    i2c_write_reg(Wire, MAX30101_ADDR, 0x11, (0x02 << 4) | 0x01);
    i2c_write_reg(Wire, MAX30101_ADDR, 0x12, (0x00 << 4) | 0x03);
    // LED currents -- comfortable PPG level for all three.
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_LED1_PA, MAX_LED_PA_FIELD);   // Red
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_LED2_PA, MAX_LED_PA_FIELD);   // IR
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_LED3_PA, MAX_LED_PA_FIELD);   // Green
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_MODE_CFG, MAX_MODE_MULTI);
    // Clear FIFO pointers
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_FIFO_WR_PTR, 0);
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_OVF_CTR,     0);
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_FIFO_RD_PTR, 0);
    return true;
}
static void max_park(void) {
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_MODE_CFG, MAX_MODE_RESET);
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_LED1_PA,  0);
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_LED2_PA,  0);
    i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_LED3_PA,  0);
}

// ─── PPG-based BPM detector (Green channel, local-max peak detection) ─────────
// Green LED (~525 nm) is strongly absorbed by oxyhaemoglobin -- much better
// SNR through skin than Red or IR. Consumer wearables (Apple/Fitbit/etc.) use
// green for HR for this reason; Red/IR is more for SpO2.
//
// Baseline is a slow-tracking EMA of the Green sample. AC = Green - baseline.
// A "beat" is a **local maximum** of the AC signal above an adaptive
// threshold with a 300 ms refractory period (200 BPM upper cap). This is
// more robust than a zero-crossing detector when the finger is planted
// firmly and the slow-tracking baseline can't chase fast enough for AC to
// swing negative between systoles -- exactly what happened in the earlier
// build where BPM stayed at "--" despite Green swinging cleanly.
//
// First ~5 s is a warm-up window where the baseline tracks fast; no BPM is
// emitted until the window closes.
//
// Staleness: if no peak in the last 2 s, BPM output is cleared -- avoids
// the "sensor lifted off skin but BPM shows the last value forever" case.
static uint32_t bpm_start_ms       = 0;
static float    bpm_baseline       = 0.0f;
static bool     bpm_baseline_ready = false;
static float    bpm_ac_prev        = 0.0f;    // AC value from the previous sample
static uint32_t bpm_last_peak_ms   = 0;
static float    bpm_intervals[4]   = {0};
static uint8_t  bpm_interval_idx   = 0;
static float    bpm_current        = 0.0f;
static float    bpm_ac_peak        = 0.0f;    // envelope of |AC| for signal quality

static void bpm_reset(void) {
    bpm_start_ms       = millis();
    bpm_baseline       = 0.0f;
    bpm_baseline_ready = false;
    bpm_ac_prev        = 0.0f;
    bpm_last_peak_ms   = 0;
    bpm_current        = 0.0f;
    bpm_ac_peak        = 0.0f;
    for (int i = 0; i < 4; i++) bpm_intervals[i] = 0;
    bpm_interval_idx = 0;
}

static void bpm_update(uint32_t green_sample, uint32_t now_ms) {
    float x = (float)green_sample;
    // Staleness: no peak in 2 s -> forget the BPM. Avoids the "sensor lifted
    // off, BPM stays at 79 forever" case.
    if (bpm_baseline_ready && bpm_last_peak_ms > 0 &&
        (now_ms - bpm_last_peak_ms) > 2000) {
        bpm_current = 0.0f;
        for (int i = 0; i < 4; i++) bpm_intervals[i] = 0;
        bpm_interval_idx = 0;
    }
    uint32_t elapsed = now_ms - bpm_start_ms;
    if (bpm_baseline == 0.0f) bpm_baseline = x;    // seed with first sample
    float alpha = (elapsed < 5000) ? 0.05f : 0.005f;
    if (elapsed >= 5000) bpm_baseline_ready = true;
    bpm_baseline = bpm_baseline * (1.0f - alpha) + x * alpha;
    float ac = x - bpm_baseline;
    float ac_abs = ac < 0 ? -ac : ac;
    if (ac_abs > bpm_ac_peak) bpm_ac_peak = ac_abs;
    else                      bpm_ac_peak *= 0.998f;
    if (!bpm_baseline_ready) {
        bpm_ac_prev = ac;
        return;
    }
    // Adaptive amplitude threshold: 30 % of recent peak, floor at 30 counts.
    float thresh = bpm_ac_peak * 0.30f;
    if (thresh < 30.0f) thresh = 30.0f;
    // Local-max detection: previous AC sample was higher than current AND
    // was above threshold AND >= 300 ms since last peak (200 BPM cap).
    if (bpm_ac_prev > ac && bpm_ac_prev > thresh &&
        (now_ms - bpm_last_peak_ms) > 300) {
        if (bpm_last_peak_ms > 0) {
            uint32_t interval = now_ms - bpm_last_peak_ms;
            if (interval > 300 && interval < 1500) {   // 40-200 BPM plausible range
                bpm_intervals[bpm_interval_idx] = (float)interval;
                bpm_interval_idx = (bpm_interval_idx + 1) & 0x3;
                float sum = 0; int n = 0;
                for (int i = 0; i < 4; i++) {
                    if (bpm_intervals[i] > 0.0f) { sum += bpm_intervals[i]; n++; }
                }
                if (n > 0) bpm_current = 60000.0f / (sum / (float)n);
            }
        }
        bpm_last_peak_ms = now_ms;
    }
    bpm_ac_prev = ac;
}

static bool tmp_init(void) {
    if (tmp_addr == 0) return false;
    // Default config = continuous, 8-sample avg, 1 s conversion cycle. Fine.
    // Just verify DEVICE_ID.
    uint8_t buf[2];
    if (!i2c_read_buf(Wire, tmp_addr, TMP_REG_DEVICE_ID, buf, 2)) return false;
    return true;
}
static void tmp_park(void) {
    // TMP117 config register 0x01 MOD field: 01 = shutdown
    uint8_t v[2] = { 0x04, 0x00 };   // 0x04 << 8 | 0x00 = shutdown-ish
    Wire.beginTransmission(tmp_addr);
    Wire.write(TMP_REG_CONFIG);
    Wire.write(v[0]);
    Wire.write(v[1]);
    Wire.endTransmission();
}

// ═════════════════════════════════════════════════════════════════════════════
// Sensor read + CSV write helpers
// ═════════════════════════════════════════════════════════════════════════════
// LIS3MDLTR sensitivity at ±16 G FS = 1711 LSB/Gauss.
static float lis_lsb_to_g_16G = 1.0f / 1711.0f;
static float lsm_lsb_to_g_2G  = 2.0f / 32768.0f;
static float lsm_lsb_to_dps   = 250.0f / 32768.0f;

static void write_csv_header(File &f, const char *cols) {
    if (!f) return;
    char now_iso[32]; rtc_read_iso(now_iso, sizeof(now_iso));
    f.printf("# sketch=7_demo_field_capture\n");
    f.printf("# mode=%s  boot_seq=%u  rec_seq=%u  ms_boot_start=%u  rtc_start=%s\n",
             MODE_INFO[current_mode].name,
             (unsigned)boot_seq, (unsigned)rec_seq,
             (unsigned)millis(), now_iso);
    f.printf("%s\n", cols);
}

static bool env_read_and_write(File &f) {
    // Wait for BME conversion done (poll ADC pressure register bit 7 in MEAS_STATUS_0 = 0x1D)
    uint32_t t0 = millis();
    while ((i2c_read_reg(Wire, BME688_ADDR, 0x1D) & 0x80) == 0) {
        if (millis() - t0 > 500) break;
        delay(2);
    }
    // Read TPH block: press_msb (0x1F), press_lsb (0x20), press_xlsb (0x21),
    //                 temp_msb (0x22), temp_lsb (0x23), temp_xlsb (0x24),
    //                 hum_msb  (0x25), hum_lsb  (0x26)
    uint8_t b[8];
    if (!i2c_read_buf(Wire, BME688_ADDR, BME_REG_PRESS_ADC, b, 8)) return false;
    uint32_t p_raw = ((uint32_t)b[0] << 12) | ((uint32_t)b[1] << 4) | (b[2] >> 4);
    uint32_t t_raw = ((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | (b[5] >> 4);
    uint16_t h_raw = ((uint16_t)b[6] << 8)  |  b[7];
    // Gas
    uint8_t g[2];
    i2c_read_buf(Wire, BME688_ADDR, BME_REG_GAS_R_MSB, g, 2);
    uint16_t gas_r_raw = ((uint16_t)g[0] << 2) | (g[1] >> 6);
    uint8_t  gas_range = g[1] & 0x0F;
    bool     gas_valid = (g[1] >> 5) & 1;
    bool     heat_stab = (g[1] >> 4) & 1;
    // VEML
    uint16_t als = 0, wh = 0;
    veml_read_word(VEML_REG_ALS, &als);
    veml_read_word(VEML_REG_WHITE, &wh);
    float lux = als * VEML_LX_PER_CT;
    uint32_t ms = millis();
    // Apply full BME688 T/P/H compensation. Gas resistance is left raw; a
    // proper gas-ohm calculation needs the per-chip range LUT and is a
    // post-processing step.
    float T_c = 0.0f, P_pa = 0.0f, H_pct = 0.0f;
    bme_compensate(t_raw, p_raw, h_raw, &T_c, &P_pa, &H_pct);
    float P_hpa = P_pa / 100.0f;
    f.printf("%u,%.3f,%.2f,%.2f,%u,%u,%u,%u,%u,%.2f\n",
             (unsigned)ms,
             (double)T_c, (double)P_hpa, (double)H_pct,
             (unsigned)gas_r_raw, (unsigned)gas_range,
             gas_valid ? 1 : 0, heat_stab ? 1 : 0,
             (unsigned)als, (double)lux);
    // Serial mirror -- throttled to ~2 Hz. Labels are the physical quantity,
    // not raw ADC (so "T" = temperature °C, not timestamp).
    static uint32_t last_ser = 0;
    if (ms - last_ser >= 500) {
        last_ser = ms;
        Serial.printf("[env ] T=%.2fC P=%.2fhPa H=%.2f%%RH  gas_r=%u range=%u v=%u stab=%u  als=%u %.1flx\n",
                      (double)T_c, (double)P_hpa, (double)H_pct,
                      (unsigned)gas_r_raw, (unsigned)gas_range,
                      gas_valid ? 1 : 0, heat_stab ? 1 : 0,
                      (unsigned)als, (double)lux);
    }
    bme_trigger_forced();     // queue next forced conversion
    return true;
}

static bool motion_read_and_write(File &f) {
    uint8_t bl[6], bs[14];
    bool ok_lis = i2c_read_buf(Wire, LIS3MDL_ADDR, LIS_AUTO_INC | LIS_REG_OUT_X_L, bl, 6);
    bool ok_lsm = i2c_read_buf(Wire, LSM6DSV_ADDR, LSM_REG_OUT_TEMP, bs, 14);
    if (!ok_lis || !ok_lsm) return false;
    int16_t mx = (int16_t)((bl[1] << 8) | bl[0]);
    int16_t my = (int16_t)((bl[3] << 8) | bl[2]);
    int16_t mz = (int16_t)((bl[5] << 8) | bl[4]);
    int16_t t  = (int16_t)((bs[1] << 8) | bs[0]);
    int16_t gx = (int16_t)((bs[3] << 8) | bs[2]);
    int16_t gy = (int16_t)((bs[5] << 8) | bs[4]);
    int16_t gz = (int16_t)((bs[7] << 8) | bs[6]);
    int16_t ax = (int16_t)((bs[9] << 8) | bs[8]);
    int16_t ay = (int16_t)((bs[11] << 8) | bs[10]);
    int16_t az = (int16_t)((bs[13] << 8) | bs[12]);
    // MROI (bit 1 of INT_SRC, 0x31) = chip-internal "measurement range
    // overflow" flag -- the LIS3MDL's own saturation report, independent of
    // our raw-count math. Separate read/transaction from the OUT_x burst
    // above since MROI lives outside the auto-increment OUT_x block.
    uint8_t int_src = i2c_read_reg(Wire, LIS3MDL_ADDR, 0x31);
    bool mroi = int_src & 0x02;
    uint32_t ms = millis();
    f.printf("%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u\n",
             (unsigned)ms, mx, my, mz, t, gx, gy, gz, ax, ay, az, mroi ? 1 : 0);
    // Serial mirror -- throttled to ~2 Hz. Convert axes to G / dps for readability.
    static uint32_t last_ser = 0;
    if (ms - last_ser >= 500) {
        last_ser = ms;
        Serial.printf("[mot ] mag=(%+.2f,%+.2f,%+.2f)G  A=(%+.2f,%+.2f,%+.2f)g  G=(%+.0f,%+.0f,%+.0f)dps  T=%d  MROI=%u\n",
                      (double)(mx * lis_lsb_to_g_16G), (double)(my * lis_lsb_to_g_16G), (double)(mz * lis_lsb_to_g_16G),
                      (double)(ax * lsm_lsb_to_g_2G), (double)(ay * lsm_lsb_to_g_2G), (double)(az * lsm_lsb_to_g_2G),
                      (double)(gx * lsm_lsb_to_dps),  (double)(gy * lsm_lsb_to_dps),  (double)(gz * lsm_lsb_to_dps),
                      (int)t, mroi ? 1 : 0);
    }
    return true;
}

static bool skin_read_and_write(File &f) {
    // MAX FIFO in Multi-LED mode: 3 slots × 3 bytes = 9 bytes per sample.
    uint8_t wr = i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_FIFO_WR_PTR) & 0x1F;
    uint8_t rd = i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_FIFO_RD_PTR) & 0x1F;
    int8_t  n  = (int8_t)((wr - rd) & 0x1F);
    if (n < 0) n += 32;
    if (n > 8) n = 8;                        // cap per-call to avoid stalling loop
    uint32_t red_last = 0, ir_last = 0, green_last = 0;
    float    tc_last  = 0.0f;
    for (int i = 0; i < n; i++) {
        uint8_t s[9];
        if (!i2c_read_buf(Wire, MAX30101_ADDR, MAX_REG_FIFO_DATA, s, 9)) break;
        uint32_t red   = ((uint32_t)(s[0] & 0x03) << 16) | ((uint32_t)s[1] << 8) | s[2];
        uint32_t ir    = ((uint32_t)(s[3] & 0x03) << 16) | ((uint32_t)s[4] << 8) | s[5];
        uint32_t green = ((uint32_t)(s[6] & 0x03) << 16) | ((uint32_t)s[7] << 8) | s[8];
        // TMP117: one temp read per sample. Its own 15 ms conversion cycle
        // is much slower than 100 Hz MAX, so we re-read the same reg often --
        // fine, TMP117 returns the last completed conversion each time.
        uint8_t tb[2];
        i2c_read_buf(Wire, tmp_addr, TMP_REG_TEMP, tb, 2);
        int16_t traw = (int16_t)((tb[0] << 8) | tb[1]);
        float tc = traw * 0.0078125f;
        uint32_t sample_ms = millis();
        bpm_update(green, sample_ms); // feed Green channel (best HR SNR through skin)
        f.printf("%u,%lu,%lu,%lu,%.4f\n",
                 (unsigned)sample_ms,
                 (unsigned long)red, (unsigned long)ir, (unsigned long)green,
                 (double)tc);
        red_last   = red;
        ir_last    = ir;
        green_last = green;
        tc_last    = tc;
    }
    // Serial mirror -- throttled to ~2 Hz. Report the latest sample this batch
    // plus current BPM (or "settling..." during the first 5 s).
    static uint32_t last_ser = 0;
    uint32_t ms = millis();
    if (n > 0 && ms - last_ser >= 500) {
        last_ser = ms;
        char bpm_str[16];
        if (!bpm_baseline_ready)      snprintf(bpm_str, sizeof(bpm_str), "settling");
        else if (bpm_current == 0.0f) snprintf(bpm_str, sizeof(bpm_str), "-- ");
        else                          snprintf(bpm_str, sizeof(bpm_str), "%.0f", (double)bpm_current);
        Serial.printf("[skin] Red=%lu IR=%lu Green=%lu  T=%.3fC  BPM=%s  ac_peak=%.0f  (batch=%d)\n",
                      (unsigned long)red_last, (unsigned long)ir_last,
                      (unsigned long)green_last, (double)tc_last,
                      bpm_str, (double)bpm_ac_peak, (int)n);
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// I2S mic + WAV
// ═════════════════════════════════════════════════════════════════════════════
static bool mic_init_if_needed(void) {
    if (i2s_up) return true;
    I2S.setPinsPdmRx(PIN_MIC_CLK, PIN_MIC_DATA);
    if (!I2S.begin(I2S_MODE_PDM_RX, 48000, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_MONO)) {
        Serial.println("[MIC ] I2S begin FAILED");
        return false;
    }
    i2s_up = true;
    delay(20);   // let the first ~1000 samples settle
    // discard
    static int16_t junk[512];
    for (int k = 0; k < 3; k++) {
        I2S.readBytes((char *)junk, sizeof(junk));
    }
    return true;
}
static void mic_shutdown(void) {
    if (!i2s_up) return;
    I2S.end();
    i2s_up = false;
}
static void wav_write_header(File &f, uint32_t sr_hz) {
    // Placeholder header; patched with true size on close.
    uint8_t hdr[44] = {0};
    memcpy(hdr,     "RIFF", 4);
    // hdr[4..7] = filesize - 8 -- patched on close
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16;                      // fmt subchunk size = 16
    hdr[20] = 1; hdr[21] = 0;          // PCM
    hdr[22] = 1; hdr[23] = 0;          // 1 channel
    hdr[24] = sr_hz & 0xFF;
    hdr[25] = (sr_hz >> 8) & 0xFF;
    hdr[26] = (sr_hz >> 16) & 0xFF;
    hdr[27] = (sr_hz >> 24) & 0xFF;
    uint32_t byte_rate = sr_hz * 2;    // 16-bit mono
    hdr[28] = byte_rate & 0xFF;
    hdr[29] = (byte_rate >> 8) & 0xFF;
    hdr[30] = (byte_rate >> 16) & 0xFF;
    hdr[31] = (byte_rate >> 24) & 0xFF;
    hdr[32] = 2;                        // block align
    hdr[34] = 16;                       // bits per sample
    memcpy(hdr + 36, "data", 4);
    // hdr[40..43] = data chunk size -- patched on close
    f.write(hdr, 44);
}
static void wav_patch_size(File &f, uint32_t data_bytes) {
    if (!f) return;
    uint32_t riff = data_bytes + 36;
    f.seek(4);   f.write((uint8_t *)&riff, 4);
    f.seek(40);  f.write((uint8_t *)&data_bytes, 4);
}

// Write one buffer of 16-bit samples (gain-adjusted with saturation) to WAV.
// Also computes RMS + peak of the post-gain buffer and prints a mic-level
// heartbeat to Serial at ~2 Hz so Ivan can see audio is flowing.
static void mic_chunk_write(File &f, int16_t *raw, size_t n, uint32_t *bytes_out) {
    int64_t sq_sum = 0;
    int32_t peak   = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = (int32_t)raw[i] * MIC_GAIN_MULT;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        raw[i] = (int16_t)v;
        int32_t av = (v < 0) ? -v : v;
        if (av > peak) peak = av;
        sq_sum += (int64_t)v * v;
    }
    f.write((uint8_t *)raw, n * 2);
    *bytes_out += n * 2;

    static uint32_t last_mic_ser = 0;
    uint32_t ms = millis();
    if (n > 0 && ms - last_mic_ser >= 500) {
        last_mic_ser = ms;
        float rms = sqrtf((float)(sq_sum / (int64_t)n));
        Serial.printf("[mic ] rms=%.0f  peak=%ld  wav_bytes=%u\n",
                      (double)rms, (long)peak, (unsigned)*bytes_out);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Filename generation + SD dir setup
// ═════════════════════════════════════════════════════════════════════════════
static void sd_mkdirs(void) {
    SD_MMC.mkdir("/data");
    SD_MMC.mkdir("/data/mic");
    SD_MMC.mkdir("/data/env");
    SD_MMC.mkdir("/data/mot");
    SD_MMC.mkdir("/data/skin");
}
static void filename_csv(char *out, size_t n) {
    snprintf(out, n, "/data/%s/s%u_r%u.csv",
             MODE_INFO[current_mode].name,
             (unsigned)boot_seq, (unsigned)rec_seq);
}
static void filename_annot(char *out, size_t n) {
    snprintf(out, n, "/data/mic/s%u_r%u_annot.wav",
             (unsigned)boot_seq, (unsigned)rec_seq);
}
static void filename_mic(char *out, size_t n) {
    snprintf(out, n, "/data/mic/s%u_r%u.wav",
             (unsigned)boot_seq, (unsigned)rec_seq);
}
static void write_annot_sidecar_txt(const char *wav_path, uint32_t dur_ms) {
    char txt[80];
    snprintf(txt, sizeof(txt), "%s.txt", wav_path);
    File s = SD_MMC.open(txt, FILE_WRITE);
    if (!s) return;
    char now_iso[32]; rtc_read_iso(now_iso, sizeof(now_iso));
    s.printf("sketch=7_demo_field_capture\n");
    s.printf("mode=%s\n", MODE_INFO[current_mode].name);
    s.printf("boot_seq=%u  rec_seq=%u\n", (unsigned)boot_seq, (unsigned)rec_seq);
    s.printf("ms_boot=%u  rtc_start=%s\n", (unsigned)millis(), now_iso);
    s.printf("mic_gain_x=%u\n", (unsigned)MIC_GAIN_MULT);
    s.printf("duration_ms=%u\n", (unsigned)dur_ms);
    s.close();
}

// ═════════════════════════════════════════════════════════════════════════════
// Flow: recording start/end helpers
// ═════════════════════════════════════════════════════════════════════════════
static void beeps_before_annot(void) {
    for (int i = 0; i < BEEP_COUNT; i++) {
        neopixelWrite(PIN_WS2812_DIN, WS_BLIP_LEVEL, 0, 0);
        drv_trigger(DRV_STRONG_CLICK);
        delay(BEEP_ON_MS);
        rgb_off();
        delay(BEEP_OFF_MS);
    }
}

static void park_current_sensor_pack(void) {
    switch (current_mode) {
        case MODE_ENV:    bme_park(); veml_park();  break;
        case MODE_MOTION: lis_park(); lsm_park();   break;
        case MODE_SKIN:   max_park(); tmp_park();   break;
        default: break;
    }
}

static void abort_recording_and_flush(void) {
    // Called from ship-mode entry. Regardless of which state we're in, close
    // any open file cleanly (WAV header patched with actual bytes) and put
    // the sensor pack back into low-power. Ship-mode may not drop BATFET
    // immediately if USB is present, so parking keeps the chips quiet
    // during the interregnum.
    if (f_wav) {
        wav_patch_size(f_wav, wav_bytes_written);
        f_wav.close();
    }
    if (f_csv) {
        f_csv.close();
    }
    if (i2s_up) mic_shutdown();
    park_current_sensor_pack();
    wav_bytes_written = 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// State transitions
// ═════════════════════════════════════════════════════════════════════════════
// Pre-warm the current mode's sensor pack. Called at the START of voice
// annotation so the sensors have ~6 s of settled operation before the CSV
// opens -- eliminates initial transients (BME conversion warmup, MAX FIFO
// seed samples, LIS/LSM startup).
static void warmup_current_sensor_pack(void) {
    switch (current_mode) {
        case MODE_ENV:
            bme_init_with_gas();     // triggers first forced conversion
            veml_init();
            break;
        case MODE_MOTION:
            lis_init();               // 40 Hz continuous
            lsm_init();               // 240 Hz continuous
            break;
        case MODE_SKIN:
            max_init_all_leds();      // Multi-LED: Red + IR + Green; FIFO fills
            tmp_init();               // TMP117 continuous conversion
            bpm_reset();              // start BPM baseline settle now (5 s window)
            break;
        default:
            break;
    }
    Serial.printf("[WARM] %s sensor pack initialised (5+ s pre-record settle)\n",
                  MODE_INFO[current_mode].name);
}

static void begin_voice_annotation(void) {
    Serial.printf("[REC ] voice annotation for %s (rec_seq=%u)\n",
                  MODE_INFO[current_mode].name, (unsigned)rec_seq);
    // Kick off sensor pack now so it's stable by the time the CSV opens.
    warmup_current_sensor_pack();
    beeps_before_annot();
    if (!mic_init_if_needed()) return;
    char path[64]; filename_annot(path, sizeof(path));
    f_wav = SD_MMC.open(path, FILE_WRITE);
    if (!f_wav) {
        Serial.println("[REC ] annot WAV open FAILED");
        return;
    }
    wav_write_header(f_wav, 48000);
    wav_bytes_written = 0;
    write_annot_sidecar_txt(path, VOICE_ANNOT_MS);
    app_state = ST_VOICE_ANNOT;
    state_enter_ms = millis();
}

static void begin_sensor_recording(void) {
    // Sensor pack was warmed up in begin_voice_annotation() ~6 s ago.
    // Prime the mode-specific "fresh sample" path so the first CSV row is
    // captured AFTER this point rather than being a stale warmup residue:
    //   ENV   : re-trigger BME forced conversion so first read is fresh.
    //   SKIN  : clear MAX FIFO so accumulated warmup samples are discarded.
    //   MOTION: no priming -- LIS/LSM continuously stream, always fresh.
    char path[64]; filename_csv(path, sizeof(path));
    switch (current_mode) {
        case MODE_ENV:
            bme_trigger_forced();
            f_csv = SD_MMC.open(path, FILE_WRITE);
            if (f_csv) write_csv_header(f_csv, "ms,T_C,P_hPa,H_pct,gas_r,gas_range,gas_valid,heat_stab,als,lux");
            break;
        case MODE_MOTION:
            f_csv = SD_MMC.open(path, FILE_WRITE);
            if (f_csv) write_csv_header(f_csv, "ms,mx,my,mz,lsm_t,gx,gy,gz,ax,ay,az,mroi");
            break;
        case MODE_SKIN:
            i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_FIFO_WR_PTR, 0);
            i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_OVF_CTR,     0);
            i2c_write_reg(Wire, MAX30101_ADDR, MAX_REG_FIFO_RD_PTR, 0);
            f_csv = SD_MMC.open(path, FILE_WRITE);
            if (f_csv) write_csv_header(f_csv, "ms,red,ir,green,tc");
            break;
        default:
            break;
    }
    drv_trigger(DRV_STRONG_CLICK);   // "recording is now underway" tick
    app_state = ST_RECORDING;
    state_enter_ms = millis();
    Serial.printf("[REC ] sensor recording start (%s, %s)\n",
                  MODE_INFO[current_mode].name, path);
}

static void begin_mic_recording(void) {
    Serial.printf("[REC ] mic-only recording (rec_seq=%u)\n", (unsigned)rec_seq);
    // Warn beeps + DRV, then straight into full-mic capture (no annot).
    beeps_before_annot();
    if (!mic_init_if_needed()) return;
    char path[64]; filename_mic(path, sizeof(path));
    f_wav = SD_MMC.open(path, FILE_WRITE);
    if (!f_wav) {
        Serial.println("[REC ] mic WAV open FAILED");
        return;
    }
    wav_write_header(f_wav, 48000);
    wav_bytes_written = 0;
    write_annot_sidecar_txt(path, RECORDING_MS);
    drv_trigger(DRV_STRONG_CLICK);
    app_state = ST_RECORDING;
    state_enter_ms = millis();
}

static void finish_recording(bool early) {
    Serial.printf("[REC ] end (%s)\n", early ? "EARLY (single click)" : "TIMEOUT 30 s");
    // Close whatever files are open. Sensor teardown per mode.
    if (f_wav) {
        wav_patch_size(f_wav, wav_bytes_written);
        f_wav.close();
    }
    if (f_csv) f_csv.close();
    switch (current_mode) {
        case MODE_ENV:    bme_park(); veml_park();          break;
        case MODE_MOTION: lis_park(); lsm_park();           break;
        case MODE_SKIN:   max_park(); tmp_park();           break;
        case MODE_MIC:    mic_shutdown();                    break;
        default: break;
    }
    if (i2s_up) mic_shutdown();
    // Long DRV fire at end of 30 s (or the single-click early-end).
    drv_trigger(DRV_LONG_BUZZ);
    rec_seq++;
    app_state = ST_STANDBY;
    state_enter_ms = millis();
    last_activity_ms = state_enter_ms;
    rgb_off();
}

// ═════════════════════════════════════════════════════════════════════════════
// Alarm pattern player (Purple)
// ═════════════════════════════════════════════════════════════════════════════
static uint8_t  alarm_playing_idx = 0;
static size_t   alarm_next_step   = 0;

static void begin_alarm(void) {
    alarm_playing_idx = alarm_idx;
    alarm_next_step   = 0;
    app_state = ST_ALARM_FIRING;
    state_enter_ms = millis();
    Serial.printf("[ALRM] firing pattern %c (%u steps, ~%u ms)\n",
                  'A' + alarm_playing_idx,
                  (unsigned)ALARM_LENS[alarm_playing_idx],
                  (unsigned)ALARM_DURATION_MS);
    rgb_set_max(MODE_ALARM);
    // Rotate index for the *next* click.
    alarm_idx = (alarm_idx + 1) % 3;
}

static void step_alarm(void) {
    uint32_t elapsed = millis() - state_enter_ms;
    while (alarm_next_step < ALARM_LENS[alarm_playing_idx] &&
           ALARM_TABLES[alarm_playing_idx][alarm_next_step].ms <= elapsed) {
        drv_trigger(ALARM_TABLES[alarm_playing_idx][alarm_next_step].effect);
        alarm_next_step++;
    }
    if (elapsed >= ALARM_DURATION_MS) {
        app_state = ST_STANDBY;
        rgb_off();
        state_enter_ms = millis();
        last_activity_ms = state_enter_ms;
        Serial.println("[ALRM] done");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Flashlight
// ═════════════════════════════════════════════════════════════════════════════
static uint8_t fl_duty_from_level(uint8_t lvl) {
    if (lvl == 0) return 0;
    uint32_t d = (uint32_t)lvl * FL_MAX_DUTY / FL_LEVELS;
    if (d > FL_MAX_DUTY) d = FL_MAX_DUTY;
    return (uint8_t)d;
}
static void fl_apply(void) { ledcWrite(PIN_FLASHLIGHT, fl_duty_from_level(fl_level)); }
static void fl_off(void)   { ledcWrite(PIN_FLASHLIGHT, 0); }

// ═════════════════════════════════════════════════════════════════════════════
// Single-click dispatch (state × mode)
// ═════════════════════════════════════════════════════════════════════════════
static void on_single_click(void) {
    switch (app_state) {
        case ST_STANDBY:
            switch (current_mode) {
                case MODE_MIC:        begin_mic_recording(); break;
                case MODE_ENV:
                case MODE_MOTION:
                case MODE_SKIN:       begin_voice_annotation(); break;
                case MODE_FLASHLIGHT:
                    app_state = ST_FL_ON;
                    fl_apply();
                    rgb_set_max(MODE_FLASHLIGHT);
                    drv_trigger(DRV_STRONG_CLICK);
                    break;
                case MODE_ALARM:      begin_alarm(); break;
                default: break;
            }
            break;
        case ST_FL_ON:
            fl_off();
            fl_level = FL_INIT_LEVEL;
            app_state = ST_STANDBY;
            state_enter_ms = millis();
            last_activity_ms = state_enter_ms;
            drv_trigger(DRV_STRONG_CLICK);
            rgb_off();
            break;
        case ST_VOICE_ANNOT:
            // Ignore single-clicks during annot -- user is talking, don't want
            // an accidental brush of the button to kill their tag.
            break;
        case ST_RECORDING:
            recording_early_end = true;
            break;
        case ST_ALARM_FIRING:
            Serial.println("[ALRM] user interrupt");
            app_state = ST_STANDBY;
            rgb_off();
            state_enter_ms = millis();
            last_activity_ms = state_enter_ms;
            drv_trigger(DRV_STRONG_CLICK);
            break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Encoder click dispatch (called from handle_encoder_poll on each detent)
// ═════════════════════════════════════════════════════════════════════════════
static void on_encoder_click(int8_t dir) {
    if (dir == 0) return;
    drv_trigger(DRV_STRONG_CLICK);          // tactile feedback on every detent
    if (app_state == ST_STANDBY) {
        int m = (int)current_mode + dir;
        while (m < 0)           m += MODE_COUNT;
        while (m >= MODE_COUNT) m -= MODE_COUNT;
        current_mode = (field_mode_t)m;
        nvs_save_mode();
        last_activity_ms = millis();        // reset "solid RGB" window
        Serial.printf("[UI  ] mode=%s\n", MODE_INFO[current_mode].name);
    } else if (app_state == ST_FL_ON) {
        int lv = (int)fl_level + dir;
        if (lv < 0)         lv = 0;
        if (lv > FL_LEVELS) lv = FL_LEVELS;
        fl_level = (uint8_t)lv;
        fl_apply();
    }
    // In VOICE_ANNOT / RECORDING / ALARM_FIRING the encoder is locked --
    // the DRV click above still fires as tactile feedback, but no state change.
}

// ═════════════════════════════════════════════════════════════════════════════
// Setup
// ═════════════════════════════════════════════════════════════════════════════
// Ping every device at boot, verify identity, and leave each in its lowest-
// power state. RTC stays running (has its own battery backing since 2026-07-05).
// Selected mode's warmup_current_sensor_pack() is what re-enables the ones we
// need for that recording.
static void boot_scan_and_probe(void) {
    Serial.println("--- boot headcount ---");

    // ── BQ25619 (bus 2, always-on charger) ─────────────────────────────────
    if (i2c_ping(Wire1, BQ25619_ADDR)) {
        uint8_t part = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_PART);
        uint8_t r00  = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_INPUT_SRC);
        uint8_t r05  = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_CTRL1);
        i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_INPUT_SRC, r00 | BQ_TS_IGNORE_BIT);
        i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_CTRL1,     r05 & ~BQ_WD_MASK);
        bq_ok = true;
        Serial.printf("  BQ25619    bus2 0x6A : ACK  PART=0x%02X  (TS_IGNORE+WD-off applied)\n", part);
    } else {
        Serial.println("  BQ25619    bus2 0x6A : NO ACK  -- ship mode disabled");
    }

    // ── DRV2605L (bus 2) + auto-cal ────────────────────────────────────────
    pinMode(PIN_DRV_EN, OUTPUT);
    digitalWrite(PIN_DRV_EN, HIGH);
    delay(10);
    if (i2c_ping(Wire1, DRV2605_ADDR)) {
        i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_MODE,       DRV_MODE_AUTO_CAL);
        i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_FEEDBACK,   DRV_FEEDBACK_LRA);
        i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_RATED_VOLT, DRV_RATED_VOLTAGE);
        i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_OD_CLAMP,   DRV_OD_CLAMP_REG);
        i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL4,      DRV_AUTO_CAL_TIME);
        i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL1,      DRV_CTRL1_DRIVE_TIME);
        uint32_t t0 = millis();
        i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_GO, 0x01);
        uint8_t go = 1;
        while (go && (millis() - t0) < 1500) {
            delay(50);
            go = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_GO) & 0x01;
        }
        uint8_t st = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
        drv_cal_ok = (go == 0) && !(st & 0x08);
        i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_MODE,    DRV_MODE_INTERNAL);
        i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_LIBRARY, DRV_LIB_LRA);
        drv_ok = true;
        Serial.printf("  DRV2605L   bus2 0x5A : ACK  auto-cal %s (%u ms) STATUS=0x%02X\n",
                      drv_cal_ok ? "PASS" : "FAIL",
                      (unsigned)(millis() - t0), st);
    } else {
        Serial.println("  DRV2605L   bus2 0x5A : NO ACK  -- encoder + alarm clicks disabled");
    }

    // ── PCF85063A RTC (bus 1, permanently running) ─────────────────────────
    if (i2c_ping(Wire, PCF85063A_ADDR)) {
        uint8_t sec = i2c_read_reg(Wire, PCF85063A_ADDR, RTC_REG_SECONDS);
        rtc_running = !(sec & RTC_OS_BIT);
        char now_iso[32]; rtc_read_iso(now_iso, sizeof(now_iso));
        Serial.printf("  PCF85063A  bus1 0x51 : ACK  %s  time=%s\n",
                      rtc_running ? "running" : "oscstop -- send SET_TIME", now_iso);
    } else {
        Serial.println("  PCF85063A  bus1 0x51 : NO ACK");
    }

    // ── VEML6030 ────────────────────────────────────────────────────────────
    if (i2c_ping(Wire, VEML6030_ADDR)) {
        veml_park();
        Serial.println("  VEML6030   bus1 0x10 : ACK  parked (shutdown)");
    } else {
        Serial.println("  VEML6030   bus1 0x10 : NO ACK");
    }

    // ── LIS3MDLTR ───────────────────────────────────────────────────────────
    if (i2c_ping(Wire, LIS3MDL_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LIS3MDL_ADDR, LIS_REG_WHO);
        lis_park();
        Serial.printf("  LIS3MDLTR  bus1 0x1C : ACK  WHO=0x%02X (expect 0x%02X)  parked\n",
                      who, LIS_WHO_VAL);
    } else {
        Serial.println("  LIS3MDLTR  bus1 0x1C : NO ACK");
    }
    uint8_t off[6];
    i2c_read_buf(Wire, LIS3MDL_ADDR, LIS_AUTO_INC | 0x05, off, 6);
    Serial.printf("  LIS3MDLTR offsets: X=0x%02X%02X Y=0x%02X%02X Z=0x%02X%02X\n",
                  off[1], off[0], off[3], off[2], off[5], off[4]);

    // ── LSM6DSV16X ─────────────────────────────────────────────────────────
    if (i2c_ping(Wire, LSM6DSV_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WHO);
        // Push it through a reset now so warmup gets a clean chip.
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x01);
        delay(10);
        lsm_park();
        Serial.printf("  LSM6DSV16X bus1 0x6B : ACK  WHO=0x%02X (expect 0x%02X)  parked\n",
                      who, LSM_WHO_VAL);
    } else {
        Serial.println("  LSM6DSV16X bus1 0x6B : NO ACK");
    }

    // ── BME688 (also loads per-chip calibration for compensation) ──────────
    if (i2c_ping(Wire, BME688_ADDR)) {
        uint8_t id = i2c_read_reg(Wire, BME688_ADDR, BME_REG_CHIP_ID);
        bme_read_calibration();
        bme_park();
        Serial.printf("  BME688     bus1 0x76 : ACK  CHIP_ID=0x%02X (expect 0x%02X)  parked, cal loaded\n",
                      id, BME_CHIP_ID_VAL);
    } else {
        Serial.println("  BME688     bus1 0x76 : NO ACK");
    }

    // ── TMP117 (ADD0 strap selects 0x48 or 0x49) ───────────────────────────
    tmp_addr = 0;
    if      (i2c_ping(Wire, TMP117_ADDR_LO)) tmp_addr = TMP117_ADDR_LO;
    else if (i2c_ping(Wire, TMP117_ADDR_HI)) tmp_addr = TMP117_ADDR_HI;
    if (tmp_addr) {
        uint8_t db[2] = {0};
        i2c_read_buf(Wire, tmp_addr, TMP_REG_DEVICE_ID, db, 2);
        uint16_t did = ((uint16_t)db[0] << 8) | db[1];
        tmp_park();
        Serial.printf("  TMP117     bus1 0x%02X : ACK  DEVICE_ID=0x%04X  parked\n",
                      tmp_addr, did);
    } else {
        Serial.println("  TMP117     bus1 0x48/0x49 : NO ACK");
    }

    // ── MAX30101 ────────────────────────────────────────────────────────────
    if (i2c_ping(Wire, MAX30101_ADDR)) {
        uint8_t part = i2c_read_reg(Wire, MAX30101_ADDR, MAX_REG_PART_ID);
        max_park();
        Serial.printf("  MAX30101   bus1 0x57 : ACK  PART_ID=0x%02X (expect 0x15)  parked\n", part);
    } else {
        Serial.println("  MAX30101   bus1 0x57 : NO ACK");
    }

    Serial.println("--- all sensors parked; only RTC + BQ stay active in standby ---");
}

void setup(void) {
    Serial.begin(115200);
    // USB CDC on the ESP32-S3 needs ~500 ms to enumerate, and the serial
    // monitor may open a beat later. Wait 2 s and flush any stale RX bytes
    // (some host TTY drivers echo the initial output back into the RX FIFO,
    // which used to feed junk into handle_serial_command()) so boot lines
    // aren't cut off and no phantom commands land.
    delay(2000);
    while (Serial.available()) Serial.read();
    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════════════╗");
    Serial.println("║  Kompic Mk I -- 7_demo_field_capture                         ║");
    Serial.println("║  Encoder = mode; button single = action; double = ship mode  ║");
    Serial.println("║  Serial: SET_TIME YYYY-MM-DDTHH:MM:SS  or  GET_TIME          ║");
    Serial.println("╚══════════════════════════════════════════════════════════════╝");

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_ENC_A,  INPUT_PULLUP);
    pinMode(PIN_ENC_B,  INPUT_PULLUP);
    // Flashlight gate hardening: force GPIO41 (MTDI, no strap) to a hard 0 V
    // and kill any internal pull BEFORE LEDC takes the pin, so the BSS138W
    // gate can never sit in the subthreshold-glow zone during boot.
    pinMode(PIN_FLASHLIGHT, OUTPUT);
    digitalWrite(PIN_FLASHLIGHT, LOW);
    gpio_set_pull_mode((gpio_num_t)PIN_FLASHLIGHT, GPIO_FLOATING);
    ledcAttach(PIN_FLASHLIGHT, 1000, 8);
    ledcWrite(PIN_FLASHLIGHT, 0);
    rgb_off();

    Wire.begin (PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 400000);
    delay(20);

    boot_scan_and_probe();

    // SD @ 20 MHz (Stage 4 verified)
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    if (!SD_MMC.begin("/sdcard", true, false, 20000)) {
        Serial.println("[SD  ] mount FAILED -- recordings will not persist");
    } else {
        Serial.printf("[SD  ] mount OK %s %llu MB\n",
                      SD_MMC.cardType() == CARD_SDHC ? "SDHC" : "SD",
                      (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
        sd_mkdirs();
    }

    nvs_load();

    // Encoder is polled (no ISR). See handle_encoder_poll() and doc 20.16.
    Serial.printf("[UI  ] ready. mode=%s. Rotate encoder to change, click to fire.\n",
                  MODE_INFO[current_mode].name);
    app_state = ST_STANDBY;
    state_enter_ms = millis();
    last_activity_ms = state_enter_ms;
}

// ═════════════════════════════════════════════════════════════════════════════
// Main loop
// ═════════════════════════════════════════════════════════════════════════════
void loop(void) {
    handle_serial_command();
    handle_button();

    // handle_button() may synchronously run beeps_before_annot (~1200 ms) or
    // ship_mode_countdown (~2000 ms). Capture "now" AFTER the callback so any
    // fresh state_enter_ms isn't underflowed by a stale pre-callback timestamp.
    uint32_t now = millis();

    // Encoder: polling state machine emits at most one click per detent-rest
    // cycle. The click callback dispatches based on current app_state.
    handle_encoder_poll();

    // Per-state work
    switch (app_state) {
        case ST_STANDBY:
            // Solid mode colour for SOLID_ON_ACTIVITY_MS after the last
            // rotation / state transition so hue is unambiguous, then start
            // the slow standby pulse.
            if (now - last_activity_ms < SOLID_ON_ACTIVITY_MS) {
                rgb_set_max(current_mode);
            } else {
                rgb_pulse(current_mode, PULSE_STANDBY_MS, now);
            }
            break;

        case ST_FL_ON:
            rgb_set_max(MODE_FLASHLIGHT);
            break;

        case ST_VOICE_ANNOT: {
            // Solid red for the whole 5 s window -- unambiguous "AUDIO IS ON".
            neopixelWrite(PIN_WS2812_DIN, WS_MAX_LEVEL, 0, 0);
            // Drain I2S into WAV
            if (i2s_up && f_wav) {
                static int16_t buf[512];
                size_t got = I2S.readBytes((char *)buf, sizeof(buf));
                if (got > 0) mic_chunk_write(f_wav, buf, got / 2, &wav_bytes_written);
            }
            if (now - state_enter_ms >= VOICE_ANNOT_MS) {
                // End of voice tag. Patch header, close, DRV cue, start sensor.
                if (f_wav) {
                    wav_patch_size(f_wav, wav_bytes_written);
                    f_wav.close();
                    wav_bytes_written = 0;
                }
                mic_shutdown();
                drv_trigger(DRV_STRONG_CLICK);   // hand-off tick
                delay(150);                       // brief gap
                begin_sensor_recording();
            }
            break;
        }

        case ST_RECORDING: {
            rgb_pulse(current_mode, PULSE_RECORD_MS, now);
            // Mode-specific write pass
            if (current_mode == MODE_MIC) {
                if (i2s_up && f_wav) {
                    static int16_t buf[512];
                    size_t got = I2S.readBytes((char *)buf, sizeof(buf));
                    if (got > 0) mic_chunk_write(f_wav, buf, got / 2, &wav_bytes_written);
                }
            } else {
                if (f_csv) {
                    switch (current_mode) {
                        case MODE_ENV:    env_read_and_write(f_csv);   break;
                        case MODE_MOTION: motion_read_and_write(f_csv); break;
                        case MODE_SKIN:   skin_read_and_write(f_csv);   break;
                        default: break;
                    }
                }
            }
            bool time_up = (now - state_enter_ms) >= RECORDING_MS;
            if (recording_early_end || time_up) {
                bool early = recording_early_end;
                recording_early_end = false;
                finish_recording(early);
            }
            break;
        }

        case ST_ALARM_FIRING:
            step_alarm();
            break;
    }

    delay(2);
}
