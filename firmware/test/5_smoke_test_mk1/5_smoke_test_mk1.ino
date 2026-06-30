/**
 * 5_smoke_test_mk1.ino -- Kompic Mk I diagnostic smoke build
 *
 * Forked from 4_demo_mk1.ino (which works great on this board) to surgically
 * fix two open issues from the 2026-06-29 bench session:
 *
 *   1. Encoder is jittery -- each detent click produces ~3-7 transitions
 *      because the prior edge-ISR with 8 ms debounce treats every quadrature
 *      transition as a click. Replaced with a polling-based detent-rest
 *      state machine: one detent click = one event, regardless of how many
 *      bounces happen during the click cycle. Direction is latched from
 *      the FIRST line that goes LOW out of rest. ISR removed; polling at
 *      ~500 Hz is more than enough for human-driven mechanical encoders.
 *
 *   2. DRV2605L auto-cal trips OC_DETECT on this board (Stage 3 § 4.4 and
 *      the 4_demo notes). First 5_smoke pass confirmed that even in forced
 *      open-loop, every click triggered OC_DETECT (STATUS=0xE5) because the
 *      drive amplitude at OD_CLAMP=0x60 (~2.0 V peak across the LRA's
 *      10-13 Ω DCR ≈ 200 mA peak) sits right at the chip's OC trip floor.
 *
 *      This pass: OD_CLAMP dropped to 0x40 (~1.4 V peak ≈ 140 mA) which
 *      should stay comfortably under OC trip. Two paths to runtime, picked
 *      by DRV_ATTEMPT_CAL:
 *        - 0 (default): forced open-loop, no cal. Click amplitude is now
 *          OD_CLAMP-limited (gentler than before).
 *        - 1: run auto-cal at boot with the low OD_CLAMP. If cal passes,
 *          chip runs closed-loop with the CAL_COMP / CAL_BEMF values it
 *          measured -- expected stronger feel than open-loop @ low clamp.
 *          If cal fails, falls back to forced open-loop automatically.
 *
 *      A_CAL_COMP / A_CAL_BEMF are printed regardless of cal outcome so we
 *      can see whether the chip's BEMF sense is alive (reasonable values
 *      0x05-0x80) or rail-stuck (chip damage suspicion).
 *
 *      Live STATUS polling stays at 250 ms cadence.
 *
 *   3. New: multi-effect cycle. With DRV_USE_MULTI_EFFECTS = 1 the sketch
 *      cycles through drv_effects[] every DRV_EFFECT_CYCLE_MS (default 3 s)
 *      and prints which effect is now loaded. Both button single-click AND
 *      encoder detent fire whatever effect is currently active, so you can
 *      try every effect in the rotation by clicking / turning during its
 *      3 s window. Set DRV_USE_MULTI_EFFECTS = 0 to lock to one effect
 *      (DRV_EFFECT_DEFAULT) like the previous behavior.
 *
 * Other changes from 4_demo_mk1:
 *
 *   - LED brightness now has 12 detent levels (was 24). One full encoder
 *     revolution (12 detents) walks 0 -> 100 % of LED_MAX_DUTY, which is
 *     a natural mapping for the hand feel.
 *   - LED_MAX_DUTY raised from 80/255 (~31 %) to 96/255 (~38 %). That's
 *     +20 % power per the bench request. R26 is still absent on the board,
 *     so this peaks at ~22 mA into the LED -- inside the part's 30 mA abs
 *     max. Restock R26 before lifting any further.
 *   - 1 Hz heartbeat status print so the serial isn't silent between events.
 *
 * KEPT VERBATIM from 4_demo_mk1 (all confirmed working):
 *   - Boot-time I2C scan + per-device identity report.
 *   - All non-essential sensors parked in their lowest-power mode.
 *   - Single-click flashlight toggle.
 *   - Double-click ship-mode entry path (BATFET drop with BATFET_RST_WVBUS=1
 *     for the USB-plugged case, BATFET_RST_EN=0 to disarm the long-press
 *     reset path). Ship mode is MANDATORY on every sketch flashed to this
 *     prototype because the LiPo is permanently attached -- without this
 *     path Ivan cannot power the board off between bench sessions.
 *   - 2 s red RGB countdown before BATFET drop.
 *   - WS2812 hue roam at half power between events.
 *   - CPU dropped to 80 MHz at boot.
 *
 * Pins / addresses match Stage-3; see top of 3_smoke_test_mk1_hotair.ino
 * for the canonical map.
 */

#include <Wire.h>
#include <math.h>

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_FLASHLIGHT    41
#define PIN_BUTTON        16
#define PIN_SDA_BUS1       1
#define PIN_SCL_BUS1       2
#define PIN_SDA_BUS2       4
#define PIN_SCL_BUS2       5
#define PIN_WS2812_DIN    42
#define PIN_DRV_EN         0    // boot strap; FW drives HIGH to wake DRV
#define PIN_ENC_A         21
#define PIN_ENC_B         43    // U0TXD -- short boot-log blip is harmless

// ── I2C addresses ────────────────────────────────────────────────────────────
#define VEML6030_ADDR     0x10
#define LIS3MDL_ADDR      0x1C
#define PCF85063A_ADDR    0x51
#define LSM6DSV_ADDR      0x6B
#define BME688_ADDR       0x76
#define BQ25619_ADDR      0x6A
#define DRV2605_ADDR      0x5A

