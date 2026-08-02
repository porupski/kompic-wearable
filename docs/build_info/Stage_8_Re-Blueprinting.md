# Stage 8 Re-Blueprinting — Kompic iv8.0

**Date opened:** 2026-07-13
**Basis:** Stage 7 Field Report (2026-07-12 → 2026-07-13). iv7.1 declared current-canon in Stage 7 § 13; this document defines **iv8.0** — the first fixed spin. When iv8.0 is fabricated and a working unit exists, that unit becomes **Kompic Mk II** — but this doc is only about the schematic / BOM / layout deltas required to *get* to Mk II from iv7.1.
**Scope explicitly excluded:** Mk II-only architecture (ADPD4101, WLCSP daughterboards, display+GPS replacements). Those go into a separate Mk II blueprint once iv8.0 is in hand.
**Sequencing:** hardware-first document with a small firmware-implications chapter (§ 10). Actual code changes for the shipped iv7.1 firmware live in Stage 7's forward list; iv8.0-specific firmware only starts once boards exist.

---

## 1. Scope + versioning

- **iv7.1 = current canon.** Everything on the bench right now, everything documented in Stages 1–7, is iv7.1. The "iv7.2" that appeared in a few docs was a naming drift; ratified in Stage 7 that all revisions with the current mask/BOM/mechanical layout are iv7.1.
- **iv8.0 = the first fabricated fix pass.** All changes below constitute the iv8.0 delta from iv7.1.
- **Mk II = when iv8.0 is assembled and a working unit exists.** That unit gets the "Mk II" designation. Not this document's problem.

This document is written as a **delta list**, not a fresh schematic redraw. Rule: if a section doesn't call out a change, iv8.0 carries it over from iv7.1 unmodified.

## 2. Power architecture

Carries forward unchanged: BQ25619 charger, TPS62840 buck, permanent-attach LiPo, single 3V3 rail for everything except the +5 V PMID boost that feeds MAX30101 LEDs / WS2812 / flashlight.

**Deltas for iv8.0**:

