# Stage 35 -- Mk1b sensor-surface trim + I2C master API migration

**Date opened:** 2026-09-27
**Date closed:** _pending_
**Board:** Mk1b iv8.0.
**Firmware baseline:** 0.4.88 (post Stage 31 GPS UART TX fix appendum).
**Firmware close:** _pending_ (target: 0.4.91).
**Status:** OPEN. Priority-inserted ahead of Stage 32 (tile subject
sweep) and Stage 33 (structural finish); those two are blocked on the
firmware being stable enough to bench for full sessions.

**Companion refs:**
- `Stage_31_Mk1b_LVGL_Foundations_Rebuild.md` §11 appendum -- GPS UART
  TX fix that landed on the same fw baseline.
- Auto-memory [[project_i2c_hw_fsm_reset_crash]] -- 6+ instances since
  fw 0.4.68; documented root fix is exactly this stage's work.
- ESP-IDF v5.5 `driver/i2c_master.h` API docs.

---

## 1. Why this stage exists

Two problems came due at the same time; solving them in one stage is
cheaper than doing them separately.

**Problem A -- chronic I2C panic.** Since roughly fw 0.4.68 (Stage 29),
the watch has been panicking under any hot-navigation stress with the
same stack trace every time: `bq25619_read_reg -> i2c_hw_fsm_reset ->
IRQ WDT`. The chronic memory has been counting instances until Stage
31 gave up counting past 6. `feedback_flash_per_checkpoint` sessions
keep running into it. `bq25619` sleep-skip prophylactics reduced
exposure but never fixed the root: the legacy `driver/i2c.h`
cmd-link driver's hardware FSM can enter an unrecoverable state when
short bursts contend on a busy bus. The documented fix is migrating
to the ESP-IDF v5.x `driver/i2c_master.h` API, which manages the FSM
internally.

**Problem B -- too many sensors we don't use.** Ivan's day-to-day
needs are: RTC, encoder + button, BQ25619, MAX17048, screen, touch,
haptic, RGB LED, flashlight, BME688 (env), LSM6DSV16X (LSM/IMU), and
MAX-M10S (GPS). The current firmware also boots MAX30101 (PPG),
VEML6030 (light), LIS3MDL (compass), and TMP117 (skin temp). None of
those four are actively used for anything Ivan is currently building,
and each adds init cost, task cost, and I2C bus traffic.

**Why they combine well.** Migrating a sensor to `i2c_master` costs
30-60 minutes per driver. Migrating four drivers we're about to stub
anyway is pure waste. Do the stub pass first; migrate what's left.

---

## 2. Scope

Three batches, dependency-ordered. Each is one flash-checkpoint per
[[feedback_flash_per_checkpoint]].

- **35.1** -- Sensor surface trim (stub the four unused sensors).
- **35.2** -- I2C master migration, Bus 1 (BQ + MAX17048 + DRV2605).
  Crash-fix batch. Success gate is "no more `i2c_hw_fsm_reset`
  panics under nav stress."
- **35.3** -- I2C master migration, Bus 0 (PCF85063 + BME688 +
  LSM6DSV16X + CST9217 touch). Consistency batch; also expected to
  cure occasional touch freezes.

---

## 3. Batch 35.1 -- Sensor surface trim

### What lands

- `boot_logic/boot_hw_init.c`: delete `max30101_init`, `veml6030_init`,
  `lis3mdl_init`, `tmp117_init` calls in `bringup_bus0`. One-line
  comment at each site: `// STUBBED Stage 35 -- see Stage_35 doc`.
- `boot_logic/boot_tasks.c`: delete `task_hr`, `task_light`, `task_mag`,
  `task_mag_cal`, `task_skin` task-creation entries.
- `lvgl_ui/tile_registry.c`: drop `health_tile_desc`, `light_tile_desc`,
  `compass_tile_desc` entries. Registry becomes 6 tiles (system, gps,
  env, haptic, rtc, imu).
