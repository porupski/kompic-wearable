# JLCPCB Assembly — Picking List
**Smartwatch PCB | Session 16 (unified, LCSC-reconciled) | Value-First Reference**

> **Assembly split:** JLC places only the small/cheap catalog parts (basic 0402
> R/C, the NTC, the 1N4148, the U16 regulator). **Everything else — every part
> marked (in hand) and the entire reflow-tray section — is hand-placed by you in
> the studio.** In-hand = physically yours AND placed by you; it does NOT go to JLC.
>
> **Source of truth for tooling:** the fenced `jlc-assembly` block at the very
> bottom of this file. The human tables here are documentation; if they ever drift
> from the block, **the block wins.** Edit the block, re-run the script.
>
> LCSC numbers verified against orders WM2603190155 (2026-03-19) and
> WM2604260016 (2026-05-19).

---

## ⚠️ Reconciliation notes (read before ordering)

1. **U16 regulator — you do NOT have it in hand.** Picking list / schematic call for
   **TPS62840DLCR (C2071859)**, VSET-programmed. Your tray contains **TPS62849DLCR
   (C2071003)** — a *different, fixed-3.4V* part you'd previously set aside. JLC sources
   the 840. **Do not substitute the 849 from your tray.** Verify JLC stock of C2071859
   before placing the order.
2. **NTC TH1 = NCP15XH103F03RC (C77131)** — now in hand (order WM2604260016, qty 100)
   AND in the schematic BOM. Listed below as JLC-placed (cheap 0402); flip to HAND if
   you'd rather place it yourself.
3. All eight **(in hand)** claims from the prior revision verified present in orders:
   47pF C432937, 10nF C60133, 2.7nH C98058, 3.9nH C5452416, 27nH C113112,
   2.2µH C88527, TVS C1974942, GPS antenna C784388. ✓

---

## CAPACITORS (all 0402)

| Value | Designators | Qty | LCSC | Place | Notes |
|---|---|---|---|---|---|
| 1.0pF | C1 | 1 | *(JLC pick)* | JLC | iffy — flip to HAND if sourced in a top-up |
| 47pF C0G | C23 | 1 | C432937 | HAND | in hand |
| 100pF | C18, C21 | 2 | *(JLC pick)* | JLC | iffy — flip to HAND if sourced |
| 10nF X7R | C24 | 1 | C60133 | HAND | in hand |
| 100nF | C8, C10, C16, C19, C20, C25, C26, C28, C29, C30, C31, C32, C33, C38, C39, C40, C41, C42, C43, C44, C45 | 21 | *(JLC pick)* | JLC | |
| 1µF | C7, C15, C34, C36, C37 | 5 | *(JLC pick)* | JLC | |
| 4.7µF | C4, C22, C27, C35 | 4 | *(JLC pick)* | JLC | |
| 10µF | C2, C3, C5, C6, C9, C11, C13, C14 | 8 | *(JLC pick)* | JLC | |

---

## RESISTORS (all 0402)

| Value | Designators | Qty | LCSC | Place | Notes |
|---|---|---|---|---|---|
| 47Ω | R23, R25, R26, R32 | 4 | *(JLC pick)* | JLC | |
| 470Ω | R16, R18 | 2 | *(JLC pick)* | JLC | Qvar electrode RC filters |
| 5.1kΩ | R3, R5, R6, R7, R8, R9, R12, R19, R21, R22, R29 | 11 | *(JLC pick)* | JLC | |
| 10kΩ | R1, R2, R4, R10, R11, R13, R14, R17, R20, R24, R27, R28, R30, R31, R33, R35, R36 | 17 | *(JLC pick)* | JLC | |
| 100kΩ | R15 | 1 | *(JLC pick)* | JLC | TPS62840 VSET → 3.3V |

---

## INDUCTORS

| Value | Designators | Qty | LCSC | Place | Notes |
|---|---|---|---|---|---|
| 2.7nH 0402 | L1 | 1 | C98058 | HAND | in hand — BLE antenna match |
| 3.9nH 0402 | L2 | 1 | C5452416 | HAND | in hand — BLE antenna match |
| 27nH 0402 | L4 | 1 | C113112 | HAND | in hand — GPS bias-T |
| 2.2µH 1008 | L3, L5 | 2 | C88527 | HAND | in hand — BQ25619 SW, TPS62840 SW |

---

## DIODES

| Value | Designators | Qty | LCSC | Place | Notes |
|---|---|---|---|---|---|
| 1N4148WS SOD-323 | D2, D3, D4 | 3 | *(JLC pick)* | JLC | basic part on JLC |
| ESDALCL5-1BM2 SOD-882 | D5, D6 | 2 | C1974942 | HAND | in hand — 5V TVS |

---

## ICs / ACTIVES (JLC-placed)

| Value | Designators | Qty | LCSC | Place | Notes |
|---|---|---|---|---|---|
| NCP15XH103F03RC 10kΩ NTC 0402 | TH1 | 1 | C77131 | JLC | in hand too; placed by JLC unless you flip |
| TPS62840DLCR VSON-8 | U16 | 1 | C2071859 | JLC | NOT in hand — JLC sources. Verify stock. Do not sub C2071003. |

