# Mk1b iv8.0 — Fix Application Log

**Rev:** Mk1b iv8.0
**Started:** 2026-08-22
**Log updated:** 2026-08-22 (fix application pass 2 — restructured circuit-side / PCB-side)
**Scope:** Same PCB, same BOM (all named ICs). JLCPCB assembles **0402 passives only**. Ivan hand-solders every IC on arrival — the "part-by-part heat-gun rework" cycle is dead. Mk1b is a single clean build pass.
**Goal:** Fold every bench-proven iv7.1 fix into the board itself, unblock the display, pull in the MAX17048 fuel gauge. Full Mk2 industrial-design pass stays deferred.

Legend: ✓ done · ✗ closed as N/A / bad-advice · ⚠ open · → deferred to Mk2

---

## Motivation — the six iv7.1 blockers

- **F-DISP-1** AMOLED FPC receptacle mirrored (display bring-up impossible on iv7.1)
- **F-FLASH-1** BSS138W die-level leak → BC847C swap
- **F-VIO-1** LSM/BME/mic VDDIO on +1V8 back-fed the rail via ESD clamps
- **F-LDO-1** XC6206 library symbol Vin/Vout swap delivered **2.8 V (backfed 3V3 through the reversed LDO) into a 1V8-rated pin**, killing a MAX30101. Not a 3V3-rail-health issue — 3V3 was fine.
- **F-FLASH-2** Flashlight FET wired source-follower (LED could never conduct)
- **F-BQ-1** BQ25619 R12 (5k1 to +3V3 on /QON) trapped ship mode

---

# PART A — CIRCUIT-SIDE (schematic / BOM / component values / net topology)

## A.1 Power — BQ25619 / TPS62840 / XC6206

| ID | Fix | Status | Note |
|---|---|---|---|
| F-BQ-1 | R12 = DNP | ✓ | BQ internal 200 kΩ pull to V_BAT is enough |
| F-BQ-2 | C15 REGN 1 µF → 4.7 µF | ✓ | 4.7 µF exact — 10 µF is off-datasheet, no upside |
| F-BQ-3 | C10 BTST 100 nF → 47 nF | ✓ | Applied |
| F-BQ-4 | R10 PMID_GOOD reroute | ✗ | Closed — PMID_GOOD is a bare pad in your schematic, no R10 exists |
| F-BQ-5 | PSEL to GND | ✓ | Confirmed |
| F-BQ-6 | BATSNS to BAT+ | ✓ | Confirmed direct to LiPo+ |
| F-BQ-7 | JP1 audit | ✗ | Closed — JP1 is PGND↔GND local star ground, not SYS↔BAT bridge |
| F-BQ-8 | /INT to GND | ✓ | Tied |
| F-BQ-9 | TS thermistor 5.36k / 31.6k → 5.1k / 30k (E24) | ✓ | Trip points shift ~2-3 °C in the safer direction. Topology: REGN → R14 (5.1k) → TS_node → R26 (30k) ∥ NTC 10k → GND |
| F-TPS-1 | MODE tied GND (power-save) | ✓ | |
| F-TPS-2 | STOP tied GND | ✓ | |
| F-LDO-1 | XC6206 symbol Vin/Vout fixed | ✓ | Fixed in schematic. Library-side refresh deferred to Mk2 (XC6206 moved position in library since last touch). Failure-mode reminder in header. |

## A.2 ESP32-S3

| ID | Fix | Status | Note |
|---|---|---|---|
| F-ESP-1 | EN RC: R33 10 k series + cap → GND | ✓ | Cap bumped 100 nF → **1 µF** for 10 ms brown-out ride-through, matches datasheet |
| F-ESP-2 | Master pinout v20 GPIO assignments unchanged | ✓ | Firmware is stable against this pinout |

## A.3 Sensor VDD / VDDIO domain split

| ID | Fix | Status | Note |
|---|---|---|---|
| F-VIO-1 | LSM6DSV16X | ✓ | **Everything on 3V3 except VDD which stays on 1V8.** 3V3 to pin 5 VDDIO, pin 1 SDO/SA0 (address bit), pin 10 OCS_Aux (unused, HIGH), R19 5k1 CS pull-up. VDD 1V8 within spec (1.71-3.6 V). |
| F-VIO-2 | BME688 | ✓ | VDDIO 3V3, VDD 1V8 |
| F-VIO-3 | MSM261DGT003 mic | ✓ | Whole VDD on 3V3 (spec min 3.3 V — cannot run on 1V8) |
| F-VIO-4 | +1V8 rail kept | ✓ | MAX30101 V+ demands 1V8; sensors that support 1V8 VDD run there for rail savings, VDDIO always on 3V3 |
| F-VIO-5 | **LIS3MDL both VDD and VDDIO on 3V3** | ✓ | Both rails 3V3, 10 µF + 100 nF decoupling. LIS spec VDD_min = 1.9 V and XC6206P182 output can drop to 1.764 V worst-case — LIS is the outlier that must not run on 1V8 |
| F-MIC-1 | Mic decoupling 100 nF + 10 µF | ✓ | C46 10 µF added |

