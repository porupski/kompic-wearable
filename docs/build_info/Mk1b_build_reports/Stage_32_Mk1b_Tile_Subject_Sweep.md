# Stage 32 -- Mk1b bulk tile -> subject conversion

**Date opened:** 2026-09-18
**Date closed:** (open)
**Board:** Mk1b iv8.0.
**Firmware baseline:** 0.4.87 (Stage 31 close: env + gps tiles are
subject-bound, drawer is a sibling pane, shared pane style + drain
timer + observer plumbing all in place).
**Status:** OPEN, ready to start in a fresh session.

**Companion refs:**
`docs/build_info/reference_files/lvgl_9/LVGL9_Kompic_Architecture.md`
(architecture spec), `Stage_31_Mk1b_LVGL_Foundations_Rebuild.md`
(the foundations that make this stage a mechanical sweep instead
of design work).

---

## 1. Why this stage exists

Stage 31 landed the LVGL 9 idioms and proved them on env (pilot,
labels-only) and gps (worst case, two-way switch + photo overlay +
robust status). Every other tile still uses the pre-Stage-31 poll
pattern (per-tile `update()` that reads broker + snprintfs into
labels every 200 ms).

This stage does the mechanical sweep: convert the remaining seven
tiles to `lv_subject_t` bindings, bring each layout inside the
corner-safe zone with bigger fonts, and shrink each tile's
`update()` to residual state (button feedback timers, one-shot
transitions). Nothing here changes hardware behavior.

## 2. Scope

Four batches, similar-shape tiles grouped for one-flash amortisation:

| Batch | Tiles                                | Theme                       | LOC pre |
|-------|--------------------------------------|-----------------------------|---------|
| 32.1  | `imu_tile` + `compass_tile`          | Motion sensors, read-only   | 369 + 387 |
| 32.2  | `haptic_tile` + `light_tile`         | UI-driven params (sliders, waveform pick) -- exercises two-way binding beyond gps power toggle | 471 + 537 |
| 32.3  | `health_tile` + `battery_tile`       | PPG/BCG strings + smallest tile | 367 + 176 |
| 32.4  | `system_tile` + `rtc_tile`           | NVS-driven, non-sensor producer shape | 290 + 300 |

Total ~2900 LOC touched. Each batch is one Opus session + one flash
cycle per [[feedback_flash_per_checkpoint]]. Wall-clock ~1-2 weeks.

## 3. Per-batch template

Every tile in every batch follows the same three-step recipe (proven
on env in 31.2 + gps in 31.4a):

**Step A -- extend ui_subjects.**
Add string subjects for value labels + int subjects for LED status /
toggles / mode indicators. Owned queue for the producer domain if not
already present. Buffer pairs behind `xxx_STR_BUF_LEN` macro.

**Step B -- producer publishes.**
After `broker_xxx_write()`, add `xQueueOverwrite(g_xxx_q, &sample)`.
When the producer task early-outs on `!enabled`, push a "disabled"
sample so the tile status reflects immediately (bme688 / max_m10s
pattern).

**Step C -- rewrite the tile.**
- Corner-safe layout: 60 px H pad, 40 px V pad for interactive
  elements per [[project_display_corner_cutoff]].
- Bigger font: `UI_FONT_TITLE` (28 px) for data rows, `UI_FONT_LABEL`
  (20 px) for chips. If 28 px still feels small, generate a 40 px
  face per [[feedback_lv_font_conv_flags]].
- Labels bind via `lv_label_bind_text(lbl, &subj_xxx_str, NULL)`.
- Switches bind via `lv_obj_bind_checked(sw, &subj_xxx_enabled)`
  with an `obs_xxx_enabled_to_broker` observer in ui_subjects.c
  (two-way).
- Sliders / mode pickers: same pattern, `lv_obj_bind_value` or
  event cb -> subject -> observer -> broker.
- `tile_update()` shrinks to LED colour + button feedback timeout.
- Header text is chip-name only (no chip-desc). Optional short
  descriptor as chip beneath status line.

**Step D -- fw bump + stage log.**
Standard per-batch flash summary.

## 4. Batch details

### 4.1 -- Batch 32.1 (imu_tile + compass_tile)

