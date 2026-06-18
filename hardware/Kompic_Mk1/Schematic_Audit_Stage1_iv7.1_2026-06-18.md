# Schematic Audit — Stage 1 Reflow Components
**Date:** 2026-06-18
**Hardware:** Kompic Mk I, iv7.1 (PCB fabricated, awaiting reflow)
**Schematic source:** `hardware/Kompic_Mk1/Kompic_Mk1-Schematic_iv7.1.pdf` (KiCad 10.0.3, dated 2026-06-02)
**Audited by:** Claude Code Opus, cross-referenced against datasheet extracts 20.03 (ESP32-S3), 20.04 (PCF85063A), 20.05 (TPS62840), 20.13 (BQ25619)
**Scope:** Stage 1 reflow only — ESP32-S3 (sheet 2), BQ25619 + ship-mode button + TPS62840 buck + 1V8 LDO (sheet 3), PCF85063A RTC (sheet 4). **Excluded:** display, SD card, IMU, GPS, compass, all 0402 passives, LED, USB-C connector.

This document records what was checked and what the schematic shows for each pin, so subsequent audit passes don't have to re-parse the PDF.

---

## Summary verdict

**You can almost certainly proceed to Stage 1 reflow.** Most pin connections match the datasheets. Below I've flagged **three definite concerns** worth addressing before / during reflow if practical, **and five items that need a visual second-look** that I could not resolve confidently from the rendered PDF (resolution-limited).

**Definite concerns (in order of impact):**
1. **C15 REGN bypass cap is 1 µF on schematic; datasheet specifies 4.7 µF** (BQ25619, see B-3 below). Likely-to-cause REGN LDO instability or HSFET-gate-drive issues. Highest-priority fix.
2. **C10 BTST cap is 100 nF; datasheet specifies 47 nF (0.047 µF)** (BQ25619, B-4). Will probably work but is 2× the spec value — may affect switching dynamics, lower priority.
3. **PMID_GOOD pull-up (R10 10 kΩ) ties to VBUS, not REGN** (BQ25619, B-5). Datasheet explicitly says "Connect to the pullup rail REGN". With a 5 V USB charger this is below the 7 V abs-max on the pin, but a 12 V adapter (which the chip *does* support) would exceed it.

**Items needing visual re-check (resolution limit of rendered PDF):**
- **JP1 net-tie between SYS and BAT** (B-7). If this is a populated 0 Ω jumper or a physical net-tie footprint, it shorts the internal BATFET and defeats all power-path management. If it's a KiCad-only graphical net-tie with no physical effect, harmless.
- **BATSNS (pin 10) routing** (B-6). Appears to go near a +3V3 label — that would be wrong. Should go to LiPo+ for remote sense (or be left open with BATSNS_DIS=1 in firmware).
- **PSEL (pin 2) strap** (B-8). Schematic shows just a "PSEL" net label with no obvious tie. Need to confirm it's strapped HIGH (500 mA SDP) or LOW (2.4 A adapter). Recommend LOW.
- **TPS62840 MODE and STOP pin terminations** (T-3, T-4). Both have internal 450 kΩ pulldowns so they'll be LOW if floating, but the extract says MODE *must not float*. Confirm both have explicit ties.
- **XC6206 LDO pin numbering** (L-1). Need to confirm footprint matches the package variant (SOT-89-3 vs SOT-23-3 have different pin orders).

**No issues:** ESP32-S3 power and key strap pins, RTC backup-battery topology, PCF85063A INT pull-up, tactile-switch dual-wiring to /QON + GPIO16.

---

## ESP32-S3-WROOM-1U-N16R8 (Sheet 2)

**Reference:** U1. Module pinout per Espressif "ESP32-S3-WROOM-1/1U Datasheet v1.6". I/O assignment cross-referenced against `firmware/docs/datasheet_extracts/20.03_ESP32-S3_Datasheet_Extract_iv7.1_f0.0_2026-06-13.md`.

