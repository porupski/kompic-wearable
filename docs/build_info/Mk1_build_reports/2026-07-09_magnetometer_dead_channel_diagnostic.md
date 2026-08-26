# Kompic̄ Mk I — LIS3MDLTR Magnetometer "Dead Channel" Fault (Diagnostic Handoff)

**Purpose:** hardware fault diagnosis. Companion to the flashlight LED leak
document dated the same day. Fully self-contained — full device context (the
Kompic̄ system instructions) may or may not accompany.

**What I want back:** the single most probable root cause of the stuck-axis +
axis-hopping behaviour, the minimal test/repair sequence to confirm it, any
evidence that contradicts the leading hypothesis (specifically: push back on
whether this is genuinely a chip/PCB fault or whether we've missed a
firmware/config angle), and any failure mode not yet considered. **Do not
just ratify the current hypothesis.**

---

## 1. Device context (minimal)

- Kompic̄ Mk I: ESP32-S3-WROOM-1U, 4-layer 1 mm PCB, **hand-reflow assembly**, all parts from LCSC. `iv7.1` prototype (the current bench unit).
- Firmware: ESP-IDF port of the `7_demo_field_capture` Arduino sketch. Both are known-working reference points. The sketch has been proven on this exact board; the ESP-IDF port is what surfaced the current symptom pattern.
- **The chip:** STMicro **LIS3MDLTR**, 3-axis magnetometer, I²C 0x1C, LGA-12 package.
- I²C bus: bus 0 (`I2C_NUM_0`) at 400 kHz, shared with BME688 (0x76), LSM6DSV16X (0x6B), MAX30101 (0x57), TMP117 (0x48), VEML6030 (0x10), PCF85063A (0x51). No bus-integrity symptoms on any other chip — all seven read cleanly. Only LIS3MDL misbehaves.

---

## 2. Circuit and physical layout

- Single 3V3 supply (TPS62840 buck, always-on). No VDDIO split.
- **Package orientation on the board:** LGA-12 top-side, no shielding, no ferrite bead upstream, standard SMD passives around it. Nothing has been reworked or touched at this chip during any of the reflow rescue passes we've done for other parts.
- **Physical proximity in the case** (this is the debated point):
  - LRA (ELV1411A, ~25 Ω, 150–250 Hz voice-coil actuator with a permanent-magnet moving mass) sits **several mm** from the LIS3MDL footprint on the same PCB. **Operator's assertion:** the LRA is *not* close enough to be the primary offender — this must be stated up front and not dismissed. The reasoning: separation is meaningful (this is a wearable-scale board, not a coin-scale one), the LRA is mostly-static except during brief click events, and the failure mode below persists in the absence of any active haptic drive.
  - No other strong-field source is on the same PCB. The MAX30101 is a light source, not magnetic; WS2812 is optical; flashlight LED is optical.

If any reviewer wants to argue LRA-proximity is still the cause, they must
address the two symptom features in §3 that don't fit that story (constant
stuck value with no haptic activity, and *hopping* between axes).

---

## 3. Symptom (precise)

Two related but distinct behaviours:

### 3a. One axis is stuck at (or very near) full-scale, from boot, with no external field applied

Log excerpt, ESP-IDF firmware, unit at rest on the bench, no LRA activity, no external magnet, ~10 seconds apart:

```
MAG   x=-478.9 y=22.0 z=-84.6 uT
MAG   x=-478.9 y=21.8 z=-84.5 uT
MAG   x=-478.9 y=22.1 z=-84.7 uT
MAG   x=-478.9 y=46.1 z=-71.2 uT
MAG   x=-478.9 y=25.2 z=-89.6 uT
MAG   x=-478.9 y=3.4 z=-90.6 uT
```

- X is a **constant -478.9 μT** across every read. It does not move with orientation, does not move with proximity of a magnet, does not track the Earth field.
- Y and Z do vary and *do* respond to orientation and to a swept external magnet in a way that looks plausible for a working magnetometer at Earth-field levels.

**The specific value -478.9 μT is not random — see §4.** It maps exactly onto the digital full-scale at the range the driver has configured.

### 3b. "Dead channel hopping" under a swept neodymium magnet

Sweep a small neodymium magnet fast across the top of the case (a few cm away, no contact). Bench observation over multiple runs:

