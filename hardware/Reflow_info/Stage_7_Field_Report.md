# Stage 7 Field Report — Kompic Mk I (iv7.1)

**Date range:** 2026-07-12 → 2026-07-13 (field-wear + bench sessions after Stage 6 close)
**Board:** iv7.1, end-of-line for hardware rework; used as the firmware-development + field-observation target.
**Builder:** Ivan
**Stage:** 7 — field wear, UX decisions, sensor-strategy reset, downstream design implications
**Prerequisite:** Stage 6 Build Report (2026-07-11). Coming in: ESP-IDF port working end-to-end, all modes functional, one open bug (USB-C charging, hardware-suspect), one deferred surprise (ECG-via-QVAR turned out to be the wrong sensor architecture).

---

## 1. Executive summary

Field-wear stage. Very little bench soldering; lots of the watch being worn, shown to people, and used in real-world contexts, which surfaced UX and reliability issues that never show on a bench-clamped board. The takeaways cluster in three places:

- **UX**: double-click shutdown is a landmine. Pocket touches trigger it, or worse, wake the device and then double-click-shutdown seconds later during pocket motion. Replacing it with a **4-second press-and-hold** with a haptic-warning ramp starting at 2 s. Release before 4 s aborts cleanly. This is now the top-priority-preempt path on GPIO16.
- **Sensor strategy reset**: QVAR is not ECG (formally admitted in Stage 6 §7); on iv7.1 it *is* usable as a touch/button sensor for the crown + wrist-facing electrode without any hardware change. So we pretend on iv7.1 that "the QVAR pins are input buttons" and put the field UX around that assumption now. HR strategy is pivoting to **MAX30101 firmware squeeze + BCG on the LSM6DSV16X**, per the HR-sensing handoff document.
- **Iv7.1 keeps being a functional but grumpy field prototype**. The one recurring reliability blocker is the button occasionally jamming after a press — for now: accept, plan mechanical fix in iv8.0. Encoder is great when the PETG crown is aligned. Magnetometer works well ~70% of the time; the 30% failures are algorithmic (hard-iron cal quality), not chip issues.

The Stage 8 doc (re-blueprinting) is being written alongside this; it captures the *hardware* implications of everything below and defines the iv8.0 spin.

---

## 2. On-wrist wear observations

Wearing it around, showing it to people, doing everyday things with it strapped on. Not a lab test, just: *does it work as a wristwatch does?*

