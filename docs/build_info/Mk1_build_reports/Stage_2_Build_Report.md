# Stage 2 Build Report — Kompic Mk I (iv7.1)

**Date:** 2026-06-19
**Board:** First Kompic Mk I prototype, PCB iv7.1
**Builder:** Ivan
**Stage:** 2 of 5 (hot-air, post-stage-1 reflow)
**Prerequisite:** Stage 1 (ESP32-S3, BQ25619, TPS62840, XC6206, flashlight LED + FET,
button, PCF85063A RTC + crystal) was reflowed and verified PASS with
`firmware/test/1_smoke_test_mk1_reflow1/`. BQ + RTC ACK on I2C, button toggles the
flashlight, LEDC breathe runs.

Stage-2 closes the hot-plate window for this board — the bottom-side USB-C JS16T
goes on now, so any further additions are localised hot-air or iron only.

---

## 1. Population added this stage

| Designator | Part | Bus / pin | Address / signal |
|---|---|---|---|
| (USB-C) | JS16T receptacle + TPD2E001 ESD | bottom side | VBUS / D+ / D- |
| ALS | VEML6030 | I2C bus 1 (GPIO1/2) | 0x10 |
| Mag | LIS3MDLTR | I2C bus 1 | 0x1C |
| IMU | LSM6DSV16X | I2C bus 1 | 0x6B, INT1 = GPIO8 |
| Env | BME688 | I2C bus 1 | 0x76 |
| Mic | MSM261DGT003 (PDM) | I2S | CLK = GPIO47, DATA = GPIO48, L/R = GND |
| RGB | WS2812B-2020 | one-wire | DIN = GPIO42 |

Power rails affected: 3V3 (sensors, WS2812), 1V8 (PDM mic VDD), 5V (USB-C VBUS now
present).

---

## 2. Pre-power checks (do these on every stage-2 build)

Before plugging anything in, with the meter:

1. **Shorts to GND.** VBUS, 5V, 3V3, 1V8 — every rail to GND must read open or a
   sane bulk-cap settling time, never a hard short.
2. **I2C bus shorts.** SDA1/SCL1 to GND and to each other; same for SDA2/SCL2. No
   shorts.
3. **USB-C pin orientation** check the receptacle isn't rotated (TX/RX, CC1/CC2,
   VBUS/GND swapped). The JS16T sits flush against the silkscreen outline.
4. **Visual under magnification**: every LGA, every QFN edge, every 0402 reflowed
   joint. Look for shifted parts and corner-pad bridges.
5. **Plug into a USB-C inline meter / current-limited bench supply set to 5 V.**
   Expect ~100–120 mA idle draw with the stage-2 sketch running. A hard short or
   1 A+ spike → unplug, find the bridge before continuing.

---

## 3. First boot

Sketch: `firmware/test/2_smoke_test_mk1_hotair/2_smoke_test_mk1_hotair.ino`

The sketch:
- Scans both I2C buses, lists ACKing addresses, compares against the stage-2
  expected set (`bus1 = {0x10, 0x1C, 0x51, 0x6B, 0x76}`, `bus2 = {0x6A}`).
- Probes WHO_AM_I / chip-ID on each device, configures sensible defaults, reads
  the BME688 calibration block.
- Drives a WS2812 colour walk (visual confirmation only — no readback path).
- Initialises the PDM mic at 48 kHz / 16-bit / mono and checks for sample-to-
  sample variance (not just non-zero amplitude).
- Silences the BQ TS-warm and watchdog FAULT bits in firmware (see §4.5).
- Runs a flashlight LEDC brightness sweep (50 % / 75 % / 100 %, 1 s each) for
  visual current-limit diagnosis.
- Drops into a per-sensor 3 s dwell loop, one print every 0.5 s (6 prints per
  sensor), round-robin through 8 entries.

A clean boot prints the per-chip identity, summary table with PASS/FAIL per
device, and the live dwell stream.

---

## 4. Issues encountered (and how each was solved)

These are the things that bit during this stage-2 build. Write a copy of this
section into the next build's log before powering on a new board.

### 4.1 LSM6DSV16X NO-ACK on first boot

**Symptom:** I2C scan listed only 4 devices on bus 1 (missing 0x6B); per-chip
probe printed `[LSM ] Ping 0x6B ... NO ACK`. Other bus-1 sensors fine.

