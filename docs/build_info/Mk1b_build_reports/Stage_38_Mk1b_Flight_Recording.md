---
Date opened: 2026-10-04
Date closed: 2026-11-19
Status: CLOSED -- flight record writes SD but device reboots mid-session; "kinda works, extremely buggy." Followup goes into the OS/UI rebuild thread, not a Stage 38 patch.
---

# Stage 38 -- Mk1b flight recording mode (FCM_FLIGHT)

Baseline: fw 0.4.97 (portrait, post-Stage 37 revert) on iv8.0.
Scope locked by `Stage_38_Mk1b_Flight_Recording_PROMPT.md`:

  - §1 Port skeleton outline  -- READ ONLY this session, no code.
  - §2 CSV standard            -- codified in `docs/build_info/Mk2_intel/
                                  CSV_Standard.md` BEFORE flight code
                                  lands, so flight is born compliant.
  - §3 Flight recording mode   -- new FCM_FLIGHT slot, LSM + LIS + BME
                                  (+ GPS when fix) time-stamped CSV.

Target fw after Stage 38: **0.4.98**.

---

## 1. Open-question answers (prompt §3.6)

| # | Question                                  | Answer                                |
|---|-------------------------------------------|---------------------------------------|
| 1 | BLACKBOX repurpose or new FCM_FLIGHT?     | **New FCM_FLIGHT slot** (BLACKBOX intact). |
| 2 | Mode colour?                              | **Reuse ENV's green `{0,26,0}`** -- flight is basically env's broader cousin. |
| 3 | SD durability cadence?                    | **fsync every 100 ms** (10 rows at 120 Hz; ~100 ms worst-case loss). |
| 4 | GPS columns pre-fix?                      | **Always include, blank when no fix** -- stable schema per §2.3. |

---

## 2. Plan

### 2.1 Order of landing

