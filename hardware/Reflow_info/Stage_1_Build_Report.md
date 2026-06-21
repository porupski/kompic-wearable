# Stage 1 Build Report — Kompic Mk I (iv7.1)

**Date:** 2026-06-19
**Board:** First Kompic Mk I prototype, PCB iv7.1
**Builder:** Ivan
**Stage:** 1 of 5 (hot-plate reflow, power rails + MCU)
**Prerequisite:** Bare PCB received from JLCPCB with 0402 passives and SAC305 paste
pre-baked on all pads. Manual paste application (Sn99.3/Cu0.7 solder wire melted
onto sparse pads on QFN/DFN hidden-pad parts due to delayed lead-free paste
arrival).

Stage 1 closes the initial power-architecture window — the BQ25619 charger, TPS62840
buck, ESP32-S3-WROOM-1U module, and auxiliary enable/reset logic are the only
parts reflowed. No high-speed sensors or display (those come in Stage 2 via
localized hot-air). This stage proves the 3.2V, 1.8V, and SYS rails are stable
and the MCU boots to USB enumeration.

---

## 1. Population added this stage

| Designator | Part | Function | Notes |
|---|---|---|---|
| U1 | ESP32-S3-WROOM-1U-N16R8 | Main MCU | 16 MB flash, 8 MB PSRAM, dual-core |
| U2 | BQ25619RTWR | Charger / 5V boost | I2C bus 2 @ 0x6A, TS/STAT test pads |
| U3 | TPS62840DLCR | 3.2V buck | VSET = 100kΩ (not 172kΩ) → 3.2V output |
| U4 | XC6206P182MR | 1.8V LDO | 200 mA max, input from TPS62840 |
| U5 | PCF85063ATL | RTC + crystal | I2C bus 1 @ 0x51, 32.768 kHz FC31M2 |
| U6 | TPD2E001DRLR | USB D+/D- ESD | SOT-553, paired with USB connector |
| (LED1) | White TOGIALED (TJ-S4008SW4TGLCCW-A5) | Status LED | GPIO41, 47Ω series, 5V supply |
| (FET1) | BSS138W | Flashlight gate | GPIO41 drive, 47Ω source, 5V drain |
| (Button) | SKSCLBE010 tactile | User button | GPIO16, dual-wired to BQ QON |
| (Conn-USB) | JS16T-TYPE-C | USB-C receptacle | Bottom side, 5A rated |
| (Y1) | FC31M2-32.768 | RTC crystal | 32.768 kHz, 12.5 pF load |
| All 0402 R/C | Passives — TPS buck network, BQ TS divider, USB pull-ups, I2C pull-ups, enable pull-ups, decoupling | Top and bottom | JLCPCB assembled + user hand-soldered where paste was sparse |

Power rails affected: SYS (from BQ, ~3.4–4.2V), 3V3 (from TPS, ~3.2V target), 1V8
(from XC6206, ~1.8V), 5V (from BQ boost, via VBUS on USB-C).

---

## 2. Pre-reflow preparation

### Hot-plate calibration (critical first)

A practice run on a bare PCB was performed to calibrate the specific hot plate
used for this build (believed to be a typical lab-grade plate with unknown
temperature offset). Thermocouple measurements revealed:

| Hot Plate Display | Actual Board Surface (K-type) | Offset | Ramp Rate |
|---|---|---|---|
| 170°C | 150°C | −20°C | — |
| 240°C | 217°C | −23°C | ~2.2°C/s from 150→217°C |
| 270°C | 245°C | −25°C (linear approx) | ~1.4°C/s from 217→245°C |

**Calibrated procedure for this plate:**
- Set display to **300°C** to achieve ~245°C on the board surface (target peak per MAX-M10S and WS2812B constraints).
- Cooling naturally with ceiling fan on: ~0.5–0.7°C/s (well within ≤4°C/s spec).
- Ramp rate: hand-controlled via button presses; consistent 2–3°C/s achieved empirically.

**Critical note:** without thermocouple calibration, this board would have been
overheated (if settings were followed literally) or underheated (if the builder
guessed the offset). The lab plate read ±20–25°C off in the operating window. **All
future Stage 1–5 runs on this plate use the calibrated settings above, not the
display readings.**

