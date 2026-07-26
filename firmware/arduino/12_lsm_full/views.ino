/*
 * views.ino  --  View management + shared per-tick accel state + tick_* handlers.
 *
 * OWNS:
 *   - Wrist-orientation compute (pitch/roll from accel, tap-zone predicate)
 *   - Per-axis peak tracker (used by tap diagnostics)
 *   - per_tick_accel() -- the one accel read per loop that feeds orientation +
 *     peaks + BCG
 *   - view_next / view_prev / on_view_change (encoder + button + tap navigation)
 *   - All tick_* handlers EXCEPT tick_tap_verbose (in tap.ino) and tick_bcg
 *     (in bcg.ino)
 *
 * TUNEABLE (live via serial console):
 *   pmin D    pitch min in deg (default -80)
 *   pmax D    pitch max in deg (default +10)
 *   rlim D    roll +/- limit in deg (default 45)
 *
 * ORIENTATION MATH:
 *   Quasi-static tilt from gravity vector:
 *     pitch = atan2(-ax, sqrt(ay^2 + az^2))   (rotation about Y axis)
 *     roll  = atan2(ay, az)                   (rotation about X axis)
 *   When the watch is roughly horizontal in front of the user, pitch and
 *   roll fall inside the configured bounds -> g_in_tap_zone = true and
 *   tap events are accepted. Outside the bounds tap events are silently
 *   suppressed (with a diagnostic IGNORED line).
 *
 * PEAK TRACKER:
 *   Per-axis exponentially-decaying peak-hold. When a tap fires, the current
 *   peak values are printed so the operator can see which axis + how much
 *   force triggered it. Decay factor 0.985 -> ~275 ms decay at 240 Hz.
 */

// ── Orientation compute ───────────────────────────────────────────────────
void update_orientation_from_accel(void) {
    g_pitch_deg = atan2f(-g_ax, sqrtf(g_ay * g_ay + g_az * g_az)) * (180.0f / (float)M_PI);
    g_roll_deg  = atan2f( g_ay, g_az) * (180.0f / (float)M_PI);
    g_in_tap_zone = (g_pitch_deg >= (float)g_pitch_min) &&
                    (g_pitch_deg <= (float)g_pitch_max) &&
                    (g_roll_deg  >= -(float)g_roll_lim) &&
                    (g_roll_deg  <=  (float)g_roll_lim);
}

// ── Peak-force tracker ────────────────────────────────────────────────────
void update_peak_from_accel(void) {
    float absx = fabsf(g_ax), absy = fabsf(g_ay), absz = fabsf(g_az);
    if (absx > g_peak_ax) g_peak_ax = absx; else g_peak_ax *= TAP_PEAK_DECAY;
    if (absy > g_peak_ay) g_peak_ay = absy; else g_peak_ay *= TAP_PEAK_DECAY;
    if (absz > g_peak_az) g_peak_az = absz; else g_peak_az *= TAP_PEAK_DECAY;
}

// ── One accel read per loop, fan out to all trackers ──────────────────────
void per_tick_accel(void) {
    float ax, ay, az;
    if (!read_accel_g(&ax, &ay, &az)) return;
    g_ax = ax; g_ay = ay; g_az = az;
    g_have_accel = true;
    update_orientation_from_accel();
    update_peak_from_accel();
    update_bcg_from_accel();
}

// ── View cycling ──────────────────────────────────────────────────────────
void on_view_change(view_t old, view_t nu) {
    if (old == VIEW_LP_MODE && nu != VIEW_LP_MODE) {
        accel_set_lp(false);
        Serial.println("[LP  ] leaving LP_MODE -- accel back to HP");
    }
    if (nu == VIEW_LP_MODE) {
        accel_set_lp(true);
        Serial.println("[LP  ] entering LP_MODE -- accel LP1 @ 15 Hz (pedometer paused)");
    }
    Serial.println();
    Serial.printf("[VIEW] -> %s\n", VIEW_STYLES[nu].label);
    g_view = nu;
}
void view_next(void) {
    view_t nu = (view_t)((g_view + 1) % VIEW_COUNT);
    on_view_change(g_view, nu);
}
void view_prev(void) {
    view_t nu = (view_t)((g_view + VIEW_COUNT - 1) % VIEW_COUNT);
    on_view_change(g_view, nu);
}

