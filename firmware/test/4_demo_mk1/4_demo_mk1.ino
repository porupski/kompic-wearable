/**
 * 4_demo_mk1.ino -- Kompic Mk I demo / first-field-test build
 *
 * Built on the stage-3 hot-air smoke test (3_smoke_test_mk1_hotair.ino).
 * Strips MAX30101, TMP117 and the SD card; keeps everything else that was
 * confirmed working in stage 3, drops it into the lowest power state we can,
 * and exposes a small interactive UI suitable for taking the board out of
 * the lab on a 400 mAh cell.
 *
 * UI
 *   single click   -- toggle flashlight on/off  (brightness from encoder)
 *   double click   -- BQ25619 ship mode (BATFET disabled)
 *                     wake = press button (BQ QON) > 1 s
 *   encoder turn   -- step flashlight brightness +/- and fire one DRV click
 *
 *   RGB LED keeps roaming through hue at half power as long as the board
 *   is on. While USB is plugged, a short (~200 ms) charge-status blip
 *   replaces the roam every 5 s:
 *       red    = pre-charge      (CHRG_STAT = 01)
 *       yellow = fast charge     (CHRG_STAT = 10)
 *       green  = charge complete (CHRG_STAT = 11)
 *       blue   = USB present but not charging (PG=1, CHRG_STAT=00 -- fault?)
 *
 * Boot (and every wake from ship mode)
 *   1. CPU clock dropped to 80 MHz (default 240 MHz) -- biggest single
 *      idle-power saving. Saves roughly 30 mA vs the smoke test.
 *   2. I2C scan + per-device identity report (same coverage as stage 3
 *      minus MAX/TMP), printed once.
 *   3. Every non-essential sensor is parked in its lowest power mode:
 *        VEML6030    -> ALS_SD=1 (shutdown)
 *        LIS3MDLTR   -> CTRL3.MD = 11 (power-down)
 *        LSM6DSV16X  -> ODRs cleared (XL + G power-down)
 *        BME688      -> soft reset (returns to sleep mode by default)
 *        PCF85063A   -> left running, costs sub-uA
 *      MIC PDM and SDMMC are not initialized at all.
 *   4. DRV2605L is configured for LRA library + ELV1411A profile, but
 *      auto-cal is NOT re-run on every boot -- the chip falls back to
 *      open-loop drive if cal isn't fresh, which is fine for field use.
 *      Re-run the stage-3 smoke test if you want to recalibrate.
 *   5. Flashlight single strobe up to 50 % then off; RGB lights up
 *      half-power and starts roaming hue.
 *
 * Pins / addresses match stage-3; see top of 3_smoke_test_mk1_hotair.ino
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

// ── I2C addresses ─────────────────────────────────────────────────────────────
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

// ── PCF85063A / VEML / LIS / LSM / BME -- identity + low-power knobs ─────────
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
#define DRV_REG_FEEDBACK     0x1A
#define DRV_REG_CTRL1        0x1B
#define DRV_REG_CTRL4        0x1E
#define DRV_DEVICE_ID_VAL    0x07
#define DRV_MODE_INTERNAL    0x00
#define DRV_MODE_AUTO_CAL    0x07
#define DRV_LIB_LRA          0x06
#define DRV_REG_A_CAL_COMP   0x18
#define DRV_REG_A_CAL_BEMF   0x19

// ELV1411A profile (same values as stage-3 smoke; see notes there).
#define DRV_RATED_VOLTAGE      0x49
#define DRV_OD_CLAMP_REG       0x60
#define DRV_CTRL1_DRIVE_TIME   0x9C
#define DRV_FEEDBACK_LRA       0xB6
#define DRV_AUTO_CAL_TIME      0x30

// Effect 1 = Strong Click 100 %. Plays in ~10 ms, perfect for an encoder tick.
#define DRV_EFFECT_CLICK       1

// ── LED PWM ──────────────────────────────────────────────────────────────────
#define LED_FREQ_HZ          1000
#define LED_RES_BITS         8
// R26 (47 Ω) still missing from the bench board, but 80/255 (~31 % duty) is
// still inside safe-average territory for the white LED here. Brightness ramp
// runs in 24 detent steps from 0 to LED_MAX_DUTY for a smooth feel.
#define LED_MAX_DUTY         80
#define LED_LEVELS           24
#define LED_INIT_LEVEL       12

// ── WS2812 levels ────────────────────────────────────────────────────────────
#define WS_HALF_LEVEL        19      // ~7.5 % of 255; matches stage-3 loop
#define WS_BLIP_LEVEL        26      // brighter for the charge blip

// ── Timing ───────────────────────────────────────────────────────────────────
#define ROAM_TICK_MS              120    // 4x slower than the old 30 ms cadence
#define ROAM_HUE_STEP             1      // full hue cycle ~31 s -- slow, smooth
#define CHARGE_POLL_MS            1000   // serial-only charge state log
#define BTN_DEBOUNCE_MS           30
#define BTN_DOUBLE_GAP_MS         350
#define ENC_DEBOUNCE_MS           8
#define ENC_INVERT                0
#define DRV_CLICK_MIN_GAP_MS      40     // throttle on fast encoder spins
#define BOOT_STROBE_RAMP_MS       350

// ── Probe state ──────────────────────────────────────────────────────────────
static bool bq_ok = false, drv_ok = false, rtc_ok = false, veml_ok = false,
            lis_ok = false, lsm_ok = false, bme_ok = false;
static uint8_t drv_device_id_raw = 0;
static uint8_t bme_chip_id = 0;

// ── Run-time state ───────────────────────────────────────────────────────────
static bool     led_on         = false;
static uint8_t  led_level      = LED_INIT_LEVEL;
static uint8_t  roam_hue       = 0;
static volatile int32_t  enc_delta   = 0;
static volatile uint32_t enc_last_us = 0;

// Brightness from the 0..LED_LEVELS encoder counter -> 0..LED_MAX_DUTY duty.
// Smooth ramp: each detent advances by LED_MAX_DUTY/LED_LEVELS = 80/24 ≈ 3.3,
// so the LED never bursts to full on a single click.
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
// Plain ledcAttach in setup, ledcWrite for duty. Same pattern as
// 2_smoke_test_mk1_hotair.ino and 3_smoke_test_mk1_hotair.ino, both of which
// drove this LED off cleanly. The detach/digitalWrite "hard-off" pattern I
// tried earlier confused the GPIO matrix and left the LED stuck.
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

// ── Encoder ISR ──────────────────────────────────────────────────────────────
static void IRAM_ATTR enc_isr_a(void) {
    uint32_t now = micros();
    if ((now - enc_last_us) < (uint32_t)ENC_DEBOUNCE_MS * 1000u) return;
    enc_last_us = now;
    int dir = (digitalRead(PIN_ENC_B) == LOW) ? +1 : -1;
    if (ENC_INVERT) dir = -dir;
    enc_delta += dir;
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
// ── Action handlers ──────────────────────────────────────────────────────────
static void on_single_click(void) {
    led_on = !led_on;
    led_apply();
    uint8_t duty = led_on ? led_duty_from_level(led_level) : 0;
    Serial.printf("[BTN ] single -> flashlight %s (level=%u/%u, duty=%u/%u)\n",
                  led_on ? "ON" : "OFF",
                  (unsigned)led_level, (unsigned)LED_LEVELS,
                  (unsigned)duty, (unsigned)LED_MAX_DUTY);
}

// 2 s red RGB countdown -- gives the user visible "shutting down" feedback and
// a window to NOT touch the button. (User explicitly does not press during
// this window, but the long-ish delay also lets any post-double-click contact
// chatter settle long before we drop BATFET.) The flashlight is forced off
// first so we don't leave it draining into shutdown.
static void ship_mode_countdown(void) {
    Serial.println("       2 s red countdown -- hands off the button.");
    led_on = false;
    led_apply();
    const uint32_t total_ms = 2000;
    const uint32_t step_ms  = 40;
    const uint32_t steps    = total_ms / step_ms;
    for (uint32_t i = 0; i < steps; i++) {
        // Slow pulse: triangle wave 0 -> WS_BLIP_LEVEL -> 0 -> ...
        float phase = (float)i / 12.5f;             // ~4 pulses across 2 s
        float s = 0.5f * (1.0f - cosf(phase * (float)M_PI));   // 0..1
        uint8_t r = (uint8_t)(s * (float)WS_BLIP_LEVEL);
        neopixelWrite(PIN_WS2812_DIN, r, 0, 0);
        delay(step_ms);
    }
    neopixelWrite(PIN_WS2812_DIN, WS_BLIP_LEVEL, 0, 0);   // hold red at peak
}

static void enter_ship_mode(void) {
    Serial.println("[BTN ] DOUBLE -> ship mode requested");
    if (!bq_ok) {
        Serial.println("       BQ not ok -- aborting (no ship mode)");
        return;
    }

    ship_mode_countdown();

    // Final button-state check after the countdown. If the line is LOW now,
    // either the user is pressing or contact chatter is still going -- bail
    // either way; safer than dropping BATFET with QON LOW.
    if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("       button LOW at end of countdown -- aborting ship mode");
        neopixelWrite(PIN_WS2812_DIN, 0, 0, 0);
        return;
    }

    // RMW REG07 (Charger Control 3):
    //   bit 5  BATFET_DIS        = 1   -- request ship mode
    //   bit 4  BATFET_RST_WVBUS  = 1   -- required for ship mode while USB plugged
    //                                    (datasheet p. 25, "Adapter present" path)
    //   bit 3  BATFET_DLY        = 0   -- BATFET off immediately, no 10-15 s delay
    //   bit 2  BATFET_RST_EN     = 0   -- disable the 8-12 s QON-hold reset path
    //                                    so a too-long press can't power-cycle us
    // Note: on v7.1 hardware as shipped, R12 (5k1) pulls /QON up to 3V3.
    // When BATFET drops, 3V3 collapses and R12 becomes a hard pull-DOWN,
    // holding /QON LOW. The BQ then trips t_SHIPMODE (0.9-1.3 s) and wakes
    // back up. Fix: lift R12. The BQ's own internal 200 kohm pull-up to
    // V_BAT (datasheet pin 12) is the manufacturer-intended pull and is
    // sufficient on its own -- same topology as the TI reference design.
    // For v7.2: mark R12 DNP.
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
        Serial.println("       NOTE: if v7.1 board still has R12 fitted, expect a ~1 s");
        Serial.println("       zombie wake. Lift R12 to fix (BQ's internal 200k to VBAT");
        Serial.println("       is sufficient on its own, per BQ25619 datasheet pin 12).");
        Serial.flush();
        while (1) { delay(100); }
    }
}

static void on_double_click(void) {
    enter_ship_mode();
}

// ── Button state machine ─────────────────────────────────────────────────────
// CRITICAL: ship-mode action must fire on the RELEASE of the second press,
// not the press itself. The button line is shared with BQ25619 QON. If we
// write BATFET_DIS=1 while QON is still being held LOW, the BQ enters ship
// mode but then sees QON held for tBATFET_RST (~10 s) and performs a BATFET
// reset -- the system "zombies" back on after exactly that delay. Waiting
// for release lets the BQ see a clean QON-high before we disable BATFET.
typedef enum {
    BTN_IDLE,
    BTN_PRESSED,         // first press held
    BTN_WAIT_DBL,        // first release, watching for second press
    BTN_PRESSED_2,       // second press held -- action deferred to release
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
        if (low) {                       // press edge
            if (state == BTN_WAIT_DBL) {
                state = BTN_PRESSED_2;   // second press -- defer action to release
            } else {
                state = BTN_PRESSED;
            }
        } else {                         // release edge
            if (state == BTN_PRESSED) {
                release_ms = now;
                state = BTN_WAIT_DBL;
            } else if (state == BTN_PRESSED_2) {
                // Button is now released. Safe to enter ship mode -- BQ sees
                // QON-high so it won't latch a BATFET reset.
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

// ── Encoder handler ──────────────────────────────────────────────────────────
// Smooth brightness ramp: 24 detents from 0 to LED_MAX_DUTY. No preview
// flash when the flashlight is off -- the DRV click is the tactile confirm.
static void handle_encoder(void) {
    static uint32_t last_click_ms = 0;
    int32_t d = enc_delta;
    if (d == 0) return;
    enc_delta = 0;

    int32_t dd = d;
    while (dd != 0) {
        bool cw = (dd > 0);
        int new_level = (int)led_level + (cw ? +1 : -1);
        if (new_level < 0)               new_level = 0;
        if (new_level > (int)LED_LEVELS) new_level = LED_LEVELS;
        led_level = (uint8_t)new_level;
        Serial.printf("[ENC ] %s -> level %u/%u  duty %u/%u%s\n",
                      cw ? "CW " : "CCW",
                      (unsigned)led_level, (unsigned)LED_LEVELS,
                      (unsigned)led_duty_from_level(led_level),
                      (unsigned)LED_MAX_DUTY,
                      led_on ? " (live)" : "");
        if (dd > 0) dd--; else dd++;
    }

    if (led_on) led_apply();

    uint32_t now = millis();
    if (drv_ok && (now - last_click_ms) >= DRV_CLICK_MIN_GAP_MS) {
        drv_trigger(DRV_EFFECT_CLICK);
        last_click_ms = now;
    }
}

// ── RGB roam ─────────────────────────────────────────────────────────────────
// Continuous, smooth hue roam at half power. Charge state goes to serial only
// -- the periodic blip was visually jarring and obscured the "alive" signal
// of the steady gradient. Plug/unplug behaviour is still visible in the BQ
// status print at CHARGE_POLL_MS cadence.
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
    // Ramp flashlight from 0 to 50% of cap (32) then off.
    const uint8_t peak = LED_MAX_DUTY / 2;
    const uint8_t steps = 24;
    for (uint8_t i = 0; i <= steps; i++) {
        uint8_t d = (uint8_t)((uint16_t)i * peak / steps);
        led_duty(d);
        delay(BOOT_STROBE_RAMP_MS / steps);
    }
    led_duty(0);

    // Light the RGB to half power so it's visible right away. The roam loop
    // will take over once we hit loop().
    uint8_t r, g, b;
    wheel(roam_hue, WS_HALF_LEVEL, &r, &g, &b);
    neopixelWrite(PIN_WS2812_DIN, r, g, b);
}

// ── setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Drop CPU clock before touching anything else. Arduino-ESP32 reconfigures
    // peripheral dividers automatically; UART baud is preserved.
    setCpuFrequencyMhz(80);

    delay(700);
    Serial.println("\n========================================");
    Serial.println("  Kompic Mk I -- demo / field-test build");
    Serial.printf ("  CPU = %u MHz (lowered from 240)\n",
                   (unsigned)getCpuFrequencyMhz());
    Serial.println("  Single click  : toggle flashlight");
    Serial.println("  Double click  : ship mode (BATFET off)");
    Serial.println("  Encoder turn  : brightness step + DRV click");
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

    // ── I2C buses ────────────────────────────────────────────────────────────
    // Drop to 100 kHz: lower-frequency clock matches the slower CPU and
    // saves a touch on bus driver power. None of the chips need 400 kHz here.
    Wire.begin (PIN_SDA_BUS1, PIN_SCL_BUS1, 100000);
    Wire1.begin(PIN_SDA_BUS2, PIN_SCL_BUS2, 100000);
    delay(20);
    Serial.printf("[I2C ] Bus1 SDA=%d SCL=%d  Bus2 SDA=%d SCL=%d  @ 100 kHz\n",
                  PIN_SDA_BUS1, PIN_SCL_BUS1, PIN_SDA_BUS2, PIN_SCL_BUS2);

    Serial.println();
    scan_bus(Wire,  "Bus1");
    scan_bus(Wire1, "Bus2");
    Serial.println();

    // ── BQ25619 -- identify, silence watchdog, ignore TS (no thermistor) ─────
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

    // ── DRV2605L -- LRA + auto-cal. Open-loop drive (no cal) is too weak to
    //    feel on the ELV1411A LRA, so we run cal every boot and fall back to
    //    the same open-loop preset 3_smoke uses if cal fails (DIAG_RESULT=1).
    Serial.printf("[DRV ] Ping 0x%02X ... ", DRV2605_ADDR);
    if (i2c_ping(Wire1, DRV2605_ADDR)) {
        uint8_t st = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
        drv_device_id_raw = st;
        uint8_t devid = (st >> 5) & 0x07;
        bool id_ok = (devid == DRV_DEVICE_ID_VAL);
        Serial.printf("%s  STATUS=0x%02X  DEVICE_ID=%u\n",
                      id_ok ? "ACK " : "BAD ID", st, devid);
        if (id_ok) {
            // Pre-program the motor profile so auto-cal has correct starting
            // points (V_rated, OD_CLAMP, drive time = half-period of 150 Hz).
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_MODE,       DRV_MODE_AUTO_CAL);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_FEEDBACK,   DRV_FEEDBACK_LRA);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_RATED_VOLT, DRV_RATED_VOLTAGE);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_OD_CLAMP,   DRV_OD_CLAMP_REG);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL4,      DRV_AUTO_CAL_TIME);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_CTRL1,      DRV_CTRL1_DRIVE_TIME);

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
            uint8_t  cal_status = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_STATUS);
            uint8_t  cal_comp   = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_A_CAL_COMP);
            uint8_t  cal_bemf   = i2c_read_reg(Wire1, DRV2605_ADDR, DRV_REG_A_CAL_BEMF);
            bool diag_pass = !(cal_status & 0x08);
            bool finished  = (go == 0);
            bool cal_ok = finished && diag_pass;
            Serial.printf(" %s (%u ms)  COMP=0x%02X  BEMF=0x%02X  STATUS=0x%02X\n",
                          cal_ok   ? "PASS" :
                          finished ? "FAIL (DIAG=1, open-loop fallback @ 150 Hz)" :
                                     "TIMEOUT (open-loop fallback @ 150 Hz)",
                          (unsigned)cal_ms, cal_comp, cal_bemf, cal_status);
            // Whether cal passed or not, leave the chip in internal-trigger
            // mode on the LRA library. CTRL1 still encodes 150 Hz drive time
            // (DRIVE_TIME = 28 = 0x1C, half-period of 150 Hz), so library
            // effects play at the LRA's resonant frequency in open-loop.
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_MODE,    DRV_MODE_INTERNAL);
            i2c_write_reg(Wire1, DRV2605_ADDR, DRV_REG_LIBRARY, DRV_LIB_LRA);
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

    // ── VEML6030 -- identify, then SHUTDOWN ──────────────────────────────────
    Serial.printf("[VEML] Ping 0x%02X ... ", VEML6030_ADDR);
    if (i2c_ping(Wire, VEML6030_ADDR)) {
        veml_write_word(VEML_REG_CONF, VEML_CONF_SHUTDOWN);
        veml_ok = true;
        Serial.println("ACK  -> shutdown (ALS_SD=1)");
    } else {
        Serial.println("NO ACK");
    }

    // ── LIS3MDLTR -- identify, then POWER-DOWN ───────────────────────────────
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

    // ── LSM6DSV16X -- identify, then both ODRs to power-down ────────────────
    Serial.printf("[LSM ] Ping 0x%02X ... ", LSM6DSV_ADDR);
    if (i2c_ping(Wire, LSM6DSV_ADDR)) {
        uint8_t who = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WHO);
        lsm_ok = (who == LSM_WHO_VAL);
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, 0x01);   // soft reset
        delay(20);
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, 0x00);   // XL ODR = 0
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, 0x00);   // G  ODR = 0
        Serial.printf("%s  WHO=0x%02X  -> XL/G ODR=0 (power-down)\n",
                      lsm_ok ? "ACK " : "BAD ID", who);
    } else {
        Serial.println("NO ACK");
    }

    // ── BME688 -- identify, soft reset (returns to sleep mode by default) ───
    Serial.printf("[BME ] Ping 0x%02X ... ", BME688_ADDR);
    if (i2c_ping(Wire, BME688_ADDR)) {
        bme_chip_id = i2c_read_reg(Wire, BME688_ADDR, BME_REG_CHIP_ID);
        bme_ok = (bme_chip_id == BME_CHIP_ID_VAL);
        i2c_write_reg(Wire, BME688_ADDR, BME_REG_RESET, 0xB6);
        delay(10);
        // mode = 00 -> sleep (already the post-reset default; explicit for clarity)
        i2c_write_reg(Wire, BME688_ADDR, BME_REG_CTRL_MEAS, 0x00);
        Serial.printf("%s  CHIP_ID=0x%02X  -> sleep (CTRL_MEAS.mode=00)\n",
                      bme_ok ? "ACK " : "BAD ID", bme_chip_id);
    } else {
        Serial.println("NO ACK");
    }

    // ── Summary ──────────────────────────────────────────────────────────────
    Serial.println();
    Serial.println("---- Demo init summary --------------------");
    Serial.printf("  BQ25619    : %s (charge LED %s)\n",
                  bq_ok  ? "ok"  : "fail", bq_ok ? "active" : "off");
    Serial.printf("  DRV2605L   : %s (encoder click %s)\n",
                  drv_ok ? "ok"  : "fail", drv_ok ? "active" : "off");
    Serial.printf("  PCF85063A  : %s (left running)\n",   rtc_ok  ? "ok" : "fail");
    Serial.printf("  VEML6030   : %s (shutdown)\n",       veml_ok ? "ok" : "fail");
    Serial.printf("  LIS3MDLTR  : %s (power-down)\n",     lis_ok  ? "ok" : "fail");
    Serial.printf("  LSM6DSV16X : %s (power-down)\n",     lsm_ok  ? "ok" : "fail");
    Serial.printf("  BME688     : %s (sleep)\n",          bme_ok  ? "ok" : "fail");
    Serial.println("-------------------------------------------\n");

    // Attach encoder ISR AFTER prints (GPIO43 = U0TXD).
    enc_last_us = micros();
    attachInterrupt(PIN_ENC_A, enc_isr_a, RISING);

    // ── Boot animation ───────────────────────────────────────────────────────
    boot_animation();

    Serial.println("[DEMO] ready -- single click = flashlight, double = ship mode\n");
}

// ── loop ─────────────────────────────────────────────────────────────────────
void loop() {
    handle_button();
    handle_encoder();
    handle_rgb();
    // tight loop is fine; handle_rgb gates the WS2812 update by ROAM_TICK_MS
    delay(2);
}