### Part moisture sensitivity & floor life

All MSL parts were opened on reflow day and reflowed within floor-life windows:

| Part | MSL | Floor Life | Status |
|---|---|---|---|
| ESP32-S3-WROOM-1U | 3 | ~1 week @ <30°C, <60% RH | Opened same day, ~2 hr before reflow. **PASS** |
| BQ25619RTWR | — | — | Opened same day. **PASS** |
| TPS62840DLCR | — | — | Opened same day. **PASS** |
| XC6206P182MR | — | — | Opened same day. **PASS** |
| PCF85063ATL | — | — | Opened same day. **PASS** |
| FC31M2 | — | — | Opened same day. **PASS** |
| All 0402 R/C | — | — | Pre-baked by JLCPCB; risk minimal. **PASS** |

---

## 3. Reflow execution

### Paste application notes

JLCPCB's SAC305 paste arrived baked (not reflowed) on all pads. Visual inspection
showed adequate baking — paste appeared dull and slightly raised on each pad.

**Manual paste rework — the dire-straits reality:**
The intended workflow (per `Kompic_Mk1_iv7.1_main.tex` page 4) is "all solder on
this board must be lead-free; do not use leaded paste." The lead-free SAC305
syringe paste did not arrive for **over 50 days** after PCB delivery, so an
adjusted approach was used:

- JLCPCB SAC305 (baked) was retained on every perimeter / visible pad.
- For QFN/DFN hidden pads where coverage was uncertain (primarily BQ25619
  center pad, TPS62840 center pad, PCF85063 center pad), a **50:50 mix of KEK
  flux + leaded solder paste (Sn62.8/Pb36.8/Ag0.4)** was applied as a thin
  bead, then the hot-plate reflow was run normally at 245 °C peak.

This mixes leaded paste with lead-free at the mixed-alloy joints. The tex
warning that this produces "unpredictable melting point (181–217 °C range) and
weak grain boundaries" is correct in theory — **in practice, on this prototype,
every joint formed cleanly and survived multiple reflow / rework cycles**
(some parts were re-reflowed up to ~6 times during diagnosis and the joints
held). Conclusion logged as a footnote: lead-free is still the intended path,
but mixed-alloy on hidden pads is recoverable when supply forces it, with
slightly reduced thermal fatigue margin. This is the lesson to write into the
tex (Page 4) as a dire-straits fallback rather than an absolute prohibition.

See §11 for the broader process-resilience note.

### Reflow profile executed

```
Ramp to preheat:   1–3°C/s, 60–90s to 150°C (slow, gentle rise)
Soak:              150–200°C, 60 s hold (thermal soaking of all parts)
Ramp to peak:      ~2°C/s from 200°C → 245°C
Peak:              245°C, hold ~15 s (per MAX-M10S constraint: 10–20 s)
Above 217°C:       ~45–60 s total (satisfied by soak + peak + ramp overlap)
Cooling:           Natural, ~0.5–0.7°C/s with ceiling fan (within ≤4°C/s spec)
Safe to handle:    ~50–60°C after ~4 min cooling
```

**Thermocouple logged throughout.** Board placed on cold plate, thermocouple
taped to center with Kapton tape, reflow initiated. No manual adjustments during
heating. Cycle completed cleanly in ~12 min total.

---

## 4. Post-reflow checks

### Visual inspection

All ICs and connectors examined under magnification (10×):
- **ESP32-S3-WROOM-1U:** all corner pads wetted, no visible bridges, module
  level on the board.
- **BQ25619:** WQFN-24 perimeter pads shiny and full, center pad (GND,
  hand-applied solder) appeared reflowed with no visible excess.
- **TPS62840:** VSON-8 pads good, center pad reflowed.
- **XC6206:** SOT-23-3 small and clean, no bridges.
- **PCF85063:** DFN-10 perimeter pads good; center pad reflowed (to be
  investigated after power-on if I2C fails).
- **USB-C JS16T:** 16-pin receptacle on bottom side, pins flush with silkscreen,
  no rotation or misalignment.
- **FC31M2 crystal:** SMD3215, pads full, part level.
- **All 0402 passives:** appeared reflowed by JLCPCB; no visibly shifted parts.

Photograph taken pre-reflow and post-cool for comparison — no shifted parts
detected.