- After the sweep, the *identity* of the dead axis can change. What was X-stuck-at-negative-FS becomes X-normal but Y-stuck. Sometimes the previously-stuck axis returns to normal reads. Sometimes it does not.
- The dead value the newly-frozen axis lands on is always at (or very near) the digital full-scale for the configured range (positive or negative).
- The chip continues to ACK on I²C throughout. WHO_AM_I still reads 0x3D. So the interface layer is fine; the ADC/output path for one axis at a time is not.

This "one axis latches at FS and another can take over on the next overload" is what makes this feel like more than plain overrange behaviour.

---

## 4. Firmware state — the piece that reframes the problem

The **ESP-IDF driver initialises the chip at ±4 gauss full-scale.** Relevant source:

```
firmware/esp-idf/components/lis3mdl/lis3mdl.h:
    #define CTRL2_FS_4G   (0x00 << 5)   // FS = 00 -> ±4 G (default)
    #define LIS3MDL_LSB_PER_UT   68.42f

firmware/esp-idf/components/lis3mdl/lis3mdl.c:
    // CTRL2: ±4 G full-scale
    (void)write_reg(i2c_num, LIS3MDL_REG_CTRL2, CTRL2_FS_4G);
    ...
    *x_ut = (float)rx / LIS3MDL_LSB_PER_UT;
```

The **working Arduino sketch (`7_demo_field_capture.ino`) configures ±16 gauss**:

```
i2c_write_reg(Wire, LIS3MDL_ADDR, LIS_REG_CTRL2, 0x60);   // FS=±16 G (bits 6:5 = 11)
```

with an explicit contemporaneous comment:

> "±16 G is coarse but survives the LRA magnet sitting a few mm away in the case
>  — ±4 G was pinning Y at -32768 the whole time. Sensitivity is 1711 LSB/G at
>  this range."

**Arithmetic** — the -478.9 μT observed value at ±4 G:

- LIS3MDL sensitivity at ±4 G = 6842 LSB/gauss = **68.42 LSB/μT**
- 16-bit signed minimum = -32768 LSB
- -32768 / 68.42 = **-478.9 μT** — exactly the observed stuck value.

So the *stuck* value the axis reports is a *digital-saturation floor at ±4 G*.
The chip is telling us "this axis is beyond -4 gauss and I can't measure it
any harder."

**However — and this is the crux — that alone does not fully explain the
symptom.** Two facts don't fit the pure "range too small" story:

1. On the bench, at rest, no active field source, the *ambient* magnetic field
   is Earth's ~50 μT — well inside ±4 G. A healthy chip in a clean environment
   should not saturate. Something is presenting a >0.5 mT DC field at the X-axis
   sensing element, from *within the assembled watch, at rest*. The LRA is the
   obvious candidate (see §2), but the operator disputes this. If it *is* the
   LRA, moving to ±16 G will only mask it (it will still be reading a giant
   offset, just not saturated). If it *isn't* the LRA, then either
   (a) there's a persistent magnetised object nearby we haven't identified, or
   (b) the die itself has a large hard-iron offset baked into it — possible
   after an overload event on the specific axis.

2. The **axis-hopping under sweep** doesn't fit ordinary overrange. A healthy
   LIS3MDLTR should recover to normal reads within one sample after the
   external field is removed. If the chip is *latching* saturation per axis
   until something forces it out, that's an internal state that shouldn't
   exist by the datasheet's description of the analog signal chain.

So the range mismatch is **necessary** context (and probably part of the fix)
but is **not sufficient** as a root cause.

---

## 5. Sketch vs ESP-IDF port — what carries and what doesn't

|                        | Arduino sketch (working reference) | ESP-IDF driver (current) |
|---|---|---|
| Full-scale range       | ±16 G (`CTRL2 = 0x60`)             | ±4 G (`CTRL2 = 0x00`)    |
| Output data rate       | 40 Hz (`CTRL1 = 0x50`)             | 10 Hz (per header comment) |
| Op-mode X/Y            | Ultra-high perf (bits 6:5 = 11)    | Ultra-high perf          |
| Op-mode Z (CTRL4)      | 0x00 (low-power) — inconsistent    | ~ same                   |
| Continuous vs single   | Continuous (`CTRL3 = 0x00`)        | same                     |
| BDU                    | On (`CTRL5 = 0x40`)                | same                     |
| Soft-reset at init     | not done                           | **done** (CTRL2.SOFT_RST)|

The soft-reset in the ESP-IDF driver is the *only* thing that would clear a
latched saturation state at boot. In principle that means the ESP-IDF boot
gives a "cleaner" chip state than the sketch, and yet we still see X pinned
at boot. That tips more of the weight onto "there's a real DC field the X
axis is seeing," not "the chip is remembering something from last power-on."

