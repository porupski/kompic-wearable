# Bench Plan — Kompic Mk I (iv7.1, this physical prototype)

**Created:** 2026-06-29
**Scope:** Everything bench-actionable on the **current iv7.1 prototype** Ivan has on his desk. Forward-looking PCB respin items live in `v8_MustFix.md`; SD-card-specific rescue lives in `SDMMC_Rescue_Plan_iv7.1.md`. This doc consolidates the rest into one ordered walk-through so the next bench session has no surprises.

The board today is a "working but battle-scarred" prototype. Battery is permanently attached. Stage 3 closed with 4/5 critical bugs resolved on the bench. The 2026-06-29 desk session (firmware-only, no bench) added one new diagnostic conclusion: **DRV2605L silicon is degraded and needs replacement.** Motor is healthy at 23.6 Ω.

## Section 1 — Rework actions (priority order)

### 1.1 SD card — [RESOLVED 2026-07-05, see Stage_4_Build_Report.md]

> **The whole SD section was a false alarm.** Iv7.1 SD bus is functional as designed — 65/65 PASSes on a 32 GB SDHC msdos card at 400 kHz → 20 MHz with the original wiring. The Stage 3 + 2026-06-29 failures were a dying 64 GB Kodak AliExpress card, not a bus fault. EXP-0 through EXP-5 in the rescue plan are not needed. **Skip this section.** Next SD work is a data-logging sketch (`6_sd_logger_mk1`), not rework — see `Stage_4_Build_Report.md § 6`.

**Open since:** Stage 3 § 4.1, regression confirmed 2026-06-29.
**Why first:** EXP-0 is a 5-minute multimeter session, no soldering. Resolves a major open question before any other work. **See `SDMMC_Rescue_Plan_iv7.1.md` for the full plan; the entry-point checks are summarized here for the bench operator.**

**Three meter checks (board unpowered, card-removed for checks 1 and 3):**

| Check | Where | Expect | If wrong → |
|---|---|---|---|
| Continuity | SD socket pin 7 (DAT0) ↔ GPIO40 pad on ESP module | <1 Ω | Cold joint at one end. Reflow that joint with iron + flux. |
| Idle voltage (card in, firmware idle) | DAT0 net at any reachable pad | ~3.2 V (pull-up) | 0 V → solder bridge to GND, inspect/wick. 3.2 V but unresponsive → line held high somewhere, line is OK. |
| Different card | Insert a different card from Stage 3's three-card set | Identical failure | If symptom changes with cards, host-side is OK, cards differ. |

**After EXP-0:** If continuity good and a card-swap doesn't help → proceed with EXP-1 (1 µF cap at socket VDD) and EXP-2 (GND bodge wire) per the rescue plan. If continuity bad → reflow, then re-run the pump test (`firmware/test/sd_pump_test_mk1/`) before deciding.

**Pass criterion for SD work:** `firmware/test/sd_pump_test_mk1/` shows ≥5/10 PASSes in a row at 1 MHz.

### 1.2 Replace DRV2605L

**Open since:** Stage 3 § 4.4, conclusion firm 2026-06-29.
**Why second:** confirmed bad. Cannot be fixed in firmware. Motor confirmed healthy → don't touch motor, just chip.

**Evidence:**
- LRA DCR measured directly at motor pads: **23.6 Ω** (ELV1411A spec 10–25 Ω, healthy).
- Same reading at the PCB trace side → no series-R from bad solder.
- With `OD_CLAMP=0x20` (~0.68 V peak) into 24 Ω = ~28 mA peak drive, chip still fires `STATUS=0xE5 OC_DETECT` on every effect.
- DRV2605L OC threshold is 250 mA min per datasheet. Chip is tripping at one-tenth of spec. **Internal OC sense circuit is degraded.**

**Likely cause (not the +1V8 incident):** DRV is on the 3V3 rail, so the +1V8 backfeed that killed MAX30101 did not touch this chip. Most plausible vector is **ESD during the many handling / rework cycles** (Stage 2/3 included multiple touches of the underside of the board where the DRV sits). The chip's OC sense is sensitive analog circuitry — first to degrade from ESD.

**Procedure:**
1. Power down board (ship mode or wait for battery drain).
2. Apply ESD precautions: grounded iron, grounded mat, grounded wrist strap if available. **This was likely the original failure mode — don't repeat it.**
3. Hot-air the DRV2605L (DGS package, underside of main board) at 320–350 °C with small nozzle until paste reflows.
4. Lift the chip with tweezers, clean pads with solder wick + flux.
5. Place fresh DRV2605L, apply paste (lead-free if available), reflow.
6. Re-flash `4_demo_mk1` (uses original profile values: `RATED_VOLTAGE=0x49`, `OD_CLAMP=0x60`, attempts auto-cal at boot).
7. Watch the serial: expect `[DRV ] auto-cal PASS (~XXX ms)  COMP=0x.. BEMF=0x.. STATUS=0xE0` with no OC_DETECT.

**Pass criterion:** auto-cal PASSes on first boot. Encoder click feels firm. No `OC_DETECT` lines in the live STATUS poll.

