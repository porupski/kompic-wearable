# Stage 19 -- Power Management Refinement + Profiling Infrastructure

**Date opened:** 2026-08-28. Baseline reframed 2026-08-30 after
firmware-grouped battery analysis exposed a Stage-11 -> Stage-18 regression.
**Board:** iv7.1 (Mk1 bench). Mk1b still in fab.
**Firmware baseline:** `iv7.1.f0.4.24` -> target `0.4.25+`.
**Status:** OPEN. Coding starts next chat.

---

## 1. Fresh-chat opening prompt

Copy the block below into the new chat to bring Claude up to speed:

> Stage 19 opens on power management. Stage 18 closed at fw 0.4.24 with
> the log audit, PCF I2C fix, LVGL tile spec, wrist gesture, and Mk1b
> delta plan all landed.
>
> Two things reframe Stage 19 from what the original opening said:
>
> 1. **The Stage-11 -> Stage-18 regression is measured.** Median discharge
>    dropped from 161 min / 284 mAh (fw=0.4.13, n=8) to 151 min / 267 mAh
>    (fw=0.4.24, n=3). ~6 % regression, ~17 mAh gone. New battery cell
>    matched the old one, so this is firmware, not chemistry. Recovering
>    Stage 11 is the floor; the real goal is past it.
> 2. **Ivan wants a park/idle/off vocab first.** Today "park" is loose --
>    it usually means the broker task is disabled, but the underlying
>    chip may still be running. Boot policy also enables IMU always-on
>    (Stage 18 §7.5) for wrist gesture; that's a big always-on drain.
>    Fix that vocab + boot policy before writing any cuts.
>
> Priorities for this stage, in order:
>
> 1. **§4 Vocab + §5.1 boot policy rewrite.** Codify PARK / IDLE / OFF
>    (see §4). Rewrite `boot_hw_init.c` so nothing modal is on at boot
>    -- everything chip-parked, broker-disabled. Wake helpers are the
>    only path to non-parked state.
> 2. **§5.2 Profiling infrastructure.** POWER_BUDGET, PM_DUMP DELTA,
>    TASK_STATS. Every subsequent cut lands with a before/after capture.
> 3. **§5.3 - §5.6.** Concrete park implementations per driver (LSM,
>    DRV, MAX30101), plus GESTURE-on-demand via LSM tap wake.
> 4. **§6 command surface** is orthogonal but should land in the same
>    stage so cuts have a clean CLI to drive them. Discuss first.
>
> Battery analysis lives in
> `docs/build_info/aux_scripts/battery/analyze_battery_csv.py`; it now
> groups by `fw=` (boot_seq is unreliable across NVS wipes). Rerun any
> time with `--out-dir` defaulting to the script folder. New reference
> numbers are frozen at
> `docs/build_info/aux_scripts/battery/battery_profile.{txt,h}` and
> `battery_analysis.png`.

---

## 2. Context carried over from Stage 18

- **Baseline draw (USB plugged, ammeter):** 105 mA normal, 94 mA
  BATT_TEST. On battery the `batt_test_vbus` PM lock lifts and
  light-sleep engages -- expected floor is much lower, but we do not
  have the number yet.
- **PM_DUMP after ~215 s of BATT_TEST plugged (Stage 18 §11):**
  - `batt_test_vbus NO_LIGHT_SLEEP` 99 % active.
  - `light_sleep_counts: 100`, zero rejects.
  - DFS is working: 68 % of active time at 40 MHz, 29 % at 240 MHz,
    1 % SLEEP.
  - `rtos0` + `rtos1` hold CPU_FREQ_MAX 30 % combined. Application-
    task pressure -- unknown source until per-task profiling.
- **RGB drain estimate (Stage 18 §11):** ~0.04 mAh over 3 hours.
  Rounding error. Not a Stage 19 target.
- **DRV drain estimate:** ~0.5-1 mA continuous in INTTRIG mode.
  Small in absolute terms; fix is a two-line change.
- **LSM drain estimate:** ~500-750 uA continuous at 240 Hz HP + gyro.
  Real target: 6.5 uA in 12.5 Hz low-power accel-only when gesture
  is off.

---

## 3. Measured baseline (fw-grouped discharge)

Reference `analyze_battery_csv.py --profile-fw 0.4.24` output on
`/2026-08-30/data/battery/` (~106 mA constant load, cutoff at UVLO):