---

## Hand-Placed reflow tray (NOT sent to JLC)

These ICs/connectors/mechanicals are placed by hand on the hot plate per the
12-page reflow procedure. All dropped from JLC BOM/CPL.

- **U1** ESP32-S3-WROOM-1U-N16R8 (C3013946)
- **U2** XC6206P182MR (C21659)
- **U4** BQ25619RTWR (C2864534)
- **U5** LSM6DSV16XTR (C5267406)
- **U6** MAX-M10S-00B (C4153167) — *second reflow pass*
- **U7** LIS3MDLTR (C478483)
- **U8** PCF85063ATL/1,118 (C404360)
- **U9** BME688 (C3664478)
- **U10** MSM261DGT003 PDM mic (C48227730)
- **U11** VEML6030 (C132182)
- **U12** BSS138W (C890266)
- **U13** MAX30101EFD+T (C2859066) — daughter PCB
- **U14** DRV2605LDGSR (C527464) — daughter PCB
- **U15** TMP117AIDRVR (C699536) — daughter PCB
- **USB1** JS16T-TYPE-C479-DWH2-FSQ (C49118447)
- **D1** TPD2E001DRLR (C150526)
- **LED1** WS2812B-2020 (C965555)
- **LED2** TJ-S4008SW4TGLCCW-A5 flashlight (C601698)
- **X1** FC31M2-32.768kHz crystal (C2842180)
- **SW1** SKSCLBE010 tactile button (C115361)
- **SW2** EC05E1220401 rotary encoder (C116648)
- **AE1** GPS ceramic antenna BWGNSCNX8-8W2 (C784388)
- **AE3** 2450AT18A100E BLE chip antenna (C89334)
- **CN1** YZR0028 Qvar pogo pin (C7429439)
- **Card1** TF PUSH SD socket (C393941)
- **J1** OK-F024-04 24-pin BTB display connector
- **J2, J3** Custom inter-board connectors
- **BT1** LiPo 3.7V 380mAh
- **BT2** ML621 RTC backup cell
- **C17** 0.22F supercap (GPS backup)
- **M1** ELV1411A LRA (daughter PCB pocket)
- **CN2** Qvar electrode test point

---

## Set Aside — NOT On This Board

- VG7669T160N0MA (C5197313) — side project
- XC6206P332MR-G (C5446) — superseded by TPS62840
- TPS62849DLCR (C2071003) — spare/side project. **Do not place as U16.**
- BWU.FL-IPEX1 (C5137195) — U.FL pigtail, off-board
- DIN-327 (C309324) — off-board connector

---

<!--
  MACHINE-READABLE SOURCE OF TRUTH for jlc_assembly_filter.py
  Format: pipe-delimited. Columns: Designator | Value | Footprint | LCSC | Place
  - Designator: single ref OR comma/space list (e.g. "C8, C10, C16")
  - Place: JLC  -> sent to JLCPCB assembly BOM + CPL
           HAND -> dropped from JLC files (you place it)
  - LCSC: leave blank for JLC-picked passives until you sort the order,
          then paste the number in. Re-run the script. Blank+JLC = JLC parser
          will prompt you at upload (fine).
  Only designators listed here as JLC are sent to the fab. Anything in your
  BOM not listed here, or listed HAND, is dropped. Edit this block, re-run.
-->
```jlc-assembly
Designator | Value | Footprint | LCSC | Place
C1 | 1.0pF | 0402 |  | JLC
C18, C21 | 100pF | 0402 |  | JLC
C8, C10, C16, C19, C20, C25, C26, C28, C29, C30, C31, C32, C33, C38, C39, C40, C41, C42, C43, C44, C45 | 100nF | 0402 |  | JLC
C7, C15, C34, C36, C37 | 1uF | 0402 |  | JLC
C4, C22, C27, C35 | 4.7uF | 0402 |  | JLC
C2, C3, C5, C6, C9, C11, C13, C14 | 10uF | 0402 |  | JLC
R23, R25, R26, R32 | 47R | 0402 |  | JLC
R16, R18 | 470R | 0402 |  | JLC
R3, R5, R6, R7, R8, R9, R12, R19, R21, R22, R29 | 5k1 | 0402 |  | JLC
R1, R2, R4, R10, R11, R13, R14, R17, R20, R24, R27, R28, R30, R31, R33, R35, R36 | 10k | 0402 |  | JLC
R15 | 100k | 0402 |  | JLC
D2, D3, D4 | 1N4148WS | D_SOD-323 |  | JLC
TH1 | NTC 10k | 0402 | C77131 | JLC
U16 | TPS62840DLCR | VSON-8_L2.0-W1.5-P0.50-BL | C2071859 | JLC
C23 | 47pF | 0402 | C432937 | HAND
C24 | 10nF | 0402 | C60133 | HAND
L1 | 2.7nH | 0402 | C98058 | HAND
L2 | 3.9nH | 0402 | C5452416 | HAND
L4 | 27nH | 0402 | C113112 | HAND
L3, L5 | 2.2uH | 1008 | C88527 | HAND
D5, D6 | ESDALCL5-1BM2 | D_SOD-882 | C1974942 | HAND
```