Same shape as env: multi-row read-only sensor data + status LED +
power toggle. No sliders, no photo view, no fancy interactions --
easy first sweep to burn in the muscle memory.

- IMU subjects: `subj_imu_accel_str`, `subj_imu_gyro_str`,
  `subj_imu_temp_str`, `subj_imu_activity_str`, `subj_imu_status`,
  `subj_imu_enabled`.
- Compass subjects: `subj_mag_field_str`, `subj_mag_bearing_str`,
  `subj_mag_hdop_str`, `subj_mag_calstate_str`, `subj_mag_status`,
  `subj_mag_enabled`.
- Producers: `lsm6dsv16x` + `lis3mdl` -- push to `g_imu_q` + `g_mag_q`.

Bench watch: tile shows live accel / gyro / mag values, LED tracks
status, power toggle works via bind_checked. Then flip theme, verify.

### 4.2 -- Batch 32.2 (haptic_tile + light_tile)

**Beyond env/gps:** these tiles have UI-DRIVEN parameters, not just
read-only sensor values. Haptic has waveform pick (dropdown / roller);
light has brightness slider + auto-brightness toggle.

- New two-way pattern to exercise: `lv_slider_bind_value` +
  observer -> `broker_light_set_manual_brightness`.
- Haptic waveform: `lv_dropdown_bind_value` + observer ->
  `haptic_set_ui_effect`.
- Blue-light toggle: `lv_obj_bind_checked` -> broker.
- If any of the widget-side binds don't have a stock LVGL 9 helper
  (e.g. `lv_slider_bind_value` might not exist yet), add a small
  observer + LV_EVENT_VALUE_CHANGED cb pair -- same shape as the
  gps two-way switch.

Bench watch: drag brightness slider, watch AMOLED backlight change
in real time. Tap waveform picker, feel the haptic play.

### 4.3 -- Batch 32.3 (health_tile + battery_tile)

Health: PPG (heart rate) + BCG derived. Currently poll-based. Add
subjects for HR bpm + PPG signal quality + BCG SNR. Producer is
`max30101` task + a bcg derivation step.

Battery: smallest tile in the tree. Read-only percent + charging
icon. Likely lands as a bonus during health's bench cycle.

### 4.4 -- Batch 32.4 (system_tile + rtc_tile)

**Different producer shape:** these read from NVS + uptime + PMU
locks + RTC registers, NOT from a sampling sensor task. Validates
that the drain pattern works for on-demand producers too.

- System tile: uptime, heap, PSRAM, cpu freq, current mode.
  Producer? Add a small `system_metrics` snapshot task that
  periodically pushes to `g_system_q`.
- RTC tile: wall time, wake alarm, RAM byte state. Producer =
  `pcf85063` task (already exists, add queue push).

## 5. Non-goals

- Alarm as sibling pane / delete `ui_navigation.c` -- Stage 33.1.
- Delete every tile's `apply_theme()` cb + delete `apply_ui_theme`
  fan-out loop -- Stage 33.3.
- Shutdown overlay -> `lv_msgbox` -- Stage 33.2.
- Delete update-cb from tile_registry contract -- Stage 33.3 (needs
  every tile's `update()` to be truly no-op first, which this stage
  gets us to).
- New GPS diagnostics / dynamic-model expansion -- 31.4 already
  covered the immediate wins; anything more waits on bench hw fix.
- Encoder INTERMITTENT recovery -- separate stage,
  [[project_encoder_dead_on_iv80]].
- i2c_master API migration -- separate stage, blocker for
  [[project_i2c_hw_fsm_reset_crash]].

## 6. Definition of done for Stage 32

1. All seven remaining tiles are subject-bound; each `tile_update()`
   is <= 40 lines (LED + feedback + residual).
2. All seven tiles use corner-safe layout (60 H / 40 V pads,
   `UI_FONT_TITLE` for data rows).
3. Each tile's producer pushes to `g_xxx_q` including the "disabled"
   snapshot path.
4. `ui_subjects.c` grows to ~15 domains but the drain-cb is still
   a simple round-robin queue check.
5. Bench: every tile displays live data (or a coherent
   "no data"/"disabled" status), power toggles work, theme swap
   works, no LVGL asserts.

## 7. Bench log

### 7.1 -- Batch 32.1 flash (fw 0.4.88)