// ── Sleep-state edge printer (used by tick_activity) ──────────────────────
static void poll_sleep_edge(bool verbose) {
    uint8_t src = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WAKE_UP_SRC);
    if (src == 0xFF) return;
    bool sleeping = (src & WAKE_UP_SRC_SLEEP_STATE) != 0;
    if (!g_sleep_prev_valid) {
        g_sleep_prev = sleeping;
        g_sleep_prev_valid = true;
        if (verbose) Serial.printf("[ACT ] initial state = %s\n", sleeping ? "SLEEP" : "ACTIVE");
        return;
    }
    if (sleeping != g_sleep_prev) {
        Serial.printf("[ACT ] transition -> %s  (src=0x%02X, t=%lus)\n",
                      sleeping ? "SLEEP" : "ACTIVE", src,
                      (unsigned long)((millis() - g_boot_ms) / 1000));
        g_sleep_prev = sleeping;
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Per-view periodic prints
// ═════════════════════════════════════════════════════════════════════════
void tick_accel_stream(void) {
    static uint32_t last = 0;
    if ((millis() - last) < 100) return;
    last = millis();
    Serial.printf("[ACC ] x=%+.3fg y=%+.3fg z=%+.3fg\n", g_ax, g_ay, g_az);
}
void tick_steps(void) {
    static uint32_t last = 0;
    if ((millis() - last) < 1000) return;
    last = millis();
    uint32_t total = pedo_read_total();
    uint32_t dt_ms = (last == g_pedo_last_report_ms) ? 1000
                                                     : (last - g_pedo_last_report_ms);
    uint32_t delta = total - g_pedo_last_report_total;
    Serial.printf("[STEP] total=%lu  +%lu since last (%lu ms)  raw16=%u  accel=%s\n",
                  (unsigned long)total, (unsigned long)delta, (unsigned long)dt_ms,
                  g_pedo_last16,
                  g_accel_lp ? "LP1 15 Hz (pedo PAUSED)" : "HP 240 Hz");
    g_pedo_last_report_ms = last;
    g_pedo_last_report_total = total;
}
void tick_activity(void) {
    static uint32_t last = 0;
    poll_sleep_edge(true);
    if ((millis() - last) < 2000) return;
    last = millis();
    uint8_t src   = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_WAKE_UP_SRC);
    uint8_t funcs = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_FUNCTIONS_ENABLE);
    Serial.printf("[ACT ] periodic: WAKE_UP_SRC=0x%02X (SLEEP_STATE=%d SLEEP_CHANGE=%d) "
                  "FUNCTIONS_ENABLE=0x%02X\n",
                  src,
                  (src & WAKE_UP_SRC_SLEEP_STATE)     ? 1 : 0,
                  (src & WAKE_UP_SRC_SLEEP_CHANGE_IA) ? 1 : 0,
                  funcs);
}
void tick_temp(void) {
    static uint32_t last = 0;
    if ((millis() - last) < 500) return;
    last = millis();
    float tc;
    if (!read_temp_c(&tc)) return;
    Serial.printf("[TEMP] on-chip = %.2f C\n", tc);
}
void tick_gyro(void) {
    static bool gyro_started = false;
    if (!gyro_started) {
        i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL2, CTRL2_ODR_240HZ_FS_2000DPS);
        gyro_started = true;
        Serial.println("[GYRO] enabling gyro @ 240 Hz +/-2000 dps");
        delay(5);
    }
    static uint32_t last = 0;
    if ((millis() - last) < 100) return;
    last = millis();
    float gx, gy, gz;
    if (!read_gyro_dps(&gx, &gy, &gz)) return;
    Serial.printf("[GYRO] x=%+.1f y=%+.1f z=%+.1f dps\n", gx, gy, gz);
}
void tick_lp_mode(void) {
    static uint32_t last = 0;
    if ((millis() - last) < 1000) return;
    last = millis();
    uint8_t ctrl1 = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1);
    Serial.printf("[LP  ] CTRL1=0x%02X (LP1 @ 15 Hz expected) accel z=%+.3fg\n",
                  ctrl1, g_az);
}
void tick_mlc_probe(void) {
    static uint32_t last = 0;
    if ((millis() - last) < 1000) return;
    last = millis();
    uint8_t mlc_status_mp = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_MLC_STATUS_MP);
    uint8_t emb_status_mp = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_EMB_FUNC_STATUS_MP);
    uint8_t mlc1_src      = emb_read(LSM_REG_EMB_MLC1_SRC);
    Serial.printf("[MLC ] MLC_STATUS_MP=0x%02X EMB_FUNC_STATUS_MP=0x%02X MLC1_SRC=0x%02X "
                  "(all zero = no classifier loaded -- plumbing OK)\n",
                  mlc_status_mp, emb_status_mp, mlc1_src);
}
