# Stage 10 — LSM6DSV16X advanced features + mode restructure

**Session:** 2026-07-22
**Firmware baseline:** `iv7.1.f0.2.1` (bumped from `f0.2.0`)
**Scope:** LSM chip features (pedometer, tap-Z, activity/inactivity, MLC scaffolding), UI/menu restructure, TEMP visual polish. Power management deferred to Stage 11.

---

## 0. Session workflow change — freeze-and-forget Arduino POCs

Per Ivan's direction, going forward every new sensor feature gets a standalone Arduino sketch as a "freeze-and-forget" reference implementation, then a refined port into ESP-IDF. The Arduino version is never touched again; it lives as a known-good bench reference. Rationale: rollback insurance for the growing ESP-IDF project.

New tooling: `hardware/Reflow_info/reference_files/make_archive.py` snapshots the project as a timestamped 7z (excluding build artifacts, datasheets, git internals). Run after each concrete step forward. Archives land in `hardware/Reflow_info/reference_files/archives/` (already gitignored).

Auto-memory: `[[feedback-arduino-sketch-pattern]]`, `[[project-mode-restructure]]`.

---

## 1. Arduino sketch — `12_lsm_full`

`firmware/arduino/12_lsm_full/12_lsm_full.ino` (~835 lines).

Full-range LSM6DSV16X demo in one interactive sketch. Boot config exercises the whole embedded-functions surface we care about, so any future ESP-IDF regression can be diffed against a known-good implementation.

**Exercised:**
- Accel HP mode @ 120 Hz, ±4 g (`CTRL1 = OP_MODE_HP | ODR_120HZ`)
- Accel LP mode 1 @ 15 Hz (via `VIEW_LP_MODE`)
- Gyro @ 240 Hz, ±2000 dps (`VIEW_GYRO` — enabled on demand)
- On-chip temperature
- Bank switching (§0 of extract 20.10b) — `FUNC_CFG_ACCESS.EMB_FUNC_REG_ACCESS`
- Embedded pedometer — `STEP_COUNTER_L/H` (0x62/0x63 in embedded bank)
- Activity / inactivity auto-sleep — `FUNCTIONS_ENABLE.INACT_EN = 10b` (LP accel + gyro sleep)
- Single-tap + double-tap on Z (`TAP_CFG0.TAP_Z_EN`, `WAKE_UP_THS.SINGLE_DOUBLE_TAP`)
- Sleep-state transition monitoring (`WAKE_UP_SRC.SLEEP_STATE`)
- MLC output register poll (`MLC1_SRC` — returns 0 without a loaded model; proves plumbing)

**Controls:**
- Button single-click → cycle demo view forward
- Button double-click → BQ25619 ship mode (`[[feedback-sketches-need-shipmode]]`)
- LSM Z single-tap → prints event, no state change
- LSM Z double-tap → cycle demo view backwards (the "go back" gesture)

**Bench-tuning targets:**
- `TAP_THS_Z_INIT = 12` (~1.5 g at ±4 g FS) — reduce if too insensitive, increase if wrist-bump false-triggers
- `TAP_DUR_INIT` — `DUR = 4` gives a ~1067 ms double-tap window at 120 Hz accel. Tighten if two-fingered/rapid taps read as separate singles

Once the sketch is happy at the bench, the same values live in the ESP-IDF driver (`components/lsm6dsv16x/lsm6dsv16x_emb.c`, `lsm6dsv16x_tap_z_enable()`) as the production defaults.

---

## 2. ESP-IDF pedometer — always-on

New file: `components/lsm6dsv16x/lsm6dsv16x_emb.c`. Extended header: pedometer, activity-sleep, tap-Z, MLC-load public API.

**Public entry points (all take/release `g_i2c_mutex` internally):**
- `lsm6dsv16x_pedometer_enable(bool)` / `_disable()`
- `lsm6dsv16x_pedometer_reset()` — writes `EMB_FUNC_SRC.PEDO_RST_STEP`
- `lsm6dsv16x_pedometer_read(uint32_t*)` — widens 16-bit chip counter to 32-bit host-side with rollover tracking