**What landed:**
- `ui_subjects.{c,h}`:
  - New IMU domain: 10 string subjects (accel x/y/z, gyro x/y/z, roll,
    pitch, temp, status) + 1 int (`subj_imu_enabled`) + `g_imu_q`
    queue. Formatter `imu_format_and_publish()` builds a status line
    from `broker_imu_hw_alive/get_status/get_enabled` and blanks the
    numeric labels on disabled/offline paths. Two-way observer
    `obs_imu_enabled_to_broker` writes broker on subject change (drain
    mirrors broker->subject the following tick).
  - New MAG domain: 4 string subjects (xyz, heading, cardinal, cal-btn)
    + 1 int (`subj_mag_enabled`) + `g_mag_q` queue. Formatter derives
    xyz + heading text from data + status; cal-button label reflects
    `d.calibrating` / `d.cal_countdown`. Two-way observer for the
    power switch.
  - Init log line updated to `env(5) + gps(9 str + 2 int) + imu(10
    str + 1 int) + mag(4 str + 1 int) subjects created; g_env_q +
    g_gps_q + g_imu_q + g_mag_q up`.
  - Drain cb extended with `xQueueReceive` branches for both queues
    (round-robin; each queue is depth-1 overwrite).
- `lsm6dsv16x.c` `task_imu_fn`:
  - Includes `ui_subjects.h`.
  - After `broker_imu_write(&bd)` -> `xQueueOverwrite(g_imu_q, &bd)`.
  - On `!enabled` early-out, push a one-shot "disabled" snapshot (edge-
    detected via `s_prev_pushed_disabled`) so the tile flips to
    "Disabled" instantly without waiting for re-enable.
- `lis3mdl.c` `task_mag_fn` + `task_mag_cal_fn`:
  - Includes `ui_subjects.h` + `freertos/queue.h`.
  - After every `broker_mag_write` (main + 3 calibration paths + cancel
    path) -> `xQueueOverwrite(g_mag_q, &bd)`.
  - On `!enabled` early-out in task_mag_fn, push one "disabled"
    snapshot (edge-detected).
- `lis3mdl/CMakeLists.txt`: added `lvgl_ui` to REQUIRES (matches
  lsm6dsv16x + bme688 pattern).
- `imu_tile.c` rewrite:
  - Corner-safe 60H/40V layout (matches env/gps convention).
  - Header text is chip-name only (`lsm6dsv16x_get_chip_name()`);
    chip-desc dropped per LOG.md 2026-09-18 note.
  - All 9 value labels + status label bound via `lv_label_bind_text`.
  - Power switch bound via `lv_obj_bind_checked(&subj_imu_enabled)`.
    Deleted `cb_power()` + `s_syncing_power` guard entirely.
  - `imu_tile_update()` shrunk from ~90 lines to ~20 -- LED colour +
    status-label semantic colour only. All text updates drain-driven.
  - `apply_theme()` still recolours labels + parent bg (Stage 33.3
    deletion scope).
- `compass_tile.c` rewrite:
  - Corner-safe 60H/40V layout.
  - Header text chip-name only.
  - XYZ / heading / cardinal / cal-button labels bound via
    `lv_label_bind_text`. Cal-button label auto-flips
    "CALIBRATE" <-> "CAL 15s" via drain (no snprintf in tile).
  - Power switch bound via `lv_obj_bind_checked(&subj_mag_enabled)`.
    Deleted `cb_power_toggle()` + `s_syncing` guard entirely.
  - `compass_tile_update()` keeps only: LED colour, heading+cardinal
    colour semantic tint, needle rotation (transform_angle), and
    cal-button colour + enable state. All text drain-driven.
- `firmware_version.h`: 0.4.87 -> 0.4.88.

**Local build:** clean. 0x14f780 B, 56% partition free (was 57% at
Stage 31 close; extra ~4 KB from IMU + MAG subject buffers + formatters).

**What to watch on flash:**
1. Boot log:
   - `UI_SUBJ: init: env(5) + gps(9 str + 2 int) + imu(10 str + 1 int)
     + mag(4 str + 1 int) subjects created; g_env_q + g_gps_q +
     g_imu_q + g_mag_q up` (once)
   - `IMU_TILE: LSM6DSV16X tile init OK (subject-bound)` (once)
   - `COMPASS_TILE: LIS3MDLTR tile init OK (subject-bound)` (once)