// ── BQ25619 registers ────────────────────────────────────────────────────────
#define BQ_REG_INPUT_SRC  0x00
#define BQ_REG_CTRL1      0x05
#define BQ_REG_MISC_OP    0x07
#define BQ_REG_STATUS     0x08
#define BQ_REG_FAULT      0x09
#define BQ_REG_PART       0x0A
#define BQ_TS_IGNORE_BIT  (1 << 6)
#define BQ_WD_MASK        (0x03 << 4)
#define BQ_BATFET_DIS         (1 << 5)
#define BQ_BATFET_RST_WVBUS   (1 << 4)
#define BQ_BATFET_DLY         (1 << 3)
#define BQ_BATFET_RST_EN      (1 << 2)
#define BQ_CHRG_STAT_MASK (0x03 << 3)
#define BQ_CHRG_NOT       (0x00 << 3)
#define BQ_CHRG_PRE       (0x01 << 3)
#define BQ_CHRG_FAST      (0x02 << 3)
#define BQ_CHRG_DONE      (0x03 << 3)
#define BQ_STATUS_PG      (1 << 2)
#define BQ_VBUS_STAT_MASK (0x07 << 5)

// ── PCF85063A / VEML / LIS / LSM / BME ───────────────────────────────────────
#define RTC_REG_CTRL1      0x00
#define RTC_REG_SECONDS    0x04
#define RTC_OS_BIT         0x80
#define VEML_REG_CONF      0x00
#define VEML_CONF_RUN      ((uint16_t)((0x03 << 11) | (0x00 << 6)))
#define VEML_CONF_SHUTDOWN ((uint16_t)0x0001)
#define LIS_REG_WHO        0x0F
#define LIS_WHO_VAL        0x3D
#define LIS_REG_CTRL3      0x22
#define LIS_PWR_DOWN       0x03
#define LSM_REG_WHO        0x0F
#define LSM_WHO_VAL        0x70
#define LSM_REG_CTRL1      0x10
#define LSM_REG_CTRL2      0x11
#define LSM_REG_CTRL3      0x12
#define BME_REG_CHIP_ID    0xD0
#define BME_REG_VARIANT_ID 0xF0
#define BME_REG_RESET      0xE0
#define BME_REG_CTRL_MEAS  0x74
#define BME_CHIP_ID_VAL    0x61

// ── DRV2605L ─────────────────────────────────────────────────────────────────
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
#define DRV_REG_CTRL3        0x1D
#define DRV_REG_CTRL4        0x1E

#define DRV_DEVICE_ID_VAL    0x07
#define DRV_MODE_INTERNAL    0x00
#define DRV_MODE_AUTO_CAL    0x07
#define DRV_LIB_LRA          0x06

// ELV1411A profile values (same as Stage-3 smoke / 4_demo) except OD_CLAMP
// dropped from 0x60 -> 0x40 to stay under the chip's OC trip floor on this
// board. 0x40 ≈ 1.4 V peak across the H-bridge -> 1.4 V / 10 Ω ≈ 140 mA
// peak through the LRA's nominal DCR. Chip OC trip is 200 mA min / 250 mA
// typ per datasheet so we have a comfortable margin. CTRL1.DRIVE_TIME
// encodes the half-period of 150 Hz so library LRA effects play at the
// LRA's resonant freq.
#define DRV_RATED_VOLTAGE      0x49
#define DRV_OD_CLAMP_REG       0x20
#define DRV_CTRL1_DRIVE_TIME   0x9C
#define DRV_FEEDBACK_LRA       0xB6
#define DRV_AUTO_CAL_TIME      0x30    // CTRL4: ~1000 ms auto-cal window

// CTRL3: default 0xA0 (NG_THRESH=10, ERM_OPEN_LOOP=1). Setting bit 0
// LRA_OPEN_LOOP=1 forces the LRA branch into open-loop drive too. With
// LRA_OPEN_LOOP=1 the chip ignores the cal compensation values (A_CAL_COMP /
// A_CAL_BEMF) and drives the motor with the bare amplitude profile from
// OD_CLAMP. No BEMF closed-loop fighting; impulsive effects don't slam the
// brake step at the end of the waveform.
#define DRV_CTRL3_OPEN_LOOP_LRA   0xA1
#define DRV_CTRL3_CLOSED_LOOP_LRA 0xA0    // default for closed-loop LRA

// DRV STATUS register bit masks (per DRV2605L datasheet Table 1).
#define DRV_STATUS_OC_DETECT   (1 << 0)
#define DRV_STATUS_OVER_TEMP   (1 << 1)
#define DRV_STATUS_DIAG_RESULT (1 << 3)
#define DRV_STATUS_DEVICE_ID_MASK   0xE0

// ── DRV effect selection ─────────────────────────────────────────────────────
// DRV_ATTEMPT_CAL: 0 = forced open-loop, no cal (recommended baseline).
//                  1 = run auto-cal at boot with the low OD_CLAMP. If cal
//                      passes, chip runs closed-loop. If cal fails, falls
//                      back to forced open-loop automatically.
#define DRV_ATTEMPT_CAL          0

// DRV_USE_MULTI_EFFECTS: 0 = always fire DRV_EFFECT_DEFAULT.
//                        1 = cycle through drv_effects[] every
//                            DRV_EFFECT_CYCLE_MS, announce in serial. Both
//                            button single-click AND encoder detent fire
//                            whatever effect is currently loaded.
#define DRV_USE_MULTI_EFFECTS    1
#define DRV_EFFECT_CYCLE_MS      3000
#define DRV_EFFECT_DEFAULT       16     // 1000 ms Alert 100% -- long and noticeable