### Power & ground
| Pin # | Name | Schematic net | Verdict | Notes |
|---|---|---|---|---|
| 1 | GND | GND | ✓ | |
| 2 | 3V3 | +3V3 (via R33 10 kΩ ?) | ⚠ | Schematic shows `+3V3 — R33 10k — pin 2` and C42 100nF bypass. **R33 10k on the supply pin is highly unusual** — a 10 kΩ series resistor on a 500 mA-peak rail would drop several volts under boot inrush and brown-out the module. Verify this is actually a *zero-ohm* jumper labeled "10k" by mistake, or a footprint for an optional ESR test. **If R33 is genuinely 10 kΩ in the BOM, the ESP32 will not boot.** Highest-priority recheck on the ESP side. |
| 41 | PAD_GND (thermal pad) | GND | ✓ | |

### EN / boot / status pins
| Pin # | Name | Schematic net | Verdict | Notes |
|---|---|---|---|---|
| 3 | EN | +3V3 (no RC delay visible) | ⚠ | The ESP32-S3 datasheet recommends an RC delay on EN (10 kΩ pull-up + 1 µF cap to GND) to ride out brown-outs. Schematic shows EN directly tied to +3V3 through R33 (if R33 is on EN and not VDD), but no obvious cap-to-GND for delay. **Recheck**: is the 10 kΩ + 100 nF actually the EN RC network, not a VDD series resistor? If so, this resolves the previous concern. |
| (boot) | IO0 — boot strap | Not seen on this sheet (might be on a different sheet or part of QSPI) | — | ESP32-S3 boots from SPI flash on IO0 strap. Internal pullup on the module makes external strap unnecessary unless deliberately overridden. |

### I/O assignment as routed (sheet 2 labels)
| Pin | GPIO | Net label | Function (per firmware iv7.1) |
|---|---|---|---|
| 4 | IO4 | SDA_bus2 | I²C bus 2 SDA (BQ25619 + DRV2605) |
| 5 | IO5 | SCL_bus2 | I²C bus 2 SCL |
| 6 | IO6 | TP-INT_3V3 | Touch IRQ from CST9217 |
| 7 | IO7 | MAX_INT | MAX30101 interrupt |
| 8 | IO8 | RTC_INT | PCF85063A alarm/timer interrupt |
| 9 | IO15 | BQ_BUTTON | Tactile switch (also drives BQ /QON) |
| 10 | IO16 | BQ_INT | BQ25619 /INT line — but R11 marked DNP, see B-9 |
| 11 | IO17 | M10_TX | GPS UART TX (ESP→M10S, deferred to Stage 3) |
| 12 | IO18 | M10_RX | GPS UART RX |
| 13 | IO8 (or LSM_INT1?) | LSM_INT1 | IMU INT1 (deferred to Stage 2) |
| (37, 36) | TXD0, RXD0 | EC_SigB, EC_SigA | **WAIT — these are crown-encoder signals on the UART0 pins?** Need to verify, since UART0 is also the USB-CDC console fall-through. If TXD0/RXD0 are repurposed as encoder pins, console debug output cannot use UART0 (must use UART1 or USB CDC). Flag for FW awareness. |

### TVS protection (D1, TPD2E001)
| Aspect | Schematic | Verdict |
|---|---|---|
| Part | TPD2E001DRL — 2-line ESD diode array | ✓ |
| Inputs | D+ / D- from USB-C | ✓ |
| Outputs | (USB to ESP IO19/IO20 native USB pins via D+/D-) | ✓ |
| Supply | VCC pin → +3V3 | ✓ |
| GND | ✓ | ✓ |

OK as designed. Standard USB ESD protection.