2. Swipe to IMU tile:
   - Initial state: labels show `X: ---` etc. Switch reflects broker.
   - Turn switch ON via tap: `UI_SUBJ: imu switch ->
     broker_imu_set_enabled(1)` at DEBUG. Within one drain tick
     (~200 ms), status flips to "Online" + all 9 value labels
     populate. Roll/Pitch track wrist orientation.
   - Turn switch OFF: within one drain tick, status = "Disabled",
     all numeric labels = "---". No lag waiting for re-enable.
3. Swipe to Compass tile:
   - Same shape. Switch tap toggles via bind_checked + observer.
   - When enabled, XYZ + heading + cardinal populate. Needle rotates
     as the board turns.
   - Tap CALIBRATE (when enabled) -> label shows "CAL 30s" counting
     down. Rotate the board in a figure-8. Countdown drives the label
     text via drain -- no per-tile snprintf.
4. Theme swap on System tile:
   - IMU + Compass value labels + headers recolour via surviving
     apply_theme (Stage 33.3 sweep will delete these).
5. Regression:
   - Env tile still populates when its switch is on.
   - GPS tile still shows status line + subject-driven labels.
   - Drawer opens on swipe-up, closes on swipe-down.
6. No new E-lines from LVGL about observer/mutex, no
   `queue_trans failed`, no assert about un-initialised subject.

**If something's off, useful escalation:**
- `esp_log_level_set("UI_SUBJ", ESP_LOG_DEBUG)` -> per-drain
  `imu sample consumed` / `mag sample consumed` ticks + switch
  observer messages.
- `esp_log_level_set("UI_SUBJ", ESP_LOG_VERBOSE)` -> per-write
  `imu published: ax=... gx=... roll=... enabled=1` firehose.

**Bench verdict:** *(fill in after flash)*

---

### 7.1-close -- Batch 32.1 confirmed landed 2026-10-02

Re-audit against the current tree (not actually re-flashed; just verified
that the Batch 32.1 contract is already met):
- `imu_tile.c:114` has `lv_obj_bind_checked(s_sw_power, &subj_imu_enabled)`.
- `imu_tile.c:121-201` has `lv_label_bind_text` for every data label.
- `lsm6dsv16x.c:382-384` already does `xQueueOverwrite(g_imu_q, &bd)` and
  the "disabled snapshot" edge push.
- `compass_tile.c:133/149/156/163/214` mirror the pattern for mag.
- `lis3mdl.c:334-336` + `:362/380/386/395` push to `g_mag_q`.
- `ui_subjects.c` has `imu_format_and_publish` + `mag_format_and_publish`
  + the two observers + queue creates.

So Batch 32.1 silently landed in a prior session and the Stage 32 doc
just hadn't been updated. Marking as done without a dedicated flash.

### 7.2 -- Batch 32.2 flash (fw 0.4.96)

**What landed:**
- `ui_subjects.{c,h}`:
  - New HAPTIC domain: 3 string subjects (status, freq, sweep_info) +
    1 int (`subj_haptic_enabled`) + `g_haptic_q` queue.
  - `haptic_format_and_publish()` builds status/freq/sweep_info from the
    broker snapshot, deriving the status string from the state hierarchy
    (hw -> enabled -> countdown -> active -> calibrating -> calibrated).
  - Two-way observer `obs_haptic_enabled_to_broker` writes broker on
    switch tap AND pushes a fresh snapshot to `g_haptic_q` so the label
    updates on next tick (write_broker is event-driven and may not fire
    for minutes otherwise).
  - Init log line extended: `... + haptic(3 str + 1 int) subjects
    created; g_env_q + g_gps_q + g_imu_q + g_mag_q + g_haptic_q up`.
  - Drain cb has new `broker_haptic_data_t haptic_sample` branch.
- `haptic.c`:
  - Includes `ui_subjects.h`.
  - After `broker_haptic_write(&bd)` -> `xQueueOverwrite(g_haptic_q, &bd)`.
    One site (sweep/calibration state changes); event-driven not periodic.