// Rotation set. Mixture of clicks (short, impulsive) and buzzes / alerts
// (longer, sustained) so we can feel which class of effect the open-loop /
// closed-loop path handles best. Effects 14, 47, 52, 88 are continuous /
// long -- they play for a defined duration per the DRV library entry and
// can be interrupted by firing another effect on top.
static const uint8_t drv_effects[] = {
     1,    // Strong Click 100%
     4,    // Sharp Click 100%
     7,    // Soft Bump 100%
    10,    // Double Click 100%
    14,    // Strong Buzz 100% (continuous-class)
    15,    // 750 ms Alert 100%
    16,    // 1000 ms Alert 100%
    17,    // Strong Click 1
    27,    // Sharp Click 1
    47,    // Buzz 1 - 100% (continuous-class)
    88,    // Long Buzz for Programmatic Stopping
};
static const char *const drv_effect_names[] = {
    "Strong Click 100%",
    "Sharp Click 100%",
    "Soft Bump 100%",
    "Double Click 100%",
    "Strong Buzz 100% (cont)",
    "750 ms Alert 100%",
    "1000 ms Alert 100%",
    "Strong Click 1 - 100%",
    "Sharp Click 1 - 100%",
    "Buzz 1 - 100% (cont)",
    "Long Buzz for Programmatic Stopping",
};
#define DRV_EFFECTS_COUNT  (sizeof(drv_effects) / sizeof(drv_effects[0]))

// ── LED PWM ──────────────────────────────────────────────────────────────────
#define LED_FREQ_HZ          1000
#define LED_RES_BITS         8
// LED_MAX_DUTY raised from 80 (4_demo) to 96 -- ~+20 % per bench request.
// R26 (47 Ω flashlight series) still absent; with the remaining single 47 Ω
// in series and 96/255 duty, peak LED current ≈ (5 V - 3 V) / 47 Ω × 96/255
// ≈ 16 mA, average ≈ 13 mA. Inside the LED's 30 mA abs max. Restock R26
// before lifting LED_MAX_DUTY beyond 96.
#define LED_MAX_DUTY         96

// 12 detent levels: one full encoder revolution walks 0 -> 100 % of cap.
#define LED_LEVELS           12
#define LED_INIT_LEVEL        6

// ── WS2812 levels ────────────────────────────────────────────────────────────
#define WS_HALF_LEVEL        19      // ~7.5 % of 255; matches Stage 3 / 4_demo
#define WS_BLIP_LEVEL        26      // brighter for the ship-mode countdown

// ── Timing ───────────────────────────────────────────────────────────────────
#define ROAM_TICK_MS              120
#define ROAM_HUE_STEP             1
#define CHARGE_POLL_MS            1000
#define HEARTBEAT_MS              1000
#define DRV_STATUS_POLL_MS         250
#define BTN_DEBOUNCE_MS             30
#define BTN_DOUBLE_GAP_MS          350
#define ENC_DETENT_REST_MS           5    // dwell at rest before emitting click
#define DRV_CLICK_MIN_GAP_MS        40
#define BOOT_STROBE_RAMP_MS        350

// ── Probe state ──────────────────────────────────────────────────────────────
static bool bq_ok = false, drv_ok = false, rtc_ok = false, veml_ok = false,
            lis_ok = false, lsm_ok = false, bme_ok = false;
static uint8_t drv_device_id_raw = 0;
static uint8_t bme_chip_id = 0;

// ── Run-time state ───────────────────────────────────────────────────────────
static bool     led_on    = false;
static uint8_t  led_level = LED_INIT_LEVEL;
static uint8_t  roam_hue  = 0;

// Current DRV effect (the one fired by encoder click + button single-click).
// Initialized to DRV_EFFECT_DEFAULT; if DRV_USE_MULTI_EFFECTS=1, cycles
// through drv_effects[] starting at index 0 once the rotation kicks in.
static uint8_t  drv_cur_effect_idx = 0;
static uint8_t  drv_cur_effect     = DRV_EFFECT_DEFAULT;
static bool     drv_used_closed_loop = false;   // true if auto-cal passed

// Encoder state (polled, no ISR). Detent-rest state machine:
//   at_rest: both A and B HIGH continuously for ENC_DETENT_REST_MS.
//   in_motion: at least one line LOW. Direction latched from which line
//              went LOW first out of rest. Subsequent bounces during the
//              click don't change the latched direction.
// On return to rest (both HIGH stably for ENC_DETENT_REST_MS) we emit ONE
// click event with the latched direction. Then we wait for the next leave-
// rest transition.
typedef struct {
    bool     in_motion;
    int8_t   latched_dir;       // 0 = none, +1 = CW, -1 = CCW (assigned at leave-rest)
    uint32_t rest_since_ms;     // when both lines last became HIGH (0 if not rested yet)
} enc_state_t;
static enc_state_t enc = { false, 0, 0 };