| Firmware | Stage      | median dur | median delivered | n |
|----------|-----------|------------|------------------|---|
| 0.4.3    | early      | 110 min    | 195 mAh          | 1 |
| 0.4.5    | early      | 111 min    | 197 mAh          | 1 |
| 0.4.12   | Stage 11   | 161 min    | 284 mAh          | 1 |
| 0.4.13   | Stage 11   | **161 min**| **284 mAh**      | 8 |
| 0.4.24   | Stage 18   | 151 min    | 267 mAh          | 3 |

- **Stage 18 vs Stage 11 = -10 min / -17 mAh median.** Roughly
  attributable to always-on IMU (Stage 18 §7.5) at ~600 uA continuous,
  which over 2.5 h ~ 1.5 mAh. That accounts for maybe 10 % of the
  regression. Rest is unknown -- the profiler in §5.2 must surface it.
- **New cell ~= old cell.** Boot 32-37 (fw 0.4.24, new cell) matches
  boot 79-83 (fw 0.4.12, old cell) in delivered mAh once fw is held
  constant. Chemistry is not drifting.
- **boot_seq is unreliable for chronology.** NVS wipes reset it, so
  the older script's pre-PM (boot <= 35) / post-PM (boot >= 81) buckets
  were catching Stage 18 data in the "pre-PM" bin. `fw=` is the
  authoritative label. This is why §7 wants an explicit `battery_tag`.

---

## 4. Vocabulary -- PARK / IDLE / OFF

Codify these three before writing any cuts. All future PM-related
comments, log strings, and helper names use them exactly.

- **OFF** -- chip is powered down completely (rail switched via load
  switch or nFET). Zero quiescent draw. Only sensors on a switchable
  rail can enter OFF; on iv7.1 that's currently none -- documented as
  a Mk1b/Mk2 hardware option.
