/*
 * hw.ino  --  Hardware primitives: I2C, bank switching, LSM6DSV16X init,
 *              WS2812 helpers, BQ25619 ship mode, physical button state machine.
 *
 * This file owns everything that talks directly to a chip register or a GPIO.
 * Higher-level modules (tap.ino, bcg.ino, views.ino) call these helpers but
 * never write registers themselves.
 *
 * NO TUNEABLE PARAMETERS: all constants here are hardware-defined (bus
 * addresses, register offsets, debounce timings tied to physical switches).
 */

// ═════════════════════════════════════════════════════════════════════════
// I2C primitives -- same shape used by every Kompic sketch since v6.
// ═════════════════════════════════════════════════════════════════════════
bool i2c_ping(TwoWire &bus, uint8_t addr) {
    bus.beginTransmission(addr);
    return bus.endTransmission() == 0;
}
uint8_t i2c_read_reg(TwoWire &bus, uint8_t addr, uint8_t reg) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return 0xFF;
    if (bus.requestFrom((int)addr, 1) != 1) return 0xFF;
    return bus.read();
}
bool i2c_write_reg(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t val) {
    bus.beginTransmission(addr);
    bus.write(reg);
    bus.write(val);
    return bus.endTransmission() == 0;
}
bool i2c_read_buf(TwoWire &bus, uint8_t addr, uint8_t reg, uint8_t *dst, size_t n) {
    bus.beginTransmission(addr);
    bus.write(reg);
    if (bus.endTransmission(false) != 0) return false;
    if (bus.requestFrom((int)addr, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; i++) dst[i] = bus.read();
    return true;
}

// ═════════════════════════════════════════════════════════════════════════
// Bank-switch helpers -- the #1 embedded-feature footgun. After enabling
// EMB_FUNC_REG_ACCESS you MUST switch back or subsequent CTRL* writes go
// to the embedded bank silently. Wrappers here enforce the pair.
// ═════════════════════════════════════════════════════════════════════════
static bool bank_enter_emb(void) {
    return i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_FUNC_CFG_ACCESS,
                         FUNC_CFG_EMB_FUNC_REG_ACCESS);
}
static bool bank_exit_emb(void) {
    return i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_FUNC_CFG_ACCESS, 0x00);
}
uint8_t emb_read(uint8_t reg) {
    if (!bank_enter_emb()) { bank_exit_emb(); return 0xFF; }
    uint8_t v = i2c_read_reg(Wire, LSM6DSV_ADDR, reg);
    bank_exit_emb();
    return v;
}
bool emb_write(uint8_t reg, uint8_t val) {
    if (!bank_enter_emb()) return false;
    bool ok = i2c_write_reg(Wire, LSM6DSV_ADDR, reg, val);
    ok = bank_exit_emb() && ok;
    return ok;
}
bool emb_read_buf(uint8_t reg, uint8_t *dst, size_t n) {
    if (!bank_enter_emb()) return false;
    bool ok = i2c_read_buf(Wire, LSM6DSV_ADDR, reg, dst, n);
    ok = bank_exit_emb() && ok;
    return ok;
}

