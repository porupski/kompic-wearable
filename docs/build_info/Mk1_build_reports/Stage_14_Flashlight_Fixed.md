# Stage 14 — Flashlight Fixed (LED-driver leak resolved, Vbat pin moved)

**Date:** 2026-08-19
**Board:** iv7.1 (bench work done on ESP32-C3 SuperMini + 72×40 OLED companion)
**Firmware baseline:** f0.0
**Status:** CLOSED for bench; iv7.1 rework pending next physical session.

Closes the fault opened in
`2026-07-09_flashlight_led_leak_diagnostic.md`. Also captures a Vbat-divider
pin relocation triggered by the same bench session, and the plan for
ECG-driven refinement of Stage 13's HR pipeline.

---

## 1. Root cause

**BSS138W batch is dead.** All measurable off-state drain-to-source leakage
in the same order:

- In-circuit: 0.75 mA LED bleed with GATE hard-shorted to SOURCE, +5 V rail
  online.
- Out-of-circuit DMM: **1.5 MΩ across D↔S at V_GS = 0**. Datasheet expects OL
  (nA-scale I_DSS, effectively GΩ).

The die itself is leaky. No amount of gate-drive discipline, pull-down
tuning, or PCB cleaning can fix a channel that conducts at V_GS = 0. The
entire batch is condemned — I do not trust any BSS138W from this reel for
anything switching-off-critical.

Suspected supplier issue (counterfeit / rejected die relabelled). Backup
plan: source next N-MOS batch from LCSC first-party (not marketplace) and
DMM-screen a sample from the reel before committing to reflow.

---

## 2. Fix (as applied on the bench)

Drop-in replacement with **BC847C** NPN — same SOT-23 footprint, same pin-1
gate-side / pin-2 emitter / pin-3 collector mapping as the BSS138W had for
this low-side switch topology. Rework literally consists of desolder-old,
solder-new.

**One value change**: base resistor from **47 Ω → 3 kΩ** (currently on the
C3 bench build; the iv7.1 rework will land the same swap).

### Base-resistor rationale

With V_GPIO = 3.3 V and V_BE ≈ 0.7 V, the drop across R_B is 2.6 V, so
I_B = 2.6 / R_B. Deep saturation (Vce(sat) ≈ 150 mV, minimal heat) needs
I_B ≥ I_C / 10, or with BC847C's h_FE ≥ 200 we can cheat to I_C / 20.

| R_B    | I_B     | Max hard-sat I_C | Verdict for this LED |
|--------|---------|-------------------|-----------------------|
| 47 Ω   | GPIO-limited ~20 mA | any | Works but burns 20 mA continuous inside the ESP32 output cell whenever the LED is on; deep over-saturation slows turn-off. **Prototype-tolerable, replace when convenient.** |
| 2 k2   | 1.18 mA | ~120 mA | Comfortable margin for indicator-plus-flashlight duty. |
| **3 k**  | **0.87 mA** | **~85 mA** | **Chosen** — same margin, saves 300 µA of GPIO current vs 2 k2. |
| 5 k1   | 0.51 mA | ~50 mA  | Fine up to indicator brightness. |
| 10 k   | 0.26 mA | ~25 mA  | Edge — anything above 25 mA starts leaving deep saturation. |

Rule for later: pick R_B so I_B ≈ I_C_target / 10, then round to nearest
E12. The 3 k choice covers the 5–50 mA range comfortably; if we ever want to
push the flashlight into the >50 mA range for genuine forest-firefly duty,
drop to 2 k2.

### Circuit as-fixed

```
+5V ──[100R]──▶│─── (LED, white) ─── C
                                     │
                                  [BC847C]  NPN, SOT-23
                                     │
                                     E ──── GND

GPIO41 ──[3k]── BASE ──[10k]── GND
```

- LED anode toward the 100 R / 5 V side, cathode to the transistor
  **collector**.
- Emitter to GND, unchanged from the FET topology.
- Base driven from **GPIO41** (iv7.1) or **GPIO2** (bench C3) through a 3 k
  series resistor; 10 k pull-down base→GND retained (same reason — keep the
  base defined during boot / GPIO high-Z windows).