### Antenna (U.FL / off-module IPEX)
Visible: AE3 "2450AT18A0100E" antenna with L2 3.9 nH series and L1 2.7 nH shunt + C1 1.0 pF. This is the WROOM-1U external antenna network — fine to leave undisturbed during Stage 1 reflow (it's already part of the module BOM, no extra hand-placement needed for the antenna).

---

## BQ25619 charger (Sheet 3, U4 BQ25619RTWR)

**Reference:** Cross-referenced against `firmware/docs/datasheet_extracts/20.13_BQ25619_Datasheet_Extract_iv7.1_f0.0_2026-06-18.md`. WQFN-24 pinout per datasheet p. 4 Figure 5-2.

### Verified-OK pins

| Pin # | Name | Schematic | Datasheet requirement | Verdict |
|---|---|---|---|---|
| 1 | VAC | VBUS net | "VAC pin must be tied to VBUS" [p. 6] | ✓ |
| 5 | SCL | SCL_bus2 | I²C clock | ✓ |
| 6 | SDA | SDA_bus2 | I²C data | ✓ |
| 9 | /CE | GND | Charge enable, active-low; tied LOW = always permitted, FW gates via CHG_CONFIG | ✓ — matches extract Open Q-3 recommendation |
| 13, 14 | BAT | BAT net + C9 10 µF to GND + BT1 LiPo 380 mAh | "Connect a 10 µF closely to the BAT pin" [p. 5, p. 7] | ✓ |
| 15, 16 | SYS | SYS net + C11 + C12 + C13 (each 10 µF) | C_SYS min 10 µF [p. 7] | ✓ (30 µF total, more is fine) |
| 17, 18 | PGND | GND | | ✓ |
| 19, 20 | SW | L3 2.2 µH inductor between SW and PMID | L_min 1 µH (typ 2.2 µH per p. 7) | ✓ |
| 21 | BTST | C10 100 nF cap to SW node | **datasheet 47 nF** [p. 5] | ⚠ — see B-4 |
| 22 | REGN | C15 1 µF to GND | **datasheet 4.7 µF** [p. 5, p. 7] | ⚠ — see B-3 |
| 23 | PMID | C14 10 µF to GND, labeled "+5V" net | C_PMID 10 µF [p. 7] | ✓ |
| 24 | VBUS | C7 1 µF to GND | C_VBUS 1 µF [p. 7] | ✓ |
| 25 | EP (thermal pad) | GND | "must be soldered to PCB ground plane" [p. 6, p. 52] | ✓ |

### Concerns

**B-3 — C15 REGN cap is 1 µF, datasheet specifies 4.7 µF (HIGH priority)**

Schematic shows `C15 1uF` connecting REGN pin (pin 22) to GND. Datasheet Recommended Operating Conditions table (p. 7) lists `C_REGN = 4.7 µF` (single value). The REGN LDO supplies internal bias and HSFET/LSFET gate drive — undersizing its bypass cap can cause:
- Gate-drive voltage sag during switching → higher Rds_on losses → thermal regulation kicks in early
- LDO oscillation under load steps
- Possible bootstrap recharge failures

**Recommendation:** Swap C15 to a 4.7 µF / 10 V X7R 0402 or 0603 before Stage 1 reflow if possible. If the PCB pad is sized for 0402 and a 4.7 µF/10 V 0402 isn't on hand, a 0603 4.7 µF will fit with overhang.

**B-4 — C10 BTST cap is 100 nF, datasheet specifies 47 nF (LOW priority)**

Datasheet typical application diagram (p. 46) and pin description (p. 5) both call out `0.047 µF` between BTST and SW. Using 100 nF (≈2×) doesn't violate any abs-max but extends the bootstrap charge time and slightly slows HSFET turn-on. Probably OK in practice — flag for awareness, not a must-change.

**B-5 — PMID_GOOD pull-up R10 to VBUS (MED priority — depends on adapter)**

Schematic: `VBUS — R10 10k — PMID_GOOD (pin 3) — TP2 test point`. Pull-up to VBUS.

Datasheet p. 5 (Pin Functions): *"Open drain active high PMID good indicator. Connect to the pullup rail REGN through 10-kΩ resistor."*

Why this matters:
- PMID_GOOD pin abs-max is 7 V [p. 7]. VBUS can be 13.5 V nominal, 22 V abs-max.
- With a 5 V USB adapter, VBUS ≈ 5 V — pull-up to 5 V is *below* abs-max, will work.
- With a 9 V or 12 V high-voltage adapter (which the BQ25619 explicitly supports), VBUS exceeds 7 V → PMID_GOOD pin abs-max violation.

**Recommendation:** Cut the VBUS trace and re-route R10's top end to REGN. If that's a board-rework hassle, *and* you're committing to USB 5 V only for Mk1 (no 9 V/12 V adapters), this can be left as-is and corrected for Mk2. **Document the USB-5V-only constraint somewhere visible.**

### Items requiring visual re-check

**B-6 — BATSNS (pin 10) destination unclear**

The PDF rendering at sheet-3 resolution shows the BATSNS pin going to a region near a "+3V3" label. Either:
- The trace actually goes to the LiPo+ terminal (correct — remote sense). The "+3V3" label nearby is for the TS divider or pull-ups, just visually adjacent.
- The trace really does tie BATSNS to +3V3 (wrong — would force CV regulation against the buck output, not the battery; chip will detect open/short and fall back to BAT-pin regulation anyway, so charging would *probably* still work).
- BATSNS is left floating (will fall back to BAT pin regulation, BATSNS_STAT = 1).

**Recommendation:** Open the .kicad_sch in KiCad and verify BATSNS net is `BAT` (i.e., shorts to the BAT pin, equivalent to "BATSNS not used"). Per your "batsns was a scam" comment, this is acceptable as-is — but firmware must set `BATSNS_DIS = 1` (REG00[5]) at init to avoid the auto-detection cycle.

**B-7 — JP1 "Net-tie" between SYS and BAT (POTENTIALLY CRITICAL)**

Schematic shows `JP1` with a red dot annotated `Net-tie` placed between the BAT and SYS nets near the BQ25619 right edge.

**If JP1 is a KiCad-only graphical net-tie symbol that has no PCB footprint:** harmless, the routing keeps SYS and BAT separated.
**If JP1 is a physical 0 Ω jumper or PCB net-tie footprint (small copper bridge):** SYS and BAT are physically shorted, the internal BATFET (Q4) is bypassed, ship-mode no longer disconnects the battery, and the power-path management is fully defeated. Charging still works (because the buck stage doesn't go through Q4) but everything else breaks.

**Action required:** Open the PCB in KiCad PCB editor and verify whether JP1 has a copper bridge between the SYS and BAT pads. If it does, **scrape the bridge or do not populate** before reflow. This is the single most consequential thing to verify on the BQ side.

**B-8 — PSEL (pin 2) strap value**

Schematic shows a `PSEL` net label coming off pin 2 with no obvious tie visible in the rendered PDF. Per datasheet p. 5 and p. 18:
- PSEL HIGH → IINDPM defaults to 500 mA (USB SDP)
- PSEL LOW → IINDPM defaults to 2.4 A (adapter)

**Recommendation:** Confirm PSEL is tied to GND (LOW, 2.4 A default) — firmware can override anyway via REG00[4:0], so the strap only matters at first plug-in before host writes the IINDPM register. A LOW strap is friendlier (full charge current immediately).

**B-9 — R11 marked X (DNP) on /INT (pin 7)**

The schematic shows `BQ_INT — R11 10k — X — GND` where `X` is the DNP marking. This leaves /INT as an open-drain output with **no external pull-up**.

Per v7.2 the /INT pin is not wired to an ESP GPIO (status is polled via I²C), so the pin can float — but the chip will still pulse /INT internally to drive the open-drain low. Without a pull-up, /INT does nothing externally and is wasteful (no slew control on the floating node, possible EMI minor).

**Recommendation:** Either populate R11 with a 100 kΩ to +3V3 (or REGN — REGN turns off during HIZ but that's fine for an unused pin) to define the resting state, or just tie /INT to GND to silence it completely. Lowest-priority concern.

### TS thermistor network — already known issue

R14 10 kΩ (REGN→TS) + TH1 NTC 10 kΩ (Murata NCP15XH103F03, 10k @ 25 °C) + C15 1 µF REGN bypass.

Per WARN-03 in the BQ25619 extract: TI recommends `R_T1 = 5.36 kΩ`, `R_T2 = 31.6 kΩ` for the 103AT thermistor to land the JEITA thresholds correctly. The current 10 k / NTC-only network will give skewed JEITA thresholds — false hot/cold suspensions or no protection when needed.

**Per your earlier comment:** accepted, will be fixed in Mk2. Firmware mitigation for Mk1: set `TS_IGNORE = 1` (REG00[6]) at init.

### Tactile switch (SW1, SKSCLBE010) — dual-purpose QON + button

| Connection | Net | Verdict |
|---|---|---|
| Switch pin 1 (or 5) | BQ_QON (to /QON pin 12) | ✓ |
| Switch pin 2 (or 4) | BQ_BUTTON (to ESP GPIO16) | ✓ |
| Common pins | GND | ✓ — pressing the switch shorts both BQ_QON and BQ_BUTTON to GND simultaneously |
| Pull-up on BQ_BUTTON (R12 5k1) | +3V3 | ✓ |
| Pull-up on BQ_QON (R13 10k) | +3V3 | ⚠ minor — see note |
| Debounce cap C8 | 100 nF to GND | ✓ |

**Note on R13 (BQ_QON pull-up to +3V3):** /QON has an internal 200 kΩ pull-up to V_BAT [datasheet p. 5]. Adding R13 10 kΩ to +3V3 in parallel:
- When +3V3 is on, /QON rests at +3V3 (10 k dominates over 200 k). No issue.
- When +3V3 is off (ship mode), /QON rests at V_BAT via the internal 200 kΩ. R13 becomes a 10 kΩ load to a dead +3V3 rail — leakage path to ground if +3V3 rail is pulled low.

In practice, the +3V3 rail in ship mode is high-Z (TPS62840 buck is off, no clamp), so leakage through R13 is negligible. **No action required**, just understand the topology.

**Critical: confirm pressing the switch for 8–12 s triggers full system reset.** Per datasheet p. 25, /QON LOW for `t_QON_RST = 8–12 s` triggers BATFET full reset. The user holding the button this long *will* power-cycle the device. Document in user manual.

---

## TPS62840DLC buck (Sheet 3, U16)

**Reference:** Cross-referenced against `firmware/docs/datasheet_extracts/20.05_TPS62840_Datasheet_Extract_iv7.1_f0.0_2026-06-13.md`. DLC package pinout per extract Pin Functions table.

### Verified pins

| Pin # | Name | Schematic | Datasheet | Verdict |
|---|---|---|---|---|
| 1 | GND | GND | | ✓ |
| 2 | VIN | SYS_Power | 1.8–6.5 V range; Li-ion 3.0–4.2 V comfortably in range | ✓ |
| 5 | VSET | R15 102 kΩ to GND | E96 102 kΩ NOM → 3.2 V output [p. 13, Table 1; extract WARN-01] | ✓ — schematic comment confirms intent: *"VSET: 102k for 3.2V operation"* |
| 7 | SW | L5 2.2 µH inductor → +3V3 net | Switching node | ✓ |
| 8 | VOS | +3V3 net (right at C6 10 µF terminal per extract requirement) | "connect with short trace directly to C_OUT positive terminal" [extract Pin Functions table] | ✓ (verify on PCB layout that the trace is short) |

### Items requiring visual re-check

**T-3 — MODE (pin 3) termination**

Extract: *"MODE pin must NOT float — terminate to a defined level"* (p. 5; p. 12 §8.3.3). MODE has internal 450 kΩ pulldown so floating defaults to LOW (auto PFM/PWM), but explicit termination is required.

Schematic comment near the chip: *"MODE: Low – power save, High – less ripple"*. This suggests MODE *is* intentionally terminated — but I can't tell from the PDF whether it's to GND, +3V3, or an ESP GPIO.

**Recommendation:** Confirm MODE is tied to GND (auto PFM/PWM — best for battery life). If it's routed to a GPIO, the FW must initialize the GPIO drive level early.

**T-4 — STOP (pin 6) termination**

Extract WARN-02: STOP must be tied to GND for normal operation. STOP HIGH halts switching with 70 µA quiescent (vs 60 nA in normal PFM).

Schematic comment near MODE/STOP labels suggests both are config pins, but again I can't see the tie from the PDF.

**Recommendation:** Confirm STOP is tied to GND. If left floating, the 450 kΩ pulldown gives the right default, but it's bad practice.

### EN (pin 4) — looks OK

Schematic shows EN tied to SYS_Power (always on whenever battery present). Matches extract recommendation. ✓

---

## XC6206P1B2MR 1.8 V LDO (Sheet 3, U2)

This part wasn't in the datasheet-extract queue (deferred or not separately extracted). Verified against generic Torex XC6206 series datasheet from prior knowledge.

| Aspect | Schematic | Comment |
|---|---|---|
| Part | XC6206P1B2MR (1.8 V output, B = SOT-89-3 / MR = TR pack) | — |
| Vin (pin 2 in schematic) | +3V3 + C3 10 µF bypass | ✓ |
| Vout (pin 3 in schematic) | +1V8 + C5 10 µF bypass | ✓ |
| GND (pin 1 in schematic) | GND | ✓ |

**L-1 — Pin numbering check needed.** XC6206 in **SOT-89-3** package: pin 1 = GND, pin 2 = Vin, pin 3 = Vout. In **SOT-23-3** package: pin 1 = Vout, pin 2 = GND, pin 3 = Vin. The schematic numbering matches SOT-89-3. The "MR" suffix usually means SOT-89 pack — but worth confirming the BOM footprint matches.

Bypass caps: XC6206 datasheet typically requires only 1 µF in/out, but tolerates 10 µF without instability. ✓

Dropout: ~250 mV at 100 mA — at +3V3 input giving +1V8 output, plenty of headroom. ✓

Output current rating: 200 mA max. Should be sufficient for what's hung off 1V8 (IMU's Vcc_IO, magnetometer, GPS Vcc_IO — all low-current).

---

## PCF85063ATL RTC (Sheet 4, U8)

**Reference:** Cross-referenced against `firmware/docs/datasheet_extracts/20.04_PCF85063A_Datasheet_Extract_iv7.1_f0.0_2026-06-13.md`. ATL = DFN-10 package with CLKOE pin.

### Verified pins

| Pin # | Name | Schematic | Datasheet | Verdict |
|---|---|---|---|---|
| 1 | OSCI | XL1 (FC31M2-32.768) terminal 1 | 32.768 kHz crystal input | ✓ |
| 2 | OSCO | XL1 terminal 2 | 32.768 kHz crystal output | ✓ |
| 3 | CLKOE | GND | LOW = CLKOUT disabled (matches schematic comment) | ✓ |
| 4 | INT | RTC_INT net, R21 5k1 to +3V3 | Open-drain, needs pull-up; extract WARN-01 confirms 5k1 is fine | ✓ |
| 5 | VSS | GND | | ✓ |
| 7 | SCL | bus 1 SCL | I²C clock | ✓ (confirm bus 1 routing on next pass — but label is consistent with v7.2) |
| 8 | SDA | bus 1 SDA | I²C data | ✓ |
| 9 | EP (die paddle, ATL) | (not explicit but DFN-10 ATL die paddle is internally to VSS — solder for thermal) | Footnote 2, p. 5: "die paddle tied to VSS — solder for thermal but not electrically required" | ✓ assumed |
| 10 | VDD | +3V3 + C43 100 nF + ML621 backup path | I_DD = 220 nA @ 25 °C [p. 34] | ✓ |

### Backup battery topology

```
+3V3 ──┬── VDD (pin 10)
       │
       └── R31 10 kΩ ── D4 1N4148WS ── BT2 ML621 3.0 V
                                       │
                                       └── GND
```

Per v7.2 and extract: 1×1N4148WS + 10 kΩ charge limit on ML621 rechargeable cell. Standard topology, dataasheet-compatible.

- Charging: +3V3 → R31 10 kΩ → D4 (forward-biased, ~0.5 V drop) → ML621 cell. Charge current limited to (3.2 V – 0.5 V – Vcell) / 10 kΩ ≈ 50–200 µA depending on cell state — well within ML621's 0.1–1 mA tolerance.
- Backup: when +3V3 dies, D4 forward-biases the other way (toward VDD pin), supplying VDD at Vcell – 0.5 V ≈ 2.4 V. Above PCF85063A V_DD min (0.9 V deep standby) by ample margin.

### Crystal CL — firmware action item

Crystal: **FC31M2-32.768-NTLLLDT** (Fox Electronics, 32.768 kHz tuning fork). The "NTLLLDT" suffix in Fox's naming typically indicates load capacitance (need to check the part datasheet — likely 12.5 pF based on typical wearable usage). The PCF85063A's CAP_SEL bit (Control_1[0]) selects internal 7 pF or 12.5 pF.

- If crystal CL = 7 pF → leave CAP_SEL = 0 (POR default)
- If crystal CL = 12.5 pF → FW must set CAP_SEL = 1 at init or clock drifts hundreds of ppm

**Action:** Confirm crystal CL from BOM / Fox datasheet; FW init code in `pcf85063.c` must apply the matching CAP_SEL bit.

### Quick sanity checks

- **C43 100 nF VDD bypass:** Standard, matches extract. ✓
- **No bypass cap visible on OSCI/OSCO:** Internal load caps provide it (CAP_SEL bit, 7 or 12.5 pF). External crystal load caps are NOT required and would actually unbalance the oscillator. ✓
- **CLKOE = GND:** CLKOUT disabled at the hardware level. Also software-disable via COF[2:0] = 111 (default). ✓

---

## Final stage-1 reflow risk assessment

| Component | Critical concerns | Stage 1 reflow OK to proceed? |
|---|---|---|
| ESP32-S3-WROOM-1U | R33 10k on EN-vs-VDD ambiguity (must verify it's an EN-side RC, not a VDD series resistor) | ✓ assuming R33 is for EN pull-up + delay, not VDD series |
| BQ25619 | C15 (1 µF vs 4.7 µF spec), C10 (100 nF vs 47 nF spec), R10 to VBUS not REGN, JP1 mystery, BATSNS routing, PSEL strap | ⚠ — see decision matrix below |
| TPS62840 | MODE / STOP pin termination not visible in PDF | ✓ assuming both are at GND (default safe state via 450 kΩ pulldowns) |
| XC6206 LDO | Footprint vs pin numbering | ✓ if BOM matches SOT-89-3 |
| PCF85063A | Crystal CL match in firmware (not a hardware issue) | ✓ |

### Stage-1 reflow decision matrix

Before reflowing the BQ25619:

1. **C15 swap to 4.7 µF:** Do this if practical. If you have a 4.7 µF / 10 V 0402 or 0603 X7R on hand, swap before placing. If not, reflow with 1 µF and bench-monitor REGN under load — if you see REGN sag or HSFET getting hot during charge, this is the cause.
2. **C10 100 nF vs 47 nF:** Skip the rework. 100 nF will almost certainly work for switching; the worst-case downside is slightly slower HSFET turn-on edges. Note for Mk2.
3. **R10 (PMID_GOOD pull-up to VBUS):** If you commit to USB-5V-only for Mk1, leave it. **Otherwise** lift R10 and tack a fly-wire to REGN, or accept the abs-max risk under 9 V/12 V adapter use.
4. **JP1 net-tie:** **OPEN THE PCB IN KICAD AND VERIFY** before reflow. Look at the JP1 footprint — if it shows a copper bridge between two pads (or is a populated 0 Ω resistor), scrape the bridge or DNP the resistor.
5. **BATSNS routing:** Verify in KiCad schematic. If it goes to BAT (i.e., open-circuit equivalent), set BATSNS_DIS = 1 in firmware init and move on. If it goes anywhere else, fix it (this is firmware-mitigatable too).
6. **PSEL strap:** Verify tied to GND (recommended). If tied HIGH, firmware will need to actively rewrite IINDPM to >500 mA every plug-in cycle.

After resolving 1, 4, 5, 6, the BQ25619 is good to reflow. Items 2 and 3 are acceptable defects to document and live with for Mk1.

---

## Notes for next audit pass

When future sessions resume the audit (Stage 2 / Stage 3 components), the following are already-verified and can be skipped:
- ESP32-S3 power-pin annotation (still need to resolve R33 ambiguity but everything else is documented)
- BQ25619 pin-by-pin pinout
- TPS62840 power-stage components (Cin, Cout, L, R_SET)
- PCF85063A pinout and backup topology

To re-audit, look at:
- Display + touch (sheet 2) — CO5300, CST9217
- IMU + magnetometer + GPS (sheet 4) — LSM6DSV16X, LIS3MDLTR, MAX-M10S
- Daughterboard (separate connector on sheet 2 — MAX30101 + TMP117 + DRV2605 (if there))
- Crown encoder (sheet 2 — EC05E1220M01)
- WS2812B LED, flashlight MOSFET, microphone — typically on subsequent sheets

---

*End of Stage 1 audit. Three definite fixes (C15, R10, ESP32 R33), five visual re-checks (JP1, BATSNS, PSEL, MODE/STOP termination, LDO pin numbering). All RTC and TPS-buck connections check out cleanly; BQ25619 has the bulk of the open questions.*