// ═════════════════════════════════════════════════════════════════════════
// Data readers (accel/gyro/temp). Called from views + per_tick_accel.
// ═════════════════════════════════════════════════════════════════════════
bool read_accel_g(float *ax, float *ay, float *az) {
    uint8_t b[12] = {0};
    if (!i2c_read_buf(Wire, LSM6DSV_ADDR, LSM_REG_OUTX_L_G, b, 12)) return false;
    // Bytes 0-5 gyro, 6-11 accel (LE).
    int16_t xr = (int16_t)((uint16_t)b[6]  | ((uint16_t)b[7]  << 8));
    int16_t yr = (int16_t)((uint16_t)b[8]  | ((uint16_t)b[9]  << 8));
    int16_t zr = (int16_t)((uint16_t)b[10] | ((uint16_t)b[11] << 8));
    const float LSB_TO_G = 1.0f / 8192.0f;   // 0.122 mg/LSB at +/-4g FS
    *ax = xr * LSB_TO_G;
    *ay = yr * LSB_TO_G;
    *az = zr * LSB_TO_G;
    return true;
}
bool read_gyro_dps(float *gx, float *gy, float *gz) {
    uint8_t b[6] = {0};
    if (!i2c_read_buf(Wire, LSM6DSV_ADDR, LSM_REG_OUTX_L_G, b, 6)) return false;
    int16_t xr = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    int16_t yr = (int16_t)((uint16_t)b[2] | ((uint16_t)b[3] << 8));
    int16_t zr = (int16_t)((uint16_t)b[4] | ((uint16_t)b[5] << 8));
    const float LSB_TO_DPS = 1.0f / 14.286f;
    *gx = xr * LSB_TO_DPS;
    *gy = yr * LSB_TO_DPS;
    *gz = zr * LSB_TO_DPS;
    return true;
}
bool read_temp_c(float *tc) {
    uint8_t b[2] = {0};
    if (!i2c_read_buf(Wire, LSM6DSV_ADDR, LSM_REG_OUT_TEMP_L, b, 2)) return false;
    int16_t raw = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    *tc = 25.0f + (float)raw / 256.0f;
    return true;
}
uint32_t pedo_read_total(void) {
    uint8_t b[2] = {0};
    if (!emb_read_buf(LSM_REG_EMB_STEP_COUNTER_L, b, 2)) return g_pedo_total;
    uint16_t now16 = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    if (now16 < g_pedo_last16) {
        g_pedo_total += (uint32_t)((uint32_t)0x10000 + now16 - g_pedo_last16);
    } else {
        g_pedo_total += (uint32_t)(now16 - g_pedo_last16);
    }
    g_pedo_last16 = now16;
    return g_pedo_total;
}

// ═════════════════════════════════════════════════════════════════════════
// LSM init -- accel HP + pedometer + activity-sleep + tap-all-axes,
// all in one shot. Tap register writes delegated to apply_tap_config().
// ═════════════════════════════════════════════════════════════════════════
bool lsm_init_all(void) {
    // 1. Soft reset -- known-good starting state.
    if (!i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, CTRL3_SW_RESET)) return false;
    delay(20);
    // 2. BDU + IF_INC.
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL3, CTRL3_BDU | CTRL3_IF_INC);
    // 3. Accel: HP mode, 240 Hz, +/-4g. Pedometer needs >= 30 Hz; tap
    //    detector benefits from higher ODR (wider internal bandpass).
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL8,  CTRL8_FS_XL_4G);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1,  CTRL1_OP_MODE_HP | TAP_TUNING_ACCEL_ODR);
    // 4. Gyro off -- enabled per-view (VIEW_GYRO turns it on lazily).
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2,  0x00);
    delay(10);
    // 5. Pedometer -- embedded bank. EMB_FUNC_EN_A bit 3 = PEDO_EN.
    if (!emb_write(LSM_REG_EMB_EMB_FUNC_EN_A, (1 << 3))) return false;
    delay(5);
    // 6. Activity/inactivity duration + threshold.
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_INACTIVITY_DUR,
                  INACT_DUR_WEIGHT_62MG5 | INACT_DUR_ODR_15HZ | INACT_DUR_WAKE_2EVENTS);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_INACTIVITY_THS, 4);
    // 7. All tap registers + FUNCTIONS_ENABLE (INTERRUPTS_ENABLE + INACT_EN).
    apply_tap_config();
    delay(10);

    // 8. Verify a couple of critical readbacks.
    uint8_t ctrl1_rb  = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1);
    uint8_t bank_rb   = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_FUNC_CFG_ACCESS);
    uint8_t fe_rb     = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_FUNCTIONS_ENABLE);
    Serial.printf("[LSM ] verify: CTRL1=0x%02X (want 0x%02X)  "
                  "FUNCTIONS_ENABLE=0x%02X (bit7 must be 1)  "
                  "bank=0x%02X (want 0x00)\n",
                  ctrl1_rb, (CTRL1_OP_MODE_HP | TAP_TUNING_ACCEL_ODR),
                  fe_rb, bank_rb);
    return (ctrl1_rb == (CTRL1_OP_MODE_HP | TAP_TUNING_ACCEL_ODR)) &&
           (bank_rb  == 0x00) &&
           ((fe_rb & FUNC_ENABLE_INTERRUPTS_EN) != 0);
}

