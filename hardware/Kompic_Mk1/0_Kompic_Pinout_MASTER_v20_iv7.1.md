# Smartwatch PCB — Unified Pinout (MASTER)
**ESP32-S3-WROOM-1U-N16R8 | CO5300 2.06" AMOLED + CST9217 Touch**
**Session v20 — 31 May 2026 — internal ref iv7.1**
*Single source of truth. Reconciled against the working bench driver (ground
truth), the current KiCad symbols/footprint, and the SLPIN/OLED_EN power
decision. Supersedes the dead Session-9 pinout and the wrong vendor display
schematic.*

---

# PART 1 — ESP32-S3 GPIO ASSIGNMENT

## Change history baked into this rev

| Signal | Old pin | Pin | Reason |
|--------|---------|-----|--------|
| QSPI_CS | 46 | **10** | Working sketch drives CS on GPIO10 (FSPICS0 IOMUX) |
| MAX_INT | 10 | **7** | Displaced by CS; must stay on a clean RTC pin to wake |
| TimePulse | 7 | **46** | Gave up its RTC pin to MAX_INT; 1PPS needs no wake |
| TP-RST (touch) | 44 → 5 → **44** | **44** | Net back to 44 (east); freed GPIO5 for SCL_bus2 pairing |
| SCL_bus2 | 5 → 44 → **5** | **5** | Now west, paired beside SDA_bus2 (IO4) for routing |
| OLED_EN | 45 | **dropped** | Panel powered down via QSPI SLPIN (0x10), no GPIO |
| QSPI_TE | unwired | **45** | Reclaims the pin OLED_EN vacated |

> **Routing note (this rev):** SCL_bus2 and TP-RST were exchanged so both I2C
> buses sit as contiguous pairs — **bus 2 on IO4(SDA)/IO5(SCL), west edge**;
> **bus 1 on IO1(SDA)/IO2(SCL), east edge**. TP-RST (a driven output) takes the
> east boot-log pin GPIO44, which is boot-tolerant for a reset line.

> ⚠️ **OLED_EN drop is PROVISIONAL pending current measurement.** SLPIN
> (0x28 DISPOFF → 0x10 SLPIN) confirmed on the bench to blank the panel and
> wake cleanly, but panel sleep current was NOT yet measured (no meter on
> hand). If SLPIN standby current is too high for the battery budget, a
> hardware kill (FET on the panel supply, driven by a GPIO) returns — and TE
> loses GPIO45.

## Master Pinout Table