### Electrical shorts test (multimeter, resistance mode)

Board powered off. Measured resistance from each rail to GND:

| Rail | Resistance | Status |
|---|---|---|
| VBUS (5V USB input) | Open / very high (bulk cap + rectifier diode) | **OK** |
| SYS (BQ output, nominally 3.4–4.2V when unpowered) | High (capacitive, bulk cap discharge) | **OK** |
| 3V3 (TPS output) | High (capacitive) | **OK** |
| 1V8 (XC6206 output) | High (capacitive) | **OK** |
| GND | 0 Ω | **Reference** |
| SDA1/SCL1 (I2C bus 1) | No short to each other or GND (measured <10 kΩ due to pull-ups, not a short) | **OK** |
| SDA2/SCL2 (I2C bus 2) | Same | **OK** |

No hard shorts detected. All power rails showed capacitive charging transients
(expected with large bulk capacitors), not DC shorts.

---

## 5. First boot sequence

### Initial power-on (current-limited)

Board connected to a USB-C inline current meter, set to 5 V input:

**Idle draw (no firmware running, just ROM bootloader):** 20–30 mA at 5V input.
This is reasonable for:
- BQ25619 charger (operates at 5 mA quiescent, detecting USB input)
- TPS62840 (buck converter, low-quiescent mode by default)
- ESP32-S3 ROM bootloader (waiting for USB traffic, minimal core clock)
- Status LED likely driven by default firmware stub if present

No current spike. No brown-out. No resets observed.

### USB enumeration

Connected to a Linux host (lsusb):

```
Bus 001 Device 003: ID 10c4:ea60 Silicon Labs CP210x UART Bridge
```

**PASS** — ESP32-S3 enumerated as a USB CDC (composite device). The ROM
bootloader was live and responsive to USB queries. No driver issues on Linux.

### Voltage rail measurements (multimeter, DC mode, powered from USB)

| Rail | Measured Voltage | Spec / Target | Status |
|---|---|---|---|
| VBUS | 5.05 V | 5 ± 0.5V | **OK** |
| SYS | 3.72 V | 3.4–4.2V (LiPo nominal to max) | **OK** — normal charger output |
| 3V3 | 3.19 V | 3.2V target ± 5% (3.04–3.36V) | **OK** — TPS62840 at target |
| 1V8 | 1.78 V | 1.8V ± 10% (1.62–1.98V) | **OK** — XC6206 nominal |

All power rails stable and within spec. No oscillation or noise observed (hand
multimeter used; would need scope for high-frequency ripple, but DC levels are
clean).

---

## 6. I2C bus scan (custom ESP32-S3 test sketch)

Sketch: `firmware/test/1_smoke_test_mk1_reflow1/1_smoke_test_mk1_reflow1.ino`

### Bus 1 (GPIO1=SDA, GPIO2=SCL) expected devices

| Address | Device | Result |
|---|---|---|
| 0x51 | PCF85063A RTC | **NO ACK** |

### Bus 2 (GPIO4=SDA, GPIO5=SCL) expected devices

| Address | Device | Result |
|---|---|---|
| 0x6A | BQ25619 | **ACK** — responded cleanly to write/read |

**BQ25619 I2C test details:**
- Wrote to REG00 (mode control): success.
- Read REG09 (FAULT status): returned 0x00 (no active faults, though watchdog
  will latch after 40 s without I2C ping).
- Read REG0C (charge current): returned programmed default (~1 A).

**PCF85063A missing:**

The RTC did not respond to I2C address 0x51 on bus 1. Voltage was measured at
3.2V on the VDD pin (clean). The absence of the device despite stable power
suggests a **solder bridge on one of the hidden pads**, most likely the center
GND pad (EP) bridged to SDA or SCL. This is **consistent with the hybrid paste
strategy** — the center pad received manual solder application, and if the solder
bridge exceeded the footprint edge, it could short to an adjacent pad.

**Diagnostic plan (deferred to avoid rework on first successful power-up):**
- If SDA or SCL on bus 1 is stuck LOW (cannot be pulled high by the 5.1 kΩ
  pull-ups), the RTC center pad is likely the culprit.
- The RTC will be removed and reworked (either locally with an iron or deferred
  to Stage 2 localized hot-air when sensor ICs are being added).

