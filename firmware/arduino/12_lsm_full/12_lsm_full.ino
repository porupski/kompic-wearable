/*
 * 12_lsm_full.ino  --  Kompic Mk I LSM6DSV16X full-range demo (freeze-and-forget POC)
 *
 * Purpose: exercise every LSM6DSV16X advanced feature we intend to use in
 * production firmware, in a standalone sketch that never gets refactored
 * once tuning is signed off. Known-good reference implementation to diff the
 * ESP-IDF port against.
 *
 * -- Sketch layout (Arduino IDE concatenates .ino files alphabetically) ------
 *   12_lsm_full.ino   main -- docstring, all register defines, all shared
 *                     state globals, VIEW_STYLES table, setup(), loop()
 *   bcg.ino           BCG (ballistocardiography) subsystem + params
 *   console.ino       serial command console (live tuning of everything)
 *   hw.ino            I2C, bank switch, LSM init, WS2812, ship mode, button
 *   tap.ino           tap detection + orientation-gated event dispatch
 *   views.ino         view cycling + orientation + peak tracker + tick_* handlers
 *
 * -- Features exercised ------------------------------------------------------
 *   - Accelerometer HP mode, 240 Hz, +/-4 g
 *   - Embedded pedometer (STEP_COUNTER_L/H) -- 30 Hz internal pipeline
 *   - Activity / inactivity auto-sleep (INACT_EN = 10)
 *   - Single/double tap on all three axes with wrist-orientation gating
 *   - Sleep-state transition monitoring
 *   - MLC output register poll (0 until a .ucf classifier is loaded)
 *   - On-chip temperature
 *   - BCG beat detection from accel Z
 *
 * -- Views (button single-click cycles forward; double = backward) -----------
 *   ACCEL_STREAM   solid BLUE          -- accel x/y/z live @ 10 Hz
 *   STEPS          solid CYAN          -- pedometer count + delta / s
 *   ACTIVITY       solid GREEN         -- sleep-state transitions
 *   TAP_VERBOSE    solid YELLOW        -- rich tap diagnostics + config
 *                                        (RED when watch out of tap zone)
 *   TEMP           solid AMBER         -- on-chip temp @ 2 Hz
 *   GYRO           solid MAGENTA       -- gyro x/y/z live @ 10 Hz
 *   LP_MODE        soft WHITE          -- accel dropped to LP1 @ 15 Hz demo
 *   MLC_PROBE      solid RED           -- MLC1_SRC + status regs poll
 *   BCG            solid ORANGE-RED    -- filtered Z-accel + beat detection
 *
 * -- Controls ---------------------------------------------------------------
 *   button single-click   : cycle view FORWARD
 *   button double-click   : cycle view BACKWARD (backup for tap-double)
 *   button hold >= 3 s    : ship mode (BQ25619 BATFET off)
 *   LSM tap SINGLE        : print event + rich diagnostics
 *   LSM tap DOUBLE        : cycle view BACKWARD (except in TAP_VERBOSE view
 *                           where the tap is diagnostic-only)
 *
 * -- Wrist-orientation gate -------------------------------------------------
 * Tap events are IGNORED unless the watch is in a tappable position (roughly
 * horizontal in front of the user). LED goes RED in TAP_VERBOSE while out of
 * zone. Bounds tunable via serial console (pmin/pmax/rlim).
 *
 * -- Serial console (Arduino Serial Monitor, "Newline" line ending) ---------
 * Every tuning parameter is settable at runtime. Type "help" for the full
 * list. "?" prints the current view's config; the tap and BCG views each
 * respond to "?" with their own parameter block.
 *
 * -- Datasheet references ---------------------------------------------------
 * Register addresses and bit positions verified against ST DS13510 rev 4
 * (LSM6DSV16X). Section references inline at point of use.
 *
 * Board: ESP32-S3, Arduino IDE, USB Mode = "Hardware CDC and JTAG".
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// ═════════════════════════════════════════════════════════════════════════════
// Pin map
// ═════════════════════════════════════════════════════════════════════════════
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1     // Wire  -> LSM6DSV16X + other sensors
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4     // Wire1 -> BQ25619
#define PIN_SCL_BUS2       5
#define PIN_WS2812_DIN    42

// ═════════════════════════════════════════════════════════════════════════════
// I2C addresses
// ═════════════════════════════════════════════════════════════════════════════
#define LSM6DSV_ADDR      0x6B
#define BQ25619_ADDR      0x6A

// ═════════════════════════════════════════════════════════════════════════════
// LSM6DSV16X register map (main bank) -- DS13510 rev 4
// ═════════════════════════════════════════════════════════════════════════════
#define LSM_REG_FUNC_CFG_ACCESS   0x01
#define LSM_REG_WHO_AM_I          0x0F
#define LSM_WHO_AM_I_VAL          0x70
#define LSM_REG_CTRL1             0x10
#define LSM_REG_CTRL2             0x11
#define LSM_REG_CTRL3             0x12
#define LSM_REG_CTRL7             0x16
#define LSM_REG_CTRL8             0x17
#define LSM_REG_OUT_TEMP_L        0x20
#define LSM_REG_OUT_TEMP_H        0x21
#define LSM_REG_OUTX_L_G          0x22   // 12B burst: GX,GY,GZ,AX,AY,AZ
#define LSM_REG_WAKE_UP_SRC       0x45
#define LSM_REG_TAP_SRC           0x46
#define LSM_REG_EMB_FUNC_STATUS_MP 0x49
#define LSM_REG_MLC_STATUS_MP     0x4B
#define LSM_REG_FUNCTIONS_ENABLE  0x50
#define LSM_REG_INACTIVITY_DUR    0x54
#define LSM_REG_INACTIVITY_THS    0x55
#define LSM_REG_TAP_CFG0          0x56
#define LSM_REG_TAP_CFG1          0x57
#define LSM_REG_TAP_CFG2          0x58
#define LSM_REG_TAP_THS_6D        0x59
#define LSM_REG_TAP_DUR           0x5A
#define LSM_REG_WAKE_UP_THS       0x5B
#define LSM_REG_WAKE_UP_DUR       0x5C
#define LSM_REG_FREE_FALL         0x5D
#define LSM_REG_MD1_CFG           0x5E

// Embedded functions bank (visible when FUNC_CFG_ACCESS.EMB_FUNC_REG_ACCESS = 1)
#define LSM_REG_EMB_EMB_FUNC_EN_A 0x04
#define LSM_REG_EMB_EMB_FUNC_EN_B 0x05
#define LSM_REG_EMB_EMB_FUNC_INT1 0x0A
#define LSM_REG_EMB_STEP_COUNTER_L 0x62
#define LSM_REG_EMB_STEP_COUNTER_H 0x63
#define LSM_REG_EMB_EMB_FUNC_SRC  0x64
#define LSM_REG_EMB_EMB_FUNC_INIT_A 0x66
#define LSM_REG_EMB_MLC1_SRC      0x70

// Bit-position defines
#define FUNC_CFG_EMB_FUNC_REG_ACCESS  (1 << 7)
#define CTRL1_OP_MODE_HP          (0x00 << 4)
#define CTRL1_OP_MODE_LP1         (0x04 << 4)
#define CTRL1_ODR_120HZ           (0x06 << 0)
#define CTRL1_ODR_240HZ           (0x07 << 0)
#define CTRL2_ODR_240HZ_FS_2000DPS  ((0x07 << 4) | (0x07 << 1))
#define CTRL8_FS_XL_4G            (0x02 << 0)
#define CTRL3_SW_RESET            (1 << 0)
#define CTRL3_IF_INC              (1 << 2)
#define CTRL3_BDU                 (1 << 6)

#define WAKE_UP_SRC_SLEEP_CHANGE_IA (1 << 5)
#define WAKE_UP_SRC_FF_IA           (1 << 4)
#define WAKE_UP_SRC_SLEEP_STATE     (1 << 3)
#define WAKE_UP_SRC_WU_IA           (1 << 2)

#define TAP_SRC_TAP_IA            (1 << 6)
#define TAP_SRC_SINGLE_TAP        (1 << 5)
#define TAP_SRC_DOUBLE_TAP        (1 << 4)
#define TAP_SRC_TAP_SIGN          (1 << 3)
#define TAP_SRC_X_TAP             (1 << 2)
#define TAP_SRC_Y_TAP             (1 << 1)
#define TAP_SRC_Z_TAP             (1 << 0)

// FUNCTIONS_ENABLE (0x50):
//   [7]   INTERRUPTS_ENABLE -- REQUIRED for tap / wake-up / etc. to fire
//   [3:2] INACT_EN          -- activity/inactivity mode select
#define FUNC_ENABLE_INTERRUPTS_EN        (1 << 7)
#define FUNC_ENABLE_INACT_LP_GYRO_SLEEP  (0x02 << 2)

// INACTIVITY_DUR (0x54)
#define INACT_DUR_WEIGHT_62MG5     (0x03 << 4)
#define INACT_DUR_ODR_15HZ         (0x01 << 2)
#define INACT_DUR_WAKE_2EVENTS     (0x01 << 0)

// TAP_CFG0 (0x56)
#define TAP_CFG0_LOW_PASS_ON_6D   (1 << 6)
#define TAP_CFG0_HW_FUNC_MASK     (1 << 5)
#define TAP_CFG0_SLOPE_FDS        (1 << 4)
#define TAP_CFG0_TAP_X_EN         (1 << 3)
#define TAP_CFG0_TAP_Y_EN         (1 << 2)
#define TAP_CFG0_TAP_Z_EN         (1 << 1)
#define TAP_CFG0_LIR              (1 << 0)

// TAP_THS_6D (0x59)
#define TAP_THS_6D_D4D_EN         (1 << 7)

// WAKE_UP_THS (0x5B)
#define WAKE_UP_THS_SINGLE_DOUBLE_TAP  (1 << 7)

// Tap-tuning defaults (bench 2026-07-22 -- see tap.ino docstring for rationale)
#define TAP_THS_X_INIT            4     // 500 mg
#define TAP_THS_Y_INIT            3     // 375 mg
#define TAP_THS_Z_INIT            3     // 375 mg
#define TAP_DUR_INIT              ((5 << 4) | (0 << 2) | (0 << 0))
#define TAP_TUNING_ACCEL_ODR      CTRL1_ODR_240HZ

// BQ25619 ship mode registers
#define BQ_REG_MISC_OP        0x07
#define BQ_REG_STATUS         0x08
#define BQ_STATUS_PG          (1 << 2)
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)

// Button + LED timings
#define BTN_DEBOUNCE_MS      30
#define BTN_DOUBLE_GAP_MS    350
#define BTN_LONG_HOLD_MS     3000
#define WS_LEVEL             26
#define TAP_FLASH_MS         120
#define TAP_PEAK_DECAY       0.985f

// ═════════════════════════════════════════════════════════════════════════════
// View enum + style table
// ═════════════════════════════════════════════════════════════════════════════
typedef enum {
    VIEW_ACCEL_STREAM,
    VIEW_STEPS,
    VIEW_ACTIVITY,
    VIEW_TAP_VERBOSE,
    VIEW_TEMP,
    VIEW_GYRO,
    VIEW_LP_MODE,
    VIEW_MLC_PROBE,
    VIEW_BCG,
    VIEW_COUNT
} view_t;

typedef struct {
    const char *label;
    uint8_t     r, g, b;
} view_style_t;
static const view_style_t VIEW_STYLES[VIEW_COUNT] = {
    { "ACCEL_STREAM",  0,        0,        WS_LEVEL },     // blue
    { "STEPS",         0,        WS_LEVEL, WS_LEVEL },     // cyan
    { "ACTIVITY",      0,        WS_LEVEL, 0        },     // green
    { "TAP_VERBOSE",   WS_LEVEL, WS_LEVEL, 0        },     // yellow
    { "TEMP",          WS_LEVEL, WS_LEVEL / 3, 0    },     // amber
    { "GYRO",          WS_LEVEL, 0,        WS_LEVEL },     // magenta
    { "LP_MODE",       WS_LEVEL / 2, WS_LEVEL / 2, WS_LEVEL / 2 }, // white-ish
    { "MLC_PROBE",     WS_LEVEL, 0,        0        },     // red
    { "BCG",           WS_LEVEL, WS_LEVEL / 6, 0    },     // orange-red
};

// ═════════════════════════════════════════════════════════════════════════════
// Shared state globals -- referenced from every other .ino file.
// Kept here because Arduino concatenates .ino files after the main sketch;
// declaring shared globals in main ensures they're visible to everything.
// ═════════════════════════════════════════════════════════════════════════════
static view_t   g_view       = VIEW_ACCEL_STREAM;
static bool     lsm_ok       = false;
static bool     bq_ok        = false;
static uint32_t g_boot_ms    = 0;

// Pedometer 16-bit -> 32-bit rollover tracking
static uint16_t g_pedo_last16 = 0;
static uint32_t g_pedo_total  = 0;
static uint32_t g_pedo_last_report_ms = 0;
static uint32_t g_pedo_last_report_total = 0;

// Sleep-state edge detection
static bool     g_sleep_prev       = false;
static bool     g_sleep_prev_valid = false;

// LED tap-flash overlay end time (millis()-based)
static uint32_t g_tap_flash_until_ms = 0;

// LP mode flag (VIEW_LP_MODE toggles)
static bool     g_accel_lp = false;

// Tap-tuning state (owned by tap.ino, mutated by console.ino)
static uint8_t  g_tap_ths_x = TAP_THS_X_INIT;
static uint8_t  g_tap_ths_y = TAP_THS_Y_INIT;
static uint8_t  g_tap_ths_z = TAP_THS_Z_INIT;
static uint8_t  g_tap_dur   = 5;    // 667 ms window @ 240 Hz
static uint8_t  g_tap_quiet = 0;
static uint8_t  g_tap_shock = 0;
static bool     g_ints_en   = true;
static bool     g_tap_lir   = true;
static uint32_t g_tap_evt_single = 0;
static uint32_t g_tap_evt_double = 0;

// Orientation state (owned by views.ino)
static float    g_pitch_deg  = 0.0f;
static float    g_roll_deg   = 0.0f;
static bool     g_in_tap_zone = true;
static int8_t   g_pitch_min  = -80;
static int8_t   g_pitch_max  = +10;
static int8_t   g_roll_lim   =  45;

// Peak-force tracker (owned by views.ino, consumed by tap.ino)
static float    g_peak_ax = 0.0f, g_peak_ay = 0.0f, g_peak_az = 0.0f;

// Last-tap timing
static uint32_t g_last_tap_ms = 0;

// BCG state (owned by bcg.ino)
static float    g_bcg_hp_state = 0.0f;
static float    g_bcg_lp_state = 0.0f;
static float    g_bcg_last_val = 0.0f;
static float    g_bcg_env      = 0.0f;
static uint32_t g_bcg_last_beat_ms = 0;
static uint32_t g_bcg_beat_count   = 0;
static float    g_bcg_bpm      = 0.0f;
static uint16_t g_bcg_hp_pct   = 3;
static uint16_t g_bcg_lp_pct   = 28;
static uint16_t g_bcg_thr_pct  = 40;
static uint16_t g_bcg_refr_ms  = 400;

// Shared accel snapshot (updated by per_tick_accel in views.ino)
static float    g_ax = 0.0f, g_ay = 0.0f, g_az = 0.0f;
static bool     g_have_accel = false;

// Serial command buffer (owned by console.ino)
static char     g_cmd_buf[64];
static uint8_t  g_cmd_len = 0;

// ═════════════════════════════════════════════════════════════════════════════
// setup + loop
// ═════════════════════════════════════════════════════════════════════════════
void setup(void) {
    Serial.begin(115200);
    for (int i = 0; i < 20 && !Serial; i++) delay(50);

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    Wire.begin (PIN_SDA_BUS1, PIN_SCL_BUS1, 400000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 400000);

    g_boot_ms = millis();

    Serial.println();
    Serial.println("=== 12_lsm_full boot ===");
    Serial.println("controls:");
    Serial.println("  button single       -> view next");
    Serial.println("  button double       -> view prev  (backup for tap-double)");
    Serial.println("  button hold >=3 s   -> ship mode (BQ25619 BATFET off)");
    Serial.println("  LSM tap SINGLE      -> print event (no state change)");
    Serial.println("  LSM tap DOUBLE      -> view prev  ('go back'; except in TAP_VERBOSE)");
    Serial.println("Type 'help' + Enter in Serial Monitor for the tuning console.");
    Serial.println();

    if (i2c_ping(Wire, LSM6DSV_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WHO_AM_I);
        if (who == LSM_WHO_AM_I_VAL) {
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
        Serial.println("  BQ25619    0x6A : NO ACK (ship mode unavailable)");
    }

    if (!lsm_ok) {
        while (1) {
            neopixelWrite(PIN_WS2812_DIN, WS_LEVEL, 0, 0); delay(200);
            neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);        delay(200);
        }
    }

    if (!lsm_init_all()) {
        Serial.println("[LSM ] init verification FAILED -- proceeding but expect misbehavior");
    } else {
        Serial.println("[LSM ] init OK");
    }

    // Seed pedometer baseline so first STEPS report shows zero delta.
    {
        uint8_t b[2] = {0};
        if (emb_read_buf(LSM_REG_EMB_STEP_COUNTER_L, b, 2)) {
            g_pedo_last16 = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
        }
        g_pedo_total = 0;
        g_pedo_last_report_ms = millis();
        g_pedo_last_report_total = 0;
    }

    on_view_change(g_view, g_view);
    rgb_show_view();
    print_status_for_view();
}

void loop(void) {
    handle_serial();
    handle_button();
    per_tick_accel();   // one accel read powers orientation + peaks + BCG
    poll_tap();

    switch (g_view) {
        case VIEW_ACCEL_STREAM: tick_accel_stream(); break;
        case VIEW_STEPS:        tick_steps();        break;
        case VIEW_ACTIVITY:     tick_activity();     break;
        case VIEW_TAP_VERBOSE:  tick_tap_verbose();  break;
        case VIEW_TEMP:         tick_temp();         break;
        case VIEW_GYRO:         tick_gyro();         break;
        case VIEW_LP_MODE:      tick_lp_mode();      break;
        case VIEW_MLC_PROBE:    tick_mlc_probe();    break;
        case VIEW_BCG:          tick_bcg();          break;
        default: break;
    }

    rgb_show_view();
    delay(2);
}
