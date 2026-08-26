# Stage 13 — Advanced Heart-Rate Monitor (PPG + BCG twin pipeline)

**Date:** 2026-08-09
**Board:** iv7.1 (unchanged since sketches 7–12; same bus mapping, same button)
**Firmware baseline:** f0.0
**Status:** WORKING-AT-REST, refinement deferred to ECG cross-check (see §7)

Stage 12 (field-capture refactor) is not fully wrapped — flashes clean but has
bug residue. Deliberately left pinned; Stage 13 opens in parallel because HR
work doesn't touch the field-capture code paths.

---

## 1. Goal

Turn the MAX30101 into a *usable* wrist heart-rate reading, driven by the
insight from Steve Mould's *"bizarre flashing lights on a smartwatch"* video
(`docs/build_info/reference_files/asr/transcripts/`) and the existing
`Kompic_Mk1_HR_Sensing_Handoff.md` squeeze plan. Fuse it with the LSM6DSV16X
BCG (already known to work at rest, sketch 12 `VIEW_BCG`) so we get a free
second HR estimate that's independent of the PPG optical channel.

The unit of work: **standalone Arduino sketch first**
(`13_adv_heartrate_monitor.ino` — per the freeze-and-forget POC pattern), then
port validated pipeline into ESP-IDF as a follow-up stage.

---

## 2. Inputs from sketch 10 bench session (2026-08-09)

Direct on-bench observations from Ivan cycling `10_test_max30101` through the
5-level LED_PA ladder, once on finger and once on wrist:

### 2.1 DC baselines (still wrist, green LED, MAX green channel)

| LED_PA | ~current | Baseline DC counts |
|--------|----------|--------------------|
| 0x0F   | ~4 mA    | 900                |
| 0x1F   | ~7 mA    | 1 400              |
| 0x3F   | ~15 mA   | 3 000              |
| 0x7F   | ~30 mA   | 6 000              |
| 0xFF   | ~50 mA   | 9 000              |

18-bit ADC ceiling is ~262 000 → **DC clip is NOT the ceiling on wrist**.
The ceiling here is *noise floor* — need more absolute AC amplitude above
digitization noise, which means more LED current is genuinely helpful up to
the point where safety / thermal / battery drain becomes the constraint.

### 2.2 Signal quality qualitative reads

- **Finger, dim green (0x0F):** beautiful clear peaks/troughs (Serial Plotter
  AC trace). Higher currents saturate / clip because finger perfusion is
  ~50× wrist → DC pedestal grows fast.
- **Finger, "red" mode (0xFF):** massive 10× AC swings at very light pressure —
  overkill but visible.
- **Wrist, "red" mode (0xFF):** the only setting with a visible PPG signal on
  the AC trace. Comparable to finger-green at strap-tight, degrading toward
  BCG-level at strap-loose.
- **Wrist, "orange" (0x7F):** BCG-level — maybe detectable, maybe just
  imagination.
- **Wrist, everything ≤ 0x3F:** below noise floor / indistinguishable.

Naming clarification: "red mode" / "orange mode" refer to the WS2812 RGB
indicator color at that button-cycled LED level, NOT the physical Red LED.
The sketch is green-LED-only throughout.

### 2.3 Motion detector idea (from Ivan's plot observation)

While still, the AC signal oscillates around the slow-EMA baseline: the two
plot traces "meet tangentally at peaks/troughs" — small |raw − baseline|,
smooth crossings. When the wrist moves, `raw` jumps by amounts much larger
than the pulsatile AC and the slow baseline can't catch up: the traces cross
"at an angle." Formalized in the pipeline as a **d(AC)/dt vs. running-envelope
threshold** — free motion artifact detector, no extra hardware.

### 2.4 Suggested policy

- **Active / daytime:** high current (0xFF-ish) to break the wrist noise floor
- **Rest / nighttime:** lower current (0x7F-ish) to save power once the wrist
  perfusion state means less light is enough

The sweep in sketch 13 exists to quantify this per-user (my wrist ≠ your wrist,
skin tone / hair / strap fit all shift the curve).

---

## 3. Deliverable this stage

`firmware/arduino/13_adv_heartrate_monitor/13_adv_heartrate_monitor.ino`

### 3.1 Session flow
- Idle: RGB slow cyan breathe, waits for button.
- Single-click: 3 s pink flashing countdown ("get still").
- Sweep: LED_PA ramps linearly from `LED_PA_MIN=0x0F` to `LED_PA_MAX=0xFF`
  over `SWEEP_DURATION_S=60` seconds in `SWEEP_STEPS=50` steps. Set steps=5
  to reproduce the discrete 5-level ladder from sketch 10. Signal-chain
  state resets ONCE at the START of the session, not between steps —
  rolling window so gradual amplitude hills are visible.