**Fallback if a fresh DRV still fails cal:** something else on this board is breaking the LRA drive — could be a trace fault, could be a daughter-connector pin issue. Probe DRV OUT+/OUT- with scope during a fired click; expect ~1.5 V_RMS bipolar at 150 Hz. If output looks dead, follow the trace from DRV OUT to LRA terminal with continuity meter.

### 1.3 Replace MAX30101

**Open since:** Stage 3 § 4.8 / § 4.9.
**Why now:** +1V8 rail is verified clean at 1.80 V (Stage 3 § 4.9 fix). Safe to fit a fresh chip. Was definitively killed by the +1V8 overvoltage during Stage 3 (rail was 2.3–2.8 V before the C29 cut, MAX30101 V_MAX is 2.0 V abs max).

**Procedure:**
1. Hot-air the dead MAX30101 (DGS-style package, daughter PCB) at 320–350 °C.
2. Verify +1V8 rail is still clean at 1.80 V at the daughter PCB side **before fitting the fresh chip.** Probe the daughter-connector +1V8 pin with the meter.
3. Place fresh MAX30101, reflow.
4. Re-flash a sketch that probes MAX (smoke test from Stage 3, `3_smoke_test_mk1_hotair`).
5. Expect I²C ACK at 0x57 on bus 1, PART_ID = 0x15.

**Pass criterion:** MAX30101 ACKs on bus 1; PART_ID register reads 0x15; LED slots can be enabled and FIFO accumulates samples.

### 1.4 Restock R26 (47 Ω 0402, flashlight series resistor)

**Open since:** Stage 3 § 4.2.
**Why:** R26 was lost during prior probing. Currently only R25 (47 Ω) in series; LED_MAX_DUTY is firmware-capped at 64 (`3_smoke`) / 80 (`4_demo`) / 96 (`5_smoke`) to compensate. With R26 restocked, full duty can return.

**Procedure:**
1. Identify R26 footprint (next to R25, near the flashlight LED and BSS138W FET).
2. Hand-solder a 47 Ω 0402 with iron + flux.
3. Re-flash any sketch with `LED_MAX_DUTY = 255` (or remove the cap entirely).
4. Confirm LED is brighter on full duty than at 96.

**Pass criterion:** flashlight at full duty draws ~18 mA average per design, not 22+ mA. Visibly brighter than the firmware-capped state.

### 1.5 Flashlight LED leakage when "off" (Bug 7)

**Open since:** Stage 3 § 8.3.
**Why low priority:** cosmetic — LED glows visibly in a dark room when GPIO drives the gate LOW. Not blocking, but noticeable when wearing the watch in a dark room.

**Likely cause:** BSS138W I_DSS leakage (1–25 µA typical) is enough to forward-bias the LED into the sub-mA visible range.

**Bench options (any one):**
- **Bleed resistor across the LED:** 1–10 kΩ between LED anode and cathode. Any leakage current drops across the bleed, doesn't forward-bias the LED. Costs a few extra µA when LED is full-on. **Easiest fix.**
- **Replace BSS138W with lower-leakage FET** (e.g. DMG3414U with <100 nA I_DSS). Direct swap, same package.

**Pass criterion:** LED is invisibly off when GPIO is LOW, in a dark room.

## Section 2 — Sanity-check measurements (no rework, just document)

Capture these readings during the next bench session. They become baselines for future work and feed into v8 design decisions.

### 2.1 LRA DCR — DONE 2026-06-29

**Reading:** 23.6 Ω across the LRA's two leads, measured at the PCB trace solder pads with board unpowered. Multimeter 200 / 2000 Ω ranges agreed. **Motor is healthy** (high end of ELV1411A spec but inside).

### 2.2 SD bus continuity (to be captured)

Per SDMMC rescue plan EXP-0. Three readings to log:
- SD socket pin 7 (DAT0) ↔ GPIO40 pad
- SD socket pin 3 (CMD)  ↔ GPIO39 pad
- SD socket pin 5 (CLK)  ↔ GPIO38 pad

All should be <1 Ω.

### 2.3 DRV-side continuity (to be captured)

With DRV chip lifted (after § 1.2 removal, before placing the new chip):
- DRV OUT+ pad ↔ LRA + lead solder
- DRV OUT- pad ↔ LRA - lead solder
- DRV VDD pad ↔ 3V2 testpoint
- DRV GND pad ↔ GND testpoint

All should be <1 Ω. Confirms the chip swap is a chip-level fix, not chasing a trace fault.

### 2.4 Visual under magnification (10×)

While doing the DRV swap and MAX30101 swap:
- Scan the entire underside of the main board for solder balls / whiskers from prior rework.
- Inspect each chip's perimeter pads under magnification.
- Photograph anything suspicious for record (this kind of evidence has value when v8 redesign happens).

## Section 3 — Optional additions on this board (future bench, no respin needed)

### 3.1 MAX17048G+T10 fuel gauge

**Status:** part chosen, not yet wired. LCSC stocks it in TDFN-8.