// Brightness from the 0..LED_LEVELS encoder counter -> 0..LED_MAX_DUTY duty.
static uint8_t led_duty_from_level(uint8_t level) {
    if (level > LED_LEVELS) level = LED_LEVELS;
    return (uint8_t)((uint32_t)level * LED_MAX_DUTY / LED_LEVELS);
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
static bool i2c_write_reg(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t val) {
    bus.beginTransmission(addr);
    bus.write(reg);
    bus.write(val);
    return bus.endTransmission() == 0;
}
static bool veml_write_word(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(VEML6030_ADDR);
    Wire.write(reg);
    Wire.write(val & 0xFF);
    Wire.write(val >> 8);
    return Wire.endTransmission() == 0;
}

// ── LED helpers ──────────────────────────────────────────────────────────────
static void led_duty(uint8_t d) { ledcWrite(PIN_FLASHLIGHT, d); }
static void led_apply(void)     { led_duty(led_on ? led_duty_from_level(led_level) : 0); }

// ── HSV → RGB (256-step wheel, V = roam value) ───────────────────────────────
static void wheel(uint8_t hue, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (hue < 85) {
        *r = (uint8_t)((uint16_t)(85 - hue) * v / 85);
        *g = (uint8_t)((uint16_t) hue       * v / 85);
        *b = 0;
    } else if (hue < 170) {
        hue -= 85;
        *r = 0;
        *g = (uint8_t)((uint16_t)(85 - hue) * v / 85);
        *b = (uint8_t)((uint16_t) hue       * v / 85);
    } else {
        hue -= 170;
        *r = (uint8_t)((uint16_t) hue       * v / 85);
        *g = 0;
        *b = (uint8_t)((uint16_t)(85 - hue) * v / 85);
    }
}

// ── DRV trigger ──────────────────────────────────────────────────────────────
static bool drv_trigger(uint8_t effect_id) {
    if (!drv_ok) return false;
    if (!i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_WAVSEQ1, effect_id)) return false;
    if (!i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_WAVSEQ2, 0x00))      return false;
    return i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_GO, 0x01);
}

// ── Bus scan ─────────────────────────────────────────────────────────────────
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

// ── Charge color from BQ status ──────────────────────────────────────────────
static const char *charge_label(uint8_t st) {
    if (!(st & BQ_STATUS_PG)) return "no-input";
    switch (st & BQ_CHRG_STAT_MASK) {
        case BQ_CHRG_NOT:  return "plugged, not charging";
        case BQ_CHRG_PRE:  return "pre-charge (low)";
        case BQ_CHRG_FAST: return "fast charge (mid)";
        case BQ_CHRG_DONE: return "charge done (full)";
    }
    return "?";
}

// ── Encoder click handler (emits exactly one event per detent) ──────────────
static void on_encoder_click(int8_t dir) {
    static uint32_t last_drv_click_ms = 0;
    bool cw = (dir > 0);
    int new_level = (int)led_level + (cw ? +1 : -1);
    if (new_level < 0)               new_level = 0;
    if (new_level > (int)LED_LEVELS) new_level = LED_LEVELS;
    led_level = (uint8_t)new_level;

    Serial.printf("[ENC ] %s -> level %u/%u  duty %u/%u%s  [drv #%u]\n",
                  cw ? "CW " : "CCW",
                  (unsigned)led_level, (unsigned)LED_LEVELS,
                  (unsigned)led_duty_from_level(led_level),
                  (unsigned)LED_MAX_DUTY,
                  led_on ? " (live)" : "",
                  (unsigned)drv_cur_effect);

    if (led_on) led_apply();

    uint32_t now = millis();
    if (drv_ok && (now - last_drv_click_ms) >= DRV_CLICK_MIN_GAP_MS) {
        drv_trigger(drv_cur_effect);
        last_drv_click_ms = now;
    }
}

// ── DRV effect rotation ─────────────────────────────────────────────────────
// Cycles drv_cur_effect through drv_effects[] every DRV_EFFECT_CYCLE_MS.
// Does NOT fire the effect itself -- only updates which effect the button /
// encoder will trigger. Prints the rotation step to serial so the user can
// correlate "what they just felt" with "which effect was active." Setup
// has already announced drv_effects[0] as the initial loaded effect, so
// the first rotation step lands on drv_effects[1] after DRV_EFFECT_CYCLE_MS.
static void handle_drv_effect_cycle(void) {
#if DRV_USE_MULTI_EFFECTS
    static uint32_t next_cycle_ms = DRV_EFFECT_CYCLE_MS;
    if (!drv_ok) return;
    uint32_t now = millis();
    if ((int32_t)(now - next_cycle_ms) < 0) return;
    next_cycle_ms = now + DRV_EFFECT_CYCLE_MS;
    drv_cur_effect_idx = (drv_cur_effect_idx + 1) % DRV_EFFECTS_COUNT;
    drv_cur_effect = drv_effects[drv_cur_effect_idx];
    Serial.printf("[DRV ] effect now #%u  %s\n",
                  (unsigned)drv_cur_effect,
                  drv_effect_names[drv_cur_effect_idx]);
#endif
}