- **PARKED** -- chip is in its datasheet-lowest register state
  (shutdown / standby / power-down bit set) AND the broker task is
  disabled. Costs uA-range (see each driver's datasheet). This is the
  default state for every modal sensor at boot.
- **IDLE** -- broker task not consuming, but the chip is in whatever
  state it was last left. **This is a bug state, not a design state.**
  It exists today because `park_all_modal_sensors()` at
  `fc_common.c:337` only flips broker flags -- the chips may still be
  running. Stage 19 removes this ambiguity.

**Consequence:** every driver must expose a `<drv>_park()` helper that
writes the chip's real low-power register and disables the broker.
`park_all_modal_sensors()` calls those helpers, not broker flags.
Symmetric `<drv>_wake_for(mode)` on the wake side.

---

## 5. Plan

### 5.1 Boot policy rewrite (do FIRST)

`boot_hw_init.c:203-211` currently enables RTC, battery, haptic and
IMU always-on. Only RTC and battery are truly non-optional. Haptic
must be reachable for boot-buzz but the chip itself can idle at
INTTRIG (Stage 19 §5.4 makes it park properly between plays).

Target boot sequence:

1. All chips power up via their brokers' `_init()` and are
   immediately dropped to PARKED via `_park()`. No exceptions.
2. RTC and BQ25619 are exempt (RTC keeps time; BQ25619 is a monitor
   we can't disable). Their brokers stay enabled.
3. Boot buzz still fires -- haptic wakes, plays, parks. No always-on
   INTTRIG. See §5.4.
4. IMU is PARKED at boot. Wrist gesture becomes wake-on-tap (§5.3),
   not always-on 240 Hz HP.
5. Log line changes from "modal sensors parked" to a real audit:
   iterate drivers, print each one's parked-state datasheet uA.

Delta vs Stage 18:

- Removes ~600 uA IMU always-on. Estimated recovery: ~1.5 mAh over
  2.5 h. Small in absolute terms, big architecturally.
- Makes every "wake for mode X" call authoritative -- no more
  hidden always-on paths that recording modes have to work around
  (§5.4 IMU-parked-during-ECG stays clean without special-casing).

### 5.2 Profiling infrastructure

Ivan's directive: "build a good profiling infrastructure while we
are laying these pipes, we gotta be smart about it".

Design goals:

- **Every driver reports its own energy budget estimate.**
  `driver_power_estimate_ua()` per driver. Returns current draw at
  present config. Dummy `0` at first is fine.
- **Aggregator + CLI dump.** `POWER_BUDGET` command prints every
  driver's estimate + total + comparison to ammeter reading.
- **PM_DUMP DELTA.** Stores previous snapshot, prints only the
  diff. Useful during live tuning.
- **TASK_STATS.** `vTaskGetRunTimeStats` hooked to CLI. Surfaces
  which tasks eat CPU_FREQ_MAX budget -- the mystery `rtos0+rtos1
  30 %` from Stage 18 §11 needs a name.
- **Extended BATT_TEST CSV.** Add per-sensor state columns
  (`imu_state, hr_state, drv_state, cpu_freq_max_pct, ls_pct`) so
  post-hoc analysis can align drain deltas with state transitions.

**Landing order for §5.2:**

1. `POWER_BUDGET` skeleton + per-driver hook.
2. `PM_DUMP DELTA` mode.
3. `TASK_STATS` CLI.
4. Extended BATT_TEST CSV columns.
5. Fill in each driver's `driver_power_estimate_ua()` with datasheet
   numbers.

Once (1)-(4) land, no cut in §5.3-§5.6 lands without a before/after
`POWER_BUDGET` + `PM_DUMP DELTA` pair captured in this stage log.

### 5.3 Gesture: wake-on-tap, not always-on

- After §5.1 the IMU is PARKED at boot. Wrist gesture cannot poll
  from that state.
- LSM6DSV16X supports activity/inactivity interrupts on INT1/INT2
  while in low-power mode (~6.5 uA @ 12.5 Hz LP accel-only).
- New API: `imu_arm_wake_source()` -- puts LSM in LP + arms INT for
  activity threshold. `imu_disarm_wake_source()` re-parks.
- CLI: `GESTURE [ON|OFF]`. NVS `sys:gesture_on` u8, default 0.
  ON -> `imu_arm_wake_source()`; OFF -> stays parked (no drain).
- The wrist-raise detector still lives in the IMU task, but the
  task only wakes when the ISR fires -- otherwise it's blocked on
  a queue with 0 poll cost.

This subsumes what §3.5 of the original doc called "LSM low-power
path" and what §3.2 called "GESTURE ON/OFF" -- they're one system
now, under the park vocab.

### 5.4 DRV real shutdown between plays

- After each `haptic_play_forced` completes (GO bit clears +
  effect duration elapses), write `MODE = 0x40` (STANDBY).
- Next play restores INTTRIG before writing GO.
- Fits the vocab: haptic_park() is the STANDBY write. Wake helper
  restores INTTRIG.

### 5.5 MAX30101 real shutdown audit

- Confirm `broker_hr_set_enabled(false)` actually reaches
  `max30101_set_shutdown(port, true)`. If not, wire it.
- Rename the pathway so the boundary is unambiguous:
  `broker_hr_park()` -> `max30101_set_shutdown(true)` at chip level.

### 5.6 Broker-wide park-helper audit

For every broker-owned chip: BME688, TMP117, VEML6030, MMC5983MA,
MAX30101, LSM6DSV16X, DRV2605, PCF85063, BQ25619:

- Does a `_park()` helper exist that writes the chip's real
  low-power state?
- Is it wired from `park_all_modal_sensors()`?
- What's the datasheet uA in that state?

Any chip that answers "no" to (1) blocks §5.1 shipping.

### 5.7 STATUS + BATT_TEST log tweaks (already landed in 0.4.24)

- BATT_TEST per-sample line restored to INFO with `vbat` leading.
- Verify on next Stage 19 flash that the field order reads well.

---

## 6. Command surface reorganisation

The CLI grew organically through Stages 11-18 and it shows: verbs at
inconsistent levels, dump options scattered, no consistent NVS entry
point per subsystem. Ivan's proposal, cleaned up:

**Top-level verbs (level 0):**

```
STATUS
TIME              [nsv]
SENSORS           [nsv]
DATA_DUMP
TEST_MODES        [nsv]
MISC              [nsv]   (or SETTINGS)
SHIPMODE
REBOOT
HELP [<level>]
```

`[nsv]` = the verb owns NVS settings; setting reads/writes happen
inside its submenu.

**Design principles:**

1. **`STATUS` becomes the everything-print.** Firmware/hw versions,
   RTC, battery, sensor states (from broker), current mode, uptime,
   heap, task stats -- one dump. Absorbs `PM_DUMP` summary line and
   `SENSORS` idle read. Detailed dumps still live under `DATA_DUMP`.
2. **`SENSORS` is the plug-and-play grid.** `SENSORS` alone prints
   every sensor + parked/idle/awake state + last reading. `SENSORS
   <name>` enters that sensor's submenu (park, wake, read, config,
   NVS). New sensors register themselves via a component-side
   `SENSOR_REGISTER(...)` macro; the CLI auto-discovers them.
3. **`DATA_DUMP`** collects `PM_DUMP`, `POWER_BUDGET`, `TASK_STATS`,
   `SD_LIST`, per-sensor `DUMP_RAW`. Reprints its own command list.
4. **`TEST_MODES`** owns BATT_TEST, ECG_SYNC, PPG_BCG, MLC_COLLECT.
   NVS defaults per test mode live under this verb.
5. **`MISC`** / `SETTINGS` owns the cross-cutting NVS the other
   verbs don't naturally own (log level, gesture on/off if we don't
   put it under SENSORS.IMU, `battery_tag` from §7, etc.).