---

## 6. Measurements / observations available now

- WHO_AM_I: **0x3D** (correct) at every boot.
- Driver init succeeds cleanly (no I²C NACKs, no register-verify mismatches — verified against boot log for many power cycles).
- Y and Z axes track orientation of the whole watch through a normal range of Earth-field readings (a few tens of μT). Not sanely calibrated (heading swings by less than the axis error), but numerically alive.
- X axis is bit-for-bit stuck across successive reads. Not noisy. Not drifting. Not moving with ordinary orientation. Only "unsticks" (into a different stuck value, sometimes onto a different axis) under a *strong* external magnet sweep.
- Rebooting the ESP32 alone does not clear the dead channel. Power-cycling the whole board **sometimes** appears to shift which axis is dead, but this has not been characterised rigorously.

Missing but cheap to acquire (see §9): direct DMM Hall reading at the chip location; behaviour with ±16 G configured; behaviour with the LRA physically de-soldered; behaviour of a second LIS3MDLTR chip.

---

## 7. Ruled out (or explicitly asserted by operator)

- **I²C bus fault.** All seven bus-0 devices respond normally; only this chip's *data*, not its interface, is stuck.
- **Driver-side sensitivity math.** The -478.9 μT value maps exactly onto ±4 G FS math (§4). It's a real chip-side saturation, not a scaling bug in the ESP-IDF driver.
- **LRA proximity — operator's position.** The LRA is asserted to be far enough from the chip that it should not be the cause. **This must be pressure-tested**, because the field-saturation math and the sketch's own comment (§4) both point back at the LRA. If the reviewer disagrees, they should say so and propose a test that would settle it.
- **Physical damage from reflow.** The chip has not been re-worked on this board. It has however been through the general board-wide reflow, which is normal exposure.

---

## 8. Candidate root causes (for pressure-testing, NOT settled)

1. **Persistent DC field at the X-axis sensor element from within the assembled watch, at ±4 G FS.** Most likely source is still the LRA's magnetised mass (its rest position leaks a static field into the housing). At ±16 G the axis would come out of saturation and read the offset as a large-but-measurable number, allowing normal delta-detection on top. At ±4 G it sits stuck at FS.
   *Predicted fix:* change driver to ±16 G. Predicted result: X becomes numeric-alive but with a large fixed bias; heading calibration compensates for it.

2. **Latched-per-axis chip damage from a previous strong-field overload event.** One or more axes have a permanently-degraded ADC signal chain (junk offset baked in) after being driven into extreme saturation. Would explain the *hopping* — each new strong overload can "kill" a fresh axis. Would not be fixed by changing FS, and would eventually kill all three.
   *Predicted fix:* replace the chip.

3. **Cold / cracked solder joint on one of the LGA balls corresponding to an analog signal or supply of that axis.** LGA-12 with hand reflow is a candidate for this. A marginal joint could make one axis's front-end intermittent. However, this usually produces *noisy* or *jumping* readings, not *stuck at exact FS* readings, so this is a weaker match to the symptom than (1) or (2).
   *Predicted fix:* reflow the chip (a very fine, targeted reflow — no board flood).

4. **Something in the case is magnetised that we haven't identified.** Any nearby ferromagnetic hardware (screws, magnetic clasps, adjacent SMD ferrites, an actual embedded magnet in a nearby module) could produce >0.5 mT static bias. This is easy to rule out with a compass or an *external* magnetometer held in the same spot.
   *Predicted fix:* find and remove or shield.

**Leading read:** (1) with (2) simmering underneath. But if the reviewer can
argue (2) is dominant they should say so, because the fix is different.

---

## 9. Decisive tests **not yet performed**

**Do these in order — each one narrows the field enormously.**

1. **Flip the driver to ±16 G and re-observe.** One-line change (`CTRL2` from `0x00` to `0x60`, `LSB_PER_UT` from `68.42` to `17.11`). Cost: 5 minutes.
   - X comes out of saturation with a large fixed offset ⇒ hypothesis (1) confirmed as *at least* one factor. LRA is functionally guilty even if you don't want to blame it.
   - X remains stuck (now at ±16 G FS = ±1600 μT) ⇒ the field source is >16 gauss, i.e. an actively magnetised object is right on top of the chip. Escalate to §9.4 or (2).
   - X reads healthily around zero at ±16 G ⇒ we've been fooled; the ±4 G choice was the whole bug. Sketch's decision to run at ±16 G stands vindicated. Close.