// ── Polled encoder state machine ────────────────────────────────────────────
// Runs once per loop() pass (~2 ms cadence). Mechanical bounce inside one
// detent cycle (10-20 ms typical) produces multiple LOW/HIGH transitions
// on each line. We DON'T count any of them. Instead, direction is sampled
// at the moment we leave rest (one line goes LOW while the other is still
// HIGH), and a single click event is emitted when both lines return HIGH
// and stay HIGH for ENC_DETENT_REST_MS.
static void handle_encoder(void) {
    uint8_t a = digitalRead(PIN_ENC_A);
    uint8_t b = digitalRead(PIN_ENC_B);
    bool at_rest = (a == HIGH) && (b == HIGH);
    uint32_t now = millis();

    if (at_rest) {
        if (enc.in_motion) {
            // Returning to rest. Need ENC_DETENT_REST_MS of continuous HIGH/HIGH
            // before we trust it's a real rest (vs. a momentary bounce-up).
            if (enc.rest_since_ms == 0) {
                enc.rest_since_ms = now;
            } else if ((now - enc.rest_since_ms) >= ENC_DETENT_REST_MS) {
                // Confirmed back at rest. Emit one click for the latched direction.
                if (enc.latched_dir != 0) {
                    on_encoder_click(enc.latched_dir);
                }
                enc.latched_dir = 0;
                enc.in_motion = false;
            }
        }
        // else: still at rest, nothing to do.
    } else {
        // Not at rest -- at least one line LOW.
        enc.rest_since_ms = 0;
        if (!enc.in_motion) {
            // Just left rest. Sample direction:
            //   A went LOW first (A=LOW, B=HIGH) -> CW   (assignment by convention;
            //                                              physical CW/CCW depends on
            //                                              wiring -- swap signs here
            //                                              if direction feels wrong).
            //   B went LOW first (A=HIGH, B=LOW) -> CCW
            //   Both LOW simultaneously: skip direction latch this cycle; next click
            //                            will catch it.
            if (a == LOW && b == HIGH) {
                enc.latched_dir = +1;
            } else if (a == HIGH && b == LOW) {
                enc.latched_dir = -1;
            } else {
                enc.latched_dir = 0;     // ambiguous; no click on this cycle
            }
            enc.in_motion = true;
        }
    }
}

// ── DRV STATUS poll (live OC / overtemp / diag visibility) ───────────────────
static void handle_drv_status(void) {
    static uint32_t next_poll_ms = 0;
    static uint8_t  last_status  = 0xFF;
    if (!drv_ok) return;
    uint32_t now = millis();
    if ((int32_t)(now - next_poll_ms) < 0) return;
    next_poll_ms = now + DRV_STATUS_POLL_MS;

    uint8_t st = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
    if (st == last_status) return;
    Serial.printf("[DRV ] STATUS=0x%02X", st);
    if (st & DRV_STATUS_OC_DETECT)   Serial.print(" OC_DETECT");
    if (st & DRV_STATUS_OVER_TEMP)   Serial.print(" OVER_TEMP");
    if (st & DRV_STATUS_DIAG_RESULT) Serial.print(" DIAG_RESULT");
    Serial.println();
    last_status = st;
}

// ── Heartbeat ───────────────────────────────────────────────────────────────
static void handle_heartbeat(void) {
    static uint32_t next_hb_ms = 0;
    uint32_t now = millis();
    if ((int32_t)(now - next_hb_ms) < 0) return;
    next_hb_ms = now + HEARTBEAT_MS;
    Serial.printf("[HB  ] up=%lus  led=%s lvl=%u/%u  duty=%u/%u\n",
                  (unsigned long)(now / 1000),
                  led_on ? "ON " : "OFF",
                  (unsigned)led_level, (unsigned)LED_LEVELS,
                  (unsigned)(led_on ? led_duty_from_level(led_level) : 0),
                  (unsigned)LED_MAX_DUTY);
}

// ── Action handlers ──────────────────────────────────────────────────────────
static void on_single_click(void) {
    led_on = !led_on;
    led_apply();
    uint8_t duty = led_on ? led_duty_from_level(led_level) : 0;
    Serial.printf("[BTN ] single -> flashlight %s (level=%u/%u, duty=%u/%u)  [drv #%u]\n",
                  led_on ? "ON" : "OFF",
                  (unsigned)led_level, (unsigned)LED_LEVELS,
                  (unsigned)duty, (unsigned)LED_MAX_DUTY,
                  (unsigned)drv_cur_effect);
    // Also fire the current DRV effect so the button is a second way to feel
    // each effect during a multi-effect rotation.
    if (drv_ok) drv_trigger(drv_cur_effect);
}

// 2 s red RGB countdown -- visible "shutting down" feedback and a hands-off
// window so post-double-click contact chatter settles before BATFET drops.
static void ship_mode_countdown(void) {
    Serial.println("       2 s red countdown -- hands off the button.");
    led_on = false;
    led_apply();
    const uint32_t total_ms = 2000;
    const uint32_t step_ms  = 40;
    const uint32_t steps    = total_ms / step_ms;
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
    if (!bq_ok) {
        Serial.println("       BQ not ok -- aborting (no ship mode)");
        return;
    }

    ship_mode_countdown();

    if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("       button LOW at end of countdown -- aborting ship mode");
        neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
        return;
    }

    // REG07 RMW for ship mode. See 4_demo notes / Stage 3 § 8.1 for the
    // full BATFET_RST_WVBUS / BATFET_RST_EN rationale.
    uint8_t r07 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP);
    uint8_t r07_new = r07;
    r07_new |=  BQ_BATFET_DIS;
    r07_new |=  BQ_BATFET_RST_WVBUS;
    r07_new &= ~BQ_BATFET_DLY;
    r07_new &= ~BQ_BATFET_RST_EN;
    Serial.printf("       writing REG07 0x%02X -> 0x%02X "
                  "(DIS=1, RST_WVBUS=1, DLY=0, RST_EN=0)\n",
                  r07, r07_new);
    Serial.flush();
    i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_MISC_OP, r07_new);
    delay(50);

    uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
    if (st & BQ_STATUS_PG) {
        Serial.println("       USB still present -- BATFET disabled; charger");
        Serial.println("       will fully enter ship mode the moment USB is unplugged.");
        neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
    } else {
        Serial.println("       BATFET off -- expecting power loss now.");
        Serial.flush();
        while (1) { delay(100); }
    }
}