- `haptic_tile.c`:
  - 3 labels now `lv_label_bind_text`-bound: status, freq, sweep_info.
  - Switch uses `lv_obj_bind_checked(s_sw_power, &subj_haptic_enabled)`.
  - `cb_power` kept but reduced to a single `haptic_play(ui_effect)` --
    the broker sync goes through the observer. No more s_syncing_power
    guard needed.
  - `haptic_tile_update()` shrunk from ~130 lines to ~65. Owns LED
    colour, status-label colour tint, sweep-info label colour tint,
    amplitude-bar value+visibility, SET-FREQ button visibility, IMU-CAL
    button enable. No `lv_label_set_text` calls remain.
  - Tile init primes `g_haptic_q` with current broker state so the
    first drain tick fills the labels before the first sweep event.
- Not touched: effect roller (keeps explicit cb_roller_changed +
  cb_confirm_effect pattern; subject-binding `lv_roller` adds re-entry
  pain without clear benefit).

**Scope deferral:** `light_tile` was originally paired with haptic in
Batch 32.2 for the "exercise two-way params" theme (brightness slider,
auto-br toggle, blue-light toggle). Skipped this flash because VEML6030
is stubbed per Stage 35; UI binding to zero-producer data is pure
busywork until the driver returns. Will fold back into whichever batch
un-stubs VEML6030.

**Flash-gate (what Ivan tests):**
- Boot log: `UI_SUBJ: init: env(5) + gps(9 str + 2 int) + imu(10 str +
  1 int) + mag(4 str + 1 int) + haptic(3 str + 1 int) subjects
  created; g_env_q + g_gps_q + g_imu_q + g_mag_q + g_haptic_q up`.
- Boot log: `BQ25619 init OK ... [WD off, safety-timer off, TS enabled,
  BATFET on]` (companion item, see §7.2b below).
- Haptic tile shows "Not Calibrated" / "Freq: --- Hz" / empty sweep-info
  initially. Toggle switch: haptic buzz fires, status flips to "Disabled"
  / "Calibrated ✓" via the drain within one tick (~200 ms). Tap IMU
  CAL: countdown appears in sweep_info ("Place flat... 5s" ... "1s"),
  bar appears when sweep starts, SET FREQ button appears, step info
  updates every ~200 ms. Latch sweep -> "Calibrated ✓" with new
  freq value, bar + button hide.

**If labels are stuck on initial text:** check for `UI_SUBJ: drain:
haptic sample consumed` at DEBUG. If missing, `g_haptic_q` wasn't
created (look for `init: xQueueCreate(g_haptic_q) FAILED`). If present
but labels don't update, verify the format function sees non-zero
`last_update_ms`.

### 7.2b -- BQ25619 safety-timer disable (bundled in fw 0.4.96)

- `bq25619.c`: cleared bit 3 (EN_TIMER) in REG05 alongside the existing
  WATCHDOG bits-5:4 clear. Addresses the "charging halts with fault 0x30
  after a few hours plugged in, resumes only on reboot" symptom Ivan
  logged 2026-09-29. Boot log changes to `[WD off, safety-timer off,
  TS enabled, BATFET on]`.
- Driver version 0.2.0 -> 0.2.1.

**Bench verdict:** *(fill after flash)*

### 7.3 -- Batch 32.3 (fw 0.4.97) -- battery_tile + build-fix

**Build-fix (what the previous flash caught):** removing
`drv2605_cmd_enable_set` from `haptic_tile.c` cb_power left
`drv2605_cmd.c` with no intra-archive caller. The linker dropped the
object and fc_cli's `HAPTIC` verb failed to resolve. Added
`WHOLE_ARCHIVE` to drv2605 CMakeLists -- per
`[[feedback_whole_archive_cmd_extract]]`. Same classification as
bq25619 / pcf85063 / encoder / ws2812 / lvgl_ui already carry.

**Battery tile:**
- `ui_subjects.{c,h}`:
  - New BATTERY domain: 6 string subjects (vbat, soc, chrg, pg, fault,
    boost) + `g_battery_q` queue. Formatter
    `battery_format_and_publish()` handles the `last_update_ms==0`
    "pre-first-sample" state too so initial values show "---" cleanly.
  - Init log line extended: `... + bat(6 str) subjects created; ... +
    g_battery_q up`.