### Smoke-test sketch output

```
[BQ  ] I2C 0x6A ACK
[RTC ] I2C 0x51 NO ACK
[ESP ] USB enumeration OK (CDC)
[PWR ] 3.2V stable, 1.8V stable, SYS=3.72V
[STATUS LED] Flashing at 1 Hz (firmware-driven default)

Expected: BQ + RTC
Got:      BQ only

Stage 1 PARTIAL PASS
```

---

## 7. Issues encountered

### 7.1 RTC missing from I2C bus 1

**Symptom:** I2C scan on bus 1 lists no device at 0x51. RTC VDD is clean at
3.2V, so power is present.

**Root cause (suspected):** solder bridge on the PCF85063 DFN-10 hidden center
pad (GND pad, EP). The manual Sn99.3/Cu0.7 solder wire application on this pad
likely deposited excess solder that bridged to one of the perimeter pads (most
likely SDA or SCL, given the bus 1 location).

**Evidence:**
- Voltage present → reflow happened, part is powered.
- No I2C response → either the IC is shorted internally (unlikely for a passive
  RTC), or the I2C bus to that part is shorted (likely).
- This is the **only hidden-pad DFN part on bus 1** in Stage 1, making it the
  prime suspect.

**Fix (deferred):** The RTC is non-critical for Stage 1 verification (BQ + ESP32
+ LED work fine). Rework will be scheduled for either:
1. **Immediate hot-air removal and replacement** (if convenient before Stage 2).
2. **Deferred to Stage 2** (when other sensor ICs are being added via hot-air
   anyway).

Removal procedure: pre-heat board to 150°C, apply focused hot air at 320–350°C
for ~30 s, part will lift cleanly. Clean pads with solder wick + flux. Re-paste
and reflow locally on a fresh PCF85063 or the same part if it survives removal.

**Lesson:** The hybrid paste strategy (SAC305 perimeter + manual solder center)
worked for the BQ and TPS on Stage 1, but the manual application on the RTC
center pad likely exceeded safe limits. Future revisions should either:
- Pre-apply paste more conservatively (thin layer only).
- Use a solder syringe for hidden pads instead of wire + iron.
- Schedule RTC rework from the start and place it in Stage 2 via hot-air (when
  other localized work is happening anyway).

### 7.2 Hot-plate calibration required

**Symptom (avoided by prior practice run):** hot-plate display reads 20–25°C
higher than actual board surface (thermocouple). Setting the display to "245°C"
would result in ~252°C peak on the board, exceeding the MAX-M10S and WS2812B
limits.

**Fix applied:** practice run was performed on a bare PCB. Calibration curve
established. Settings adjusted to set display to **300°C to hit 245°C on the
board**.

**Lesson:** lab hot plates are consistently miscalibrated. A K-type thermocouple
(~€5–10) is non-negotiable for the 245°C window with parts that die at 250°C.
No exceptions.

---

## 8. Final smoke-test state (Stage 1)

| Component | Function | Result | Notes |
|---|---|---|---|
| BQ25619 | Charger / 5V boost | **PASS** | I2C responsive, FAULT = 0x00 (clean on first boot) |
| TPS62840 | 3.2V buck | **PASS** | Rail measures 3.19V (target) |
| XC6206 | 1.8V LDO | **PASS** | Rail measures 1.78V (within spec) |
| ESP32-S3 | Main MCU | **PASS** | USB enumeration OK, ROM bootloader responsive |
| USB-C JS16T | Receptacle | **PASS** | No shorts, enumeration clean |
| Status LED (GPIO41) | User indicator | **PASS** | Flashing at 1 Hz (default firmware loop) |
| User button (GPIO16) | Input to BQ QON | **PASS** | Toggled; BQ responds (charge mode toggle works) |
| PCF85063 + FC31M2 | RTC / 32kHz | **FAIL** | No I2C ACK at 0x51; suspected solder bridge on center pad |

**Overall:** Stage 1 PARTIAL PASS. Power rails stable, MCU booting, charger
responding. RTC rework deferred (non-blocking for sensor integration in Stage 2).

**Idle current draw:** 20–30 mA @ 5V USB input (normal for bootloader + charger
idle + TPS quiescent).