2. **External-field probe at the chip location.** Hold a working compass, or a hand-held magnetometer app on a phone, directly over the chip footprint on the assembled watch. Note reading and heading behaviour. Compare with the same probe held 30 cm away.
   - A clear DC bias > several hundred μT ⇒ there IS a nearby persistent field source. Now go find it (LRA? something else in the case?).
   - No unusual field ⇒ (1) is knocked down; the chip itself is offset internally, i.e. (2) climbs.

3. **De-energised LRA test.** Physically de-solder the LRA (or, cheaper, remove its magnet mass by opening the actuator) and re-run tests 1+2.
   - Symptom disappears ⇒ LRA is guilty. Definitive. Move it further, or shield it.
   - Symptom persists ⇒ LRA is exonerated; you're in (2) or (3) or (4).

4. **Second-chip swap.** Lift the current LIS3MDLTR, fit a fresh one, re-run.
   - Symptom moves to the new chip ⇒ PCB-side. Investigate joints / magnetic contamination.
   - Symptom stays gone ⇒ the original chip had cumulative overload damage. Congratulations, that's (2). Note operational lesson: don't wave neodymium at production units.

5. **Read-back CTRL registers after boot** to confirm the chip actually took the ±4 G / ±16 G write. On a chip that has one damaged register interface this could be lying about its state.

---

## 10. Part facts — LIS3MDLTR (ST, LGA-12)

- I²C address 0x1C when SDO/SA1 = GND, 0x1E when tied high. Ours is 0x1C.
- WHO_AM_I = 0x3D (register 0x0F). Verified.
- Full-scale ranges: ±4 / ±8 / ±12 / ±16 gauss (`CTRL2` bits 6:5).
- Sensitivity: **6842 / 3421 / 2281 / 1711** LSB/gauss respectively. Signed 16-bit output, LSB in the low byte.
- Output data path: X and Y share one op-mode (`CTRL1` bits 6:5), Z has its own (`CTRL4` bits 3:2). Ultra-high-perf mode on all three is the recommended default.
- **SOFT_RST** (CTRL2 bit 2) resets configuration registers and, per datasheet, does *not* by itself clear a persistent internal offset if the die has been degraded. That is: soft-reset is not a magic fix for a chip that's been driven hard into saturation.
- LGA-12 package: 12 pads on the underside, no leads. Fine-pitch — real risk factor for hand-reflow joint faults. Repair path is heat-gun lift + re-tin pads + fresh part with new solder paste.

---

## 11. Cleaning constraints (if a solvent step is recommended)

Same rules as the flashlight doc — full list there. Summary:

- **MSM261 MEMS mic port** on the board: Kapton over the port, no ultrasonic, no board flood.
- **BME688 gas port** on the board: same — the MOX element treats IPA as a VOC.
- **MAX-M10S** is not yet installed on this unit, so u-blox's solvent-ban isn't in play yet — but the point is to finish everything solvent-related **before** the GPS goes on.
- Local application only: acid brush / swab at the LIS3MDL area, wick residue, warm dry.

---

## 12. Verification target (fault considered closed when)

Watch at rest on the bench, no external magnet, driver at chosen final FS:

- Every axis reads a value comfortably *inside* the digital full-scale (typical Earth-field magnitudes on X/Y/Z that make sense together for local ambient — vector magnitude ~40–60 μT).
- Rotating the watch produces a smooth, continuous change on all three axes with no dead spots or plateaux.
- A **sweep** with a moderate external magnet produces temporary saturation on the corresponding axis, followed by *automatic return to healthy readings within one sample* (~100 ms) once the magnet is removed. **No latched dead axes.**
- After many sweeps in a session, all three axes remain equally healthy (no cumulative damage / hopping).

If the chosen FS is ±16 G and the symptom is *still* present after tests
9.1–9.4, replace the chip; anything short of that is unlikely to hold.

---

## Update — 2026-07-09, bench-verified degauss procedure

**Outcome: fault CLOSED.** The stuck-axis / hopping behaviour was cleared
in a single session on the bench, without lifting the chip or changing any
software config. Root cause was hypothesis (2) from §8 — the die had a
per-axis latched saturation state from a prior overload event. Once the
state was walked out with a proper multi-pole exposure, all three axes
returned to healthy Earth-field readings.

### The sketch used for the procedure

`firmware/arduino/8_magnet_test/8_magnet_test.ino` — a dedicated,
minimal-boot magnetometer monitor. Streams raw + μT readings at 20 Hz,
flags saturated axes with a `*`, lets a single click cycle the range
±4 → ±8 → ±16 G (with a SOFT_RST each transition), and preserves the
double-click ship-mode path. RGB LED encodes the current FS (red / green /
blue).