- `bq25619.c`: `#include "ui_subjects.h"` + `xQueueOverwrite(g_battery_q,
  &bd)` immediately after `broker_battery_write(&bd)` in `task_battery_fn`.
- `bq25619/CMakeLists.txt`: added `lvgl_ui` to REQUIRES (mirrors the
  cycle already in place for lsm6dsv16x).
- `lvgl_ui/CMakeLists.txt`: added `bq25619` to REQUIRES so ui_subjects.h
  can include bq25619.h for `broker_battery_data_t`.
- `battery_tile.c`:
  - All 6 labels `lv_label_bind_text`-bound; init table-driven using
    the subject-pointer-per-row shape.
  - `battery_tile_update()` shrunk from ~30 lines to 4: LED colour
    only (text comes from drain path).
  - Tile init primes the queue with current broker state so the first
    drain tick fills labels instead of leaving them at init defaults.
- Driver bump: BQ25619 0.2.1 -> 0.2.2.

**Scope deferral:** `health_tile` was paired with battery in Batch 32.3
for the PPG/BCG + "smallest tile" theme. Skipped this flash because
MAX30101 is stubbed per Stage 35 -- same pattern we did for light_tile
in 32.2. Fold back when the driver un-stubs.

**Flash-gate:**
- Boot log: `UI_SUBJ: init: ... + bat(6 str) subjects created; ... +
  g_battery_q up`.
- Battery tile: Vbat / Fuel / State / Power / Fault / Boost labels all
  populate within ~200 ms of tile open. Toggle charger plug: `Power:
  USB OK` / `Power: Battery only` flips within one drain tick. Pull +
  reconnect USB: `State: Fast charge` appears, `Fault: 0x00` holds.
- Together with the Batch 32.2 BQ safety-timer disable, a multi-hour
  charge should no longer trip fault=0x30.

**If labels stay stuck at "---" after >1 s:** check for `UI_SUBJ: drain:
battery sample consumed` at DEBUG. If missing, g_battery_q wasn't
created or bq25619 task hasn't run yet.

---

### 7.X -- Deferred: LV_COLOR_DEPTH 16 -> 32 (not landed)

Originally planned for this batch as "text crispness polish". Spiked
before coding -- Stage 28 §4.3 already ran this experiment on 2026-09-13,
bench verdict was "text still fuzzy", reverted. The real color-order
fix was MADCTL BGR (Stage 30.2). My earlier recommendation to re-try
32-bit was based on a stale project memory. Updated
`project_display_text_crispness.md` with a "do not re-try" note +
listed real crispness leads (font bpp, subpixel layout, esp_lvgl_port
migration) for the next time this comes up.

---

## 8. Next-steps hook -- Stage 33

**Stage 33 -- structural finish** (~3 batches):
- 33.1: alarm as sibling pane; delete the alarm branch of
  `ui_navigation.c`; delete `ui_navigation.c` entirely if empty.
- 33.2: shutdown overlay -> `lv_msgbox` on `lv_layer_top()`; audit
  lock overlay + notification overlay layer usage.
- 33.3: delete every tile's `apply_theme()` cb; delete
  `apply_ui_theme` fan-out loop; delete `broker_xxx_read()` sensor
  accessors (queue is now private to drain timer); delete
  tile `update` cbs from registry contract.

After Stage 33 close, the LVGL 9 rebuild is done end-to-end.

## 9. References

- Stage 31 close doc: `Stage_31_Mk1b_LVGL_Foundations_Rebuild.md`
- LVGL 9 architecture spec:
  `docs/build_info/reference_files/lvgl_9/LVGL9_Kompic_Architecture.md`
- Corner-safe zone rule: [[project_display_corner_cutoff]]
- Font conversion: [[feedback_lv_font_conv_flags]]
- Flash cadence: [[feedback_flash_per_checkpoint]]
- Venue batching: [[feedback_group_work_by_venue]]
- Active-tile-at-Col-1 workflow: [[feedback_active_tile_at_col_1]]
- CLI-first testability: [[feedback_cli_first_testability]]
- Log tiering: [[feedback_esp_log_verbosity_practice]]
- MD frontmatter convention: [[feedback_md_frontmatter_convention]]