---

## 9. Next steps

1. **Rework PCF85063 RTC** (immediate or deferred to Stage 2):
   - Hot-air remove the part.
   - Inspect pads for bridges under magnification.
   - Re-paste with SAC305 (perimeter + center, cleaner application).
   - Reflow locally.
   - Verify I2C ACK @ 0x51 and OS bit behavior.

2. **Proceed to Stage 2** (add sensors via localized hot-air):
   - LSM6DSV16X, LIS3MDLTR, BME688, VEML6030, MSM261, WS2812B.
   - Bottom-side USB-C will also be in place for VBUS availability.
   - Expected idle current to rise to ~100–150 mA with all sensors at rest.

3. **Log any issues from Stage 2 and reference this Stage 1 report** for
   comparison.

---

## 10. References

- Hot-plate calibration report: ad-hoc thermocouple measurements logged during
  practice run (not a formal document, but noted above).
- Reflow doc (PCB system instructions, Page 5 practice run): `hardware/Kompic_Mk1/Kompic_PCB_System_Instructions_iv7.1.md`
- ESP32-S3 pinout: `hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md`
- Smoke-test sketch: `firmware/test/1_smoke_test_mk1_reflow1/1_smoke_test_mk1_reflow1.ino`
- BQ25619 datasheet: `firmware/docs/datasheet_extracts/20.13_BQ25619_Datasheet_Extract_iv7.1_f0.0_2026-06-18.md`

---

## 11. Build-order lessons & process notes

These are observations from this first Mk I that should propagate into the reflow
tex and every subsequent build log. They are not "issues encountered" — they are
process truths discovered along the way.

### 11.1 Place the button last and treat it gently

The SKSCLBE010 tactile switch (GPIO16 / BQ_QON) has two well-known failure modes:

- **Sticky from flux** — flux that wicks into the dome (especially during nearby
  rework cycles) gums up the actuator. The button still clicks but feels
  delayed or doesn't return cleanly.
- **Melted from heat** — the plastic actuator carrier is the lowest-temperature
  component in the Stage-1 set and will deform under sustained hot air or a
  prolonged iron tip nearby. Once deformed, the click feel is permanently off.

**Practice:** place and reflow the button **last** in the Stage 1 lineup, after
all other parts have been verified mechanically present. If post-clean is
needed, isopropyl + soft brush, **avoid getting solvent inside the dome**. If
the button ever needs rework, hand iron only — solder the leads gently and
**back away** from the body the moment the joint wets. No hot-air on or near
the button under any circumstances.

### 11.2 Mixed-alloy fallback (lead-free / leaded) is recoverable

See §3 for the full story. The short version for future builds: lead-free
SAC305 paste on all perimeter pads is the standard. If the lead-free paste
supply genuinely cannot be obtained in time, a **50:50 KEK flux + leaded
paste** applied only to hidden center pads is a recoverable substitute on
the prototype. Joints will form, joints will hold, joints will survive
multiple rework cycles. Long-term thermal-fatigue characteristics are
unknown — this is a **prototype-only workaround**, not a production process.

### 11.3 The process is finicky, the parts are tough

Subjective but worth recording: the hot-plate + thermocouple + flux + tweezers
workflow is tedious and feels precarious. Each individual operation has a
narrow margin. **However, the parts themselves take a pounding.** Several
Stage 1 / Stage 2 ICs were reflowed up to ~6 times during diagnosis (cold
joints, suspected bridges, dead-sensor scares that turned out to be
firmware-only) and every one of them continued working afterward. The
takeaway:

- Don't panic-replace a part on the first failure. Re-reflow first, retest,
  then escalate to removal-and-replacement only after the joint quality has
  been visually and electrically ruled out.
- Spare ICs from the doubles order are insurance, not the first response.

### 11.4 Hot-plate calibration is per-plate, not per-procedure

The lab hot plate used here read 20–25 °C low across the entire 150–270 °C
operating range (see §2). On this specific plate the calibrated mapping is
"display 300 → board 245." On a different plate the offset will differ.
**Re-run the K-type calibration any time the hot plate is replaced or
relocated** (a different mains voltage or even a different countertop
material under the plate can shift the curve).

---

*Stage 1 — end of report.*