**Wiring summary (from 2026-06-28 chat):**
- I²C address 0x36 (7-bit). Lives on bus 1 or bus 2, Ivan's call. Bus 2 keeps battery telemetry together with BQ25619 — recommended.
- VDD = battery positive (sense input, 2.5–4.5 V). Pre-regulator tap, *not* off 3V2.
- 0.1 µF decoupling close to VDD. No other passives.
- ALRT pin: leave unconnected (no spare GPIO).

**Bench procedure:**
1. Free a tiny patch of board space by tightening silkscreen refs (R10–R15 etc.) per Ivan's plan.
2. Place MAX17048 footprint + 0.1 µF.
3. Wire VDD via fine bodge wire to BAT+ (pre-regulator).
4. Wire SDA/SCL to bus 2 nets (shared with BQ25619).
5. Write a probe sketch that reads VCELL (0x02) and SOC (0x04). Expect VCELL ≈ V_BAT × 78.125 µV per LSB, SOC = read16 / 256 = battery %.

**Pass criterion:** voltage readback matches multimeter on BAT+, SOC reports a reasonable value across the charge range, CRATE shows expected sign during charge / discharge.

This is a "nice to have" for the iv7.1 prototype but is now part of the v8 BOM regardless — see `v8_MustFix.md`.

## Section 4 — Bench session order

A practical walk-through for the next bench session, assuming ~2 hours bench time:

1. **(5 min)** Quick: power up, confirm board still alive, ship mode works, encoder/flashlight nominal via `4_demo`. Baseline check before touching anything.
2. **(5 min)** SD bus continuity (§ 2.2) — three meter readings. Document.
3. **(10 min)** Run SDMMC pump test, observe failure pattern. Compare to 2026-06-29 baseline (0/41 PASS at 643 ms timeout). If same → continue to SD work; if different → note the change.
4. **(20 min)** SD rework per SDMMC rescue plan EXP-1 → EXP-2 → as needed. **Stop the moment pump test passes consistently.**
5. **(20 min)** DRV2605L removal (§ 1.2). Document continuity at OUT± pads while chip is off (§ 2.3). Place fresh chip. Reflow.
6. **(15 min)** Flash `4_demo`, verify auto-cal PASS, encoder click feels firm. If yes → DRV is back. If no → scope OUT± per § 1.2 fallback.
7. **(15 min)** MAX30101 removal + replacement (§ 1.3). Verify +1V8 rail before fitting the fresh chip.
8. **(10 min)** R26 restock (§ 1.4). Lift LED duty cap, verify brightness.
9. **(remaining)** Bug 7 mitigation (§ 1.5) if time. Or queue for next bench session.

If anything goes sideways on a step, stop, document, defer to next bench session. **Don't burn a working board chasing a flaky one.**

## Section 5 — Cross-references

- SDMMC: `SDMMC_Rescue_Plan_iv7.1.md`
- v8 PCB respin items: `v8_MustFix.md`
- Stage build reports: `Stage_1_Build_Report.md`, `Stage_2_Build_Report.md`, `Stage_3_Build_Report.md`
- Master pinout: `../Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md`
- Current sketches:
  - `firmware/test/4_demo_mk1/4_demo_mk1.ino` — production-ish demo, auto-cal at boot
  - `firmware/test/5_smoke_test_mk1/5_smoke_test_mk1.ino` — diagnostic build (encoder rewrite, DRV open-loop + multi-effect, live STATUS poll)
  - `firmware/test/sd_pump_test_mk1/sd_pump_test_mk1.ino` — SD retry loop with drive-cap instrumentation

## Section 6 — Update log

| Date | Item | Status / reading | Note |
|---|---|---|---|
| 2026-06-29 | LRA DCR | 23.6 Ω | Healthy; do not replace. |
| 2026-06-29 | DRV2605L OC threshold | Trips at ~28 mA (one-tenth of spec) | Chip silicon damaged; queued for replacement. ESD during rework is the suspected cause. |
| 2026-06-29 | SDMMC drive cap | CAP_3 sticks through library begin() | Drive cap is NOT the bottleneck. |
| 2026-06-29 | SDMMC failure mode | Deterministic 643 ms timeout at ACMD51, 0/41 PASS, clock-invariant | Reclassified as single-line electrical fault (vs Stage 3's borderline-SI story). EXP-0 priority. |
| 2026-07-05 | SDMMC final verdict | **RESOLVED — not a bus fault.** 65/65 PASSes on 32 GB SDHC msdos card at 400 kHz through 20 MHz with original iv7.1 wiring. Was a dying 64 GB Kodak AliExpress SDXC card + exFAT filesystem. § 1.1 obsolete. See `Stage_4_Build_Report.md`. |
| 2026-07-05 | USB-C reversibility bug | Iv7.1 wires only one D+/D- pair. Only one plug orientation enumerates; other orientation produces dmesg `error -71`. New CRITICAL item for v8 (`Stage_3b_v8_MustFix.md`). |
| 2026-07-05 | `3c_sd_pin_probe_mk1` diagnostic sketch added | Replicates ESP-IDF PIN-recovery-time test in Arduino. Confirmed all three SD pins healthy. |