- **USB-C connector footprint upgrade.** iv7.1 field-observation: VBUS pad fatigue after ~50 mating cycles, charging drops out (Stage 7 § 11 bug #2). Options:
  - Same connector part but with reinforced through-hole anchoring pads to the ground plane.
  - Or move to a "mid-mount" USB-C that has more mechanical support against the PCB edge.
  - Layout review should call out which.
- **Fuel gauge: add MAX17048.** 2×2 mm 6-lead WLP, I²C on the existing bus 1, no shunt resistor needed (model-based SoC estimation). Fits under a coin-cell footprint's worth of area. ~€1. Provides proper `%` SoC readout instead of the BQ25619's raw voltage. Firmware side is a new small driver + broker channel; low risk.
- **Titanium case → GND bond point.** Single point on the PCB where the case shell physically contacts a plated pad tied to system GND. Two purposes: (a) 60 Hz mains hum rejection for anything doing biopotential-like sensing (the QVAR touch pads and any future ECG AFE), (b) ESD path for the case shell during human contact. Layout must call out the pad location so the case designer knows where to put the internal contact spring / gasket.
- **PMID boost cap re-audit.** iv7.1's flashlight cosmetic sub-µA leak (Stage 4 open bug, diagnostic doc at `hardware/Reflow_info/2026-07-09_flashlight_led_leak_diagnostic.md`) may or may not be related to the boost switching pattern. Layout review should look at the PMID cap + FET drive path together.

## 3. Sensor lineup — carry-forward

All sensors on iv7.1 carry forward to iv8.0 unchanged (part numbers, addresses, bus assignments):

| Sensor | Bus | Addr | Role | iv8.0 changes |
|---|---|---|---|---|
| BME688 | 0 | 0x76 | Env (T, H, P, gas) | none |
| LSM6DSV16X | 0 | 0x6B | IMU + QVAR-for-touch + BCG for HR (new role) | none HW; firmware role expanded |
| LIS3MDLTR | 0 | 0x1C | Magnetometer | none |
| VEML6030 | 0 | 0x10 | Ambient light | none |
| MAX30101 | 0 | 0x57 | Optical HR (PPG) | none HW; optics improvements (§ 6) |
| TMP117 | 0 | 0x48 | Skin temp | none |
| PCF85063A | 0 | 0x51 | RTC | none |
| DRV2605L | 1 | 0x5A | Haptic | none |
| BQ25619 | 1 | 0x6A | Charger | none HW; add fuel gauge alongside (§ 2) |
| MSM261 | I²S | — | Mic | none |
| WS2812B | RMT | GPIO42 | Status LED | none |
| MAX-M10S | UART | — | GPS | reinstall (was removed for solvent-restriction reasons; fresh unit) |
| CO5300 | QSPI | — | AMOLED display | reinstall (was destroyed on iv7.1 — new FPC connector footprint TBD) |
| CST9217 | I²C | — | Touch controller | with display |

**Not changing chips** — every iv7.1 sensor is doing its job. The magnetometer's 30 % bad-cal rate is algorithmic (Stage 7 § 9), not a chip issue. The MAX30101's wrist SNR is architectural (no DC-cancel DAC — see HR-sensing handoff § 1), which we mitigate via firmware squeeze + iv8.0 optics improvements, not chip swap.

## 4. New parts on iv8.0

**Confirmed additions**:

- **MAX17048** fuel gauge — see § 2.
- **Series AC-coupling caps for QVAR pins**: 2 × 100 nF X7R 0402, ceramic, ≥ 25 V, one per QVAR line. Placement: between the existing 500 Ω + 110 pF shunt / ESD network and the LSM6DSV16X chip pin (i.e. the *chip side* of the ESD, so ESD strikes are clamped before the cap). Framed *not* as "for ECG" — QVAR is a touch sensor (Stage 7 § 4) — but for touch/gesture reliability: kills DC skin galvanic drift during long-press-hold so the amp doesn't drift near a rail during sustained contact.
- **10 MΩ body-bias resistor**: from the titanium-case-GND bond (§ 2) through a soft-pull to one QVAR input reference. Improves CMRR against mains coupling when the operator's wrist is touching the case ring. Small cost, big UX effect on QVAR reliability.

**Conditional additions** — layout review decides:

- **AD8232 single-lead ECG AFE** (LFCSP-20, 4×4 mm). If iv8.0 layout finds board area, add for on-demand crown-touch spot ECG. Wiring: Qvar1 (crown pad) + Qvar2 (wrist pad) → AD8232 IN+/IN- → ESP32-S3 ADC. Explicitly not committed; if there's no room, defer to a later spin. Handoff § 3 flagged this as the "under consideration, not committed" question and Stage 7 didn't change that.

**No new parts** for optics / mag / BCG / TEMP mode — those are firmware-plus-mechanical, not electrical.

## 5. QVAR-as-touch electrode redesign

This is where Stage 7 § 4–6 land in hardware. Two conductive touch electrodes wired to the LSM6DSV16X's Qvar1 and Qvar2 pins, each behind the ESD/RC/AC-coupling network of § 4.

### 5.1 Qvar1 → touch button "next to the crown"

- On iv7.1: pogo pin between the button and the encoder in the crown stack (Stage 7 § 5). Was miswired mechanically — pin's compression flexed the encoder axis. Not currently installed.
- On iv8.0: the crown mechanical stack-up reorders to `button | encoder | pogo-pin | rubber-ring-in-casing | crown top`. The pogo pin's compression now travels axially into the sealing ring, not laterally into the encoder. Cleaner rotation, cleaner touch.
- Long-term (Mk II with titanium crown): the crown itself becomes the touch electrode — no separate pogo pin. Iv8.0 keeps the pogo pin as the intermediate step until the metallic-crown production is ready.

### 5.2 Qvar2 → "watch on hand" pad

- On iv7.1: no dedicated pad, Qvar2 wired to a shorted trace under the case bottom. Firmware assumes "on hand" always.
- On iv8.0: add a small (~5 × 5 mm) conductive pad on the wrist-facing side of the case bottom. Wire to Qvar2. Provides the sustained-charge baseline shift that firmware uses as `g_watch_on_hand`. Saves PPG / BCG / flashlight LED power when off-wrist.

### 5.3 QVAR electrical topology per pin

```
Case pad → [ESD diode to VCC/GND] → 500 Ω → [110 pF shunt to GND] → 100 nF series → LSM Qvar pin
```

- ESD diode closest to the pad — clamps external strikes.
- 500 Ω current-limits during ESD.
- 110 pF shunt gives RF filtering (~3 MHz corner).
- **100 nF series is new for iv8.0** — blocks DC skin galvanic offset so the amp doesn't drift near a rail during sustained touch. See Stage 6 § 7 for the placement rationale (cap between the ESD/filter network and the chip pin, not on the electrode side, so the ESD diode still gets first crack at any strike).

### 5.4 Behaviour distinction (firmware, called out here for hardware sanity check)

- **Sustained charge for ≥ 300 ms** → treat as *touch* (valid button press or "on hand" confirmed).
- **Charge transient without sustained level** → treat as *bump* (log, ignore).

This is the classic QVAR-for-touch dispatcher pattern.

## 6. Optical (MAX30101) squeeze — hardware side

Firmware squeeze is already documented in the HR handoff. Hardware-side improvements for iv8.0:

- **Light guide / reflective cup** between the MAX30101 LED window and the case optical dome. Currently iv7.1 has the LEDs firing into the dome directly — good but not optimal. A small anodised-aluminium or black-plastic ring lensed around the LED windows would (a) direct more of the LED emission into the tissue, (b) block ambient light from bleeding into the PD. Design detail: the ring OD needs to sit inside the dome, ID needs to clear the sensor die area.
- **Opaque bezel around the sensor package** on the case bottom's inner face. Prevents ambient light from leaking in around the sensor from case seams.
- **Case optical window material**: iv7.1 is PLA, which is translucent — some ambient light already gets in. iv8.0 titanium case should have a **thin (0.5-0.8 mm), clear** window over the sensor area (polycarbonate, sapphire, or clear resin), with a black opaque bezel forming a light shroud.
- **Case bottom dome geometry**: the ~0.2 mm dome protrusion on iv7.1 is a small win — skin seals cleanly against it. Iv8.0 titanium case should replicate this feature, machined into the case bottom around the optical window.
- **Strap tension considerations for PPG contact pressure**: the strap attachment lugs should be positioned so a natural strap tension pushes the sensor firmly against skin. If the lugs are too far from the sensor, strap force doesn't couple. iv8.0 layout review should verify.

Sensor-side firmware config is already: green-only Multi-LED slot, PW 411 µs, SR 100 Hz, ADC full-scale 16384 nA, SMP_AVE 8, LED3_PA bench-tuned via `firmware/arduino/10_test_max30101`.

## 7. SD card connector + line conditioning

Field-hardening changes for wrist-worn use.

**Connector**:

- **iv7.1**: push-spring microSD holder. Works, but wearable-form-factor concern is that a knock ejects the card. Field observation: not seen yet, but conceptually vulnerable.
- **iv8.0**: **flip-up hinged cover** microSD holder. Card stays locked in until the cover is deliberately opened. Same 6-pin SDMMC footprint, mechanically more robust.

**Pull-ups on the SDMMC lines**:

- **iv7.1** current bodge: 3.3 kΩ (works fine).
- **iv8.0 default**: 5.1 kΩ on all three lines — CMD, DAT0, and (new for iv8.0) CLK. The `10 kΩ` default from the ESP-IDF example schematic is technically OK for the SDMMC's rated rise-time budget but leaves less margin for cable capacitance; empirical bench experience says 5.1 kΩ is more robust.
- Iv7.1 has **no SD_CLK pull-up** (missed in original schematic). Iv8.0 fixes this.
- Still 1-bit SDMMC mode (only DAT0 routed, DAT1-3 not on GPIO). No change; matches ESP32-S3 pin budget.

**Physical layout consideration**: SDMMC bus is high-speed digital next to the sensor bus — trace length and coupling should be reviewed on iv8.0 to keep SDMMC clock noise out of the analog bus.

## 8. GPIO / pin-map deltas

**Freed on iv8.0 (candidates)**:

- **MAX30101 INT1 line** — currently used for wake-on-FIFO-full but never actually leveraged; the driver polls the FIFO from the task. If we drop the pin, that's one GPIO freed. Trade-off: no INT-driven wake for HR path. Recommend keeping (small cost) unless the pin is needed for something else.

**Consumed on iv8.0 (candidates)**:

- **Case-GND bond**: not a GPIO, single tie point.
- **New wrist-facing electrode**: uses Qvar2 (existing LSM pin). No new GPIO.
- **AD8232 output (if it lands per § 4)**: needs 1× ADC input on ESP32-S3. Available.
- **MAX17048 fuel gauge**: shares existing I²C bus 1, no new GPIO.

**Net delta**: probably zero or +1 free GPIO after iv8.0. Layout review to confirm.

## 9. Case + mechanical implications for the electronics

Called out in Stage 7 § 5 and § 6.

- **Crown mechanical stack-up reorder**: iv8.0 target `button | encoder | pogo-pin | rubber-ring-in-casing | crown top`. Solves the encoder-axis-flex problem observed in iv7.1.
- **Button return spring / grease**: iv7.1 button occasionally sticks after press. Iv8.0 options: (a) source a switch with stronger return spring, (b) design for a light silicone-grease application at assembly. Cost approximately zero either way; call it out.
- **Case optical dome for MAX30101**: keep the ~0.2 mm protrusion feature (Stage 7 § 2). Skin sealing works well.
- **PETG-to-metal crown transition**: iv7.1 PETG crown slips against the encoder shaft when PCB is misaligned. Iv8.0 metallic crown (titanium or aluminium) with a proper machined interface removes the failure mode. Also enables treating the crown itself as the QVAR touch electrode down the line.
- **Titanium case with case-to-GND bond point** — see § 2.
- **Strap attachment lug placement** — see § 6 optical.

## 10. Firmware implications of the iv8.0 hardware changes (small chapter)

Not the roadmap for the *iv7.1* firmware — that's Stage 7's forward list. This is what the iv8.0 hardware changes *require* on the firmware side:

- **MAX17048 fuel gauge driver** — new small component under `firmware/esp-idf/components/max17048/`. I²C on bus 1 (shared with BQ25619, DRV2605). Provides `broker_battery_data_t.percentage` from actual model-based SoC, replacing the current voltage-derived estimate.
- **QVAR touch dispatcher** — small component that reads Qvar1 / Qvar2 continuously, produces two event streams:
  - Crown-touch (sustained ≥ 300 ms on Qvar1)
  - On-hand state (sustained baseline shift on Qvar2 → `g_watch_on_hand` broker flag)
  Also detects "bump" (transient without sustained) and logs it separately.
- **Sensor-gate on `g_watch_on_hand`** — MAX30101 wake path, BCG / stillness gate, TEMP-mode aggregator all check the flag and skip if the watch is off-arm. Saves LED power + battery.
- **AD8232 ECG path (if § 4 conditional lands)** — new mode reading ADC continuously, applying HP + notch filtering, plotting to Serial or SD. Replaces the shelved QVAR-ECG mode.
- **BCG-on-LSM fusion component** — post-bench-validation via sketch `firmware/arduino/11_test_bcg`, ports to a new `components/bcg_fusion/` that reads LSM6DSV16X accel at 240 Hz, bandpass-filters 0.8–10 Hz, peak-detects or autocorrelates, gates on stillness (MLC classifier future), publishes BPM to broker alongside PPG.
- **Firmware layer for the crown mechanical reorder**: none — encoder driver doesn't care where the pogo pin sits mechanically as long as the electrical connection to Qvar1 works.
- **SDMMC CLK pull-up + 5.1 kΩ on DAT0/CMD**: pure hardware change, firmware sees no difference.

Firmware side stays the "Lego module" architecture: each sensor is its own component with its own broker channel; orchestrator state lives in `main.c` / `field_capture.c`.

## 11. Standing bugs and how iv8.0 addresses each

| # | Iv7.1 bug | Iv8.0 fix path |
|---|---|---|
| 1 | Flashlight LED sub-µA cosmetic leak | Review FET drive path + PMID cap layout during iv8.0 schematic pass. Diagnostic doc at `hardware/Reflow_info/2026-07-09_flashlight_led_leak_diagnostic.md` |
| 2 | USB-C charging fault (VBUS pad fatigue suspected) | Reinforced connector footprint (§ 2) |
| 3 | Button occasionally sticks after press | Stronger return spring or grease at assembly (§ 9) |
| 4 | Magnetometer 30 % bad-cal rate | Firmware (outlier rejection + on-demand recal via QVAR touch, see Stage 7 § 9) — no hardware change |
| 5 | `qvar_ecg` REQUIRES-cycle in build graph | Firmware (`ecg_tile.c` move) — no hardware change |
| 6 | `bq25619.c` uses wrong I²C mutex | Firmware — no hardware change |
| 7 | `lsm6dsv16x.h` CTRL1 bit layout reversed | Firmware — no hardware change |
| 8 | MAX30101 wrist SNR marginal | Optics (§ 6) + firmware squeeze already scoped |
| 9 | QVAR-as-ECG dead-end | Repurposed as touch (§ 5) — no hardware change; iv7.1 pins already usable as-is |
| 10 | RTC coin-cell removed for USB-C rework will lose time | Firmware (RTC CLI already shipped) — hardware unchanged |

## 12. BOM changes summary

**Added on iv8.0**:

| Part | Package | Qty | Cost | Purpose |
|---|---|---|---|---|
| MAX17048 fuel gauge | 2×2 mm WLP-6 | 1 | ~€1 | Proper SoC estimation |
| 100 nF ceramic X7R ≥ 25 V | 0402 | 2 | ~€0.01 | QVAR AC-coupling (§ 5.3) |
| 10 MΩ 0.1 % | 0402 | 1 | ~€0.05 | QVAR body-bias to case GND |
| 5.1 kΩ 1 % | 0402 | 3 | ~€0.03 | SDMMC pull-ups (§ 7) |
| Flip-up microSD holder | — | 1 | ~€1 | Card retention |
| USB-C connector (reinforced) | — | 1 | ~€1 | Mating-cycle robustness |
| Conductive case pads (wrist-facing + crown-adjacent) | — | 2 | — | Touch electrodes |
| Case-to-GND spring / gasket | — | 1 | — | Body-bias reference |
| AD8232 ECG AFE **(conditional)** | LFCSP-20 4×4 | 1 | ~€3 | On-demand ECG |

**Removed on iv8.0**: nothing removed. All iv7.1 sensors carry forward.

**Total BOM delta**: ~€6–9 added (depending on AD8232 go/no-go), no removals.

## 13. Open questions for the iv8.0 layout pass

Decisions this document doesn't take — layout / mechanical review does:

1. **USB-C connector part number** — reinforced through-hole variant vs mid-mount vs same-part-as-iv7.1. Depends on what's in stock + mechanical strength requirement.
2. **AD8232 go / no-go** — is there room on iv8.0's board area for the LFCSP + reference network + ADC feed path? If no, defer to a later spin.
3. **Case-to-GND bond point** — one point or ring contact? Where on the PCB?
4. **Wrist-facing electrode pad**: exact size + placement on the case bottom. Depends on ergonomics (where does the wrist actually contact?).
5. **Crown-adjacent touch pad**: same question — where does a user's finger naturally rest during crown use?
6. **Optical light-guide** — off-the-shelf part or custom-milled?
7. **PMID boost cap layout** — is there space for a bigger cap or a different topology if the flashlight leak turns out to be layout-sensitive?
8. **Whether the daughterboard for future flex-PCB Mk II parts uses an FPC connector** and if so, which one (Mk II decision but relevant if iv8.0 wants to be future-compatible).

---

## Appendix: Mk II forward ideas (not iv8.0)

Reproduced from the HR handoff § 3 for continuity; **not part of iv8.0**:

- **ADPD4101** as unified PPG + ECG + BioZ front end. Correct "synced discrete LEDs + PD" architecture with on-chip DC cancellation + ambient rejection (~75 dB SNR PPG @ ~30 µW). WLCSP 0.4 mm pitch — requires either JLC-placed fine-pitch or a **flex-PCB daughterboard with FPC connector to the main iv8.0 board**. The flex-daughterboard approach avoids the "solder a million small wires" pain and is under active consideration for Mk II.
- **Metallic titanium crown** as the QVAR touch electrode directly, replacing the iv8.0 pogo-pin arrangement.
- **Bigger battery** if wearable form factor allows — no iv7.1 or iv8.0 constraint here; Mk II industrial-design pass will decide.

Iv8.0 must not add architecture that blocks the Mk II flex-daughterboard path — specifically, keep unused I²C addresses + one SPI bus's worth of pins reserved.
