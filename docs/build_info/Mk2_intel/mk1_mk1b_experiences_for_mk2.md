# Mk1 + Mk1b field-learnings for Mk2 design

**Date opened:** 2026-09-14
**Status:** JOTTING. This is a running-notes file jotted from bench
memory + recent stage bench logs. Not a finished document.

**Full-scope pass planned:** during Mk2 PCB fab wait -- collapse this
+ every `Mk1_build_reports/*.md` + `Mk1b_build_reports/*.md` +
`Stage_15_ECG_*.md` reference specs + auto-memory into a single ~100 pp
readable Mk2 design brief. That is a separate, later effort. Do NOT try
to make this file complete today.

**Purpose here:** capture the hard-won stuff **before it evaporates**,
because it lives in Ivan's head + scattered bench log entries and can
easily be forgotten by the time Mk2 layout starts.

---

## 1. Component vulnerability tier list

Ordered by how easy each part is to kill (or damage into flaky state) on
Mk1/Mk1b bench builds. High tier = handle carefully in Mk2 design (better
placement, better ESD, better thermal isolation, or plan for hot-swap).

### Tier S -- extremely fragile

- **MAX30101 (PPG).** Killed multiple times on Mk1/Mk1b bench. Sensitive to
  reflow temp and probably ESD. Mk2: consider socketed variant during
  bring-up, or place in a corner reachable for hand-soldering rework.
  Also consider MAX86141 as successor (same-family, better power, more
  channels).
- **MAX-M10S (GNSS).** Also easy to kill; suspected reflow heat + LDO
  strain. Mk2: check reflow profile margin, add a decoupling+bulk cap
  audit, and consider a heat-shield in the reflow oven for this corner
  of the board. Alternatively, look at the L86-M33 / MIA-M10Q as
  drop-in successors.

### Tier A -- reflow-sensitive but resilient once working

- **VEML6030 (ALS).** Optically fine, but if overburnt in reflow the
  package **darkens to yellowish**. That shifts its spectral response
  and its absolute lux calibration. Mk2: hit the low end of the reflow
  window for the sensor-cluster corner.
- **RGB LED (WS2812-ish).** Same failure mode as VEML: the transparent
  package yellows on overheat, colors shift. Same reflow-window
  discipline.

### Tier B -- consistent gotcha, mechanically small

- **PCF85063A RTC.** The exposed **GND pad on the bottom loves to short
  itself** to adjacent traces / vias / pours during reflow (solder pull
  toward the pad). Mk2: add a small keep-out ring under the RTC's GND
  pad, or slightly recess the pad in the copper pour to give the paste
  somewhere to go besides sideways.

### Tier C -- easy to work with

- **LSM6DSV16X (IMU) + LIS3MDL (mag).** Once the I2C addresses + power
  sequencing were figured out, both behaved. See [[project_kompic_mk1]]
  and Stage 25/26 bench logs. Mk2: keep them, no changes needed.
