/*
 * console.ino  --  Serial command console (Arduino Serial Monitor, Newline).
 *
 * Every tuning parameter in the sketch is settable at runtime via a one-line
 * text command. Type "help" for the full grammar, "?" for the current view's
 * config dump.
 *
 * NEW-COMMAND CHECKLIST:
 *   1. Add a global state variable in 12_lsm_full.ino
 *   2. Add a match arm in process_cmd()
 *   3. Add a help line in print_help()
 *   4. If the change requires re-writing a chip register, either call
 *      apply_tap_config() at the bottom (for tap params) or write directly
 *      in the match arm.
 *   5. Add the parameter to the appropriate print_*_status() function so
 *      "?" surfaces it.
 *
 * NO TUNEABLE PARAMETERS OF ITS OWN.
 */

// ── ODR label + setter (small helper used by the odr command) ─────────────
static const char *odr_bits_label(uint8_t bits) {
    if (bits == CTRL1_ODR_120HZ) return "120 Hz";
    if (bits == CTRL1_ODR_240HZ) return "240 Hz";
    return "?";
}
static void set_odr(uint8_t which) {
    uint8_t bits = (which == 0) ? CTRL1_ODR_120HZ : CTRL1_ODR_240HZ;
    i2c_write_reg(Wire, LSM6DSV_ADDR, LSM_REG_CTRL1, CTRL1_OP_MODE_HP | bits);
    Serial.printf("[ODR ] accel ODR set to %s (note: DUR/QUIET/SHOCK LSB scale with ODR)\n",
                  odr_bits_label(bits));
}

// ── Help printer ──────────────────────────────────────────────────────────
static void print_help(void) {
    Serial.println();
    Serial.println("=== SERIAL CONSOLE ===");
    Serial.println("-- tap thresholds --");
    Serial.println("  x N       X threshold (0..31)   1 LSB = 125 mg");
    Serial.println("  y N       Y threshold (0..31)");
    Serial.println("  z N       Z threshold (0..31)");
    Serial.println("  dur N     DUR   (0..15)         1 LSB = 32/ODR");
    Serial.println("  quiet N   QUIET (0..3)          1 LSB = 4/ODR");
    Serial.println("  shock N   SHOCK (0..3)          1 LSB = 8/ODR");
    Serial.println("  ie 0|1    INTERRUPTS_ENABLE  (must be 1 for tap to fire)");
    Serial.println("  lir 0|1   TAP_CFG0.LIR       (1 = latched -- required for polling)");
    Serial.println("  odr 0|1   accel ODR (0=120 Hz, 1=240 Hz)");
    Serial.println("-- orientation gate (tap events ignored when out of zone) --");
    Serial.println("  pmin D    pitch min in deg (default -80)");
    Serial.println("  pmax D    pitch max in deg (default +10)");
    Serial.println("  rlim D    roll +/- limit in deg (default 45)");
    Serial.println("-- BCG (VIEW_BCG heart-beat detector) --");
    Serial.println("  bhp N     BCG HP alpha percent    (default 3)");
    Serial.println("  blp N     BCG LP alpha percent    (default 28)");
    Serial.println("  bthr N    BCG threshold pct of env (default 40)");
    Serial.println("  bref N    BCG refractory ms       (default 400)");
    Serial.println("-- misc --");
    Serial.println("  r         reset event counters (tap + BCG beats)");
    Serial.println("  ?         show config for current view + orientation");
    Serial.println("  help      this help");
    Serial.println();
}

// ── Orientation printer (used by `?` in every view) ───────────────────────
static void print_orientation_status(void) {
    Serial.printf("[ORI ] pitch=%+6.1f (%d..%d)  roll=%+6.1f (+/-%d)  in_zone=%s\n",
                  g_pitch_deg, g_pitch_min, g_pitch_max,
                  g_roll_deg,  g_roll_lim,
                  g_in_tap_zone ? "YES" : "NO");
}