- `field_capture/fc_cli.c`: the `HR`, `LIGHT`, `LIS3MDL/COMPASS`, and
  `TEMP_DUMP` verbs stay in the dispatcher but print
  `"<verb>: stubbed at Stage 35 -- sensor not in current build"`
  and return `ESP_OK`. Keeps CLI discoverability.
- `firmware_version.h`: 0.4.88 -> 0.4.89.
- Data-broker fields untouched; consumers already handle
  `hw_alive == false`.

### What is NOT deleted

- Component folders (`max30101/`, `veml6030/`, `lis3mdl/`, `tmp117/`)
  and their CMakeLists.txt. They still build; only boot-time
  activation goes away. Un-stubbing later is a few lines.
- Tile source files (`health_tile.c`, `light_tile.c`, `compass_tile.c`).
  They still compile; they're just no longer registered.
- `broker_hr_*`, `broker_light_*`, `broker_mag_*`, `broker_skin_*`
  functions. Sentinel state is a valid state.

### Success gate

1. Boot log shows 6 tile init lines, not 9. No `HR:`, `LIGHT:`,
   `MAG:`, `SKIN:` `Task started` lines.
2. `STATUS` output covers the 12 kept sensors only.
3. All 6 tiles reachable via tileview navigation; drawer still works.
4. Stubbed CLI verbs print the stub notice.
5. Boot time same or faster than 0.4.88.

### Effort

3-4 files touched, ~150-200 lines net removed. Half a session.
Flash + verify before starting 35.2.

---

## 4. Batch 35.2 -- I2C master migration, Bus 1 (crash fix)

### What lands

- `boot_logic/boot_hw_init.c`:
  - `bringup_bus1()` -> `i2c_new_master_bus` (replaces
    `i2c_param_config` + `i2c_driver_install`). Stores the handle
    in a new module-scope `g_i2c1_bus_handle`.
  - `i2c_probe()` helper rewritten around `i2c_master_probe`.
  - `g_i2c2_mutex` kept -- the mutex solves atomicity for the
    battery task's read burst; the new API solves FSM state.
    Orthogonal concerns.
- `bq25619/bq25619.c`:
  - Add `static i2c_master_dev_handle_t s_bq_dev;`.
  - `bq25619_init` adds itself to the bus via
    `i2c_master_bus_add_device` (100 kHz, 7-bit addr 0x6B).
  - `bq25619_read_reg` -> `i2c_master_transmit_receive(s_bq_dev,
    &reg, 1, val, 1, 20 /*ms*/)`.
  - `bq25619_write_reg` (if present) -> `i2c_master_transmit`.
- `max17048/max17048.c`: same pattern.
- `drv2605/drv2605.c` + `drv2605/haptic.c`: same pattern; auto-cal
  loop becomes a sequence of `_transmit_receive` calls.
- CMakeLists for each affected component: replace `driver` REQUIRES
  entry with `esp_driver_i2c`.
- `firmware_version.h`: 0.4.89 -> 0.4.90.

### Success gate

1. **Primary metric:** run the known-crash reproducer (rapid drawer
   swipe up/down for ~30 s while battery task ticks). Zero panic in
   5 min = pass. Signature `bq25619_read_reg -> i2c_hw_fsm_reset ->
   IRQ WDT` does not appear.
2. Battery % on main screen updates ~1 Hz.
3. Haptic still buzzes on encoder click.
4. `SHIPMODE` from CLI works (BQ write path).
5. Boot log: `BQ25619: hw alive`, `MAX17048: hw alive`,
   `DRV2605: cal OK, boot buzz queued` -- all present.

### Effort

4 files heavily touched, ~300-400 lines. One session. Bench cycle
probably 2 flashes (first pass will miss a CMake REQUIRES or an
unused-driver-symbol residue).

---

## 5. Batch 35.3 -- I2C master migration, Bus 0 (stability)

### What lands

- `boot_logic/boot_hw_init.c` `bringup_bus0()` -> `i2c_new_master_bus`
  for Bus 0. Handle in `g_i2c0_bus_handle`. `g_i2c_mutex` stays.