**Broker channel:** `broker_steps_data_t { step_count; last_update_ms; enabled; }` in `lsm6dsv16x.h`. Standard `BROKER_MODULE_IMPL` in `data_broker.c`. Populated by `task_imu_fn` at 1 Hz.

**Boot:** `boot_hw_init.c` calls `lsm6dsv16x_pedometer_enable(true)` right after `lsm6dsv16x_init()` succeeds, then flags `broker_steps_set_hw_status(true)` / `broker_steps_set_enabled(true)`. Ship mode does NOT explicitly reset the counter yet — the chip counter itself gets a reset on soft-reset at boot (via `CTRL3.SW_RESET` in `lsm6dsv16x_init`), so the count starts at zero every power-on. Explicit ship-mode reset can be added later if we want across-shutdown continuity.

**Field-verification checklist (do this on the bench):**
1. Boot, look for `LSM6DSV 0x6B OK  (pedo+tap-Z+act-sleep armed)` in log
2. Look for `pedometer ENABLE (ok)` from `LSM_EMB`
3. Look for `STEPS hw_alive: YES` from `BROKER`
4. Walk a known number of steps, enter STEPS mode (LSM submenu), verify the count matches ±10%
5. Verify a soft reset (button hold) restores count to 0

---

## 3. Activity / inactivity auto-sleep — **armed but currently a no-op**

`lsm6dsv16x_activity_sleep_enable(true)` is called at boot. Register configuration:
- `INACTIVITY_DUR = 0x35`: `WU_INACT_THS_W = 62.5 mg/LSB`, `XL_INACT_ODR = 15 Hz`, `INACT_DUR = 2 events`
- `INACTIVITY_THS = 4` → 250 mg activity threshold
- `FUNCTIONS_ENABLE.INACT_EN = 10b` — LP accel + gyro sleep on inactivity

**Why it's a no-op right now:** the firmware forces accel to HP mode at 120–240 Hz for MOTION and BCG use cases. The activity/inactivity classifier can only drop the ODR when the host isn't forcing it. Two paths to actually cash the battery win:
1. **Mode-dependent ODR policy** — drop accel to LP1 @ 30 Hz outside MOTION/BCG (pedometer still works, activity sleep engages). Not implemented this session; punted for Stage 11.
2. **Full power management pass** — `CONFIG_PM_ENABLE=y`, DFS, light-sleep with LSM INT1 as wake source. Ivan's explicit next-session target.

The plumbing is in place either way — as soon as we ODR-mode-switch, the sleep path is free.

**Field-verification when we get to (1) or (2):** `[ACT] transition -> SLEEP` log line when the board sits still for ~2 s; `-> ACTIVE` when it moves. `WAKE_UP_SRC` bit 3 reads back.

---

## 4. Tap-Z as a UI input

**Config baked into `lsm6dsv16x_tap_z_enable()`** (matching sketch 12 defaults):
- `TAP_CFG0 = TAP_Z_EN` — only Z axis, LIR=0 (poll and let chip auto-clear)
- `TAP_THS_6D = 12` — ~1.5 g threshold at ±4 g FS
- `TAP_DUR = 0x40` — DUR=4 (~1067 ms double-tap window at 120 Hz accel)
- `WAKE_UP_THS.SINGLE_DOUBLE_TAP = 1` — enable both single and double

**Event delivery — dual-publish:**
- `task_imu_fn` polls `TAP_SRC` on every tick (20 ms). If a Z-axis tap fires, it (a) increments `s_tap_z_single_count` or `s_tap_z_double_count` monotonic counters, and (b) sends a best-effort `UI_EVENT_TAP_Z_SINGLE` / `_DOUBLE` to `g_ui_event_q` for future LVGL overlay handling.
- `field_capture` (Core 1) reads `lsm6dsv16x_tap_z_double_count()` at the top of its main loop, dedupes on strict increase, and acts on it.