static void on_double_click(void) {
    enter_ship_mode();
}

// ── Button state machine ─────────────────────────────────────────────────────
// Ship-mode fires on the RELEASE of the second press, not the press itself.
// See 4_demo / Stage 3 § 8.1 for the BQ /QON timing rationale.
typedef enum {
    BTN_IDLE,
    BTN_PRESSED,
    BTN_WAIT_DBL,
    BTN_PRESSED_2,
} btn_state_t;

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
            if (state == BTN_WAIT_DBL) {
                state = BTN_PRESSED_2;
            } else {
                state = BTN_PRESSED;
            }
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

// ── RGB roam ─────────────────────────────────────────────────────────────────
static void handle_rgb(void) {
    static uint32_t next_roam_ms     = 0;
    static uint32_t next_charge_poll = 0;
    static uint8_t  cached_status    = 0xFF;
    uint32_t now = millis();

    if (bq_ok && (int32_t)(now - next_charge_poll) >= 0) {
        next_charge_poll = now + CHARGE_POLL_MS;
        uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
        if (st != cached_status) {
            Serial.printf("[BQ  ] STATUS=0x%02X  %s\n", st, charge_label(st));
            cached_status = st;
        }
    }

    if ((int32_t)(now - next_roam_ms) >= 0) {
        next_roam_ms = now + ROAM_TICK_MS;
        roam_hue = (uint8_t)(roam_hue + ROAM_HUE_STEP);
        uint8_t r, g, b;
        wheel(roam_hue, WS_HALF_LEVEL, &r, &g, &b);
        neopixelWrite(PIN_WS2812_DIN, r, g, b);
    }
}

// ── Boot animation ───────────────────────────────────────────────────────────
static void boot_animation(void) {
    Serial.println("[DEMO] boot animation: flashlight strobe to 50 %, RGB roam on");
    const uint8_t peak = LED_MAX_DUTY / 2;
    const uint8_t steps = 24;
    for (uint8_t i = 0; i <= steps; i++) {
        uint8_t d = (uint8_t)((uint16_t)i * peak / steps);
        led_duty(d);
        delay(BOOT_STROBE_RAMP_MS / steps);
    }
    led_duty(0);

    uint8_t r, g, b;
    wheel(roam_hue, WS_HALF_LEVEL, &r, &g, &b);
    neopixelWrite(PIN_WS2812_DIN, r, g, b);
}

// ── setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    setCpuFrequencyMhz(80);

    delay(700);
    Serial.println("\n========================================");
    Serial.println("  Kompic Mk I -- 5_smoke (encoder + DRV diag)");
    Serial.printf ("  CPU = %u MHz (lowered from 240)\n",
                   (unsigned)getCpuFrequencyMhz());
    Serial.println("  Single click  : toggle flashlight");
    Serial.println("  Double click  : ship mode (BATFET off)");
    Serial.println("  Encoder turn  : brightness step + DRV click");
    Serial.printf ("  LED levels=%u (cap=%u/255), encoder=polled detent-rest\n",
                   (unsigned)LED_LEVELS, (unsigned)LED_MAX_DUTY);
    Serial.printf ("  DRV: %s, %s\n",
                   DRV_ATTEMPT_CAL ? "auto-cal at boot (closed-loop if pass)"
                                   : "forced open-loop, no cal",
                   DRV_USE_MULTI_EFFECTS ? "multi-effect rotation 3 s"
                                         : "single effect locked");
    Serial.println("========================================\n");

    ledcAttach(PIN_FLASHLIGHT, LED_FREQ_HZ, LED_RES_BITS);
    led_duty(0);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_ENC_A,  INPUT_PULLUP);
    pinMode(PIN_ENC_B,  INPUT_PULLUP);
    pinMode(PIN_DRV_EN, OUTPUT);
    digitalWrite(PIN_DRV_EN, HIGH);
    delay(5);
    Serial.println("[GPIO] flashlight LEDC (duty=0), button, encoder, DRV_EN HIGH");

    Wire.begin (PIN_SDA_BUS1, PIN_SCL_BUS1, 100000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 100000);
    delay(20);
    Serial.printf("[I2C ] Bus1 SDA=%d SCL=%d  Bus2 SDA=%d SCL=%d  @ 100 kHz\n",
                  PIN_SDA_BUS1, PIN_SCL_BUS1, PIN_SDA_BUS2, PIN_SCL_BUS2);

    Serial.println();
    scan_bus(Wire,  "Bus1");
    scan_bus(Wire1, "Bus2");
    Serial.println();

    // ── BQ25619 -- identify, silence watchdog, ignore TS ────────────────────
    Serial.printf("[BQ  ] Ping 0x%02X ... ", BQ25619_ADDR);
    if (i2c_ping(Wire1, BQ25619_ADDR)) {
        uint8_t part  = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_PART);
        uint8_t reg00 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_INPUT_SRC);
        uint8_t reg05 = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_CTRL1);
        i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_INPUT_SRC, reg00 | BQ_TS_IGNORE_BIT);
        i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_CTRL1,     reg05 & ~BQ_WD_MASK);
        delay(5);
        uint8_t st = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_STATUS);
        uint8_t fa = i2c_read_reg(Wire1, BQ25619_ADDR, BQ_REG_FAULT);
        bq_ok = true;
        Serial.printf("ACK  PART=0x%02X STATUS=0x%02X FAULT=0x%02X  %s\n",
                      part, st, fa, charge_label(st));
    } else {
        Serial.println("NO ACK -- ship mode + charge LED disabled");
    }

    // ── DRV2605L -- forced open-loop OR auto-cal, switched by DRV_ATTEMPT_CAL.
    //    OD_CLAMP is dropped to 0x40 (~140 mA peak into LRA's 10-13 Ω DCR),
    //    well under the chip's 200 mA min OC trip. With the lower amplitude
    //    auto-cal probe pulses should no longer trip OC, so cal might actually
    //    pass. If it does, we leave the chip in closed-loop -- expected to
    //    feel stronger than open-loop @ low clamp.
    Serial.printf("[DRV ] Ping 0x%02X ... ", DRV2605_ADDR);
    if (i2c_ping(Wire1, DRV2605_ADDR)) {
        uint8_t st = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
        drv_device_id_raw = st;
        uint8_t devid = (st >> 5) & 0x07;
        bool id_ok = (devid == DRV_DEVICE_ID_VAL);
        Serial.printf("%s  STATUS=0x%02X  DEVICE_ID=%u\n",
                      id_ok ? "ACK " : "BAD ID", st, devid);
        if (id_ok) {
            // Common pre-cal profile regardless of path.
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_FEEDBACK,   DRV_FEEDBACK_LRA);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_RATED_VOLT, DRV_RATED_VOLTAGE);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_OD_CLAMP,   DRV_OD_CLAMP_REG);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL4,      DRV_AUTO_CAL_TIME);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL1,      DRV_CTRL1_DRIVE_TIME);

            bool cal_ok = false;