Post-fix bench measurement: LED fully dark with GPIO commanded 0, no bleed
at DMM resolution. Section 12 verification target of the diagnostic doc met.

---

## 3. NPN vs N-MOS — the tradeoffs we accepted

None of these bite at LED-driver scale, but documented so we know what we
gave up:

1. **Continuous base current.** ~0.87 mA (with R_B = 3 k) is drawn whenever
   the LED is on. A MOSFET gate would be leakage-only (< 1 µA). Over a
   full-brightness hour that's ~0.9 mA·h — well below the LED's own draw and
   inside noise for the Kompic battery budget.
2. **Higher on-state voltage drop.** V_ce(sat) ≈ 150–300 mV vs a small
   N-MOS's ~50 mV. At 20 mA that's 3–6 mW of extra heat, at 200 mA it becomes
   30–60 mW. SOT-23 handles either comfortably; matters only if we push into
   high-current territory.
3. **Slower turn-off.** BJT minority-carrier storage adds ~100 ns of turn-off
   delay. Invisible at Kompic's PWM frequencies (≤ 5 kHz typical).
4. **Saturation-depth-vs-h_FE.** BJT saturation depth depends on I_B × h_FE
   ≥ I_C. FET is fully on the moment V_GS > V_th. Matters near the edge of
   deep saturation — pick R_B correctly and it doesn't.
5. **Base drive is current-mode.** GPIO drooping under load pulls the BJT
   out of saturation. FET doesn't care about current, only voltage. Not a
   concern with a healthy ESP32 GPIO.

**Verdict:** for a PWM'd indicator/flashlight LED at ≤ 100 mA the BC847C is a
clean drop-in. The FET route is worth revisiting only if (a) we source a
trustworthy N-MOS batch and (b) we want the microamp-idle draw for
deep-sleep budgets.

---

## 4. Bench archive — sketch 14

`firmware/arduino/14_led_driver_test/14_led_driver_test.ino` — standalone
ESP32-C3 SuperMini + 72×40 SSD1306 OLED test rig, matching the Arduino
freeze-and-forget POC pattern ([[feedback_arduino_sketch_pattern]]).

- LED driver on GPIO2 (bench pin; iv7.1 uses GPIO41 for the same net).
- PWM 5 kHz / 8-bit via `ledcAttach`.
- Two-phase loop:
  1. **STEPPER**: 10 × 10 % steps, 2 s hold each → 20 s total.
  2. **GRADIENT**: 1 % / 200 ms continuous sweep 0→100 % → 20 s total.
- OLED shows current PWM %, raw duty / max, sweep-time %, and a bottom
  progress bar.

This is the fixture I used to prove the batch was dead (leak identical to
iv7.1) and then to prove the BC847C swap cleaned it up.

---

## 5. Vbat divider relocated: GPIO9 → GPIO18

Triggered by "the screen connector is coming, I want to keep Vbat sense once
GPIO9 becomes QSPI-D2 again." Full pin hunt done against the master pinout
(`hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md`).

### The pin-space constraint

- ESP32-S3 ADC lives only on GPIO1–20 (ADC1 = 1–10, ADC2 = 11–20).
- Every ADC1 pin is claimed (I2C ×2, QSPI reset, TP-INT, MAX_INT, LSM_INT1,
  QSPI-D2, QSPI_CS).
- Every ADC2 pin is claimed **except** GPIO17 / GPIO18 (the GPS UART pair,
  free on this prototype because no M10S is being fitted) and the USB pins
  (hardware-reserved).

### First attempt (GPIO46) — dead-end

Physically moved the 5 k1 / 5 k1 divider from GPIO9 to GPIO46. GPIO46 (the
freed TimePulse pin per master-doc rev 20) has **no ADC hardware** — it is
digital-only. Wasted rework; noted so future-me / anyone reading this
skips the same trap: **GPIO ≥ 21 on ESP32-S3 cannot sample voltage.**

### Second attempt (GPIO18) — chosen

- GPIO18 = M10-RX (ESP-side RX from GPS), **ADC2_CH7**, RTC-capable, clean
  boot behaviour.