- **Weight and fit**: fine. 3D-printed PLA case is bulky but the strap holds it stable. Not something you forget you're wearing, but not annoying either.
- **Reachability of the controls**: the button and encoder are on the crown side (right, index-finger accessible). Cycling modes with the encoder is easy one-handed. Pressing the button for a recording is easy. **Double-click for shutdown** — awkward one-handed and *far too easy to trigger by accident in a pocket, in a bag, or when adjusting a sleeve*. See § 3.
- **Battery life during field wear**: not yet characterised end-to-end (BQ25619 doesn't do proper SoC estimation, and USB-C charging is currently borked on this specific unit). Field observation only: multi-hour continuous operation without power-down.
- **Case optical dome for the MAX30101**: unexpectedly good. The dome protrudes ~0.2 mm below the case surface, so when the strap is snug, skin seals against the dome cleanly with no visible air gap. The MAX30101 window is centred in the dome. Whatever the wrist-PPG problem is, it's not this — the mechanical mount is one of the parts of iv7.1 that actually works well.
- **Showing it to other people**: interesting failure mode. Someone else picks it up, doesn't know the double-click gesture exists, presses the button curiously, presses it again — and the watch is now in shutdown. This alone motivated the § 3 change.

## 3. Shutdown UX change — double-click to 4-second hold with buzz ramp

**Removed**: double-click on GPIO16 → immediate ship mode.

**New behaviour**: press-and-hold GPIO16 for **≥ 4 seconds**, continuously LOW.
- **0 – 2 s**: LED glows solid red (visual confirmation the button is registered as pressed).
- **2 – 4 s**: DRV emits a short click every **500 ms** (haptic countdown — the "you are about to shut down" tell).
- **≥ 4 s**: LED goes to full red, DRV fires a long buzz, `bq25619_enter_ship_mode()` writes REG07, BATFET drops.
- **Release at any point before 4 s** → state resets instantly, no ship-mode fired, LED and haptic back to whatever the current mode was doing.

Rationale: pocket-triggering ship mode is unacceptable. Even a stuck button (see § 2 — the button occasionally jams) with the current double-click semantics could shut the watch down if a second bounce hit within 350 ms. Hold-based shutdown requires *intent* — 4 s is long enough that no accidental press achieves it, but not so long that a deliberate shutdown feels labor-intensive. The haptic warning at 2 s gives the operator time to abort even if they didn't mean to.

**Priority stays the same**: dedicated Core-0 watcher task at priority 6, 5 ms poll cadence, own state machine independent of `field_capture`. Cannot be blocked by any recording / mic capture / plot loop.

## 4. QVAR realization arc — what it is, what it isn't, what we do with it now

Stage 6 § 3.6 / 3.7 chased ECG on the LSM6DSV16X's QVAR block, closed with the correction in Stage 6 § 7: QVAR is a **charge-variometer** designed for user-interface touch / tap / swipe, not for biopotential measurement. The two register-layout bugs found in the process (`AH_QVAR_EN` bit position, `CTRL1` field ordering) were real bugs that needed fixing regardless. The ECG mode itself works — it streams QVAR samples, notch-filters mains hum, runs a beat detector — but the samples aren't cardiac potentials, so nothing that looked like an ECG waveform ever emerged.

**Stage 7 pivot**: use QVAR for its designed purpose *right now*, on iv7.1, without changing hardware. Two channels of QVAR are wired out on this board. Treat them as inputs, one each:

- **Qvar1** → conductive touch pad to be positioned **next to the crown** (temporary placeholder for the metallic crown planned for a titanium case). Touching this pad = "button next to the button", used for auxiliary UX like triggering magnetometer recalibration during compass mode, or a "confirm" action.
- **Qvar2** → conductive pad on the **wrist-facing side of the case bottom**. Sustained charge (baseline shift over ~500 ms) = "watch is worn". Used to gate PPG / BCG / etc. so the sensor stack doesn't burn LED power when the watch is off-wrist.

Behaviour distinction to bake in from the start:
- **Sustained charge** (both electrodes above threshold for ≥ 300 ms) → treat as a *touch event*.
- **Charge transient without sustained level** (spike then decay) → treat as a *bump*, log but ignore.

This is the classic QVAR tap-vs-swipe pattern from ST's own app notes. It works because QVAR's amplifier has a very-high input impedance so charge dumps decay slowly on sustained contact but decay fast on brief transients.

## 5. Crown as touch button, and the crown mechanical stack-up

Right-side crown assembly, left-to-right (case-side to outside):

**Current iv7.1**: `button | pogo-pin (Qvar) | encoder | rubber-ring-in-casing | crown top`.

**Problem observed**: the pogo pin (not currently installed) sitting between button and encoder applies lateral pressure that crooks the crown's rotation axis. Rotation feels sloppy when the pin is compressed.

**iv8.0 fix**: reorder the stack so the pogo pin is between the encoder and the sealing ring, where it can compress axially without deflecting the encoder shaft:

**Iv8.0 target**: `button | encoder | pogo-pin (Qvar) | rubber-ring-in-casing | crown top`.

Also observed during field use:

- **Encoder is genuinely great**. Detent clicks are crisp, the polled state machine (from Stage 6 § 3.3) catches every detent, DRV click per detent feels right. Wonky PETG crown geometry sometimes lets it slip against the encoder shaft if the PCB isn't perfectly aligned — a mechanical / material fix, not a firmware issue.
- **Button occasionally jams after a press**. Not always, roughly 5% of presses. Two suspected causes: (a) dry mechanical fit that would benefit from vaseline / silicone grease, (b) the switch's return spring is marginal for this actuation force. Either fix is trivial on iv8.0 (grease it or spec a stronger switch). For iv7.1: accept, and the 4-s-hold shutdown means a stuck button just triggers ship mode after a while — annoying but not dangerous.
- **The whole assembly** (encoder body pressed against the case wall via PCB flex, with the USB-C rubber o-ring providing the return force) is a small miracle of packaging. Works but should not be the iv8.0 approach; document the assembly path taken so the same thing can be done deliberately or replaced with something cleaner.

## 6. Wrist-facing electrode as "watch on hand" detector

Currently no dedicated electrode pad on the wrist-facing side of iv7.1's case bottom. For field usage in Stage 7 the Qvar2 line goes to a shorted trace under the case — effectively floating. The "on-hand" detection concept is thus **firmware-defined for iv7.1** but not physically wired yet.

**Iv7.1 firmware behaviour**: treat "on-hand" as always true (assumed) for now. All PPG / BCG / temp reads happen regardless.

**Iv8.0**: add a small conductive pad on the wrist-facing side of the case bottom, wired to Qvar2. Qvar2 sustained baseline shift → `g_watch_on_hand = true`. Sensor tasks read this and gate themselves — MAX30101 LED off, BCG stillness gate paused, etc. Saves battery when the watch is off the arm.

## 7. MAX30101 wrist SNR — status and plan

Summary of the HR-sensing handoff document (`hardware/Reflow_info/reference_files/Kompic_Mk1_HR_Sensing_Handoff.md`):

- **Hard ceiling** is the MAX30101 not having a DC-cancellation DAC in its signal chain. It digitises DC + AC directly. Since PPG's pulsatile AC is <1–2 % of a static-reflection DC, raising LED3_PA moves DC *and* AC linearly, and DC clips the 18-bit ADC before AC gets useful. This is the architectural gap the ADPD4101 (Mk II) fills.
- **Squeeze strategy** (still worth doing): green-only Multi-LED slot, max pulse width (411 µs), lower ODR (50–100 Hz) to free integration budget, ADC full-scale up to 16384 nA for headroom, hardware sample-averaging 4–8× for √N SNR. Sweep LED3_PA to find the current where DC clips *just* below the ADC ceiling and PI = AC/DC peaks.
- **Optics beat electronics** on this class of problem. Light-guide / cup between LED and PD, opaque bezel around the sensor, firm skin contact via strap tension.

Field observation on iv7.1: the case optical dome is unexpectedly well-executed (§ 2), which removes one variable. The remaining bench work is the LED-current sweep + optional light-guide + firm strap.

**Sequencing**: Arduino sketch `firmware/arduino/10_test_max30101` already streams raw green counts for the sweep. Next bench pass: extend with automatic PA sweep + PI logging; find the best-PA point; bake into the ESP-IDF driver.

## 8. BCG on the LSM6DSV16X — plan

Straight from the HR handoff § 2:

- Every heartbeat recoils the body (~tens of mg at the wrist). At rest, low-noise IMU resolves it.
- Zero BOM cost, zero GPIO cost — LSM is already on the board.
- Not a PPG replacement — a **complementary** rest/sleep channel that can carry HR when the PPG is gated for power.
- Requires stillness (motion artifact swamps the recoil signal). The MLC's job later: classify "still enough" → "BCG valid".

**Sequencing** (per handoff): Arduino bench validation first with a new sketch `firmware/arduino/11_test_bcg` — ODR ≥ 240 Hz, FS ±2 g, HP mode, bandpass 0.8–10 Hz, peak-detect or autocorrelation. Cross-check BPM against MAX30101 finger PPG or a manual pulse count. Only after acceptance does it get ported into ESP-IDF as a fusion component.

## 9. Magnetometer field observations

After the Stage 6 § 3.5 degauss fix, the magnetometer is a working compass mode. Field observations:

- **~70 % of cal runs produce a usable heading** — north points where it should, rotation feels linear, N/S DRV pulse at ± 5° works.
- **~30 % of cal runs produce garbage** — heading drifts, N points sideways, or the DRV pulses at meaningless angles. Requires reboot to retry.

Failure modes hypothesised (not confirmed with instrumentation):

1. **Undersweep**: operator didn't rotate the watch through a full 360° on both axes during the 10 s cal window. Only X min/max got a good spread; Y stays near noise floor; heading is unreliable.
2. **Nearby interference**: laptop, phone, magnetic clasp, chair frame, or the LRA magnet's own DC field affecting the cal.
3. **Single-sample spike**: a fast field transient (elevator? passing car? someone's phone?) latches one axis's min or max to a wildly wrong value that skews the offset.

Small firmware upgrades planned:

- **Outlier rejection during cal**: only accept a new min or max if its delta from the running median of recent samples is within a plausible bound. Kills the single-spike case.
- **Recalibrate-on-demand via Qvar touch**: hold the Qvar1 touch pad for 3 s while in compass mode → runs a fresh cal. Currently cal is boot-locked and only runs the first time you enter compass mode after a reboot; adding a mid-session re-cal gate makes the 30 % failure recoverable without a reboot.
- **N/S colour convention**: current implementation is `cos(heading)` → red for N, blue for S. This matches the near-universal convention (compass needles, aviation, marine, orienteering all put red on the north-pointing tip). No change needed; verified this stage.

Also filed: full magnetometer diagnostic + degauss procedure at `hardware/Reflow_info/2026-07-09_magnetometer_dead_channel_diagnostic.md`. Recurrence procedure documented; the chip has held up since.

## 10. New TEMP mode — full thermal map of the board

Motivation: the watch has *five* temperature sensors on it right now (four dedicated, one incidental in the SoC). Real-time visibility on all of them is:

1. Debugging aid — "why is the reading weird? because the SoC is running at 65 °C and heating the neighbouring chip".
2. Health signal — battery temp, skin temp, SoC temp all in one glance.
3. Foundation for a future graphical heat map ("here's the watch, colour-coded temps at each part").

**Sensors to aggregate**:

| Source | Where | Purpose |
|---|---|---|
| TMP117 | I²C 0x48 | Skin / case-contact temp (existing skin mode) |
| BME688 | I²C 0x76 | Ambient temp (die + air), also gas / hum / pressure |
| LSM6DSV16X | I²C 0x6B (reg 0x20/0x21 `OUT_TEMP_L/H`) | IMU die temp |
| MAX30101 | I²C 0x57 (reg 0x1F/0x20 `TINT/TFRAC`, requires DIE_TEMP_RDY read) | Optical-sensor die temp |
| ESP32-S3 | Internal temp-sensor peripheral (`temp_sensor_read_celsius()`) | SoC junction temp |

**Behaviour proposed**:

- New `FCM_TEMP` mode inserted into the mode ring.
- LED signature: cycling through warm colours (red → orange → yellow → red) at ~ 0.5 Hz to make it visually distinct from every other mode's signature.
- Single-click enters the mode; 1 Hz serial print showing all five temps in a fixed-width row.
- Optional SD log to `/sd/data/temp/s<boot>_r<seq>.csv`.
- Single-click during mode exits back to STANDBY.

**Future path (iv8.0 + display)**: when the display is back, render a stylised watch outline with the five sensor bubbles positioned at their approximate physical locations, colour-coded by temperature. The firmware side (aggregator task + broker channel) is exactly the same; only the display renderer changes.

## 11. Standing bugs and where they stand at Stage 7 close

| # | Bug | Category | Status | Path forward |
|---|---|---|---|---|
| 1 | Flashlight LED sub-µA cosmetic leak | HW iv7.1 | Open since Stage 4 | Diagnostic doc at `hardware/Reflow_info/2026-07-09_flashlight_led_leak_diagnostic.md`; iv8.0 to review FET drive path or accept |
| 2 | USB-C charging fault on this unit | HW iv7.1 | Open — Stage 6 §3.10 addressed all *software* causes; hardware VBUS pad fatigue suspected | Mechanical rework on iv7.1 (this unit) or move to a fresh iv8.0 board |
| 3 | Button occasional stick after press | HW iv7.1 | Observed field bug | Grease it, or spec stronger return spring in iv8.0 |
| 4 | Magnetometer 30 % bad-cal rate | Firmware algorithmic | Understood | Outlier rejection + on-demand recal via Qvar touch (§ 9) |
| 5 | `qvar_ecg` REQUIRES-cycle in build graph | Build hygiene | Bypassed via inline QVAR helpers in field_capture.c | Move `ecg_tile.c` into `lvgl_ui/` or its own component |
| 6 | `bq25619.c` uses wrong I²C mutex | Driver latent race | Observed | One-line fix, deferred |
| 7 | `lsm6dsv16x.h` CTRL1 bit layout reversed | Driver | Local override in field_capture.c ECG path | Global fix in the driver, port over |
| 8 | MAX30101 wrist SNR marginal | Physics + optics | Squeeze strategy known | Bench sweep + iv8.0 optics |

Not new; enumerated so nothing gets lost.

## 12. Firmware behaviour changes shipped in Stage 6 (recap for closure)

Not re-litigating in detail — see Stage 6 § 3 — but for the record, the following landed and are verified working in field wear:

- RTC CLI on USB-CDC (`SET_TIME` / `GET_TIME`) with boot-time RTC time print
- Priority shutdown watcher on GPIO16 (in Stage 7 rewritten from double-click to 4-s hold — see § 3)
- BQ25619 watchdog disable + TS_IGNORE + BATFET_DIS clear at boot
- Sensor park policy (only mode-relevant sensors wake per recording)
- Compass mode (10 s figure-8 cal + red-blue gradient + DRV lock pulse)
- ECG mode (streams QVAR, notch filter, beat detector — but signal source is wrong, see § 4)
- Magnetometer degauss procedure documented
- Field-capture archive backup at `firmware/esp-idf/archive/9_working_demo_iv7.1_11july2026.7z`

## 13. Handoff into Stage 8 (re-blueprinting)

Everything in the above stage that has a *hardware* implication gets a section in `Stage_8_Re-Blueprinting.md`. Explicit crossover mapping:

- § 3 (shutdown UX) → firmware only, but the button mechanical fix (§ 5, jamming) is a Stage 8 concern.
- § 4 – 5 (QVAR as touch button) → Stage 8 §5 electrode/touch redesign.
- § 6 (wrist-facing electrode) → Stage 8 §5.
- § 7 (MAX30101 optics) → Stage 8 §6 optical dome + light-guide + shroud.
- § 8 (BCG on LSM) → firmware only (existing sensor).
- § 9 (mag cal) → firmware only.
- § 10 (TEMP mode) → firmware only (existing sensors).
- § 11 bugs 1–3 → Stage 8 §11 (bugs and how iv8.0 addresses them).

Versioning note ratified this stage: **iv7.1 is current canon**. All revisions carrying the current mask + BOM + physical layout are considered iv7.1+. The first fixed board (with iv8.0 changes) becomes **iv8.0**. Once iv8.0 is assembled and a working unit exists, that unit becomes **Kompic Mk II**. No versioning changes on the existing physical prototype.
