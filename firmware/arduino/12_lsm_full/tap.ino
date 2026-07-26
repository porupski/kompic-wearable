/*
 * tap.ino  --  LSM6DSV16X single/double tap detection with wrist gating
 *
 * ROLE:
 *   Own the tap-detection subsystem end-to-end: chip register configuration,
 *   TAP_SRC polling, orientation-gate suppression, and the rich diagnostic
 *   printout used to bench-tune thresholds.
 *
 * WHY ALL THREE AXES:
 *   On iv7.1 the chip Z-axis is tilted ~60 deg off vertical when the watch
 *   is on the wrist. A finger-tap on the case top projects mostly onto X
 *   (peaks 0.5-0.75 g in bench data), some onto Y (0.25-0.35 g), and only
 *   a little onto Z (0.15-0.20 g). Z-only detection missed nearly every
 *   deliberate tap. Enabling all three axes with per-axis thresholds
 *   catches a wrist tap regardless of watch orientation nuance.
 *
 * TUNEABLE PARAMETERS (live via serial console):
 *   x N       X threshold (0..31), 1 LSB = 125 mg at +/-4g FS
 *   y N       Y threshold (0..31)
 *   z N       Z threshold (0..31)
 *   dur N     DUR   bits [7:4] of TAP_DUR (0..15)   1 LSB = 32/ODR_XL
 *              Max time between two taps for double. Also = how long SINGLE
 *              takes to commit (the chip waits DUR for a possible 2nd tap).
 *   quiet N   QUIET bits [3:2] of TAP_DUR (0..3)    1 LSB = 4/ODR_XL
 *              Post-tap dead time; no other overthreshold allowed in it.
 *   shock N   SHOCK bits [1:0] of TAP_DUR (0..3)    1 LSB = 8/ODR_XL
 *              Max duration of the overthreshold signal itself.
 *   ie 0|1    FUNCTIONS_ENABLE.INTERRUPTS_ENABLE (bit 7)
 *              REQUIRED = 1 for TAP_SRC to populate at all. Datasheet §9.53.
 *   lir 0|1   TAP_CFG0.LIR (bit 0)
 *              1 = latched: TAP_SRC bits stay set until read. Required for
 *              polled operation -- with LIR=0, SINGLE_TAP pulses can be
 *              gone by the time we poll (poll interval ~20 ms, pulse can
 *              be shorter).
 *
 * ORIENTATION GATE:
 *   Tap events are suppressed when the watch is outside the "tappable
 *   position" bounds (pitch/roll from accel). Configured via pmin/pmax/rlim
 *   in orientation code. When out of zone, an IGNORED-line prints so the
 *   operator sees the event they made would have triggered.
 *
 * NAVIGATION BEHAVIOUR:
 *   Double-tap in any view EXCEPT VIEW_TAP_VERBOSE cycles the view backward
 *   (the "go back" gesture that will bind to LSM submenu exit in ESP-IDF).
 *   In VIEW_TAP_VERBOSE, taps are diagnostic-only: they print their forces
 *   and delta-t but do NOT cycle the view. Encoder / button crown is the
 *   only way to navigate while tuning.
 */

// ── Apply all tap-related registers from the g_tap_* globals ──────────────
// Called from lsm_init_all() and from the serial console after any change.
// No bank switching -- pure main-bank writes.
void apply_tap_config(void) {
    uint8_t cfg0 = TAP_CFG0_TAP_X_EN | TAP_CFG0_TAP_Y_EN | TAP_CFG0_TAP_Z_EN;
    if (g_tap_lir) cfg0 |= TAP_CFG0_LIR;
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_TAP_CFG0, cfg0);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_TAP_CFG1,  g_tap_ths_x & 0x1F);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_TAP_CFG2,  g_tap_ths_y & 0x1F);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_TAP_THS_6D, g_tap_ths_z & 0x1F);
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_TAP_DUR,
                  ((g_tap_dur   & 0x0F) << 4) |
                  ((g_tap_quiet & 0x03) << 2) |
                  ( g_tap_shock & 0x03));
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_WAKE_UP_THS, WAKE_UP_THS_SINGLE_DOUBLE_TAP);
    // FUNCTIONS_ENABLE holds both INTERRUPTS_ENABLE (bit 7, REQUIRED) and
    // the INACT_EN activity-sleep mode bits.
    uint8_t fe = (g_ints_en ? FUNC_ENABLE_INTERRUPTS_EN : 0)
               | FUNC_ENABLE_INACT_LP_GYRO_SLEEP;
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_FUNCTIONS_ENABLE, fe);
}