- `pcf85063/pcf85063.c`: device handle + `_transmit_receive` for the
  7-byte time burst read; `_transmit` for the write path.
- `bme688/bme688_drv.c`: the Bosch BME68X managed component takes
  I2C callback function pointers with a legacy signature. Rewrite the
  callback internals to use the new API; keep the outer signature.
  Fallback: if pointer-arithmetic tangles up, keep BME688 on legacy
  for one more stage and log a Stage 36 follow-up.
- `lsm6dsv16x/lsm6dsv16x.c`: already uses `i2c_master_write_read_device`
  (semi-modern). Straight rename to the new API + device-handle
  bookkeeping.
- `cst9217/cst9217.c` (touch): device handle + `_transmit_receive` for
  the ISR-driven touch-point read.
- CMakeLists REQUIRES: `driver` -> `esp_driver_i2c` for each.
- `firmware_version.h`: 0.4.90 -> 0.4.91.

### Success gate

1. Boot: RTC / BME688 / LSM / touch all `hw alive`.
2. Touch feels the same or better -- no freeze after ~5 min heavy nav.
3. Env values populate on env_tile.
4. Encoder + button still work (encoder is GPIO -- sanity regression).
5. RTC time survives reboot.
6. `grep -rn '#include "driver/i2c.h"' firmware/esp-idf/` returns
   nothing.

### Effort

5 files heavily touched, ~400-500 lines. One session (possibly two
if BME688 callback shape needs work).

---

## 6. Non-goals

- Deleting the stubbed-out component folders. They stay buildable.
- Changing the enable/disable policy for kept sensors.
- Diagnosing "GPS load every ~1 s + signal-strength dip" that Ivan
  mentioned -- separate future work, requires outdoor bench per
  [[project_max_m10s_wiring]].
- Stage 32 tile subject sweep, Stage 33 structural finish, Stage 34
  field_capture rename -- still queued.
- Migrating `mic_pdm` (I2S/PDM, not I2C -- unrelated).

---

## 7. Definition of done

1. Firmware 0.4.91 boots cleanly with 6 tiles + 12 sensors.
2. 30 min bench soak with periodic random taps: zero
   `i2c_hw_fsm_reset` in the log.
3. Auto-memory [[project_i2c_hw_fsm_reset_crash]] updated with
   "resolved at Stage 35, fw 0.4.91."
4. Archive `stage35_close_i2c_master_and_trim` created.

---

## 8. Non-goals for _outside_ Stage 35

_(sensor un-stub schedule, if any)_

- MAX30101: possibly Stage 36 or later once PPG use case surfaces.
- LIS3MDL: only if a heading-holds-north tile ships.
- VEML6030: only if auto-brightness ships.
- TMP117: only if a skin-temp readout matters.

None are firm plans; the point of the stub is that we don't burn
budget on features we're not using.

---

## 9. Bench log

_(populated as each batch lands, per [[feedback_stage_log_workflow]])_

### 9.1 -- Batch 35.1 flash

_pending_

### 9.2 -- Batch 35.2 flash

_pending_

### 9.3 -- Batch 35.3 flash

_pending_

---

## 10. References

- `Stage_31_Mk1b_LVGL_Foundations_Rebuild.md` §11 appendum.
- ESP-IDF v5.5 `driver/i2c_master.h` header and its docstring examples.
- ESP-IDF v5.5 migration guide: legacy `driver/i2c.h` -> `driver/i2c_master.h`.
- Auto-memory:
  [[project_i2c_hw_fsm_reset_crash]],
  [[project_kompic_mk1]],
  [[project_mk1b_arrived]],
  [[feedback_flash_per_checkpoint]],
  [[feedback_group_work_by_venue]],
  [[feedback_stage_log_workflow]],
  [[feedback_mk1b_stage_naming]],
  [[feedback_commits]],
  [[feedback_md_frontmatter_convention]].