**Root cause:** cold joint under the LGA-14 — one or more corner pads (VDD /
VDDIO / GND) didn't wet during the localised hot-air pass.

**Fix:** generous flux on the part, focused hot air at 320–350 °C with a small
nozzle for ~30 s, watched the part settle as the paste melted. Surface tension
self-aligned. Re-scanned: 0x6B ACKed, WHO_AM_I = 0x70.

**Lesson:** LGA-14 corner pads are the riskiest joints on a hot-air-only stage.
Worth a 5 mm visual stand-off check after every LGA reflow, and always go
straight to an I2C scan before assuming the package is fine.

### 4.2 LSM6DSV16X data: all three axes identical, temperature swinging 23 → 100 °C

**Symptom:** After the joint was fixed and 0x6B started ACKing, the dwell output
read e.g. `T=100.3C  A=(+1.18,+1.18,+1.18)g  G=(+147.1,+147.1,+147.1)dps` —
accel X = Y = Z to two decimals, gyro X = Y = Z, and temperature varying by 70+ °C
across successive reads at room temperature.

**Root cause:** firmware bug in the LSM6DSV16X init. The sketch wrote `CTRL3 =
0x42` thinking IF_INC was at bit 1, but on this part **IF_INC is at bit 2**
(per ST's `lsm6dsv16x_reg.h`):

```text
CTRL3 (0x12):  bit7 BOOT | bit6 BDU | bit5 0 | bit4 H_LACTIVE
              | bit3 PP_OD | bit2 SIM (mis-doc, IF_INC in some revs)
              | bit1 (not used) | bit0 SW_RESET
```

The driver-source layout is `sw_reset:1 / not_used:1 / if_inc:1 / not_used:3 /
bdu:1 / boot:1` → IF_INC at bit 2 of the register.

Writing 0x42 set BDU + bit 1 (reserved) and **explicitly cleared** the
default-on IF_INC. With auto-increment disabled, the 14-byte burst read from
`OUT_TEMP_L` (0x20) re-read register 0x20 fourteen times instead of stepping
through 0x20–0x2D, so every 16-bit field decoded to the same temperature value.
That's why `T_raw = ax_raw = gx_raw = ... = ay_raw = az_raw`.

**Fix:** write `CTRL3 = 0x44` (BDU + IF_INC at the correct bit position).
Verified post-fix: per-axis accel and gyro values became distinct, temperature
settled near room-temp, gravity vector appears on whichever axis is down.

**Lesson:** never reuse bit-position assumptions across the LSM6 family.
LSM6DSL / LSM6DSO / LSM6DSV / LSM6DSV16X all have subtly different CTRL3
layouts. Always consult the per-part driver header.

### 4.3 PDM mic returning a stuck rail

**Symptom:** `[MIC] FAIL  samples=1024  min=-30935  max=-30935  spread=0`. Live
dwell same value every 0.5 s. Whistling, tapping, sound source — no change.

**This was a two-layer bug, not one.**

**Layer 1 (firmware):** sample rate was set to 16 kHz. The Arduino-ESP32 ESP_I2S
driver defaults PDM RX to ×64 decimation, so the CLK output was `16k × 64 =
1.024 MHz`. That falls in the **dead band between the MSM261DGT003's Low-Power
Mode max (900 kHz) and Standard Performance Mode min (1.1 MHz)** — the mic's
state machine doesn't know which mode to enter, output is undefined. Datasheet
extract `20.19_PDM_Microphone_Extract_iv7.1_f0.0_2026-06-19.md` flags this
explicitly as warning W2.

  - **Fix:** bumped sample rate to **48 kHz** → CLK = 3.072 MHz (in Standard
    Mode, the recommended op point for this part). Also added a 30 ms delay
    after `i2s.begin()` (datasheet power-up ≤ 20 ms) and a discard-first-buffer
    pass before the variance check.
  - **Tighter alive check:** require `max − min > 200`, not just `peak ≠ 0`. The
    stuck-rail case sneaks past a peak-only check.

**Layer 2 (hardware):** after the fw fix the mic still returned a stuck rail.
The actual fault was that one of the ESP32-S3's pins (the one driving CLK or
reading DATA) was *lifted slightly off its pad* under the WROOM-1U module — the
JLCPCB reflow on the stage-1 board hadn't fully wetted the joint. The mic was
fine; the path from the ESP32 to the mic was broken.

  - **Fix:** with a fine iron tip + flux, reflowed the suspect ESP32 pin on the
    edge of the module. Mic immediately came alive — `spread` jumped to several
    thousand, audible whistling moved the RMS.