- **Mic (whichever PDM/analog we're on).** Well-behaved throughout. No
  Mk2 concern flagged.
- **BME688 (env).** **Easiest of the bunch.** Reflows cleanly, boots
  clean, self-calibrates, its BSEC library is annoying but that's a
  firmware problem not a hardware problem. Mk2: keep.

### Tier: connectors (their own category)

- **OF-24 (whatever exact P/N -- see schematic) display FPC connector.**
  **Awful.** Contacts die under normal use. Clogs with flux residue and
  fails intermittently. Was a major bench-time sink on iv7.1 and Mk1b.
  Mk2: **switch to a different display connector family entirely.**
  Wurth or Hirose ZIF FPCs with proper actuator latches, or move to a
  soldered board-to-flex tail.
- **USB micro (or whatever we have).** Dies from mechanical stress
  (repeated insert/remove cycles + cable side-load). Ivan has already
  picked a better alternative for Mk2 -- USB-C mid-mount or a
  through-hole reinforced variant. Confirm the exact P/N in Mk2 BOM.
- **PCB antenna area (WROOM module).** Not a connector per se, but the
  ceramic antenna region is finicky and blocked us from adding an
  antenna on Mk1b at all -- see §3.

---

## 2. PCB manufacturing lessons

Notes from hand-reflow on the hotplate + hand-solder rework.

- **Reflow overheat is visible.** Symptoms in escalating order:
  1. Silkscreen darkens (first sign).
  2. Silkscreen goes yellow (definitely too hot).
  3. Solder mask darkens (over the line).
  4. Transparent packages (VEML, RGB LED) go yellow (component-side
     damage).
- **Two-sided board is the process's biggest cost driver on our side.**
  Every additional bring-up is two paste applications + two reflow
  cycles + two rework passes. Mk2: audit whether we can shift low-heat
  components to one side + high-heat to the other, or plan the sequence
  so the second reflow is at a lower peak.
- **Tiny components stack the difficulty.** 0402 / 0201 passives, WLCSP
  ICs, and QFNs with no external leads all compound. Mk2: rejig the BOM
  to prefer 0603 where footprint allows and skip WLCSP unless density
  demands it. The ~2 mm² we lose per swap is cheaper than one dead
  build.
- **Flux management matters more than we thought.** OF-24 connector
  failures suspected to be flux-clog-driven. Mk2: standardize on a
  flux that cleans off easier, or budget an IPA + brush pass before
  final assembly.

---

## 3. Radio -- WiFi / BT + antenna

**Mk1b (current):** WiFi/BT stack is stubbed. No chip antenna was
installed because Ivan didn't want the complication + the power budget
is already tight + the antenna would have been a hacky post-hoc add-on
to the WROOM module's built-in ceramic antenna area.

**Mk1 (legacy):** Ivan may attempt an antenna migration onto a spare
Mk1 board as a *test-only* exercise, if he wants radio range data
before Mk2 layout locks. Probably won't happen -- Mk1 is shelved per
[[project_mk1_shelved]].

**Mk2 (planned):** **Explode the ESP32-S3-WROOM module into discrete
components.**
- Reclaims significant board area (WROOM is ~18×20 mm).
- Lets us do a **clean antenna implementation** instead of a hacky
  add-on -- proper matching network, keep-out zone respected, RF
  reference plane not carved up by traces, etc.
- Downside: more components to place, RF layout expertise required,
  BOM cost slightly up. Ivan's judgment: worth it for the space +
  cleanliness.

**Antenna type TBD for Mk2:** ceramic chip antenna is easiest;
PIFA on the PCB itself is cheaper but takes tuning; wire antenna is
the classic hack. Titanium case (see §4) may constrain choice.

---

## 4. Case + enclosure progression

- **Mk1b current:** 3D printed **PLA @ 0.4 mm layer height**. Works.
- **PETG considered** for better temp resistance + toughness, but
  Ivan doesn't have nice colors on hand. Deferred.
- **Titanium** planned for a much later revision -- probably Mk2 at
  the earliest, possibly Mk3. Machined titanium case is nice, but:
  - **Blocks radio** -- antenna cutouts + dielectric windows will
    need to be planned into the case CAD, and the antenna choice in
    §3 has to accommodate.
  - **EMI / thermal** implications not yet audited.
  - **Ivan has explicitly punted this** -- "so much to debug and
    develop still" before enclosure decisions matter.

---

## 5. Cross-cutting decisions carried forward

- **Mk1 iv7.1 shelved** as of 2026-09-11 -- display footprint mirrored,
  panel FPC pin 13 (GND) lands on PCB pin 1. Not fixable on that
  revision. Mk2 must inherit Mk1b's corrected footprint.
  [[project_iv71_display_dead]] [[project_mk1_shelved]].
- **BQ25619 has no usable VBAT ADC on Mk1b.** Cell voltage source of
  record is MAX17048's VCELL. Mk2: either keep MAX17048 (works) or
  pick a charger with a usable ADC (would delete one chip from the BOM).
  [[project_bq_no_vbat_adc]].
- **Vbat on GPIO18 (ADC2 CH7) on iv7.1 prototype** was a workaround --
  ADC2 has Wi-Fi conflict caveats. Mk1b uses the correct pin. Mk2:
  make sure the divider is on ADC1 by construction.
- **BOOT button missing on iv7.1** forced USB-MSC + TinyUSB to be
  lazy-init + reboot-on-exit. Mk1b/Mk2 should keep a BOOT button
  reachable (even if via case-hole with a pin), so USB stacks can be
  full-init at boot when desired.
- **Rounded corners eat ~15% of screen + touch area on each side.**
  Mk2 case design (whichever material) should preserve this ratio or
  choose a rectangular display for full-area use. Firmware treats
  those zones as aesthetic-only.

---

## 6. Photo-GPS Plan B

Not Mk2-related but worth capturing here since it's dormant intel:
Waveshare ESP32-S3 + old prototype's GPS is the fallback photo-only
watch if Mk1b screen fails permanently. Dormant unless triggered.
[[project_photo_gps_plan_b]]. Mk2 will render this moot.

---

## 7. What still needs to be jotted

Placeholders -- Ivan or a future session should fill these in when
the memory is fresh:

- [ ] Exact reflow profile that has been "safe" vs "yellowing" (peak
      temp, dwell, ramp). Currently in
      `docs/build_info/reflow_profile.py` / `reflow_profile.png` but
      the *learned* margins live only in Ivan's head.
- [ ] Which chip died in which bench session -- cross-ref to
      Mk1/Mk1b build reports would give a failure-rate estimate per
      component.
- [ ] Power budget for the radio when added -- how much of the
      battery does BLE advertising cost, does WiFi even fit at all.
- [ ] Whether MAX17048 stays or a charger-with-ADC replaces both.
- [ ] Battery form-factor decision for Mk2 -- current pouch cell
      dimensions, alternatives, capacity budget.
- [ ] Display panel long-term plan -- CO5300 supply, alternative
      QSPI AMOLEDs (RM67162 family etc.), or move to a MIPI-DSI
      controller for higher refresh + more flexibility.

---

## References

- `docs/build_info/Mk1_build_reports/` and `Mk1b_build_reports/` --
  full bench history, ~30+ stage docs. Source of truth; this file is
  extract-only.
- `docs/build_info/reflow_profile.png` -- current profile visual.
- `docs/build_info/Kompic_Mk1_iv7.1_main.pdf` -- iv7.1 hardware
  reference.
- Auto-memory (see MEMORY.md for full list):
  [[project_kompic_mk1]], [[project_mk1_shelved]],
  [[project_iv71_display_dead]], [[project_bq_no_vbat_adc]],
  [[project_power_topology_mk1b]], [[project_photo_gps_plan_b]],
  [[project_display_landscape_direction]].