**UI binding:**
- Double-tap-Z → exit LSM submenu ("go back") — **only acts while `ST_STANDBY`**, never during an active recording
- Single-tap-Z → published as event but currently unbound in UI. Available for future gestures without needing more chip config

**LVGL:** `lvgl_ui.c::drain_ui_event_queue()` gets explicit no-op handlers for both events so they don't log "unknown event type" spam.

---

## 5. Mode restructure — LSM submenu

Per `[[project-mode-restructure]]`, the flat FCM_ list is transitioning to per-sensor slots with submenus. LSM is the first sensor to migrate; others stay flat during transition.

**Enum layout** (`field_capture.h`):
```
FCM_MIC, FCM_ENV, FCM_MOTION*, FCM_SKIN, FCM_FLASHLIGHT, FCM_ALARM,
FCM_COMPASS, FCM_ECG, FCM_TEMP, FCM_BCG*,
FCM_LSM,             ← Stage 10: top-level gateway (yellow)
FCM_STEPS*,          ← Stage 10: LSM submenu
FCM_MLC_COLLECT*,    ← Stage 10: LSM submenu
FCM_COUNT
```
`*` = LSM submenu-only, skipped by the top-level encoder cycle.

**Presentation order in submenu:** `FC_LSM_SUBMENU[] = { MOTION, BCG, STEPS, MLC_COLLECT }` (extend as needed).

**Navigation rules:**
- Top-level encoder rotate → `top_mode_step()` (skips submenu-only entries)
- Button single-click on `FCM_LSM` → enter submenu, land on `FCM_MOTION`
- In submenu, encoder rotate → `lsm_submenu_step()` (cycles through the four)
- In submenu, button single-click → dispatch to the appropriate handler (existing `run_bcg_mode` etc., new `run_steps_mode`, new `run_mlc_collect_mode`)
- Tap-Z double → exit submenu back to `FCM_LSM`

**LED:**
- Top-level `FCM_LSM` idle → solid yellow (`rgb_set_max(FCM_LSM)`) then breathe
- In submenu → `rgb_lsm_submenu_indicator(sub)` alternates yellow / submode-color at 1 Hz (500 ms each half)

**NVS backward compatibility:** on boot, if the persisted mode is a submenu-only entry (`is_lsm_submode(s_mode)` true), the submenu latch is auto-set to true. Handles both intentional save-in-submenu and pre-Stage-10 firmware that had `FCM_MOTION` or `FCM_BCG` as flat top-level modes.

**Other sensors still flat.** Migration is opportunistic per the memory.

---

## 6. TEMP fire strobing

`rgb_temp_warm_cycle()` reimplemented as `rgb_temp_fire_strobe`-behavior:
- Palette advances at ~5 Hz through red → orange → yellow
- Per-frame ~30 Hz pseudorandom brightness jitter (Knuth multiplicative hash, no PRNG state) 18–33 amplitude
- Never dips to zero (avoids reading as "fault")

Function name preserved for source compatibility with existing call sites in TEMP-mode LED loops.

---

## 7. MLC infrastructure

**`.ucf` loader:** `lsm6dsv16x_mlc_load_ucf(const char *ucf_text, size_t len)` in `lsm6dsv16x_emb.c`. Parses ST Unico / MEMS Studio export format:
```
-- header lines ignored
Ac 01 80        <- enter embedded bank
Ac 04 08        <- write to embedded reg 0x04
Ac 01 00        <- exit embedded bank
```
Parser is permissive (comment-friendly, whitespace-flexible), applies writes in order, aborts on first i2c error. Does NOT wrap in bank enter/exit — the `.ucf` owns bank protocol. Post-run insurance: force `FUNC_CFG_ACCESS = 0x00` to guarantee we end in main bank.