Following prompt §2.6 ("codify §2 so flight writes compliant CSVs on
first flash"):

  1. Write `docs/build_info/Mk2_intel/CSV_Standard.md`.
  2. Append `FCM_FLIGHT` to `fc_mode_t` enum (BEFORE `FCM_COUNT`, so
     NVS boot-mode indices stay stable per prompt §1.1 Tier-1 note).
     Add `MODE_INFO[FCM_FLIGHT]` row sharing ENV's RGB `{0, 26, 0}`.
  3. New file `components/field_capture/fc_flight.c`:
       - `fc_flight_start()` / `fc_flight_stop()` -- the single
         `foo_cmd_*` handler per [[project_cli_ui_two_way_pattern]].
       - `flight_task` pinned to Core 0.
       - LSM poll at 120 Hz (nearest supported ODR; prompt said 100
         Hz, deviation noted -- `ts_ms` carries truth).
       - LIS poll at 10 Hz (driver's existing POLL_MS; prompt said
         80 Hz, deviation noted -- flight dynamics don't need 80 Hz
         mag).
       - BME forced-mode 1 Hz.
       - GPS: pull from `broker_gps_read()` each row; blank fields
         when `fix != GPS_FIX_3D`.
       - CSV open via a new `fc_flight_csv_open()` that writes the
         §2.2 header block (NOT reusing `csv_open()` -- that writer
         emits the legacy header; migrating all callers to the new
         header is out of scope per [[feedback_recording_datetime_
         opportunistic]]).
       - fsync every 10 rows (~83 ms at 120 Hz).
       - Haptic: `DRV_STRONG_CLICK` on start, `DRV_MEDIUM_CLICK` x2
         on stop, `DRV_LONG_BUZZ` on write failure (fire-and-forget,
         never blocks).
  4. Wire encoder click dispatch in `field_capture.c:
     run_recording_for_current_mode()` -- new `if (s_mode == FCM_
     FLIGHT) fc_flight_start()` branch.
  5. CLI verbs `flight start` / `flight stop` in `fc_cli.c` --
     dispatch to the same `fc_flight_*` handlers per [[project_cli_
     ui_two_way_pattern]] + [[feedback_cli_first_testability]].
  6. Flight tile UI (portrait): elapsed time, sample count, SD OK
     flag, est free MB. Keep as a per-tile duplicate consistent with
     [[project_lvgl_foundations_rebuild_planned]] (foundations
     rebuild lives in a future stage).
  7. Bump `KOMPIC_FW_VERSION` 0.4.97 -> 0.4.98 in
     `firmware_version.h`.

### 2.2 CSV schema for flight (prompt §3.3)

Columns (first row below the §2.2 header block):

```
ts_ms, ax_mg, ay_mg, az_mg, gx_mdps, gy_mdps, gz_mdps,
mx_mG, my_mG, mz_mG, temp_c, p_hpa, h_pct, gas_ohm,
mag_fresh, env_fresh,
gps_lat_deg, gps_lon_deg, gps_alt_m, gps_fix
```

Freshness strategy: emit one row per LSM sample; sparse channels
(LIS, BME, GPS) carry **last-known value** every row + a `_fresh`
boolean columns (`mag_fresh`, `env_fresh`). Chose this over
"blank between commas" because the postprocessor can filter on
freshness without timestamp arithmetic. §2.3 of the standard allows
either -- flight picks explicit freshness.

GPS columns: `gps_lat_deg`/`gps_lon_deg`/`gps_alt_m` blank when
`gps_fix == 0`; `gps_fix` enum: 0=none, 2=2D, 3=3D.

### 2.3 Sensor config deviations from prompt §3.2

| Sensor | Prompt     | Stage 38 actual        | Why                     |
|--------|------------|------------------------|-------------------------|
| LSM accel ODR | 100 Hz | **120 Hz** | Nearest driver-supported ODR; no reconfig layer today. |
| LSM FS_XL | ±16 g | **±4 g** (driver default) | Changing per-mode would need a reconfig layer -- out of scope. 4 g covers taxi/takeoff/turbulence; airframe hard impacts would clip but we don't need faithful crash capture. |
| LSM gyro ODR | 100 Hz | **120 Hz** | Same bus as accel. |
| LSM FS_G | ±2000 dps | **driver default** | Not touching per-mode config. |
| LIS mag ODR | 80 Hz | **10 Hz** (driver default) | Driver fixed at 10 Hz in current build; adequate for heading context, not for mag-based orientation tracking. |
| BME | 1 Hz forced | 1 Hz forced | Match. |

If flight data proves the 4 g clip is painful, Tier-5 of the port
(prompt §1.1) will add a mode-local reconfig layer.

### 2.4 Non-goals for Stage 38 (echoing prompt §3.4)

  - PSRAM ring buffer. fsync every 10 rows is the Stage 38 move.
  - Mic / audio sync clip. Mic IC TBD, out of scope.
  - Landscape rework. Portrait, as-shipped.
  - Any sensor outside LSM / LIS / BME / GPS.
  - Reconfiguring LSM / LIS to higher ODR / wider FS.

---

## 3. Implementation log

### 3.1 CSV_Standard.md
Landed at `docs/build_info/Mk2_intel/CSV_Standard.md`. v1 convention:
datetime filename, `#`-prefixed metadata block (device_hw / device_fw /
device_id / record_mode / record_start_utc / rtc_valid_at_start /
sample_rate_hz / columns / units), §3 column rules, §4 three allowed
durability cadences, §5 rotation triggers. §7 migration table shows
`flight_` born compliant; legacy writers migrate as touched.

### 3.2 FCM_FLIGHT enum + MODE_INFO
`field_capture.h`: `FCM_FLIGHT` appended before `FCM_COUNT`
(preserves NVS boot-mode indices for all existing modes). `fc_common.c`:
`MODE_INFO[]` row `{ "flight", 0, 26, 0 }` (shares env's green slot).
`ensure_sd()` pre-creates `/sd/data/flight`. `wake_sensors_for_mode()`
enables IMU + mag + env brokers for the mode (GPS is background).

### 3.3 fc_flight.c
New file. `run_flight_mode()` is the blocking mode body (called from
field_capture.c dispatch, same shape as `run_bcg_mode()` etc.).
Short buzz on start, double-click haptic on stop, long buzz if any
fsync fails. CSV opened via local `flight_csv_open()` that emits the
§2 header block — not reusing `csv_open()` since that writer emits the
legacy header and migrating all callers is out of scope. Row cadence
~125 Hz (`vTaskDelay(pdMS_TO_TICKS(8))`); fsync every 12 rows (~96 ms
loss window). Freshness tracked via broker `last_update_ms` deltas,
emitted as `mag_fresh` / `env_fresh` boolean columns. GPS always-column
with blanks on no fix; CSV `gps_fix` carries NMEA-style 0/2/3 (translated
from the broker's internal 0/1/2 enum). State struct exposed via
`fc_flight_get_state()` for the tile; `fc_flight_cmd_stop()` is the
CLI-callable stop verb.

### 3.4 Dispatch wiring (encoder + CLI)
`field_capture.c`: new `else if (s_mode == FCM_FLIGHT)` branch calls
`run_flight_mode()`. `fc_cli.c`: new `FLIGHT [STOP]` verb added
alphabetically ahead of `FLASH`. No `FLIGHT START` -- Stage 38
asymmetry: start lives on the encoder/tile only (consistent with
every other recording mode in this codebase). Full two-way parity is
a port-era refactor per [[project_cli_ui_two_way_pattern]].

### 3.5 Flight tile UI
`flight_tile.c` + `flight_tile.h` added to `field_capture/`. Portrait
read-only tile: `FLIGHT` header, big status line (`READY` / `REC
HH:MM:SS` -- green when SD healthy, red when fsync has failed),
sample count, SD OK/fail + free MiB + write-error count, GPS fix
status, hint footer "click = start/stop". Registered at tileview
Col 1 per [[feedback_active_tile_at_col_1]] (Stage 38 is the active
iteration); GPS pushed to Col 2 for the duration. `tile_registry.c`
header comment updated to reflect the new order.

CMake wiring: `field_capture/CMakeLists.txt` adds `flight_tile.c`,
plus `max_m10s`, `lvgl`, and `lvgl_ui` to REQUIRES. `lvgl_ui/CMakeLists
.txt` adds `field_capture` to REQUIRES so `tile_registry.c` can see
`flight_tile.h`. Mirrors the existing `lvgl_ui <-> max_m10s` cycle --
ESP-IDF accepts it.

### 3.6 Firmware version bump
`firmware_version.h`: `KOMPIC_FW_VERSION "0.4.97"` -> `"0.4.98"`.
Emitted into every flight CSV's `# device_fw = ...` metadata line.

---

## 4. Flash gate

**What am I testing this flash:**
- Boot comes up on fw 0.4.98 (log line + CSV header).
- Encoder cycles to the FLIGHT tile at Col 1 (first swipe-right from
  System). Tile shows "FLIGHT" / "READY" / zero samples / SD status.
- Encoder click: haptic click, LED green pulse, tile flips to "REC
  00:00:00" + sample count climbing at ~120 /s.
- Double-tap the watch body twice ~2 s apart. Expect clear accel
  spikes in the eventual CSV (bench this later).
- Encoder click: haptic double-click, tile returns to "READY" with
  the final sample count visible.
- `FLIGHT` over USB-CDC: prints `active=0 ... samples=... sd_ok=1`
  reflecting the last session.
- Pull SD, open `/sd/data/flight/kompic_YYYY-MM-DD_HH-MM-SS_flight.csv`:
    - §2 header block present (`# device_hw`, `# device_fw = 0.4.98`,
      `# columns = ...`, `# units = ...`), then the column header row.
    - `ts_ms` monotonic, ~8 ms spacing.
    - `ax_mg`/`ay_mg`/`az_mg` reasonable at rest (~0, 0, 1000).
    - `mag_fresh` toggles ~every 10 rows; `env_fresh` ~every 100.
    - GPS columns blank with `gps_fix=0` if no fix (expected
      indoors).

Known tweaks already deviating from prompt (§2.3 of this doc):
LSM at 120 Hz not 100; LSM FS stays ±4 g (driver default); LIS at
10 Hz driver default; no PSRAM ring buffer (fsync-per-N instead).

---

## 4a. Revision 2026-10-04 -- tile-button + background task

Ivan bench feedback after first build: "can't activate flight, as enc
selects the color" (FLIGHT shares env's green slot, so the physical
mode-cycle click is ambiguous) + "no rgb flashing, super distracting,
need more discrete" + "swipe to tile, press record, go back to main,
double-tap for display off."

Rewired as:
  - RECORD / STOP button on the flight tile is the primary start/stop
    surface. Click toggles `fc_flight_cmd_start()` /
    `fc_flight_cmd_stop()`.
  - Recording now runs in a dedicated background task spawned on start
    (`task_flight` on Core 0, prio 1, stack 4096). Task self-deletes on
    stop. UI stays responsive; user can swipe tiles / sleep the display
    without affecting the row stream.
  - RGB LED is left to rgb_policy during flight -- no mode pulse.
  - FCM_FLIGHT mode-cycle entry is now a safe no-op (logs a line, no
    recording side effects). Prevents accidental trigger from the
    colour-coded hw button cycle.
  - CLI gains `FLIGHT START` to match the tile button (two-way
    pattern per [[project_cli_ui_two_way_pattern]]).
  - Double-tap-to-sleep already works through
    `field_capture_back_gesture()` (btn-16 double + LSM tap_double
    both route there). No wiring needed.
  - `wake_sensors_for_mode(FCM_FLIGHT)` case removed from fc_common.c
    (dead now; brokers are enabled directly in cmd_start).
  - fw bumped 0.4.98 -> 0.4.99.

Updated flash gate below.

### Revised flash gate

**What am I testing this flash:**
1. Boot: fw 0.4.99 in log + eventual CSV header.
2. Swipe right from System tile -> FLIGHT at Col 1. Tile shows
   "FLIGHT", "READY", "Samples: 0", red RECORD button at bottom.
3. Tap RECORD. Haptic click. Button turns green, label changes to
   "STOP". Status flips to "REC 00:00:00" climbing. Sample count
   climbs at ~120/s. SD line shows "OK".
4. Swipe left back to System tile. Double-tap screen or btn-16 (or
   wait 30 s). Display sleeps. Recording continues (verify by
   waking after 60 s -- sample count should be ~7000 higher).
5. Wake display, swipe to FLIGHT tile. Tap STOP. Double haptic
   buzz. Button goes back to red "RECORD". State returns to
   "READY". Final sample count visible.
6. `FLIGHT` over USB-CDC: prints `active=0 samples=... sd_ok=1`.
   `FLIGHT START` / `FLIGHT STOP` work the same as the tile.
7. Pull SD: `/sd/data/flight/kompic_YYYY-MM-DD_HH-MM-SS_flight.csv`
   opens with §2 header (`# device_fw = 0.4.99`), monotonic ts_ms
   ~8 ms apart, resting ~(0,0,1000) mg, GPS blank with gps_fix=0.
8. During the "display off while recording" window: physical double-
   tap to wake, UI shows the current REC counter -- not a stale
   number.

Known deviations from prompt §3.2 unchanged (§2.3 of this doc).

---

## 4b. Revision 2026-10-05 -- LIS3MDL un-stubbed

First real flight capture (`kompic_2026-10-05_00-27-50_flight.csv`,
3391 rows / 42 s) landed with mag channels at a flat zero -- expected
because LIS3MDL was stubbed in Stage 35 ("legacy driver/i2c.h
incompatible with new i2c_master API"). Ivan: "mag is offline, we
need it back for this".

Ported `lis3mdl.c` to the new API in-place:
  - `lis3mdl.h` include flipped `driver/i2c.h` -> `driver/i2c_master.h`;
    LIS3MDL_DRIVER_VERSION 0.2.0 -> 0.3.0.
  - `lis3mdl.c`: added `static i2c_master_dev_handle_t s_lis_dev`.
    `write_reg` -> `i2c_master_transmit`, `read_reg` / `read_burst`
    -> `i2c_master_transmit_receive`. `lis3mdl_init()` now runs
    `i2c_master_bus_add_device(g_i2c0_bus_handle, ...)` on first
    call (idempotent). Legacy `i2c_num` parameters kept for API
    stability, ignored internally -- same pattern as `bme688_drv_init`.
  - `lis3mdl/CMakeLists.txt`: SRCS swapped from `lis3mdl_stub.c` to
    `lis3mdl.c`; REQUIRES extended with `esp_driver_i2c`, `esp_timer`,
    `freertos`, `boot_logic`, `lvgl_ui` (matches lsm6dsv16x pattern).
    `compass_tile.c` deliberately NOT re-added -- not registered in
    tile_registry today, scope-creep for Stage 38.

Boot wiring:
  - `boot_hw_init.c`: `#include "lis3mdl.h"` restored; 0x1C probe +
    `lis3mdl_init(I2C_NUM_0)` + `broker_mag_set_hw_status(true)`
    added where Stage 35 had left a comment block.
  - `boot_tasks.c`: `task_mag_fn` extern restored + spawned on Core 0
    priority 3 stack 4096 (same tier as `task_gps` / `task_battery`).

Side effects:
  - `broker_mag` now publishes live data at 10 Hz (CTRL1 DO = 10 Hz,
    POLL_MS = 100). Flight CSV `mx_mG` / `my_mG` / `mz_mG` columns
    should flip from flat zeros to real microtesla readings; the
    `mag_fresh` column will toggle to 1 every ~12-13 rows at 125 Hz
    row cadence.
  - No calibration task spawned yet -- `s_cal` sits at defaults
    (offset 0, scale 1, calibrated=false). Hard-iron offsets will
    show up in CSV but can be subtracted post-flight. If flight
    needs clean compass heading on-device, revisit `task_mag_cal_fn`.
  - fw 0.4.99 -> 0.4.100.

### Revised flash gate addendum

In addition to the §4a gate:
  - `WHOAMI` CLI (or boot log) should show `LIS3MDL 0x1C OK`.
  - On the flight tile with a live recording, numeric mag values
    appear in sidecar `FLIGHT` CLI dumps (if we later expose them)
    OR, pulling the CSV: `mx_mG` / `my_mG` / `mz_mG` are non-zero
    (roughly within a few hundred mG of Earth's ~500 mG; LRA offset
    may skew by up to ~1 G).
  - `mag_fresh=1` fraction in notebook stats cell should land near
    10 Hz / 125 Hz = 0.08 (previously it was ~0 because the stubbed
    broker never updated).

---

## 4c. Close 2026-11-19 -- bench verdict + context switch

Ivan ran it. Flight CSV wrote, mode engaged, SD healthy -- then the
device **rebooted mid-session**. No crash capture, no isolated repro.
Verbatim: "the flight recorder i guess worked fine but it crashed
rebooted so whatever man. kind of works, but still extremely buggy."

Not patching this in a Stage 38a. The symptom is of a piece with the
broader Mk1b OS/UI story -- months of incremental patches, each fix
revealing another soft spot. That's the real finding from Stage 38:
the current `field_capture` + `lvgl_ui` scaffolding is reaching the
end of what patch-work gets us.

Where the follow-up actually lives:
  - OS/UI rebuild thread (new initiative; see [[project_waveshare_
    edgeai_exploration]] below). Ivan is stepping sideways to explore
    edge-ai-lab's Chip OS on a Waveshare ESP32-S3 dev board as a
    reference point before deciding what Kompic's next-gen stack
    looks like.
  - Reboot-under-flight should be the first regression test the new
    stack has to survive, whatever shape it takes. Note for the
    future: capture `esp_reset_reason()` + partition "coredump" at
    the next boot, and surface it on the STATUS tile so we stop
    losing these.

No Mk1b firmware changes queued behind this stage. Mk1b stays on fw
0.4.100 until the architecture discussion lands.

---

## 5. Next steps

If the flash gate passes:
  - Fly with it. Collect at least one real taxi/takeoff/flight/
    landing CSV; verify double-tap spikes line up with video.
  - After the flight, decide whether:
      * ±4 g accel FS is painful (if yes, Tier-5 of the port needs
        a mode-local reconfig layer).
      * fsync-per-12-rows is a durability win vs. SD wear (if too
        aggressive, slide to every ~50 rows = 400 ms window).
      * `mag_fresh` / `env_fresh` columns are useful or wasteful
        (post-process will tell us).

Deferred to later stages:
  - CSV ring buffer in PSRAM (prompt §1.4).
  - Mic sync clip once the mic IC lands.
  - CLI `FLIGHT START` (requires state-machine mediation beyond
    Stage 38 scope).
  - Landscape rework. Still blocked on the Arduino MADCTL
    characterization sketch (prompt §1.3).

Memory updates to consider at close:
  - If flight gets used often, add a `feedback_flight_tile_at_col_1`
    override to [[feedback_active_tile_at_col_1]] so the slot
    convention tracks.
  - If any CSV field surprise lands, update
    `docs/build_info/Mk2_intel/CSV_Standard.md` in place.