// ── View-aware `?` dispatch ───────────────────────────────────────────────
// print_tap_status is in tap.ino; print_bcg_status is in bcg.ino.
static void print_status_for_view(void) {
    Serial.printf("[VIEW] current=%s\n", VIEW_STYLES[g_view].label);
    print_orientation_status();
    if (g_view == VIEW_TAP_VERBOSE || g_view == VIEW_ACCEL_STREAM ||
        g_view == VIEW_ACTIVITY   || g_view == VIEW_STEPS) {
        print_tap_status();
    }
    if (g_view == VIEW_BCG) {
        print_bcg_status();
    }
}

// ── Command dispatch ──────────────────────────────────────────────────────
static void process_cmd(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0) return;

    char *sp = line;
    while (*sp && *sp != ' ' && *sp != '\t') { *sp = (char)tolower((unsigned char)*sp); sp++; }
    while (*sp == ' ' || *sp == '\t') { *sp = 0; sp++; }
    long arg = strtol(sp, NULL, 10);

    // No-hardware commands.
    if      (!strcmp(line, "?"))     { print_status_for_view(); return; }
    else if (!strcmp(line, "help"))  { print_help(); print_status_for_view(); return; }
    else if (!strcmp(line, "r"))     { g_tap_evt_single = 0; g_tap_evt_double = 0;
                                       g_bcg_beat_count = 0; g_bcg_bpm = 0.0f;
                                       g_bcg_last_beat_ms = 0;
                                       Serial.println("[CMD ] counters reset"); return; }
    // Orientation gate.
    else if (!strcmp(line, "pmin")) { g_pitch_min = (int8_t)arg; print_orientation_status(); return; }
    else if (!strcmp(line, "pmax")) { g_pitch_max = (int8_t)arg; print_orientation_status(); return; }
    else if (!strcmp(line, "rlim")) { g_roll_lim  = (int8_t)(arg < 0 ? -arg : arg);
                                       print_orientation_status(); return; }
    // BCG params.
    else if (!strcmp(line, "bhp"))  { g_bcg_hp_pct  = (uint16_t)arg; print_bcg_status(); return; }
    else if (!strcmp(line, "blp"))  { g_bcg_lp_pct  = (uint16_t)arg; print_bcg_status(); return; }
    else if (!strcmp(line, "bthr")) { g_bcg_thr_pct = (uint16_t)arg; print_bcg_status(); return; }
    else if (!strcmp(line, "bref")) { g_bcg_refr_ms = (uint16_t)arg; print_bcg_status(); return; }
    // Tap-register commands.
    else if (!strcmp(line, "x"))     { g_tap_ths_x = (uint8_t)(arg & 0x1F); }
    else if (!strcmp(line, "y"))     { g_tap_ths_y = (uint8_t)(arg & 0x1F); }
    else if (!strcmp(line, "z"))     { g_tap_ths_z = (uint8_t)(arg & 0x1F); }
    else if (!strcmp(line, "dur"))   { g_tap_dur   = (uint8_t)(arg & 0x0F); }
    else if (!strcmp(line, "quiet")) { g_tap_quiet = (uint8_t)(arg & 0x03); }
    else if (!strcmp(line, "shock")) { g_tap_shock = (uint8_t)(arg & 0x03); }
    else if (!strcmp(line, "ie"))    { g_ints_en   = (arg != 0); }
    else if (!strcmp(line, "lir"))   { g_tap_lir   = (arg != 0); }
    else if (!strcmp(line, "odr"))   { set_odr((uint8_t)arg); print_tap_status(); return; }
    else { Serial.printf("[CMD ] unknown: '%s' (type 'help')\n", line); return; }

    apply_tap_config();
    print_tap_status();
}

// ── Non-blocking Serial reader ────────────────────────────────────────────
void handle_serial(void) {
    while (Serial.available()) {
        int ch = Serial.read();
        if (ch < 0) break;
        if (ch == '\r') continue;
        if (ch == '\n') {
            g_cmd_buf[g_cmd_len] = 0;
            if (g_cmd_len > 0) process_cmd(g_cmd_buf);
            g_cmd_len = 0;
            continue;
        }
        if (g_cmd_len < (uint8_t)(sizeof(g_cmd_buf) - 1)) {
            g_cmd_buf[g_cmd_len++] = (char)ch;
        }
    }
}