#if DRV_ATTEMPT_CAL
            // ── Attempt auto-cal ────────────────────────────────────────────
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_MODE, DRV_MODE_AUTO_CAL);
            Serial.print("       auto-cal");
            uint32_t t0 = millis();
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_GO, 0x01);
            uint8_t go = 1;
            while (go && (millis() - t0) < 1500) {
                delay(50);
                Serial.print('.');
                go = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_GO) & 0x01;
            }
            uint32_t cal_ms = millis() - t0;
            uint8_t cal_status = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
            uint8_t cal_comp   = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_A_CAL_COMP);
            uint8_t cal_bemf   = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_A_CAL_BEMF);
            bool diag_pass = !(cal_status & DRV_STATUS_DIAG_RESULT);
            bool finished  = (go == 0);
            cal_ok = finished && diag_pass;
            Serial.printf(" %s (%u ms)  COMP=0x%02X  BEMF=0x%02X  STATUS=0x%02X\n",
                          cal_ok   ? "PASS" :
                          finished ? "FAIL (DIAG=1)" :
                                     "TIMEOUT",
                          (unsigned)cal_ms, cal_comp, cal_bemf, cal_status);
#else
            Serial.println("       cal skipped (DRV_ATTEMPT_CAL=0) -- forced open-loop");
            // Read existing cal values for visibility even though we ignore them.
            uint8_t cal_comp = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_A_CAL_COMP);
            uint8_t cal_bemf = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_A_CAL_BEMF);
            Serial.printf("       (stale cal values, unused: COMP=0x%02X BEMF=0x%02X)\n",
                          cal_comp, cal_bemf);