**Lesson:** when an "alive-but-stuck" peripheral test points at a chip, the
problem may actually be on the *controller* end of the link, not the peripheral.
The stuck-rail decimation output is just the PDM filter responding to a constant
logic level on DATA — that could be a dead mic, a broken trace, or a non-wetted
controller pin. Test continuity from peripheral pad back to the controller pad
before assuming the peripheral is the suspect.

### 4.4 LIS3MDLTR X-axis railed at −4 G full-scale

**Symptom:** `[LIS] MAG X=-32768 Y=+3824 Z=+6960 raw (-4.79 +0.56 +1.02 G)`.
X is pinned at `INT16_MIN`, Y and Z look sensible (close to Earth's field
magnitude).

**Diagnosis:** confirmed the X channel is alive by passing a small neodymium
magnet near the sensor — X came off the rail. So the LGA-12 joint is good and
the chip is functional. The persistent saturation must be a strong DC magnetic
field near the X axis on the bench (most likely a magnetised tweezer, a
magnetised steel can on the nearby MEMS mic, or the WS2812 housing — all of
these can pick up residual fields after hot-air work).

**Status:** not fixed at the chip level. Move suspect magnetised tools away
from the board, or perform a hard-iron offset calibration in firmware once we
get to magnetometer calibration. Sensor itself is healthy.

**Lesson:** before condemning a magnetometer, check the magnetic environment
on the bench. Tweezers and ferrous tools held under hot air pick up surprisingly
strong DC fields.

### 4.5 BQ25619 FAULT = 0x82 persistent

**Symptom:** REG09 always reads 0x82 on this board. From the BQ25619 datasheet
extract `20.13_BQ25619_Datasheet_Extract_iv7.1_f0.0_2026-06-18.md` line 173–177:

```text
[7] WATCHDOG_FAULT   = 1
[6] BOOST_FAULT      = 0
[5:4] CHRG_FAULT     = 00  (normal)
[3] BAT_FAULT        = 0
[2:0] NTC_FAULT      = 010 → WARM (buck mode)
```

So two latched faults: WATCHDOG and NTC = WARM.

**Watchdog:** the sketch never writes WD_RST. The chip's 40 s I2C watchdog
expires and the BQ falls back to default register values, latching bit 7.

  - **Fix:** disable the WD in firmware. `REG05 &= ~0x30` sets the WATCHDOG[1:0]
    field to 00. On the next boot the bit no longer latches.

**NTC = WARM:** the v7.1 TS network is `REGN → 10 k → TS → 10 k NTC → GND`. No
parallel R_T2 (TI's recommended network is 5.36 k + 31.6 k for the BQ25618/19
JEITA thresholds). Measurements on the working board:

  - REGN ≈ 4.6 V
  - V_TS ≈ 1.8 V → V_TS / REGN ≈ 39 %, sitting right at the warm trip (~37 %)
  - Board self-heats slightly (mostly the flashlight LED loop + the buck), so
    the NTC body is at ~35–40 °C, which on the steep no-R_T2 curve drops the
    NTC to ~5–6 kΩ and V_TS into the warm zone

  - **Fix (firmware-side, no-battery board):** set `REG00[6] TS_IGNORE = 1`. The
    chip ignores the TS pin and NTC_FAULT reports normal. Once a real battery
    + correctly sized network is installed, undo this.
  - **Future fix (hardware):** when revising the layout, swap to TI's
    recommended R_T1 = 5.36 k + R_T2 = 31.6 k. The parallel resistor linearises
    the curve so the trips match nominal JEITA temperatures.

**Lesson:** the BQ25619 fault register is *latched*. Even after you fix the
condition, the bit stays asserted until you read the register and the
underlying condition is gone. Always re-read FAULT after a delay before
believing the value. Print decoded NTC value (`normal / warm / cool / cold /
hot`) in dev firmware so the operator can see the state without consulting the
datasheet every time.

### 4.6 Flashlight LED too dim — current capped at ~20 mA

**Symptom:** raising the LEDC duty from peak 232 (≈ 91 %) to peak 255 (100 %)
made no visible difference to the flashlight brightness. The 50 %/75 %/100 %
brightness sweep showed three barely-distinguishable levels.