## A.4 Flashlight

| ID | Fix | Status | Note |
|---|---|---|---|
| F-FLASH-1 | BSS138W → BC847C | ✓ | Drop-in SOT-23. Vetted low-leakage FET returns in Mk2. |
| F-FLASH-2 | Redraw as low-side switch | ✓ | +5V → R25(100R) → LED → BC847C collector → emitter → GND |
| F-FLASH-3 | R_base (R32) → 3 kΩ | ✓ | β_forced ~125 at 100 mA LED — comfortable saturation |
| F-FLASH-4 | R26 47 Ω restored | ✗ | Closed — with BC847C + R25=100R, R26 as series is redundant. R26 designator now the TS-network 30k (F-BQ-9). |
| F-FLASH-5 | R24 base pull-down 10 k | ✓ | **Keep at 10 k** — guaranteed-OFF at boot when GPIO41 is Hi-Z. Waste when ON is 70 µA, negligible |

## A.5 SD — electrical (pull-ups, decoupling values)

| ID | Fix | Status | Note |
|---|---|---|---|
| F-SD-1 | CLK pull-up 10 k populated | ✓ | R34 = 10 k to 3V3 |
| F-SD-3 | Pull-ups on CLK/CMD/DAT0/DAT1/DAT2/DAT3 | ✓ | **All four DAT + CMD + CLK at 10 k to 3V3.** SD spec allows 10-100 kΩ; 10 k is standard and reliable at ESP32-S3 SDMMC speeds. Stage_8 §7's "5.1k default" recommendation is closed as **bad advice for this design** — over-specified for a use case that runs 1-bit at ~20 MHz. |
| F-SD-2 (electrical part) | Bulk decoupling 1 µF + 100 nF at SD socket | ✓ | C47 1 µF added; placement note in B.2 |
| — | 1-bit vs 4-bit SDMMC throughput (ESP32-S3) | — | 1-bit ≈ 2 MB/s sustained write, 4-bit ≈ 8-10 MB/s. Sensor logging at kB/s → 1-bit plenty |

## A.6 Fuel gauge — Mk2 pull-in

| ID | Item | Status | Note |
|---|---|---|---|
| P-FG-1 | **MAX17048G+T10** fuel gauge, TDFN-8, I²C 0x36 on bus 1, ~€1 | ⚠ | Wiring: VDD on BAT+ (pre-regulator, direct LiPo), 0.1 µF decoupling close to VDD, ALRT unconnected. Also serves as the Vbat-sense replacement — no ADC divider on Mk1b. Not yet placed in schematic. |

## A.7 AMOLED connector — schematic side

| ID | Fix | Status | Note |
|---|---|---|---|
| F-DISP-1a | Connector **symbol** pin mirroring | ✓ | Symbol pin 13 now = GND net, pin 1 now = 3V3 net (matches the physical FPC). PCB verification pending — see B.1. |

## A.8 Closed / deferred — circuit side

| ID | Item | Status | Note |
|---|---|---|---|
| F-USB-1 | Wire both D+/D- pairs | ✗ | **Fake news** — both pairs were already wired correctly on iv7.1. USB failure was a physically loose / broken receptacle from bench abuse, not schematic |
| F-QVAR-1 | Series AC-coupling caps | ✗ | QVAR path depopulated / deprecated |
| F-QVAR-2 | 10 MΩ body-bias resistor | ✗ | Same |
| F-VBAT-1 | Vbat divider to ADC1 pin | ✗ | No divider on Mk1b — MAX17048 (P-FG-1) replaces it. ADC-based Vbat returns in Mk2 with BQ25896-with-ADC |
| F-MAG-1 | Relocate LIS away from LRA | ✗ | Fake news — dead-channel was rework-magnetization, not LRA proximity. Location fine. (See F-DOC-1 for degauss procedure note) |

---

# PART B — PCB-SIDE (footprint / layout / silk / mechanical / documentation)

## B.1 AMOLED receptacle — footprint / PCB verification

| ID | Fix | Status | Note |
|---|---|---|---|
| F-DISP-1b | Verify netlist maps correctly to physical FPC after PCB update-from-schematic | ⚠ | Symbol pin-mirroring done (A.7). After updating the PCB from the schematic, physically lay the CO5300 FPC across the footprint contacts-down and verify pin 1 (3V3), pin 13 (GND), pin 24 lands where expected. |
| F-DISP-2 | Re-audit ordered part suffix (T vs B contact-face variant) | ⚠ | Pin exact suffix in BOM once verified |
| F-DISP-3 | Pin-1 silkscreen dot next to J1 | ⚠ | Also covered by general F-ASM-1 |
| F-DISP-4 | Test-pads on 14 named display nets adjacent to J1 | ⚠ | QSPI ×6, TP-SDA/SCL/INT/RST, DISP_RST, TE, +3V3, GND — lets us DMM-verify the pinout before mating the panel |

