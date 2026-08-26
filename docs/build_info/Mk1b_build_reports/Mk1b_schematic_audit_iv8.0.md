# Mk1b iv8.0 — Schematic Audit vs Mk1b_fix_list.md

**Date:** 2026-08-23
**Sources:** `hardware/Kompic_Mk1b/Kompic-Mk1b_Schematic_iv8.0/Kompic_Mk1b.pdf` (7 pages) cross-referenced against `docs/build_info/Mk1b_build_reports/Mk1b_fix_list.md`
**Method:** Every F-* / P-* item checked against the drawn schematic.

Legend: ✓ correctly applied · ⚠ verify / minor discrepancy · ✗ NOT applied · → deferred (as expected)

---

## Top-line verdict

Most fixes are correctly on the schematic. Nothing is silently broken. **Seven items need eyeball-verification before Gerber export** — mostly minor labeling / value questions plus a possible designator collision.

---

## Part A — Circuit-side cross-reference

### A.1 Power (BQ25619 / TPS62840 / XC6206)

| ID | Expected | Observed on schematic | Verdict |
|---|---|---|---|
| F-BQ-1 | R12 DNP | R12 not present in the button network on Sheet 3 (only R13 10k + C8 100nF around SW1) | ✓ |
| F-BQ-2 | C15 REGN = 4.7 µF | C15 labeled `4u7F` on REGN pin 22 side | ✓ |
| **F-BQ-3** | **C10 BTST = 47 nF** | **C10 labeled `4u7F` (= 4.7 µF) between BTS pin 21 and SW node** | **⚠ VERIFY — reads as 4.7 µF, not 47 nF. Either a labeling error or the fix was applied to C15 only, not C10. Zoom in and confirm; if truly 4.7 µF, change to 47 nF.** |
| **F-BQ-4** | R10 closed as "no connection to PMID_GOOD" | **R10 10k IS drawn in the schematic near PMID_GOOD, with TP2 test point** | **⚠ VERIFY — doesn't match your comment "PMID_GOOD is just a pad, no connection". Confirm what R10's top connects to. If VBUS: fine for 5V-USB-only. If REGN: F-BQ-4 fix applied. If GND: makes no sense on an open-drain output.** |
| F-BQ-5 | PSEL → GND | PSEL tied to GND | ✓ |
| F-BQ-6 | BATSNS → BAT+ | BATSNS on pin 10 → BAT net | ✓ |
| F-BQ-7 | JP1 net-tie = PGND↔GND star | "Net-tie" label visible between PGND and GND locally | ✓ |
| F-BQ-8 | /INT → GND | INT tied to GND | ✓ |
| F-BQ-9 | TS network R14=5.1k / R26=30k / NTC=10k | R14 5k1 in REGN→TS path, R26 30k ∥ TH1 NTC 10k to GND — topology matches TI app note | ✓ |
| F-TPS-1 | MODE → GND | MODE tied GND on TPS62840 | ✓ |
| F-TPS-2 | STOP → GND | STOP tied GND | ✓ |
| **—** | R15 sets Vout | **R15 symbol says 100 k, note next to it says "102k for 3.2V operation"** | **⚠ VERIFY — 100 k gives ≈3.3 V, 102 k gives 3.2 V. Confirm which value is actually going onto the board. If you want 3.2 V for the R15-availability reason, R15 must be 102 k.** |
| F-LDO-1 | XC6206 symbol Vin/Vout fixed | Pin 3 = Vin ← 3V3, pin 2 = Vout → 1V8, pin 1 = GND. Schematic annotation says "**library symbol NOT fixed!**" (deferred to Mk2 per fix list). Wiring is correct locally. | ✓ (library defer noted) |

### A.2 ESP32-S3

| ID | Expected | Observed | Verdict |
|---|---|---|---|
| F-ESP-1 | R33 10 k series + 1 µF cap on EN | R33 10 k from 3V3 → EN, C42 1 µF from EN → GND | ✓ |
| F-ESP-2 | Master pinout v20 assignments unchanged | Every GPIO on the ESP32-S3 symbol matches the v20 master pinout (SDA=1, SCL=2, QSPI-RESET=3, SDA_bus2=4, SCL_bus2=5, TP-INT=6, MAX_INT=7, LSM_INT1=8, QSPI ×6 on 9–14, RTC_INT=15, BQ_BUTTON=16, M10 UART 17/18, D–/D+ 19/20, EC_SigA=21, SD_DAT1=26, DRV_EN=27, SD_DAT2=28, SD_DAT3/CD=29, SD_DAT0=38, SD_CMD=39, SD_CLK=40, GPIO_FLASHLIGHT=41, Sig_LED_Din=42, EC_SigB=43, TP-RST=44, QSPI_TE=45, TimePulse=46, Mic_CLK=47, Mic_Dout=48) | ✓ |