| GPIO | KiCad Label | Signal | IOMUX | RTC Wake | Boot Behavior |
|------|-------------|--------|-------|----------|---------------|
| 0  | IO0  | DRV_EN | — | ✓ | **Strap (boot-mode), internal pull-UP only DURING reset.** Pull-up releases after boot — does NOT sustain enable at runtime. Drive as FW output. Jumper alt: pull EN at the DRV directly, freeing this pin. |
| 1  | IO1  | SDA (bus1) | — | ✓ | Clean. 5.1k→3V3. East edge |
| 2  | IO2  | SCL (bus1) | — | ✓ | Clean. 5.1k→3V3. East edge (paired with IO1) |
| 3  | IO3  | QSPI-RESET (DISP_RST) | — | — | **Strap: JTAG source-select. NO internal ESP pull.** No external resistor present — panel-side holds RST defined; FW drives it as output. Watch only the pre-init boot window if a cold-boot display fault ever appears. |
| 4  | IO4  | SDA_bus2 | — | ✓ | Clean. 5.1k→3V3. West edge |
| 5  | IO5  | SCL_bus2 | — | ✓ | Clean. 5.1k→3V3. West edge (paired with IO4). (was TP-RST) |
| 6  | IO6  | TP-INT_3V3 (touch INT) | — | ✓ | Clean. **Wake: tap.** West edge |
| 7  | IO7  | MAX_INT | — | ✓ | Clean. **Wake: PPG FIFO.** (was TimePulse) |
| 8  | IO8  | LSM_INT1 | — | ✓ | Clean. **Wake: raise-to-wake** |
| 9  | IO9  | QSPI-I2 (D2) | FSPIHD | ✓ | Clean. North edge |
| 10 | IO10 | QSPI_CS | FSPICS0 | ✓ | Clean. North edge. (was MAX_INT) |
| 11 | IO11 | QSPI-I0 (D0/MOSI) | FSPID | ✓ | Clean. North edge |
| 12 | IO12 | QSPI-CLK | FSPICLK | ✓ | Clean. North edge |
| 13 | IO13 | QSPI-I1 (D1/MISO) | FSPIQ | ✓ | Clean. North edge |
| 14 | IO14 | QSPI-I3 (D3) | FSPIWP | ✓ | Clean. North edge |
| 15 | IO15 | RTC_INT | — | ✓ | Clean. **Wake: alarm** |
| 16 | IO16 | BQ_BUTTON | — | ✓ | Clean. **Wake: button + ship-mode exit.** Dual-wire to BQ25619 QON |
| 17 | IO17 | M10-TX (→GPS RX) | U1TXD | ✓ | Clean |
| 18 | IO18 | M10-RX (←GPS TX) | U1RXD | ✓ | Clean |
| 19 | IO19 | D- | USB | — | Reserved (fixed USB) |
| 20 | IO20 | D+ | USB | — | Reserved (fixed USB) |
| 21 | IO21 | EC_SigA (encoder A) | — | ✓ | Clean. North edge. Crown encoder; wake-capable (bonus) |
| 35–37 | — | PSRAM (octal) | — | — | Unavailable (N16R8) |
| 38 | IO38 | SD_CLK | — | — | Clean (no RTC). East edge |
| 39 | IO39 | SD_CMD | — | — | Clean (no RTC). East edge |
| 40 | IO40 | SD_DAT0 | — | — | Clean (no RTC). East edge |
| 41 | IO41 | GPIO_FLASHLIGHT | — | — | Clean (MTDI), ext pull-down. East edge |
| 42 | IO42 | Sig_LED_Din (WS2812) | — | — | Clean (MTMS). East edge |
| 43 | TXD0 | EC_SigB (encoder B) | — | — | **Boot-log out ~3ms** — one spurious edge at boot, FW ignores pre-init. East edge |
| 44 | RXD0 | TP-RST_3V3 (touch reset) | — | — | **Boot-log RX.** Driven reset output — boot-log blip harmless. East edge. (was SCL_bus2) |
| 45 | IO45 | QSPI_TE | — | — | **Strap (VDD_SPI), internal pull-DOWN → LOW.** Panel-driven, NO external pull-up. Panel unpowered at boot → strap reads LOW correctly. North edge. (was OLED_EN) |
| 46 | IO46 | TimePulse | — | — | **Strap (boot-msg), internal pull-DOWN → LOW.** No pull-up at all; 1PPS idles low, GPS gated at boot → strap OK. North edge. (was QSPI_CS) |
| 47 | IO47 | Mic_CLK | — | — | Clean (no RTC), 3.3V domain on R8. North edge |
| 48 | IO48 | Mic_Dout | — | — | Clean (no RTC), 3.3V domain on R8. North edge |

## Physical edge grouping (footprint: antenna at SW, part rot 180°)

- **West (pads 1–14):** GND, 3V3, EN, SDA_bus2(4), SCL_bus2(5), TP-INT(6),
  MAX_INT(7), RTC_INT(15), BQ_BUTTON(16), M10-TX(17), M10-RX(18), LSM_INT1(8),
  D-(19), D+(20). → **I2C bus 2 paired here.**
- **North (pads 15–26):** QSPI-RESET(3), TimePulse(46), QSPI block (9,10,11,12,
  13,14), EC_SigA(21), Mic_CLK(47), Mic_Dout(48), QSPI_TE(45).