- End: winner LED_PA by composite MAX quality, RGB latches to winner color,
  JSON summary written, back to idle.
- Long-hold ≥ 3 s: BQ25619 ship mode (unchanged).

### 3.2 Dual-channel signal pipeline (identical shape for MAX-green + LSM-BCG)
1. Baseline EMA (DC follower; fast 0.10 for first 3 s, slow 0.02 after)
2. Bandpass IIR (2nd-order Butterworth biquad)
   - MAX: 0.5–4 Hz (cardiac fundamental)
   - LSM: 0.8–15 Hz (BCG fundamental + harmonics)
3. Motion flag: `|Δac/Δt| > k × running envelope` → 500 ms hang time
4. Adaptive-threshold peak detect: `thr = k × leaky-max(|bp|)`, refractory 250 ms
5. Autocorrelation cross-check: every 2 s over last 5 s of bandpassed signal
6. Composite quality = geometric mean of `(PI_norm × IBI_reg × motion_clean ×
   autocorr_agr)` scaled 0..100

### 3.3 BCG source
`sqrt(x² + y² + z²)` accel magnitude (orientation-robust) — upgraded from the
z-only signal used in sketch 12's `VIEW_BCG`. Fed through the same pipeline
as MAX; the two channels are "twins."

### 3.4 Files (SD card)
```
/data/sk13_data/sk13_NNNN_YYYYMMDD_HHMMSS.csv
/data/sk13_data/sk13_NNNN_YYYYMMDD_HHMMSS.json
```
NNNN = monotonic session index (1 + max existing on card, scanned at boot) so
filenames sort by run order across reboots. Timestamp from PCF85063A (bus1
@ 0x51). RTC OS ("oscillator stopped") flag is ignored — sanity-check on year
gates whether the read is trusted; the OS flag latches on any power blip and
practically never clears, matching what the ESP-IDF firmware does.

CSV is interleaved single file — one row per sample, `src` column = `M` (MAX)
or `L` (LSM). Columns: `t_ms, src, led_pa, raw, baseline, ac, bp, motion,
beat, bpm_pk, bpm_ac, quality`. Loads cleanly into pandas with
`df.groupby('src')` after read.

JSON has per-step aggregates and the winning LED_PA.

### 3.5 Two output modes (compile-time toggle at top of file)
- `OUT_MODE_PLOTTER`: 8 tab-separated traces for Serial Plotter — MAX raw,
  baseline, ac, bp, motion; LSM raw, bp, motion.
- `OUT_MODE_SERIAL`: 1 Hz human-readable status block.

---

## 4. ECG side-quest (parked note)

Ivan wants a bench-grade ECG for training a wrist-HR estimator against ground
truth. Feasibility assessment done this session:

- **TENS electrodes** work as-is (Ag/AgCl or carbon-rubber, standard ECG
  chemistry). Fresh gel confirmed on-hand.
- **On-hand parts inventory:** J113 JFETs (buffers), LME49721 (~10, TI, low
  noise), OPA1656 (×2, JFET-input dual, premium), OPA1652 (×2), NE5532-alike
  "COS5532" (many), unidentified "COS822SR", "COS2374", "TP10-2", "TP2262".
- **Recommended topology:** either
  - (a) 2× J113 source-follower buffers + 1× COS5532 dual for diff amp + DRL —
    saves the premium chips entirely, or
  - (b) 1× OPA1656 dual as front-end unity buffers (no J113 needed) + 1× COS5532
    for diff + DRL — halves the discrete count, uses 1 of Ivan's 2 premium
    duals.
- **Sync-with-watch:** NTP-sync both devices' clocks, button-start on each,
  cross-correlate R-peak train against watch BPM offline for tight alignment.
- **Weekend-doable:** yes for both topology (a) or (b). Full schematic +
  bring-up plan to be written when Ivan gives the go on which topology.

**Deferred:** actually building the ECG is out of scope for stage 13; the
outcome of the stage-13 sketch might make ECG unnecessary if PPG + BCG cross-
check turns out to be self-consistent enough for training data. To re-visit
once we have session data from sketch 13.

**Prompt for a fresh Claude instance** to design the ECG front end lives at
`docs/build_info/reference_files/ECG_prompt_for_fresh_claude.md` — self-
contained, no repo knowledge assumed. Hand it off in a separate conversation
so this one stays focused on firmware.

---

## 5. Wrap (2026-08-19)

Bench sessions with the sketch on the iv7.1 board:

- **PPG (MAX30101 green) at rest, wrist strapped snug**: pulls a plausible BPM
  that matches manual radial-pulse count within a few beats. Signal collapses
  the moment the wrist moves; the motion gate hides most false beats but
  doesn't recover a valid BPM through the noise, so we lose reads not
  mispredict them.
- **BCG (LSM6DSV16X `sqrt(x²+y²+z²)`) at rest, arm supported**: also pulls a
  plausible BPM, roughly agreeing with the PPG channel. Same fragility to
  motion — this is a "sit still" reading, not a wearable-during-motion one.
- **Twin pipeline value**: when both channels agree at rest, the fused BPM is
  more trustworthy than either alone; when they disagree, the disagreement is
  itself a useful "don't trust either" gate. Good enough to justify keeping
  the twin structure through to the ESP-IDF port.
- **What still needs work**: absolute accuracy against ground truth, the
  threshold constants (peak-detect leaky-max `k`, motion envelope `k`, decay
  rates), and whether the motion gate can be replaced by a smarter "hold-last
  valid BPM through motion, resync on stillness" state machine.

**The final refinement lives on a data set we don't have yet.** All of the
above is qualitative bench observation. The refinement pass — thresholds,
biquad coefficients, the motion-gate replacement — is deferred to the ECG
cross-check described in §7.

### Deliverables checklist

- [x] First-run data recorded on SD; PPG and BCG both usable at rest.
- [x] BCG channel behavior: twin pipeline pulls plausible BPM at rest.
- [x] Motion gate effectiveness: nulls false beats but drops the read entirely
      under motion. Acceptable for now.
- [ ] PPG–BCG BPM agreement histogram at rest — qualitative only, no plot yet.
- [ ] Pipeline tuning constants — deferred to ECG-synced refinement.
- [ ] Port to ESP-IDF — deferred; existing `fc_mode` restructure ([[project_mode_restructure]])
      is the natural landing spot, but wait on ECG data before baking the
      constants in.

---

## 6. Follow-ups (may spawn stages of their own)

- Port validated pipeline into ESP-IDF `max30101/health_tile` fusion component
- Adaptive LED-PA policy (Ivan's active-vs-rest split) as a new `fc_mode` or
  a background policy running under `field_capture`
- Stage 12 residual bugs (still pinned)

---

## 7. ECG cross-check — decision made (2026-08-19)

The ECG side-quest is no longer parked. A **standalone ECG device is built**
— ESP32-S3 with onboard SD card, working end-to-end. Purpose is to give
"objective" heart signals which Kompic's PPG and BCG pipelines can be tuned
against.

### 7.1 Synchronised recording

To make the two data sets align at millisecond precision:

- ECG carries a **DS3231** (better absolute time than Kompic's PCF85063A, but
  the drift over a single recording session is negligible either way).
- Kompic carries the PCF85063A.
- Sync scheme: **USB-C ↔ USB-C between the two devices; ECG sends a `SYNC`
  command over USB (a `SET_TIME use-my-time-now` payload) to Kompic just
  before starting a session.** Kompic replies with acknowledgement of the new
  time, both devices timestamp on their own RTC thereafter.
- Post-session: rows correlate on shared timestamp column; ECG's R-peak train
  is ground truth for Kompic's fused BPM.

The two RTCs are good enough for this — DS3231 is ±2 ppm typ., PCF85063A is
worse but still adequate for a single-session offset that will get closed by
cross-correlation of the R-peak train anyway.

### 7.2 Kompic-side mode change (edit, don't add)

Edit an existing `fc_mode` to add a **PPG + BCG raw CSV timestamped mode**
that mirrors the ECG's output format. Goal: three CSVs per session (ECG, PPG,
BCG — or two, if PPG+BCG stay on the interleaved single-file format from §3.4)
that can be aligned offline in pandas and used to iterate the constants in
§3.2.

Which existing mode gets edited is TBD — likely the batt-test or the raw-BCG
capture mode, since neither is currently the primary user-facing mode. To be
decided when the edit lands. This follow-up is separate from the mode
restructure in [[project_mode_restructure]] — that reshuffles the menu; this
one adds a data-capture format.

### 7.3 What this unblocks

Once both devices are syncing and dumping aligned CSVs, the refinement
checklist in §5 gets its data:

- Peak-detect thresholds tuned against real R-peaks.
- Biquad coefficients validated against known-clean ECG-derived cardiac band.
- Motion-gate policy: replace the drop-when-moving behaviour with
  "hold last valid, resync on stillness" — testable against ECG ground truth
  during controlled motion sessions.
- Absolute accuracy claim: PPG–ECG BPM agreement over a resting session,
  BCG–ECG likewise, then fusion vs ECG.
