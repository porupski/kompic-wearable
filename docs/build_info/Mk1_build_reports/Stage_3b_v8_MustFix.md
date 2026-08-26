# v8 PCB Must-Fix List — Kompic Mk

**Created:** 2026-06-29
**Scope:** Forward-looking. Every PCB-level change that needs to land in the next board revision (whether that's Mk I v8 or Mk II v8), distilled from Stages 1–3 build reports plus the 2026-06-29 desk session.

This is the PCB designer's bench plan. Bench-actionable items on the iv7.1 prototype live in `Bench_Plan_iv7.1.md`. The SD-specific rescue plan is `SDMMC_Rescue_Plan_iv7.1.md`.

**Items are tagged:**
- **[CRITICAL]** = bug that breaks the board, must fix or v8 will fail in the same way.
- **[IMPROVEMENT]** = quality-of-life / robustness, strongly recommended but not strictly blocking.
- **[NEW]** = added feature / part that wasn't on iv7.1.

---

## Section 1 — Schematic library / pinout bugs (CRITICAL)

These are silent failures. ERC won't catch any of them because the connectivity is internally consistent — it just doesn't match the physical chip.

### 1.1 [CRITICAL] XC6206P182MR symbol pin swap

**Origin:** Stage 3 § 4.8.
**Bug:** Library symbol `Kompic_Mk1:XC6206P182MR` has Vin / Vout pin names swapped. Pin 2 is labeled "Vin" but on the datasheet it's Vout; pin 3 is labeled "Vout" but the datasheet says Vin.
**Consequence on iv7.1:** the +1V8 rail was driven backwards into the chip. Rail measured 2.3–2.6 V (internal ESD diodes leaking) instead of 1.8 V. Killed the MAX30101.
**Fix for v8:** edit the symbol so pin 2 = Vout, pin 3 = Vin. Re-run ERC. Verify against the SOT-23-3 datasheet pinout before fab.
**Owner:** library file under `hardware/Kompic_Mk1/Kompic_Mk1_library/`.

### 1.2 [CRITICAL] LSM6DSV16X and BME688 VDDIO routing

**Origin:** Stage 3 § 4.9.
**Bug:** Both chips' VDDIO / Vdd_IO pins are routed to +1V8, but the I²C bus they sit on runs at +3V3. The chips' I/O ESD clamps go to VDDIO, not VDD, so each SDA/SCL line back-feeds current from +3V3 through the chip into the +1V8 rail. With three chips × multiple I/O pins, the cumulative back-feed prevented the LDO from regulating, settled the rail at ~`3V3 − Vf` ≈ 2.3–2.8 V. Contributed to MAX30101 overvoltage death.
**Consequence on iv7.1:** bench-fixed by cutting the +1V8 trace at C29 and jumpering the affected branch to +3V3. LSM/BME/mic now run entirely on +3V3.
**Fix for v8:** in `4_Air_Sensors.kicad_sch` and `3_Space_Time_Sensors.kicad_sch`, route VDDIO of LSM6DSV16X and BME688 to +3V3. The rule going forward: **VDDIO matches the bus voltage. VDD is free to be lower.** Audit every chip with a separate VDDIO pin against this rule before fab.
**Note:** LIS3MDLTR is correctly wired on iv7.1; no change needed.
**Reference:** datasheets in `firmware/docs/datasheets/`: `lsm6dsv16x.pdf` Table 2 p. 11, `bst-bme688-ds000.pdf`, `lis3mdl.pdf` Table 7 p. 8.

### 1.3 [CRITICAL] PDM mic VDD voltage

**Origin:** Stage 3 § 4.9 (carried into rework, same family of issue).
**Bug:** MSM261DGT003 VDD min spec is 3.3 V (or 3.6 V depending on rev). iv7.1 routed it to +1V8.
**Consequence on iv7.1:** mic was operating below VDD min before the C29 cut moved it to +3V3.
**Fix for v8:** mic VDD on +3V3 net by design, no jumpering required.

---

## Section 2 — Schematic topology bugs (CRITICAL)

### 2.1 [CRITICAL] Flashlight FET source-follower → low-side switch

**Origin:** Stage 3 § 4.2.
**Bug:** `5_Lights.kicad_sch` has the BSS138W wired as a source follower. With GPIO41 at 3.2 V, the FET's source pinches at ~1.5–2.0 V — below the LED's V_f. The LED can never light no matter how good the FET is.
**Consequence on iv7.1:** bench-fixed with two cuts + two jumpers (LED moved from FET source to drain side, FET source jumpered to GND). Lost R26 during probing — restocking needed in `Bench_Plan_iv7.1.md` § 1.4.
**Fix for v8:** redraw the FET symbol so source is on the GND rail. Standard low-side switch topology:
```
+5 V → R25 → R26 → LED_anode → LED_cathode → FET_drain → FET_source → GND
```

### 2.2 [CRITICAL] USB-C both-side D+/D- routing

**Origin:** 2026-07-05 bench session (Stage 4 Build Report § 2.5).
**Bug:** Iv7.1's USB-C connector only wires *one* D+/D- pair to the ESP32-S3 (GPIO19/20). USB-C is reversible only if either both A-side and B-side D+/D- pairs are wired, or a CC-controlled mux switches between them. Iv7.1 has neither → one plug orientation enumerates cleanly, the other produces `device descriptor read/64, error -71` in dmesg because the ESP is looking at floating pads instead of the cable's data lines.
**Consequence on iv7.1:** minor annoyance; work around by always plugging the cable the correct way. But this cost half a bench session diagnosing what looked like a firmware/USB-PHY hang.
**Fix for v8:** wire both D+/D- pairs — either directly (tie A6↔B6 and A7↔B7, works if you don't need USB 3 sideband channels) or through a CC-based orientation mux (TS3USB221, PI3USB102, etc.). Either approach makes the cable truly reversible.

### 2.3 [CRITICAL] BQ25619 /QON pull-up routing (R12)

**Origin:** Stage 3 § 8.1.
**Bug:** R12 (5.1 kΩ) pulls /QON to +3V3. When ship mode drops BATFET, +3V3 collapses and R12 becomes a hard pull-DOWN — holds /QON LOW, BQ trips t_SHIPMODE after 0.9–1.3 s, BATFET re-enables, board wakes itself. Ship mode unusable without lifting R12.
**Consequence on iv7.1:** R12 lifted by hand. BQ's own internal 200 kΩ to V_BAT (pin 12) is sufficient — same topology as TI's EVM reference design.
**Fix for v8:** **mark R12 as DNP** in the schematic, leave the footprint as a depopulated placeholder. Don't reroute to V_BAT or SYS — the internal 200 kΩ is the manufacturer-intended pull.

---

## Section 3 — SDMMC bus design (mostly IMPROVEMENT after 2026-07-05)

> **Reclassified 2026-07-05:** the iv7.1 SDMMC bus is functional as designed. See `Stage_4_Build_Report.md`. Items 3.1 / 3.2 / 3.4 / 3.5 / 3.6 / 3.7 below are demoted from CRITICAL to IMPROVEMENT — good practice for margin, not required for function. Item 3.3 (CLK pull-up populated by BOM) remains CRITICAL because the empty footprint required a Stage 3 bench bodge.

**Origin:** Stage 3 § 4.1 + 2026-06-29 desk session (both retracted 2026-07-05).
**Original framing (retained for context):** Even with short traces (CLK = 6 mm, CMD/DAT0 = 15 mm) and DAT1/DAT2 pull-ups populated, SDMMC 1-bit init failed deterministically on iv7.1. Best diagnosis points at insufficient VDD decoupling at the socket, plus possible ground-return discontinuity under the ESP module.
**Corrected framing (2026-07-05):** the failures were card-side (dying 64 GB Kodak AliExpress SDXC + exFAT filesystem), not bus-side. 65/65 PASSes on a 32 GB SDHC msdos card at 400 kHz through 20 MHz with original iv7.1 wiring. Auto-memory: `feedback_suspect_cheap_sd_cards.md`.

### 3.1 [IMPROVEMENT] VDD decoupling on the SD socket

**Bug:** iv7.1 has 100 nF decoupling but on the **opposite face** of the PCB from the socket, ~2–3 mm away across via stack. For the card's 50–100 mA transients during data-block reads, that's effectively no decoupling at SDMMC speeds.
**Fix for v8:** **1 µF X7R + 100 nF ceramic, both on the same face as the SD socket, vias to plane *adjacent* to the cap pads, not 2–3 mm away.**

### 3.2 [IMPROVEMENT] Default pull-up values

**Bug:** iv7.1 defaults to 10 kΩ pull-ups on CLK / CMD / DAT0. Spec allows 10–100 kΩ for legacy mode, but every real-world SDMMC layout above ~1 MHz uses 4.7 kΩ.
**Fix for v8:** **default 4.7 kΩ pull-ups on CLK / CMD / DAT0.** Leave 10 kΩ on DAT1/2/3 (weak pull-up for noise immunity, lines are unused in 1-bit mode).

### 3.3 [CRITICAL] CLK pull-up footprint populated by BOM

**Bug:** iv7.1 CLK pull-up footprint was empty (Stage 3 bench-fixed by hand-soldering a 10 kΩ in).
**Fix for v8:** populate the CLK pull-up in the BOM, not as a "to be added in rework" item.

### 3.4 [IMPROVEMENT] Ground plane continuity ESP ↔ SD socket

**Bug:** SD socket sits under the ESP module on iv7.1. Return current path may have to cross plane discontinuities (modulator cavity, USB-C shell vicinity, antenna keepout). Ringing on CLK / CMD is consistent with high return-loop inductance.
**Fix for v8:** **continuous ground plane between ESP and SD socket.** Stitching via fence along both sides of the SDMMC bus traces. No moats, no slots, no plane cutouts in the bus area.

### 3.5 [IMPROVEMENT] CLK series-R footprint (DNP)

**Fix for v8:** add a 22–47 Ω series-R footprint on CLK between the ESP and the socket, defaulted DNP (0 Ω jumper or omitted at first fab). Belt-and-suspenders for signal integrity once HS speeds are wanted. Damps overshoot at the load without slowing the rise materially.

### 3.6 [IMPROVEMENT] DAT3 routing / CS provisioning

**Bug:** iv7.1 leaves DAT3 floating. With no DAT3 routed, the rescue path "fall back to SPI mode" requires a GPIO0 / DRV_EN sacrifice to get a CS pin (Stage 3 § 4.1 Plan B).
**Fix for v8:** **route DAT3 to a real GPIO** with no strapping conflicts. This unlocks both 4-bit SDMMC (if ever wanted) and SPI fallback without bodge wires. Even if SPI is never used, the option-value is worth one GPIO.

### 3.7 [NEW] Replace push-push socket with Molex 472192001 (flip type)

**Origin:** 2026-06-30 session.
**Bug:** iv7.1 uses a push-push microSD socket. Push-push sockets have notoriously variable contact spring impedance (50 mΩ to a few Ω depending on insertion cycle / oxidation), which contributes parasitic capacitance and contact noise to an already-marginal SDMMC bus. They also have a ~14 × 15 mm plan footprint that's larger than needed since the watch case won't expose an SD slot anyway — card insertion is service-only, push-push eject ergonomics are wasted.
**Part:** Molex **472192001** (LCSC **C164170**). Hinged-lid microSD socket: lid flips open, card slides in under it, lid clicks closed.
**Why it's an upgrade:**
- Plan footprint ~12 × 11 mm vs ~14 × 15 mm push-push — frees board area.
- Lower profile than push-push, helps the watch sandwich.
- Hinged contacts: firmer / less variable than push-push spring tabs. May also help SDMMC signal integrity by reducing contact-impedance variability.
- 8-pin microSD standard, identical pin assignment — zero firmware impact.
**Trade-off:** card change requires opening the case (acceptable since the case has no SD slot exposed anyway). Hinge can fatigue after ~thousands of cycles, irrelevant for "factory load + occasional service" use.
**Fix for v8:** swap the socket in `5_Lights.kicad_sch` (or wherever the SD subsheet lives) for the Molex 472192001 footprint. Verify pad pitch against the datasheet before fab.

---

## Section 4 — Power architecture (NEW + CRITICAL)

### 4.1 [NEW] MAX17048G+T10 fuel gauge

**Origin:** discovered in 2026-06-28 chat — BQ25619 datasheet § 20.13 extract confirms no VBAT readback on the BQ. Without a fuel gauge, the host has no way to compute SoC.
**Part:** MAX17048G+T10 (LCSC stocks it in TDFN-8). Voltage-only ModelGauge m5 fuel gauge, no sense resistor needed.
**Fix for v8:** add to BOM. Wiring:
- I²C address 0x36 (7-bit). Place on bus 2 alongside BQ25619 — keeps all battery-related telemetry on one bus.
- **VDD = battery positive (pre-regulator, 2.5–4.5 V tap). Not off 3V2.**
- 0.1 µF decoupling at VDD, single 0402.
- ALRT pin left unconnected (no spare GPIOs, polling SOC is fine for this use case).
- No other passives.

**Footprint:** TDFN-8 is hand-solderable. Room budget: tighten BOM silkscreen refs (R10–R15 etc.) per Ivan's plan to free a small patch.

### 4.2 [IMPROVEMENT] BQ25619 TS network

**Origin:** Stage 2 § 4.5.
**Bug:** iv7.1 uses 10k + 10k NTC divider. Doesn't match TI's recommended R_T1 = 5.36 kΩ + R_T2 = 31.6 kΩ for the BQ25618/19 JEITA thresholds. Bench workaround: `TS_IGNORE=1` in firmware.
**Fix for v8:** R_T1 = 5.36 kΩ (E96) and R_T2 = 31.6 kΩ (E96). JEITA thresholds will land at nominal temperatures. Drop `TS_IGNORE=1` from firmware once correct network is in place.

### 4.3 [IMPROVEMENT] Mic decoupling per datasheet

**Origin:** Stage 2 § 6.6.
**Fix for v8:** MSM261DGT003 datasheet calls for 100 nF + 10 µF near the mic VDD pin. iv7.1 layout may be missing the 10 µF. Verify and add for v8.

---

## Section 5 — DRV2605L / haptic (CRITICAL + IMPROVEMENT)

### 5.1 [CRITICAL — process] ESD precaution on DRV pads

**Origin:** 2026-06-29 conclusion. DRV silicon degraded — OC threshold collapsed to ~28 mA (one-tenth of spec). Most plausible cause is ESD during rework.
**Fix for v8 / process:** no schematic change required. Add a process note: **DRV2605L pads need standard ESD precautions during hand-rework** (grounded iron, mat, wrist strap). Should have been the rule on iv7.1 too, but documenting it explicitly is the lesson.
**Optional [IMPROVEMENT]:** add a TVS array on the DRV's OUT± outputs to clamp ESD on the LRA-side traces. ELV1411A is on the wrist, easy ESD entry vector. Costs two SOD-882 parts.

---

## Section 6 — Flashlight residual leakage (IMPROVEMENT)

**Origin:** Stage 3 § 8.3.
**Bug:** BSS138W I_DSS leakage (1–25 µA typical) is enough to forward-bias the LED into visible glow when GPIO is LOW.
**Fix options for v8 (pick one):**
- **Bleed resistor** 1–10 kΩ across the LED (anode-to-cathode). Cheapest. Any sub-µA leakage drops across the bleed, doesn't make LED visible. Adds a few µA load at full duty.
- **Lower-leakage FET** (DMG3414U or similar, I_DSS <100 nA). Direct swap, same package. More expensive part.
- **Switch high-side with a PMOS instead of low-side with NMOS.** Gate off pulls drain *and* source to +5V; no path through LED. Most robust but inverts gate logic — `digitalWrite(HIGH)` would mean OFF.

Recommend bleed-resistor approach for v8 — simplest, no logic inversion, gives a fully-off LED.

---

## Section 7 — Carry-forward from earlier reports (IMPROVEMENT)

These items were flagged across the build reports but aren't critical fail-modes. Bundling here so v8 schematic review can sweep them.

### 7.1 PDM CLK voltage-domain mismatch

**Origin:** Stage 2 § 6.7 / PDM extract W1.
**Bug:** 3.3 V GPIO into a 1.8 V-rail device (per the iv7.1 wiring). After the C29 rework the mic is now on 3V3 so the issue is gone for this board, but v8 should design the mic on 3V3 from day one (per § 1.3) with no domain mismatch.

### 7.2 LSM6DSV16X INT1 → GPIO8 wake-from-sleep not yet exercised

**Origin:** Stage 2 § 6.4.
**Status:** firmware-side todo, not v8 schematic change. Listed here so it's not forgotten.

### 7.3 LIS3MDL hard-iron offset calibration

**Origin:** Stage 2 § 6.3 / Stage 3 § 4.7.
**Status:** firmware-side todo (figure-8 swing, persist OFFSET_X/Y/Z_REG to NVS). Not v8 schematic change.

### 7.4 ESDALCL5-1BM2 BOM note

**Origin:** Stage 3 § 4.3.
**Fix:** add a BOM line note on the ESDALCL5-1BM2: "healthy reads OC in DMM diode mode (bidirectional 5 V TVS; DMM test voltage is below breakdown)." Prevents the next assembler from desoldering all five "diodes" during bring-up under the misapprehension that they're broken.

---

## Section 8 — Architectural questions for v8 scope decision

These aren't bugs — they're open questions Ivan is weighing for whether v8 is "Mk I v8" (minimal-change patch of iv7.1) or "Mk II v8" (clean-sheet re-architecture).

### 8.1 Decompose ESP32-S3-WROOM-1U → ESP32-S3 core + PSRAM + crystal + antenna network

**Trade-off:** module is plug-and-play (RF cert pre-done) but takes board area and gives no design flexibility. Discrete approach lets the RF section be tuned but requires RF cert work and good layout discipline.
**Ivan's current call:** low priority for the next revision; attempt is optional, certification can wait.

### 8.2 Mk I patch vs Mk II clean-sheet

**Status:** undecided. If v8 is "Mk I patched," only critical bugs above need fixing. If v8 is "Mk II clean-sheet," every item here gets reconsidered against the new architecture, and the v8 doc itself gets re-scoped.

---

## Section 9 — Footprint design rule: extend perimeter pads outward

**Origin:** 2026-06-30 session, generalized from the Stages 1–3 rework experience.

**Bug:** EasyEDA's auto-converted footprints follow JEDEC standard — pads sized tight to the package body, optimized for high-density automated assembly. On iv7.1 this meant the LGA / DFN parts (LSM6DSV16X, LIS3MDLTR, BME688, TPS62840, XC6206) have pads entirely *under* the chip body. Three real costs:
1. **Smaller pad area → smaller paste volume → higher cold-joint risk.** Especially with the manual-paste fallback (50:50 lead-free + leaded on hidden pads).
2. **No probe point** during bring-up. The reason BQ25619 was easy to debug in Stage 3 was that its WQFN-24 leads stick out past the body — the LSM/LIS/BME/TPS could not be probed at all without lifting the chip.
3. **No visible fillet** under magnification — joint quality has to be inferred from electrical behavior, not visual inspection.

**Fix for v8:** **manually edit every footprint after EasyEDA → KiCad import to extend perimeter pads outward from the package body.** Direction is **outward only** (toward board edge), never inward. Standard extension by part class:

| Part class | Extend? | How much |
|---|---|---|
| QFN / WQFN with already-exposed leads (BQ25619 style) | already done | n/a |
| LGA with hidden pads (LSM6DSV16X, LIS3MDLTR, BME688) | **yes, extend** | 0.4–0.5 mm outward |
| DFN with hidden pads (TPS62840, XC6206, PCF85063A) | **yes, extend** | 0.4–0.5 mm outward |
| Fine-pitch (≤0.4 mm pitch) chips | extend small | 0.2–0.3 mm — bridge risk past that |
| Center thermal pads (QFN bellies) | leave standard | thermal connection only, no probe value |
| 0402 passives | **leave standard** | extension causes tombstoning during reflow (asymmetric thermal mass) |
| 0603 / 0805 passives | optional small extension | 0.1–0.2 mm OK, low tombstone risk |

**Wins per Stage-3 lessons:**
- More paste flow per pad → less cold-joint risk (matches the dire-straits paste workflow we keep falling back to).
- Probe access for the LGA/DFN parts that were un-debuggable on iv7.1.
- Better visual inspection of solder fillet at the package edge.
- Larger pad mass = better thermal anchor + less mechanical stress concentration.

**Don't apply** to: center thermal pads, 0402 passives, anything finer than 0.4 mm pitch (judgment call per part).

## Section 10 — 3D model housekeeping

EasyEDA-derived STEP/WRL models carry small "EasyEDA" branding visible in the 3D viewer. Acceptable for this personal / educational project — keep the models as-is. If v8 ever moves toward commercial review, swap the chip-specific models for manufacturer-sourced STEP (ST, TI, Bosch all host them) and use KiCad's built-in 3D library for generic passives.

## Section 11 — Update log

| Date | Item | Status |
|---|---|---|
| 2026-06-29 | Initial draft, post-2026-06-29 desk session | Captured all known critical bugs from Stages 1–3 + new conclusions (DRV chip damage, SDMMC regression, MAX17048G add). |
| 2026-06-30 | Added § 3.7 (Molex 472192001 flip SD socket), § 9 (footprint pad-extension design rule + per-part table), § 10 (EasyEDA 3D model branding note). Renumbered update log → § 11. | — |
| 2026-07-05 | § 3 SDMMC items 3.1/3.2/3.4/3.5/3.6/3.7 demoted CRITICAL → IMPROVEMENT after Stage 4 verification. 3.3 still CRITICAL (empty CLK pull-up footprint). Added new § 2.2 CRITICAL: USB-C both-side D+/D- routing (iv7.1 wires only one, breaks reversibility). Renumbered BQ /QON to § 2.3. | See `Stage_4_Build_Report.md`. |