- **East (pads 27–40):** DRV_EN(0), SD(38/39/40), FLASHLIGHT(41), WS2812(42),
  EC_SigB(43/TXD0), TP-RST(44/RXD0), SCL(2), SDA(1), GND. → **I2C bus 1 paired
  here; encoder B + touch reset cluster here.**

## IOMUX

| Peripheral | GPIOs | Note |
|------------|-------|------|
| Display QSPI (SPI2/FSPI) | 9, 10, 11, 12, 13, 14 | CS on FSPICS0 (10) — full IOMUX |
| GPS UART (UART1) | 17, 18 | U1TXD / U1RXD direct IOMUX |

## Sleep Wake Sources (RTC GPIO, ≤21)

| GPIO | Signal | Trigger | Priority |
|------|--------|---------|----------|
| 6  | TP-INT_3V3 | Tap to wake | Required |
| 7  | MAX_INT | PPG FIFO during sleep HR | Required |
| 8  | LSM_INT1 | Raise to wake | Required |
| 15 | RTC_INT | Alarm wake | Required |
| 16 | BQ_BUTTON | Button + ship-mode exit | **Required** |
| 21 | EC_SigA | Crown turn (optional) | Nice-to-have |

## Strapping / Boot-Sensitive Pin Audit

| GPIO | Strap role | Required @reset | Our signal | Idle state | OK? |
|------|-----------|-----------------|------------|------------|-----|
| 0  | Boot mode | HIGH (int pull-up, boot-only) | DRV_EN | FW-driven output | ✓ |
| 3  | JTAG src  | none — NO int pull | QSPI_RESET | held by panel + FW output | ✓ |
| 45 | VDD_SPI   | LOW (int pull-down) | QSPI_TE | LOW (no ext pull, panel off) | ✓ |
| 46 | Boot msg  | LOW (int pull-down) | TimePulse | LOW (no pull, GPS gated) | ✓ |
| 43 | U0TXD     | boot-log out ~3ms | EC_SigB | — | ✓ blip harmless |
| 44 | U0RXD     | boot-log RX | TP-RST (output) | driven | ✓ blip harmless on reset line |

## Signals NOT on a GPIO (poll / I2C / test pad only)

BQ_INT (poll via I2C bus 2 — no GPIO), BQ_STAT, SD_Cd, M10S_RST,
LSM_INT2, LIS_INT, LIS_DRDY, TMP_ALRT, VEML_INT.

## Critical Warnings

⚠️ **GPIO45 (QSPI_TE): NEVER add an external pull-up.** VDD_SPI strap, must
read LOW at reset. Panel drives TE but is unpowered at ESP boot, so the
internal pull-down sets the strap. An external pull-up breaks VDD_SPI
(flash/PSRAM voltage) selection.

⚠️ **GPIO46 (TimePulse): NEVER add an external pull-up.** Boot-msg strap,
must read LOW at reset. Currently has no pull at all — keep it that way.

⚠️ **GPIO0 (DRV_EN): internal pull-up is boot-only.** It does NOT hold the
haptic enabled at runtime — the pull-up releases after the ROM bootloader.
Drive GPIO0 as a firmware output to control DRV_EN, or use the jumper to pull
EN at the DRV directly (which frees this ESP pin).

⚠️ **GPIO3 (QSPI_RESET): JTAG-source strap, NO internal ESP pull, and NO
external resistor on this design.** RST is held defined by the panel side;
firmware drives GPIO3 as an output. Confirmed working from cold boot after
extended shelf storage. Only revisit if a cold-boot display fault appears.

⚠️ **GPIO7 (MAX_INT) is a wake source — keep it on a clean RTC pin.** Do not
relocate to 38–48 (no RTC, cannot wake).