### A.3 Sensor VDD / VDDIO domain split

| ID | Expected | Observed | Verdict |
|---|---|---|---|
| F-VIO-1 | LSM6DSV: everything 3V3 except VDD=1V8 | Pin 1 SDO/SA0, pin 4 SDO_Aux, pin 5 Vdd_IO, pin 10 OCS_Aux all → 3V3. Pin 12 CS via R19 5k1 → 3V3. Pin 14 Vdd → 1V8 with C22 4.7 µF + C41 100 nF | ✓ |
| **—** | LSM inline note | **Note in schematic reads "SDO/SA0: Tie to VDD_IO (1.8V) → address 0x6B" — text is stale from pre-fix state. Wiring is to +3V3 (which IS VDD_IO now), so behaviorally correct, but the note misleads a future reader.** Also **"Address 0x51" text label under LSM is wrong** — LSM6DSV is at 0x6A/0x6B, 0x51 is the RTC. | ⚠ tidy up labels |
| F-VIO-2 | BME688: VDDIO=3V3, VDD=1V8 | Pin 6 VDDIO → 3V3, pin 8 VDD → 1V8 | ✓ |
| F-VIO-3 | Mic VDD=3V3 | MSM261 pin 1 VDD → 3V3 | ✓ |
| F-VIO-4 | +1V8 rail kept | XC6206P182 still on the schematic, +1V8 net used by LSM VDD, BME VDD, and BME688 note. Also serves the MAX30101 V+ on the daughterboard. | ✓ |
| F-VIO-5 | LIS3MDL: both VDD and VDDIO on 3V3 | Both pins tied to 3V3 with C27 4.7 µF + C28 100 nF | ✓ (caps are 4.7 µF, not the 10 µF you mentioned — functionally equivalent) |
| F-MIC-1 | C46 10 µF + 100 nF | C46 10 µF + C31 100 nF on mic VDD | ✓ |

### A.4 Flashlight

| ID | Expected | Observed | Verdict |
|---|---|---|---|
| F-FLASH-1 | BSS138W → BC847C | Symbol still labeled **U12 BSS138W_C890266** with schematic note "BSS138 batch is dead. USE: BC847C as temp drop-in replacement. R32 then needs to be 3k instead of 47 with FET" | ⚠ BOM change needed — LCSC part in `0_LCSC_parts.txt` must be swapped to a BC847C SOT-23 P/N before order |
| F-FLASH-2 | Low-side switch, LED anode → +5V → R25 → LED → drain | +5V → R25 100R → LED2 anode → cathode → U12 drain → source → GND | ✓ |
| F-FLASH-3 | R32 base = 3 k | R32 3k from GPIO_FLASHLIGHT → base | ✓ |
| F-FLASH-4 | R26 removed from flashlight | Not present in flashlight sub-circuit | ✓ |
| F-FLASH-5 | R24 10 k pull-down | R24 10k from base → GND | ✓ |

### A.5 SD

| ID | Expected | Observed | Verdict |
|---|---|---|---|
| F-SD-1 | SD_CLK pull-up R34 10 k → 3V3 | R34 10 k on SD_CLK | ✓ |
| F-SD-3 | 10 k pull-ups on all six SD lines | Six 10 k pull-ups visible: R34 (CLK), R1 (DAT2), R2 (DAT3), R26 (CMD), R27 (DAT3/CD), R30 (DAT0). All go to 3V3. Plus R4 10 k on CD. | ✓ (with the R26 caveat below) |
| **—** | Designator uniqueness | **R26 appears twice in the schematic**: once on **Sheet 2 as the SD_CMD 10 k pull-up**, and once on **Sheet 3 as the 30 k TS-thermistor bottom leg** (the repurposed flashlight designator from F-FLASH-4). KiCad ERC will flag this. | **⚠ FIX — one of them must be renamed. Suggest keeping the SD one as R26 (older) and renaming the TS one (e.g., R37).** |
| F-SD-2 (values) | 1 µF + 100 nF at SD socket VDD | C40 100 nF + C47 1 µF | ✓ |

### A.6 Fuel gauge (P-FG-1)

MAX17048G+T10 (U3) on Sheet 3 — checked pin-by-pin against the datasheet Simple Circuit Diagram:

| Pin | Datasheet | Observed | Verdict |
|---|---|---|---|
| 1 CTG | GND | GND | ✓ |
| 2 CELL | Battery+ | BAT | ✓ |
| 3 VDD | Battery+ | BAT (tied to CELL, C26 100 nF local decouple to GND) | ✓ |
| 4 GND | GND | GND | ✓ |
| 5 QSTRT | GND (unused) | GND | ✓ |
| 6 SCL | I²C | SCL_bus2 | ✓ |
| 7 SDA | I²C | SDA_bus2 | ✓ |
| 8 ALRT | float or 3V3 pull-up | Left floating (label "Reserved / left floating") | ✓ |
| 9 EP | GND | GND | ✓ |

Bus 2 population = BQ25619 (0x6A) + MAX17048 (0x36) + DRV2605 (0x5A) — three devices, no address collision. Battery bulk cap C9 10 µF at BT1. Fuel gauge is textbook-correct.

### A.7 AMOLED connector — schematic side

| ID | Expected | Observed | Verdict |
|---|---|---|---|
| F-DISP-1a | Symbol pins mirrored so pin 13 = GND and pin 1 = 3V3 net | J1 `Conn_02x12_OK-14F024-04_v1` has 24 pins + 27/28 (mounting shells). Net assignments visible on the schematic. | ⚠ **eyeball at high zoom** — from the exported PDF I cannot 100% verify every pin's net against your intended mapping. Recommend printing the J1 sub-schematic 1:1 and laying the FPC across it before Gerber export (which is exactly F-ASM-4). |

### A.8 Closed items — verified as closed

| ID | Expected state | Observed | Verdict |
|---|---|---|---|
| F-USB-1 | Both D+/D- pairs wired | JS16T pins A6/B6 tied to D+, A7/B7 tied to D-, 5k1 on CC1 to GND | ✓ |
| F-QVAR-1/2 | Electrodes depopulated | Both CN1 and CN2 crossed out, D5/D6 crossed out on both sheets 4 and 7. R16/R18/C18/C21 remain in schematic but electrodes are DNP. | ✓ |
| F-VBAT-1 | No divider | No Vbat divider anywhere; MAX17048 owns that job | ✓ |
| F-MAG-1 | LIS3MDL location unchanged | Sheet 4 — not moved | ✓ (per your "fake news" note) |

---

## Part B — PCB-side (cannot verify from schematic alone)

| ID | Item | Status |
|---|---|---|
| F-DISP-1b | FPC-vs-footprint physical verification | ⚠ pending — do it right after "Update PCB from Schematic" |
| F-DISP-2 | Receptacle part suffix in BOM | ⚠ pending |
| F-DISP-3, F-DISP-4 | Pin-1 silk + 14 test-pads at J1 | ⚠ pending (layout) |
| F-SD-4 | GND plane / stitching vias | ⚠ pending (layout) |
| F-ASM-1, ASM-2, ASM-3, ASM-4 | Silk / pad-extension / BOM note / pre-fab checklist | ⚠ pending (layout + doc) |
| P-CASE-1 | Case-to-GND bond pad | ⚠ pending (layout) |
| F-USB-2 | Reinforced USB-C footprint | → deferred to Mk2 (as planned) |
| F-DOC-1 | Degauss procedure noted | ✓ (documented in fix list) |

---

## Findings requiring action (ordered)

1. **F-BQ-3 C10 value** — labeled 4u7F on the schematic; datasheet requires 47 nF. Zoom in and confirm; correct if truly 4.7 µF. **HIGH priority — wrong BTST cap causes slower HSFET turn-on and reduced buck efficiency**, though usually still functional.
2. **R26 designator collision** — used twice (SD_CMD pull-up on Sheet 2, TS thermistor bottom leg on Sheet 3). Rename one before ERC. Suggest the TS-network one → R37.
3. **F-BQ-4 R10 mystery** — R10 10 k IS drawn on the schematic despite your comment that PMID_GOOD is a bare pad. Verify what R10's top connects to. If VBUS: OK for 5V-USB-only Mk1b, note it. If REGN: fix applied. If GND: doesn't make sense.
4. **R15 TPS62840 VSET** — 100 k on symbol vs 102 k in the sidecar note. 100 k ≈ 3.3 V, 102 k ≈ 3.2 V. Pick one and make symbol match note.
5. **F-FLASH-1 BOM swap** — schematic still says BSS138W_C890266; verify `0_LCSC_parts.txt` line for the flashlight FET has the BC847C LCSC ID before you order.
6. **LSM6DSV label tidy** — remove stale "Tie to VDD_IO (1.8V)" note and stray "Address 0x51" text. Real address is 0x6B (SDO tied HIGH).
7. **F-DISP-1a J1 pinout** — visually verify at high zoom that every panel-side net lands on the correct receptacle pin. Companion to F-DISP-1b physical FPC-lay check.

Nothing on this list is a showstopper — items 2 and 6 are cosmetic, 1/3/4/5 are single-value swaps. **Fix these before Gerber export.**