// ═════════════════════════════════════════════════════════════════════════
// LP-mode toggle for VIEW_LP_MODE (drops accel to LP1 @ 15 Hz for the
// current-draw demo, restores HP @ 240 Hz on exit).
// ═════════════════════════════════════════════════════════════════════════
void accel_set_lp(bool lp) {
    if (lp) {
        // LP mode 1 @ 15 Hz. Pedometer stops (needs >= 30 Hz).
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1,
                      CTRL1_OP_MODE_LP1 | 0x03 /* ODR = 15 Hz */);
    } else {
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1,
                      CTRL1_OP_MODE_HP  | TAP_TUNING_ACCEL_ODR);
    }
    g_accel_lp = lp;
    delay(5);
}

// ═════════════════════════════════════════════════════════════════════════
// WS2812 helpers
// ═════════════════════════════════════════════════════════════════════════
void rgb_off(void) { neopixelWrite(PIN_WS2812_DIN, 0, 0, 0); }
void rgb_show_view(void) {
    if (millis() < g_tap_flash_until_ms) {
        neopixelWrite(PIN_WS2812_DIN, WS_LEVEL, WS_LEVEL, WS_LEVEL);   // white flash
        return;
    }
    // In TAP_VERBOSE: red overlay when watch is outside the tappable zone.
    if (g_view == VIEW_TAP_VERBOSE && g_have_accel && !g_in_tap_zone) {
        neopixelWrite(PIN_WS2812_DIN, WS_LEVEL, 0, 0);
        return;
    }
    const view_style_t &s = VIEW_STYLES[g_view];
    neopixelWrite(PIN_WS2812_DIN, s.r, s.g, s.b);
}

// ═════════════════════════════════════════════════════════════════════════
// BQ25619 ship mode (verbatim from sketch 9). Long-hold triggers, release
// during countdown confirms, keep holding = abort.
// ═════════════════════════════════════════════════════════════════════════
static void ship_mode_countdown(void) {
    Serial.println("[BTN ] LONG-HOLD -> ship mode (2 s red countdown, release to confirm)");
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
void enter_ship_mode(void) {
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

// ═════════════════════════════════════════════════════════════════════════
// Button state machine
//   single         -> view next
//   double         -> view prev (backup for tap-double)
//   hold >= 3 s    -> ship mode
// ═════════════════════════════════════════════════════════════════════════
static void on_button_single(void) {
    Serial.println("[BTN ] single -> view next");
    view_next();
}
static void on_button_double(void) {
    Serial.println("[BTN ] double -> view prev (tap backup)");
    view_prev();
}
typedef enum {
    BTN_IDLE,
    BTN_PRESSED,
    BTN_LONG_FIRED,
    BTN_WAIT_DBL,
    BTN_PRESSED_2,
} btn_state_t;
void handle_button(void) {
    static btn_state_t state       = BTN_IDLE;
    static uint32_t    last_change = 0;
    static uint32_t    press_ms    = 0;
    static uint32_t    release_ms  = 0;
    static bool        prev_low    = false;
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
                press_ms = now;
            }
        } else {
            if (state == BTN_PRESSED) {
                release_ms = now;
                state = BTN_WAIT_DBL;
            } else if (state == BTN_PRESSED_2) {
                state = BTN_IDLE;
                on_button_double();
            } else if (state == BTN_LONG_FIRED) {
                state = BTN_IDLE;
            }
        }
    }
    if (state == BTN_PRESSED && (now - press_ms) >= BTN_LONG_HOLD_MS) {
        state = BTN_LONG_FIRED;
        enter_ship_mode();
    }
    if (state == BTN_WAIT_DBL && (now - release_ms) > BTN_DOUBLE_GAP_MS) {
        state = BTN_IDLE;
        on_button_single();
    }
}