**Not wired to any UI path yet** — invoked from test code or a future settings tile when a trained model is available.

**Read-back:** `lsm6dsv16x_mlc1_read()` reads `MLC1_SRC` from embedded bank. Returns 0 until a classifier is loaded.

**`FCM_MLC_COLLECT` mode:** records raw 6-axis IMU samples to `/sd/data/mlc_train/s%04d_r%04d.csv` at ~50 Hz (matches `LSM6DSV16X_POLL_MS`). Columns: `t_ms,label,ax,ay,az,gx,gy,gz`. Encoder rotation cycles the `label` column 0..3 live during recording — the operator can mark class boundaries by rotating on activity change. Button single-click ends session.

**Data-collection workflow (for the BCG-quality-classifier pilot from Next_Session_10):**
1. Enter LSM submenu → `MLC_COLLECT`
2. Sit still, click to start recording, rotate encoder to label 0 ("still")
3. Type on keyboard for ~30 s, rotate to label 1 ("typing")
4. Walk for ~30 s, rotate to label 2 ("walking")
5. Repeat for other activities
6. Click to end. Pull `s%04d_r%04d.csv` off the SD card
7. Feed to Unico-GUI / MEMS Studio for training → get `.ucf`
8. In a future session, load `.ucf` via `lsm6dsv16x_mlc_load_ucf()` and consume `lsm6dsv16x_mlc1_read()` in `task_imu_fn`

---

## 8. QVAR refinement proposal (doc-only)

Stage 9 punch #1 (`sign hysteresis on QVAR touch classifier`) and Next_Session_10 §2 (power-state audit) both still open. Concrete steps for a future session:

**A. Power-state audit** — verify `qvar_local_disable()` in `field_capture.c` reliably clears `CTRL7.AH_QVAR_EN` on ECG-session exit. Instrument with a read-back of `CTRL7` post-disable in the log. If clean, close the punch item; if leaky, add explicit `write_reg(CTRL7, 0)` and re-verify.