#endif

            // Switch to internal-trigger mode for runtime, library = LRA.
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_MODE,    DRV_MODE_INTERNAL);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_LIBRARY, DRV_LIB_LRA);

            if (cal_ok) {
                // Closed-loop runtime: leave CTRL3 default so cal values drive the loop.
                i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL3, DRV_CTRL3_CLOSED_LOOP_LRA);
                drv_used_closed_loop = true;
                Serial.println("       runtime: closed-loop LRA (cal values active)");
            } else {
                // Forced open-loop fallback (or default when cal not attempted).
                i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL3, DRV_CTRL3_OPEN_LOOP_LRA);
                drv_used_closed_loop = false;
                Serial.println("       runtime: forced open-loop LRA (CTRL3.LRA_OPEN_LOOP=1)");
            }

            uint8_t st_after = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
            Serial.printf("       config:  OD_CLAMP=0x%02X  RATED=0x%02X  "
                          "CTRL1=0x%02X  FEEDBACK=0x%02X  STATUS=0x%02X\n",
                          DRV_OD_CLAMP_REG, DRV_RATED_VOLTAGE, DRV_CTRL1_DRIVE_TIME,
                          DRV_FEEDBACK_LRA, st_after);
            drv_ok = true;
        }
    } else {
        Serial.println("NO ACK -- encoder clicks disabled");
    }

    // ── PCF85063A -- identify only, leave running (sub-uA) ───────────────────
    Serial.printf("[RTC ] Ping 0x%02X ... ", PCF85063A_ADDR);
    if (i2c_ping(Wire, PCF85063A_ADDR)) {
        uint8_t ctrl = i2c_read_reg(Wire, PCF85063A_ADDR, RTC_REG_CTRL1);
        uint8_t sec  = i2c_read_reg(Wire, PCF85063A_ADDR, RTC_REG_SECONDS);
        rtc_ok = true;
        Serial.printf("ACK  CTRL1=0x%02X SEC=0x%02X OS=%s (running, untouched)\n",
                      ctrl, sec, (sec & RTC_OS_BIT) ? "set" : "clr");
    } else {
        Serial.println("NO ACK");
    }

    Serial.printf("[VEML] Ping 0x%02X ... ", VEML6030_ADDR);
    if (i2c_ping(Wire, VEML6030_ADDR)) {
        veml_write_word(VEML_REG_CONF, VEML_CONF_SHUTDOWN);
        veml_ok = true;
        Serial.println("ACK  -> shutdown (ALS_SD=1)");
    } else {
        Serial.println("NO ACK");
    }

    Serial.printf("[LIS ] Ping 0x%02X ... ", LIS3MDL_ADDR);
    if (i2c_ping(Wire, LIS3MDL_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LIS3MDL_ADDR, LIS_REG_WHO);
        lis_ok = (who == LIS_WHO_VAL);
        i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL3, LIS_PWR_DOWN);
        Serial.printf("%s  WHO=0x%02X  -> power-down (CTRL3.MD=11)\n",
                      lis_ok ? "ACK " : "BAD ID", who);
    } else {
        Serial.println("NO ACK");
    }

    Serial.printf("[LSM ] Ping 0x%02X ... ", LSM6DSV_ADDR);
    if (i2c_ping(Wire, LSM6DSV_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WHO);
        lsm_ok = (who == LSM_WHO_VAL);
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x01);   // soft reset
        delay(20);
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, 0x00);
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, 0x00);
        Serial.printf("%s  WHO=0x%02X  -> XL/G ODR=0 (power-down)\n",
                      lsm_ok ? "ACK " : "BAD ID", who);
    } else {
        Serial.println("NO ACK");
    }

    Serial.printf("[BME ] Ping 0x%02X ... ", BME688_ADDR);
    if (i2c_ping(Wire, BME688_ADDR)) {
        bme_chip_id = i2c_read_reg(Wire, BME688_ADDR, BME_REG_CHIP_ID);
        bme_ok = (bme_chip_id == BME_CHIP_ID_VAL);
        i2c_write_reg(Wire, BME688_ADDR, BME_REG_RESET, 0xB6);
        delay(10);
        i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS, 0x00);
        Serial.printf("%s  CHIP_ID=0x%02X  -> sleep (CTRL_MEAS.mode=00)\n",
                      bme_ok ? "ACK " : "BAD ID", bme_chip_id);
    } else {
        Serial.println("NO ACK");
    }

    // ── Summary ──────────────────────────────────────────────────────────────
    Serial.println();
    Serial.println("---- 5_smoke init summary -----------------");
    Serial.printf("  BQ25619    : %s (ship mode %s)\n",
                  bq_ok  ? "ok"  : "fail", bq_ok ? "armed" : "disabled");
    Serial.printf("  DRV2605L   : %s (%s, %s)\n",
                  drv_ok ? "ok"  : "fail",
                  drv_used_closed_loop ? "closed-loop" : "open-loop",
                  drv_ok ? "encoder + button click active" : "triggers off");
    Serial.printf("  PCF85063A  : %s (left running)\n",   rtc_ok  ? "ok" : "fail");
    Serial.printf("  VEML6030   : %s (shutdown)\n",       veml_ok ? "ok" : "fail");
    Serial.printf("  LIS3MDLTR  : %s (power-down)\n",     lis_ok  ? "ok" : "fail");
    Serial.printf("  LSM6DSV16X : %s (power-down)\n",     lsm_ok  ? "ok" : "fail");
    Serial.printf("  BME688     : %s (sleep)\n",          bme_ok  ? "ok" : "fail");
    Serial.println("-------------------------------------------\n");

    // Initialize encoder state (no ISR -- pure polling).
    enc.in_motion = false;
    enc.latched_dir = 0;
    enc.rest_since_ms = 0;

    // Initialize current DRV effect based on rotation mode.
    if (DRV_USE_MULTI_EFFECTS) {
        drv_cur_effect_idx = 0;
        drv_cur_effect     = drv_effects[0];
    } else {
        drv_cur_effect_idx = 0;
        drv_cur_effect     = DRV_EFFECT_DEFAULT;
    }

    boot_animation();

    if (drv_ok) {
        if (DRV_USE_MULTI_EFFECTS) {
            Serial.printf("[DRV ] effect now #%u  %s  (rotation in 3 s)\n",
                          (unsigned)drv_cur_effect,
                          drv_effect_names[drv_cur_effect_idx]);
        } else {
            Serial.printf("[DRV ] effect locked at #%u  (DRV_EFFECT_DEFAULT)\n",
                          (unsigned)drv_cur_effect);
        }
    }

    Serial.println("[5SMOKE] ready -- single = flashlight + drv, double = ship mode\n");
}

// ── loop ─────────────────────────────────────────────────────────────────────
void loop() {
    handle_button();
    handle_encoder();
    handle_drv_status();
    handle_drv_effect_cycle();
    handle_rgb();
    handle_heartbeat();
    delay(2);
}