Sketch `firmware/arduino/15_amoled_touch_test/15_amoled_touch_test.ino` is preserved and will be Mk1b's first-flash for display bring-up.

## B.2 SD layout

| ID | Fix | Status | Note |
|---|---|---|---|
| F-SD-2 (PCB part) | 1 µF X7R + 100 nF placed on same face as SD socket, close to socket VDD pin | ⚠ | Component values added in A.5; placement pending during layout |
| F-SD-4 | Continuous GND under SDMMC bus + stitching vias | ⚠ | Check during routing |
| F-SD-5 | Route DAT3 to a real GPIO | ✗ | Closed — no free GPIO |
| F-SD-6 | Molex 472192001 flip-up socket (same footprint) | → | Deferred to Mk2 |

## B.3 Assembly rules — silk / pad extension / BOM notes

| ID | Rule | Status | Note |
|---|---|---|---|
| F-ASM-1 | **Pin-1 silkscreen dot** on every FPC and every QFN/LGA | ⚠ | Highest ROI item — would have caught F-DISP-1 on iv7.1 |
| F-ASM-2 | Perimeter pad extension 0.4-0.5 mm outward on LGA/DFN parts (LSM, LIS, BME, TPS62840, XC6206) | ⚠ | Enables hot-air rework to catch pads from outside |
| F-ASM-3 | BOM note: `ESDALCL5-1BM2 reads OC in DMM diode mode when healthy` | ⚠ | Cheap doc-only step |

## B.4 Case bond — Mk2 cheap pull-in

| ID | Item | Status | Note |
|---|---|---|---|
| P-CASE-1 | 2×2 mm bare-copper GND pad where the future titanium case shell will contact | ⚠ | Costs nothing on the PCB, future-proofs the case bond |

## B.5 Closed / deferred — PCB side

| ID | Item | Status | Note |
|---|---|---|---|
| F-USB-2 | Reinforced / mid-mount USB-C footprint | → | Deferred to Mk2. iv7.1's port fatigue was rework-cycle induced, not the footprint per se — the "no more heat-gun abuse" build discipline addresses the underlying cause |

## B.6 Documentation / procedural

| ID | Item | Status | Note |
|---|---|---|---|
| F-ASM-4 | **Pre-fab checklist file** — mandate physical FPC / polar-part lay across every KiCad footprint before Gerber export | ⚠ | Highest-priority process fix. Seed with F-DISP-1 as the founding case study |
| F-DOC-1 | Magnetometer degauss procedure | ✓ (note-only) | **Fix for stuck / saturated LIS channel**: rotate a small magnet in a circular pattern near the LIS, then slowly withdraw the magnet away from the board while continuing to rotate it. Decays magnetic bias in the sensor's core below the reset threshold. |

---

# Assembly note

JLCPCB assembles 0402 passives only. All ICs (ESP32-S3-WROOM-1U, BQ25619, TPS62840, XC6206, MAX-M10S, MAX30101, LSM6DSV16X, LIS3MDLTR, BME688, TMP117, PCF85063A, DRV2605L, MSM261DGT003, MAX17048, BC847C, USB-C, SD socket, receptacle J1, WS2812B-2020) hand-soldered by Ivan on arrival in a single clean build pass — no more part-by-part rework cycles.

BOM must be split clean: `JLC-assembles` (0402 R/C only) vs `Ivan-hand-solders` (everything else). No mixed-tier parts.

---

# Remaining actions before Gerber export (ordered by priority)

1. ⚠ **P-FG-1** — MAX17048 fuel gauge placed and wired (VDD on BAT+, 0.1 µF decoupling, ALRT NC, I²C bus 1). *Circuit-side, not yet drawn.*
2. ⚠ **F-DISP-1b** — after PCB update-from-schematic, physically lay CO5300 FPC across footprint contacts-down and verify the netlist.
3. ⚠ **F-DISP-3, F-DISP-4** — pin-1 silk dot at J1, test-pads on 14 named display nets.
4. ⚠ **F-ASM-4** — seed the pre-fab checklist file with F-DISP-1 as founding case study.
5. ⚠ **F-ASM-1** — silk pin-1 dots on every FPC / QFN / LGA (this closes F-DISP-3 by rule).
6. ⚠ **P-CASE-1** — case-to-GND bond pad.
7. ⚠ **F-ASM-2, F-ASM-3** — perimeter pad extensions + ESD-diode BOM note.
8. ⚠ **F-SD-2 (PCB part), F-SD-4** — bulk cap placement + SD GND-plane / stitching-via check.

Everything else in Part A is done or explicitly closed.

Go.