6. **`HELP <level>`** -- `HELP 0` lists level-0 verbs; `HELP 1`
   drills one more layer down.

**Plug-and-play sensor registration.** Each component gets a small
snippet (probably a `SENSOR_DECLARE()` macro placed at component
init) that hands the CLI:

- name string
- park / wake / read function pointers
- optional NVS-key list
- optional read-out format string

The CLI iterates a link-section-collected array of these
descriptors. Zero touch in `main.c` per new sensor; the SENSORS
menu auto-populates.

**Alignment with power management:** the same descriptor that gives
CLI `park`/`wake` handles is what §5.1 boot policy uses to hit every
chip at boot. Two features, one plumbing.

**Discuss before coding.** Structure is right, but the descriptor
schema needs one design pass before we start moving CLI code. Land
after §5.2 profiling infra; do NOT block §5.1 boot policy on this.

---

## 7. Battery test tracking follow-up

Two small things came out of the 2026-08-30 analysis session:

1. **`fw=` tag already works.** `fc_battery_test.c:208` writes
   `fw=<KOMPIC_FW_VERSION>` into the CSV header; the analyser now
   groups by it. Every future BATT_TEST run gets classified
   correctly with no user action.
2. **Add a `battery_tag` NVS key + CLI verb.** When we swap the
   physical cell (dead battery, new build), we can't distinguish
   two cells that ran on the same firmware. Cheap fix:
   - NVS key `sys:batt_tag` (short string, e.g. `cell_A`,
     `cell_new_2026-08-30`).
   - CLI `MISC BATT_TAG <string>` writes it.
   - `fc_battery_test.c` includes it in the header comment:
     `batt_tag=cell_new_2026-08-30`.
   - Analyser groups first by `batt_tag`, then by `fw=`, then by
     `boot_seq`. Small dict extension in `_read_csv()`.
3. **`--profile-fw` and `--out-dir` land in the analyser today**
   (2026-08-30). Profile lives in
   `docs/build_info/aux_scripts/battery/`. `battery_profile.h`
   there is currently built from fw=0.4.24 (n=3), our Stage 18
   frozen reference. Update on each stage boundary.

---

## 8. Results

_(populated as the fresh-chat coding session runs)_

---

## 9. Next steps

_(fill at wrap)_

---

## References

- `docs/build_info/aux_scripts/battery/analyze_battery_csv.py` --
  fw-grouped analyser, `--out-dir` defaults to script folder.
- `docs/build_info/aux_scripts/battery/battery_profile.{txt,h}` --
  fw=0.4.24 frozen reference (Stage 18 baseline).
- `docs/build_info/Mk1_build_reports/Stage_18_Log_Audit_PCF_Tile_Spec.md`
  §10, §11, §12 -- battery-management roadmap, first PM_DUMP, and
  the small policy tweaks that already landed.
- `docs/build_info/reference_files/LOG_LEVEL_POLICY.md` -- log-level
  classification any Stage 19 code cuts must respect.
- `docs/build_info/reference_files/IV71_TO_MK1B_FIRMWARE_DELTA.md` --
  Mk1b changes that Stage 19 must not accidentally break.