- No GPS module on this prototype → the pad is electrically idle → the
  divider can drive it with no contention.
- **ADC2 caveat**: ADC2 shares the SAR path with Wi-Fi RF calibration. Reads
  return `ESP_ERR_TIMEOUT` when Wi-Fi is active. Kompic runs BLE only,
  Wi-Fi not enabled in the stack, so this is safe today. Flagged in code
  comments so a future Wi-Fi enable will trigger a rethink.
- Curve-fitting calibration is supported on ADC2 for ESP32-S3, so the
  existing cali path works unchanged.

### Prototype-scope note

This is a **single-unit override**, not a master-pinout revision. The
master pinout keeps GPIO18 assigned to M10-RX; if a future build populates
GPS, the divider comes off. I have not edited the master doc — that
document stays as the design intent.

### Firmware change

`firmware/esp-idf/components/field_capture/fc_battery_test.c` — patched
`vbat_adc_ensure_init()` and `vbat_adc_read_mv()`:

- `ADC_UNIT_1` → `ADC_UNIT_2`
- `ADC_CHANNEL_8` (GPIO9 on ADC1) → `ADC_CHANNEL_7` (GPIO18 on ADC2)
- Comment header + log string updated with the new pin, ADC2/Wi-Fi caveat,
  and the reason this pin was chosen.

Divide ratio (5 k1 / 5 k1 → ÷2) is unchanged; the `mv_at_pin * 2` and
`raw_avg * 3300 / 4095 * 2` in `vbat_adc_read_mv` still apply.

---

## 6. Future work (queued, not for this stage)

### 6.1 iv7.1 board rework

- Desolder BSS138W at the flashlight FET position.
- Solder BC847C in its place (SOT-23, same pads, no keep-out issues).
- Replace the 47 R gate/base resistor with **3 k**.
- Re-verify Section 12 of the diagnostic doc: gate/base ≈ 0 V, LED fully
  dark, base→GND ≈ 3 k.
- Physical move of the Vbat divider from GPIO9 to GPIO18 pad.

### 6.2 N-MOS retry

If we ever want the microamp-idle-current path back, source a fresh N-MOS
batch from LCSC first-party (or Digi-Key). Screen a sample from the reel
with the DMM D↔S test before committing to reflow.

### 6.3 ECG ↔ Kompic USB sync + PPG/BCG CSV mode

Stage 13's HR pipeline works at rest but needs ground-truth data to lock in
the peak-detect / biquad / motion-gate constants. Plan (moved to Stage 13
§7 in detail — this is the pointer):

- **Physical link:** USB-C ↔ USB-C between the standalone ECG device and
  Kompic.
- **Sync command:** ECG sends `SET_TIME use-my-time` payload over USB before
  each session; Kompic acknowledges and adopts the ECG's timebase. Both
  devices then timestamp on their own RTCs (DS3231 on the ECG,
  PCF85063A + [[project_pcf_ram_byte]] on Kompic) — good enough for a
  session-scale offset that gets closed by cross-correlation of the R-peak
  train anyway.
- **Kompic-side mode edit** (not a new mode): pick an existing `fc_mode`
  and add a **PPG + BCG raw CSV timestamped capture** format that mirrors
  the ECG's output. Two/three CSVs per session get aligned offline in
  pandas; that's the data set that unblocks the Stage 13 §5 refinement
  checklist.
- Distinct from the [[project_mode_restructure]] menu reshuffle — that
  changes navigation, this adds a data-capture format.

---

## 7. Reference links

- Diagnostic (open → resolved): `2026-07-09_flashlight_led_leak_diagnostic.md`
  (§13 added today)
- Master pinout (GPIO18 = M10-RX / ADC2_CH7):
  `hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md`
- Bench sketch archive:
  `firmware/arduino/14_led_driver_test/14_led_driver_test.ino`
- Stage 13 (HR pipeline, ECG cross-check plan lives in §7 there):
  `Stage_13_adv_HR.md`
- Firmware change:
  `firmware/esp-idf/components/field_capture/fc_battery_test.c`