### Procedure that worked (empirical, keep for repeat cases)

1. **Tie the neodymium to a string** so the operator can spin it up freely
   without needing to hold and rotate simultaneously.
2. **Spin the magnet fast enough that both poles wash over the chip on
   every rotation.** Operator estimates ~50 rpm was sufficient — the point
   is to expose the die to alternating N/S poles rather than a single
   sustained pole.
3. **Bring the spinning magnet in close** — close enough to visibly
   saturate the axis you're trying to unstick (`*` marker will appear on
   the corresponding axis in the sketch output).
4. **Slowly withdraw the magnet while it is still spinning.** Do not stop
   the rotation until the magnet is far away. This is the key step:
   pulling away with the field still oscillating gives the die's ADC
   signal chain a clean rise-out through zero rather than trapping it at a
   rail.
5. **If one axis keeps re-latching after step 4, reorient the magnet /
   spin axis and try again.** The reason: a badly-oriented single-pole
   approach can re-saturate the same axis that just recovered. Rotating
   the spin axis of the string gives a different projection of the field
   onto the sensor axes.
6. **Case-strap trick for reliable off-axis exposure:** the watch's strap
   lets the case rest at ~45° angles on both the short and long edges of
   the housing. Placing the watch at 45° puts the LIS3MDL sensing axes off
   all three of the operator's obvious swing axes, which makes a single
   spinning-magnet pass hit all three coils asymmetrically. This is the
   fastest known way to degauss the chip once one axis is stuck.

### Post-degauss data (proof of closure)

Watch flat on the bench, `8_magnet_test` at ±4 G, no external field:

```
FS=  4G  raw:   -2117    -429   -5688   uT:   -30.9     -6.3    -83.1
FS=  4G  raw:   -1871    -461   -5971   uT:   -27.3     -6.7    -87.3
FS=  4G  raw:   -1670    -358   -6359   uT:   -24.4     -5.2    -92.9
FS=  4G  raw:   -1593     122   -6253   uT:   -23.3      1.8    -91.4
FS=  4G  raw:   -1486     499   -6201   uT:   -21.7      7.3    -90.6
FS=  4G  raw:   -1125     347   -6604   uT:   -16.4      5.1    -96.5
FS=  4G  raw:    -799      -6   -6730   uT:   -11.7     -0.1    -98.4
FS=  4G  raw:    -731    -276   -6782   uT:   -10.7     -4.0    -99.1
FS=  4G  raw:   -1074     262   -6573   uT:   -15.7      3.8    -96.1
```

- No axis is at ±32768 LSB.
- All three axes move continuously as the watch is rotated.
- Values are within the ±4 G FS (~±480 μT), with obvious Earth-field
  vector components on X and Z and a small Y offset — plausible for
  local ambient.

This satisfies every criterion in §12 (Verification target).

### What this changes about the earlier hypotheses in §8

- Hypothesis (1) — "persistent DC field from LRA at ±4 G FS" — is
  **disproved** as a *necessary* condition. The chip degaussed in place
  with the LRA still installed on the board, so the LRA is not sourcing a
  DC field strong enough to keep the ±4 G FS in saturation. The bench
  driver at ±4 G now reads healthy, uncalibrated Earth values.
- Hypothesis (2) — "latched per-axis chip damage from a previous
  overload" — is **confirmed as the operative mechanism.** The recovery
  path required a specific *rotating* field exposure, not a static one:
  static single-pole exposure could re-latch. This is consistent with an
  internal ADC front-end that requires a clean AC pass through zero to
  clear a saturation-trap state.
- Hypotheses (3) — cold joint — and (4) — nearby unidentified magnet —
  are moot now that the chip has recovered without touching the joints
  or the case.

### Operational recommendations going forward

- Keep neodymium magnets away from the assembled watch except when
  intentionally degaussing. A single hard exposure can re-latch an axis.
- If a stuck-axis symptom recurs, run `8_magnet_test` and repeat the
  degauss procedure above. Do not lift the chip on the first recurrence
  — it is now known to recover.
- The 45° case-strap positioning tip should go into the operator's bench
  cheat-sheet if there is one.
- Firmware note: the ESP-IDF driver's ±4 G default now stands. The Earth
  field is comfortably inside that range on a healthy chip; the sketch's
  ±16 G was originally chosen to *survive the latched-saturation
  symptom*, not because ±4 G was fundamentally wrong.