**B. Sign hysteresis** — the current classifier flaps between `Qvar1` / `Qvar2` states because the HPF output crosses zero within its dead-band. Two mitigations to try:
1. Introduce a rectified-envelope hysteresis: only transition when the smoothed |HPF| exceeds 1.3× the previous state's exit-threshold (up-transition) or falls below 0.7× (down-transition). Adds latency but kills flap.
2. Widen the smoothing window (increase `bd_baseline` alpha's inverse). Trades transient response for stability.

**C. Consider retiring** — Ivan noted QVAR isn't really useful in current form. If touch-gesture is what we want, LSM tap-Z (now landed) is a strictly better UI input. QVAR's remaining unique use is the raw ECG stream, which is a separate, valuable diagnostic. Recommendation: keep QVAR mode but stop pretending it's a touch button; label it "ECG-raw" and remove the flap-prone classifier entirely.

---

## 9. What else could we squeeze from the LSM (pruned)

Everything not already touched this session, ranked effort × value:

| Feature | Effort | Value | Note |
|---|---|---|---|
| Wake-on-motion via INT1 → light-sleep exit | Med | **High** | Prerequisites: Stage 11 PM work. Register: `MD1_CFG.INT1_WU = 1`. Wire GPIO8 to `esp_sleep_enable_ext1_wakeup`. |
| Free-fall detection | Low | Low | Register: `FREE_FALL (0x5D)`. Nice to log "watch fell" but no strong UX today. |
| 6D / 4D orientation change events | Low | Med | `TAP_THS_6D.D4D_EN` + polling `EMB_FUNC_STATUS_MP`. Cleaner "wrist-up detected" than gyro-integrated approach. |
| Tilt detection | Low | Low | Semi-redundant with 6D. Skip. |
| Significant motion (`SIGN_MOTION_EN`) | Low | Med | Wake host from long idle only when the user has moved substantially. Complements wake-on-motion. |
| Sensor fusion (SFLP) game rotation vector | Med | Med | Replaces host-side complementary filter. Saves CPU cycles but the CF already works. Defer unless we need heading stability. |
| MLC classifier (real one, not scaffolding) | **High** | **High** | Data-collection is now possible via `FCM_MLC_COLLECT`. Training loop is next. |
| FSM (finite state machine) programs | Very High | Med | 20.10b §8. Days of work per gesture. Defer to v2+. |
| Sensor hub Mode 2 (LSM as I2C master to LIS3MDL) | Med | Low | Would save an I2C mutex hop but LIS3MDL is on a different bus segment. Effort not justified. |
| Adaptive Self-Configuration | Med | Low | Skip for wearable use. |
| Analog hub (beyond QVAR) | Very High | Very Low | Skip. |

---

## 10. Version bumps

Every touched driver PATCH-bumped, plus `KOMPIC_FW_VERSION`:

| Component | 0.2.0 → 0.2.1 |
|---|---|
| `LSM6DSV16X_DRIVER_VERSION` | new `_emb.c` + advanced-features API |
| `DATA_BROKER_DRIVER_VERSION` | `broker_steps_*`, `UI_EVENT_TAP_Z_*` |
| `FIELD_CAPTURE_DRIVER_VERSION` | LSM submenu, TEMP fire strobing, new modes |
| `KOMPIC_FW_VERSION` | rolling aggregate |

`imu_tile.c`, `lvgl_ui.c`, `boot_hw_init.c` also modified but they don't carry their own version macros (aggregated under above).

---

## 11. Test / verification checklist for the bench

Compile-checked, not yet bench-verified. Ivan's next-session opener:

1. [ ] Flash `12_lsm_full.ino`. Cycle demo views. Confirm:
   - Pedometer count increments when walking (`VIEW_STEPS`)
   - Tap-Z double-tap reliably reverses the view (bench-tune `TAP_THS_Z_INIT` if false-positive or false-negative)
   - Activity transitions log on `VIEW_ACTIVITY` after ~2 s of stillness
2. [ ] Flash current ESP-IDF build (`smartwatch.bin`). Confirm boot log shows `pedo+tap-Z+act-sleep armed`.
3. [ ] Cycle to LSM slot (yellow). Single-click enters submenu.
4. [ ] In submenu, cycle to STEPS. Single-click starts session, walk, single-click ends. Verify `/sd/data/steps/s*_r*.csv` written with delta column.
5. [ ] In submenu, tap-Z double while STANDBY → exits back to LSM top-level.
6. [ ] Cycle to TEMP → verify fire strobing (not the old slow warm cycle).
7. [ ] Cycle to MLC_COLLECT. Click, record ~10 s each of "still" and "walking" (rotate encoder between segments). Verify CSV written with label column changes.

---

## 12. Known limitations / punch for Stage 11

1. Activity/inactivity is a no-op until we implement mode-dependent accel ODR or full PM (Ivan's next-session target).
2. Ship-mode does not explicitly reset the pedometer counter (chip soft-reset at boot handles it, but if we ever call `SW_RESET` mid-session that would zap the count silently).
3. `MLC_COLLECT` records via broker (~50 Hz) — for high-rate training (>60 Hz) we'd need to bypass the broker and read directly from `task_imu_fn`. Fine for BCG-window classifiers.
4. QVAR sign-hysteresis fix not implemented (see §8).
5. Encoder-based label toggle in `MLC_COLLECT` is 0..3 hardcoded. Extend `MLC_LABEL_COUNT` to widen. No UI to change it live.
6. Fire strobing looks great in isolation; may want to gate on user-config later (some users hate flicker).
7. `12_lsm_full.ino` uses accel HP @ 120 Hz for tap timing; production driver runs 120 Hz too. If future BCG work bumps back to 240 Hz, re-tune `TAP_DUR` (LSB depends on ODR).