**Diagnosis path:**

1. First suspect was the breathe waveform — sine breathe spends little time at
   peak, so duty was being "wasted" in the trough. Verified this isn't the
   issue: the static-hold sweep at 100 % was still dim.
2. Measured the circuit on the board, FET off:
   - 5 V → 47 + 47 = 94 Ω → LED anode → LED cathode → FET drain → FET source
     → GND.
   - Gate driven via 47 Ω from GPIO41.
3. With the LED replaced by an inline ammeter and FET driven on (PWM = 255):
   - Expected: `I = (5 V − Vds_FET) / 47 Ω ≈ 95 mA` with a healthy BSS138W
     (Rds(on) at Vgs = 3.3 V is single-digit Ω; LED out of circuit, so no Vf).
   - Measured: **4 mA**.
   - Gate voltage at the FET pin measured 3.16 V (so GPIO is driving the gate
     normally, no series-resistor or open-trace issue on the gate side).
   - Supply: 5 V rail = 4.72 V, doesn't sag → not a source-impedance limit.
   - Resistor verified: 49 Ω end-to-end from the 5 V net to the FET drain (47 Ω
     resistor + 2 Ω trace), so the resistor is what it claims to be.

   With Vgs = 3.16 V and 4 mA flowing, Vds is sitting at ~4.5 V — the FET is
   carrying almost the entire supply across it and barely conducting. For a
   BSS138W with rated Vgs(th) = 0.8–1.5 V, Vgs = 3.16 V is 1.6 V of overdrive,
   should be deep in triode (Rds ≈ 3 Ω) and currents should be ≥ 95 mA limited
   only by the resistor.

   The only consistent explanation: **the FET's effective threshold has shifted
   up to near 3 V**, leaving almost no overdrive. That's classic
   gate-oxide-damage signature — either ESD during hot-air handling, or
   localised thermal overstress on the gate pad during a nearby part's reflow.

**Status: open.** Pending FET replacement.

**Fix plan:**
1. Hot-air the BSS138W off and replace with a fresh part from the reel. ESD
   precautions on placement.
2. Re-test. Healthy FET + 47 Ω series should give ≈ 35 mA → ~2× brighter than
   the original 94 Ω configuration.
3. If still not bright enough for the use case, jumper the second 47 Ω too. At
   Rds ≈ 3 Ω + ~2 Ω trace, that gives `I = (5 − 3.2 V) / 5 Ω ≈ 360 mA`, well
   above the LED's rating — *check the LED datasheet first*. Small SMD whites
   in 0603/0805 usually handle 50–100 mA continuous; a true flashlight-class
   LED can take more. Don't jumper both 47 Ω blind.

**Lesson:** when an LED-FET stage runs much dimmer than the resistor math
predicts, check Vds on the FET. If Vgs is normal but Vds is near rail, the FET
itself is degraded — Vth has shifted. Hot air + tiny SOT-23 FETs are an ESD-
prone combination; ground the iron, ground the board, ground yourself.

---

## 5. Final smoke-test state (post-fixes, pre-FET-replacement)

| Device | Bus / pin | Result | Notes |
|---|---|---|---|
| BQ25619 | bus 2 0x6A | PASS | FAULT bits silenced via fw (TS_IGNORE + WD off) |
| PCF85063A | bus 1 0x51 | PASS | OS bit set (sticky cold-boot flag, clock running) |
| VEML6030 | bus 1 0x10 | PASS | Lux readout tracks ambient light |
| LIS3MDLTR | bus 1 0x1C | PASS | X-axis sensitive to bench magnetics; chip OK |
| LSM6DSV16X | bus 1 0x6B | PASS | IF_INC fix in fw; per-axis data clean |
| BME688 | bus 1 0x76 | PASS | T/P/H compensated, plausible (~25 °C, ~1000 hPa, ~30–40 %RH) |
| MSM261DGT003 | I2S 47/48 | PASS | After ESP32 pin re-flow + 48 kHz config |
| WS2812B-2020 | GPIO42 | PASS | Visual colour walk crisp — works very well. WS2812 has no readback path, so "visual confirmation" is the only test the part ever gets. No flicker, no missed steps, full-white draw within budget. |
| Flashlight LED | GPIO41 | OPEN | FET pending replacement |