// ── Poll TAP_SRC and dispatch events ──────────────────────────────────────
void poll_tap(void) {
    static uint32_t last_poll = 0;
    uint32_t now = millis();
    if ((now - last_poll) < 20) return;
    last_poll = now;

    uint8_t src = i2c_read_reg(Wire, LSM6DSV_ADDR, LSM_REG_TAP_SRC);
    if (src == 0xFF || !(src & TAP_SRC_TAP_IA)) return;
    if (!(src & (TAP_SRC_X_TAP | TAP_SRC_Y_TAP | TAP_SRC_Z_TAP))) return;

    const char *axis = (src & TAP_SRC_X_TAP) ? "X"
                     : (src & TAP_SRC_Y_TAP) ? "Y"
                     :                          "Z";
    bool is_double = (src & TAP_SRC_DOUBLE_TAP) != 0;
    bool is_single = (src & TAP_SRC_SINGLE_TAP) != 0;
    if (!is_double && !is_single) return;   // pending classification

    // Orientation gate: log the ignored event so the operator sees what
    // would have triggered if bounds are wrong.
    uint32_t dt_ms = (g_last_tap_ms == 0) ? 0 : (now - g_last_tap_ms);
    if (!g_in_tap_zone) {
        Serial.printf("[TAP-] IGNORED (out of zone)  %s %-6s src=0x%02X  "
                      "pitch=%+.0f roll=%+.0f  dt=%lums\n",
                      axis, is_double ? "DOUBLE" : "SINGLE", src,
                      g_pitch_deg, g_roll_deg, (unsigned long)dt_ms);
        return;
    }

    g_tap_flash_until_ms = now + TAP_FLASH_MS;
    g_last_tap_ms = now;

    // Rich diagnostic print: axis + type + peak forces + orientation +
    // delta from previous tap + running counters. Used at the bench to
    // pick thresholds that catch deliberate taps and reject wrist swings.
    const char *type = is_double ? "DOUBLE" : "SINGLE";
    if (is_double) g_tap_evt_double++; else g_tap_evt_single++;

    Serial.printf("[TAP+] %s %-6s src=0x%02X sign=%s  "
                  "peaks x=%.2fg y=%.2fg z=%.2fg  "
                  "pitch=%+.0f roll=%+.0f  dt=%lums  cnt(s/d)=%lu/%lu",
                  axis, type, src,
                  (src & TAP_SRC_TAP_SIGN) ? "neg" : "pos",
                  g_peak_ax, g_peak_ay, g_peak_az,
                  g_pitch_deg, g_roll_deg,
                  (unsigned long)dt_ms,
                  (unsigned long)g_tap_evt_single,
                  (unsigned long)g_tap_evt_double);

    // In TAP_VERBOSE, taps are diagnostic-only. Only cycle in other views.
    if (is_double && g_view != VIEW_TAP_VERBOSE) {
        Serial.println("  -> view prev");
        view_prev();
    } else {
        Serial.println();
    }
}

// ── 2 Hz heartbeat in VIEW_TAP_VERBOSE -- orientation + peaks + counters ─
void tick_tap_verbose(void) {
    static uint32_t last = 0;
    if ((millis() - last) < 500) return;
    last = millis();
    Serial.printf("[TAP-H] %s  pitch=%+6.1f (%d..%d)  roll=%+6.1f (+/-%d)  "
                  "peaks x=%.2fg y=%.2fg z=%.2fg  s/d=%lu/%lu\n",
                  g_in_tap_zone ? "IN_ZONE " : "OUT_ZONE",
                  g_pitch_deg, g_pitch_min, g_pitch_max,
                  g_roll_deg,  g_roll_lim,
                  g_peak_ax, g_peak_ay, g_peak_az,
                  (unsigned long)g_tap_evt_single,
                  (unsigned long)g_tap_evt_double);
}

// ── Config dump for `?` ───────────────────────────────────────────────────
void print_tap_status(void) {
    Serial.printf("[TAP-CFG] x=%u (%u mg)  y=%u (%u mg)  z=%u (%u mg)  "
                  "dur=%u  quiet=%u  shock=%u  ie=%u  lir=%u\n",
                  g_tap_ths_x, g_tap_ths_x * 125,
                  g_tap_ths_y, g_tap_ths_y * 125,
                  g_tap_ths_z, g_tap_ths_z * 125,
                  g_tap_dur, g_tap_quiet, g_tap_shock,
                  g_ints_en ? 1 : 0, g_tap_lir ? 1 : 0);
    Serial.printf("[TAP-CNT] single=%lu  double=%lu\n",
                  (unsigned long)g_tap_evt_single, (unsigned long)g_tap_evt_double);
}