⚠️ **NEVER put an I2C line on GPIO43 or GPIO44 (boot-log pins).** The ~3ms
boot-log activity would corrupt a live bus transaction. Only boot-tolerant
signals (encoder lines, driven resets) belong on 43/44 — which is exactly why
TP-RST (not SCL_bus2) lives on 44.

---

# PART 2 — AMOLED DISPLAY CONNECTOR

**CO5300 2.06" (PY206-W38-V2) | 24-pin Conn_02x12 (OK-14F024-04 receptacle)**
Traced from the PHYSICAL breakout (vendor schematic was wrong for this panel).
14 named pins; remainder GND / 3V3 / NC.

> ⚠️ Supersedes the old J1 (Conn_02x12_Counter_Clockwise). KiCad J1 redrawn
> to this layout — Side B numbered 24→13 (descending) to match the physical
> connector, so each table row shows the two pins that sit across from each
> other.

## Connector — physically opposite pins per row

| Side A pin | Signal | GPIO | ‖ | Side B pin | Signal | GPIO |
|:----------:|--------|:----:|:-:|:----------:|--------|:----:|
| 1  | QSPI_RESET (RST) | **3** | ‖ | 24 | GND | — |
| 2  | GND | — | ‖ | 23 | QSPI-I0 (D0/MOSI) | **11** |
| 3  | +3V3 | — | ‖ | 22 | GND | — |
| 4  | NC | — | ‖ | 21 | QSPI-CLK | **12** |
| 5  | +3V3 | — | ‖ | 20 | GND | — |
| 6  | +3V3 | — | ‖ | 19 | QSPI-I3 (D3) | **14** |
| 7  | GND | — | ‖ | 18 | NC | — |
| 8  | TP-RST_3V3 (touch reset) | **44** | ‖ | 17 | QSPI-I2 (D2) | **9** |
| 9  | TP-INT_3V3 (touch INT) | **6** | ‖ | 16 | QSPI_CS | **10** |
| 10 | SDA (bus1) | **1** | ‖ | 15 | QSPI-I1 (D1/MISO) | **13** |
| 11 | SCL (bus1) | **2** | ‖ | 14 | +3V3 | — |
| 12 | +3V3 | — | ‖ | 13 | QSPI_TE | **45** |

## Panel power — 3V3 ONLY

This panel has **no separate VBAT / boost-input pin**. Panel runs entirely
from **+3V3** (the multiple 3V3 pins above). There is no SYS/VBAT rail to the
display. The old "VBAT min 3.5V" note from the vendor schematic does NOT apply
to this panel — ignore it.

- Bench-confirmed working on 3V3.
- Watch for brownout at max brightness (boost works harder from 3V3 than from
  a higher rail) — first suspect if display glitches under high load.

## Power control — NO hardware enable pin

No OLED_EN pin exists on this panel. Display power-down is firmware-only:

- **Sleep:** `0x28` DISPOFF → `0x10` SLPIN (shuts OLED boost + source drivers)
- **Wake:** `0x11` SLPOUT → full re-init (COLMOD 0x77 etc.) → `0x29` DISPON → repaint

> ⚠️ Provisional until sleep current is measured (see Part 1 warning).

## Controller / interface facts (from working driver)

- Driver IC: **CO5300AF-51**, QSPI via SPI2/FSPI IOMUX.
- **COLMOD 0x77 (RGB888) mandatory** — RGB565 QIO lane mapping is broken on
  CO5300; pixels packed as 3 bytes R,G,B.
- Commands: instruction `0x02` (1-wire). Pixels: `0x32` (4-wire QIO), addr 0x003C00.
- Panel native 410 × 502, column offset 22.
- Touch: **CST9217**, I2C bus 1 @ **0x5A**, reg 0xD000, ACK 0xAB, 400kHz.

## Remaining verification

1. Connector mechanical position on the FPC ribbon edge — still tape-measure;
   refine via Onshape scan-and-trace (per display measurement workflow).
2. Confirm SLPIN sleep current once a meter is available (gates OLED_EN drop).