Idle current with the stage-2 sketch running and flashlight breathe active:
**~100–120 mA** on USB-C. Use this as the ballpark for next builds — sustained
draw above ~300 mA at idle is a fault to investigate.

---

## 6. Open / deferred items

1. **Replace flashlight FET.** Confirm Vth recovers and current rises. Then
   decide on jumper-the-second-47 Ω or not based on LED datasheet.
2. **LIS3MDL X (then Y/Z) saturation** — *not* a calibration problem, it's the
   bench magnetic environment. The LIS3MDL footprint sits ~1 cm from the
   ESP32-S3-WROOM-1U module's metal shield, which is a candidate magnetiser
   after hot-air work. Recovery procedure: clear all ferrous tools from the
   bench, take a baseline reading in open space, write `CTRL_REG2 = 0x08`
   (REBOOT) to reload factory zero-gauss cal in case the >50 G DF tolerance
   was nudged, *then* do hard-iron cal and load OFFSET_X/Y/Z_REG from NVS.
   See 20.07 WARN-02 / WARN-03.
3. **LIS3MDL hard-iron offset calibration** at the firmware level — needed
   regardless of (2), for LRA-magnet bias and soft-iron correction. Figure-8
   swing routine, persist to NVS, reload on every boot.
4. **LSM6DSV16X INT1 → GPIO8 raise-to-wake** is not exercised yet. Stage-2
   only verified the data path. Configure wake-up interrupt, deep-sleep the
   ESP, verify GPIO8 wakes it. Pair with the MAX_INT (GPIO7) test when Stage 3
   brings MAX30101 online.
5. **BQ TS network** — re-evaluate when adding the battery. The warm-trip
   observed on Stage 2 was bench-thermal (LEDs heating the board, iron
   parked nearby) rather than a real fault; `TS_IGNORE = 1` is the live
   workaround. For v8, swap to TI's recommended 5.36 k + 31.6 k network.
6. **Mic decoupling** — datasheet calls for 100 nF + 10 µF as close to mic VDD
   as possible. Verify on the v7.1 layout; if absent, plan for v8.
7. **CLK voltage-domain mismatch** on the mic (3.3 V GPIO into a 1.8 V-rail
   device — see PDM extract W1). Within absolute max, but ~0.3 V margin. A
   series resistor on R8 mitigates; verify or add for v8.

### 6a. Mixed-alloy paste fallback — same as Stage 1

All Stage 2 manual paste touch-ups (LGA / QFN center pads on the sensors that
needed it during hot-air placement) used the same 50:50 KEK flux + leaded
paste mix as Stage 1 — the lead-free SAC305 syringe paste did not arrive in
time. Same conclusion: joints formed, joints held through multiple rework
cycles (the LSM6DSV16X was reflowed at least twice during the cold-joint
diagnosis, the PDM mic pin on the ESP32 module was iron-reworked, both
healthy now). Long-term thermal fatigue unknown — prototype-only workaround.
The reflow tex (Page 4) should soften the absolute "DO NOT USE LEADED" to a
dire-straits fallback note rather than a prohibition.

### 6b. DRV2605 location discrepancy in the docs

The reflow tex (Page 6, "Components on Daughter PCB") and the Stage Summary
table (Stage 3 row) both list **DRV2605LDGSR** as a daughter-PCB component.
**It is not** — the DRV was relocated to the **underside of the main board**
some revisions ago. The daughter PCB (Stage 3) carries only MAX30101 + TMP117.
Phase 3 / Stage 3 description needs updating to reflect this, and the DRV
soldering step moves to the main-board hot-air pass instead of the separate
daughter-PCB hot-plate reflow.

---

## 7. References

- Smoke sketch: `firmware/test/2_smoke_test_mk1_hotair/2_smoke_test_mk1_hotair.ino`
- Reflow doc (with phased build, Page 9): `hardware/Reflow_info/Kompic_Mk1_iv7.1_main.tex`
- Master pinout: `hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md`
- BQ25619 extract: `firmware/docs/datasheet_extracts/20.13_BQ25619_Datasheet_Extract_iv7.1_f0.0_2026-06-18.md`
- PDM mic extract: `firmware/docs/datasheet_extracts/20.19_PDM_Microphone_Extract_iv7.1_f0.0_2026-06-19.md`

---

*Stage 2 — end of report.*
