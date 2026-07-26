/*
 * bcg.ino  --  Ballistocardiography (BCG) beat detection from accel Z-axis
 *
 * WHAT IS BCG?
 *   Ballistocardiography measures the tiny recoil forces produced by the heart
 *   as it ejects blood with each beat. A wrist-worn accel picks up a fraction
 *   of a milli-g of Z-axis motion at ~1-15 Hz per beat when the wrist is
 *   still. Not medical-grade -- diagnostic quality only. Best when the arm
 *   is resting on a surface and the user isn't moving.
 *
 * PIPELINE:
 *   accel_z --> IIR HPF (removes DC + slow drift like breathing / posture)
 *           --> IIR LPF (kills sharp noise / motion transients)
 *           --> adaptive-threshold peak detector with refractory period
 *
 * TUNEABLE PARAMETERS (all live-adjustable via serial console):
 *   bhp N     HP alpha percent   (default 3)
 *              Higher = FASTER high-pass, keeps more high frequencies.
 *              Effective corner: alpha=0.03 at ~240 Hz sample rate =>
 *              first-order HPF corner ~= alpha * fs / (2*pi) ~= 1.1 Hz.
 *
 *   blp N     LP alpha percent   (default 28)
 *              Lower = MORE smoothing, kills more noise.
 *              alpha=0.28 => LPF corner ~= 10.7 Hz. Together with the HPF
 *              gives a ~1-11 Hz bandpass (the BCG band).
 *
 *   bthr N    threshold percent of running envelope   (default 40)
 *              A candidate beat must exceed thr_pct * running_envelope.
 *              Auto-scales to signal strength so a strong signal isn't
 *              triggered by noise, and a weak signal still finds beats.
 *              Absolute floor of 3 mg prevents triggering at rest.
 *
 *   bref N    refractory ms between beats   (default 400)
 *              Minimum time between accepted beats. Caps BPM at
 *              60000/refr = 150 BPM at default. Prevents double-counting
 *              the ballistic recoil (the "double bump" a single beat can
 *              produce).
 *
 * BPM DISPLAY:
 *   If no beat has been detected for > BCG_STALE_MS (2000 ms), BPM prints
 *   as "---" instead of a stale number. Prevents the operator from
 *   thinking the sketch is showing a live rate when it isn't.
 */

#define BCG_STALE_MS   2000    // BPM shows "---" when older than this

// ── BCG update (called every loop tick from per_tick_accel) ────────────────
void update_bcg_from_accel(void) {
    // 1-pole IIR HP + LP on accel Z, then adaptive-threshold peak detect
    // with a refractory period. All params live-tunable via serial console.
    float alpha_hp = (float)g_bcg_hp_pct * 0.01f;
    float alpha_lp = (float)g_bcg_lp_pct * 0.01f;
    g_bcg_hp_state = g_bcg_hp_state * (1.0f - alpha_hp) + g_az * alpha_hp;
    float hp_out   = g_az - g_bcg_hp_state;         // high-passed
    g_bcg_lp_state = g_bcg_lp_state * (1.0f - alpha_lp) + hp_out * alpha_lp;
    float y        = g_bcg_lp_state;                // bandpassed

    // Envelope: peak-hold on |y| with slow decay.
    float y_abs = fabsf(y);
    if (y_abs > g_bcg_env) g_bcg_env = y_abs;
    else                   g_bcg_env *= 0.9995f;

    // Adaptive threshold + refractory beat detect.
    uint32_t now = millis();
    float thresh = g_bcg_env * ((float)g_bcg_thr_pct * 0.01f);
    if (thresh < 0.003f) thresh = 0.003f;   // absolute floor (~3 mg)
    if (y > thresh && g_bcg_last_val <= thresh &&
        (now - g_bcg_last_beat_ms) > g_bcg_refr_ms) {
        if (g_bcg_last_beat_ms > 0) {
            uint32_t interval = now - g_bcg_last_beat_ms;
            if (interval > 300 && interval < 2000) {
                g_bcg_bpm = 60000.0f / (float)interval;
            }
        }
        g_bcg_last_beat_ms = now;
        g_bcg_beat_count++;
    }
    g_bcg_last_val = y;
}

// ── Print a BPM cell, using "---" for stale. Formatter helper. ────────────
static void print_bpm_cell(void) {
    uint32_t age = (g_bcg_last_beat_ms == 0) ? 0xFFFFFFFFu
                                             : (millis() - g_bcg_last_beat_ms);
    if (age > BCG_STALE_MS || g_bcg_bpm <= 0.0f) {
        Serial.print("---");
    } else {
        Serial.printf("%.0f", g_bcg_bpm);
    }
}

// ── Periodic printout ─────────────────────────────────────────────────────
void tick_bcg(void) {
    static uint32_t last = 0;
    if ((millis() - last) < 250) return;
    last = millis();
    Serial.printf("[BCG ] hp=%u%% lp=%u%% thr=%u%% refr=%ums  "
                  "y=%+.4f env=%.4f  beats=%lu  bpm=",
                  g_bcg_hp_pct, g_bcg_lp_pct, g_bcg_thr_pct, g_bcg_refr_ms,
                  g_bcg_lp_state, g_bcg_env,
                  (unsigned long)g_bcg_beat_count);
    print_bpm_cell();
    Serial.println();
}

// ── Config dump for `?` in BCG view ───────────────────────────────────────
void print_bcg_status(void) {
    Serial.printf("[BCG-CFG] hp=%u%% lp=%u%% thr=%u%% refr=%ums  "
                  "beats=%lu bpm=",
                  g_bcg_hp_pct, g_bcg_lp_pct, g_bcg_thr_pct, g_bcg_refr_ms,
                  (unsigned long)g_bcg_beat_count);
    print_bpm_cell();
    Serial.println();
}
