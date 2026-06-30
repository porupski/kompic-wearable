# PHASE 6 BATCH — INFRASTRUCTURE (BROKER + XD + NVS + FUSION + ALARM + UI)
**Assigned:** 13 June 2026
**Firmware version:** iv7.1.f0.0
**Master instructions:** docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md (required reading)

---

## CONTEXT

Phases 0–5 ported every individual chip driver to the v7.2 part list (CO5300, CST9217,
PCF85063, BQ25619, BootPower, LSM6DSV16X, LIS3MDLTR, VEML6030, MAX-M10S, BME688,
MAX30101, TMP117, DRV2605, CrownEncoder, WS2812, Flashlight, SDCard, PDM_Mic, QvarECG).
Each chip driver now exposes its own `broker_xxx_data_t` struct, `BROKER_XXX_TIMEOUT_MS`
constant, and (where applicable) tile descriptor.

Phase 6 ports the **cross-cutting infrastructure** that sits *on top of* the chip drivers
and was copied verbatim from SmartWatch v5: it still references dead chip headers
(`tu10f.h`, `qmc5883p.h`, `bh1750.h`, `qmi8658.h`, `app_battery.h`, `bme280_drv.h`,
`max30102.h`) and stale CMake dependencies. The behaviour is mostly correct, the
plumbing needs to be re-pointed.

Hit the ground running: when hardware arrives, the firmware should compile end-to-end
with every v7.2 part wired through the broker → cross_driver → fusion → UI stack.

---

## MODULES TO PORT (in dependency order)

> Order is non-negotiable: data_broker first (everyone else #includes it); UI last
> (depends on everyone). Do not interleave.

### Module 1: data_broker (central state)

**Source files:**
- `firmware/components/data_broker/data_broker.h`
- `firmware/components/data_broker/data_broker.c`
- `firmware/components/data_broker/CMakeLists.txt`
- `firmware/components/data_broker/power_flags.h` (already v7.2-clean, verify only)
- `firmware/components/data_broker/ui_event.h` (chip-agnostic, verify only)

**Code audit — header swaps required:**

| Old #include (v5)      | New #include (v7.2)   | Notes                                                                       |
|------------------------|-----------------------|-----------------------------------------------------------------------------|
| `tu10f.h`              | `max_m10s.h`          | GPS. Confirm `broker_gps_data_t` is defined there with `position_valid`, `fix`, `altitude_m`, `first_fix_notified`. |
| `qmc5883p.h`           | `lis3mdl.h`           | Magnetometer. Confirm `broker_mag_data_t` with `calibrating`, `calibrated`.   |
| `app_battery.h`        | `bq25619.h`           | Battery moved into the BQ25619 driver. Confirm `broker_battery_data_t` with `percentage`. |
| `bh1750.h`             | `veml6030.h`          | Light sensor. Confirm `broker_light_data_t`.                                  |
| `qmi8658.h`            | `lsm6dsv16x.h`        | IMU. Confirm `broker_imu_data_t`.                                             |
| `bme280_drv.h`         | `bme688.h`            | Environment. Confirm `broker_env_data_t` (pressure_hpa preserved for fusion). |
| `max30102.h`           | `max30101.h`          | HR/SpO2/PPG. Confirm `broker_hr_data_t`.                                      |
| `pcf85063.h`           | `pcf85063.h`          | RTC — same chip, no change.                                                   |
| `drv2605.h`            | `drv2605.h`           | Haptic — same chip, no change.                                                |
| `alarm.h`              | `alarm.h`             | Same (this batch).                                                            |
| `fusion.h`             | `fusion.h`            | Same (this batch).                                                            |

**Implementation scope:**
- Swap the 7 chip headers above (one-line edits, no API surface changes).
- For each new driver header, verify the broker contract (`broker_xxx_data_t` typedef
  + `BROKER_XXX_TIMEOUT_MS` macro). If absent, **stop** and log as a CRITICAL defect
  blocking the chip driver, not the broker.
- Update `CMakeLists.txt` REQUIRES: drop `tu10f`, `qmc5883p`, `bh1750`, `qmi8658`,
  `app_logic` (battery), and add `max_m10s`, `lis3mdl`, `veml6030`, `lsm6dsv16x`,
  `bq25619`, `bme688`, `max30101`. Keep `pcf85063`, `drv2605`, `alarm`, `fusion`,
  `freertos`, `esp_timer`.
- Sanity-check the BROKER_MODULE_IMPL / BROKER_MODULE_NO_STATUS macro calls — every
  slot referenced in `broker_state_t` must have a corresponding `set_hw_status` /
  `hw_alive` pair. Add slots and HW flags for any new module that needs a broker slot
  but does not yet have one (TMP117 skin temp is the main candidate — see Open
  questions).
- Fix the mojibake (`â†'`, `Â§`) that survived the cp from v5 — UTF-8 arrows and §.

**Test harness:**
- `firmware/test/test_data_broker.c` — instantiate broker_init() on a bare ESP32,
  exercise each `broker_xxx_write` + `broker_xxx_read` round-trip, verify status
  transitions OFFLINE→DISABLED→ACQUIRING→ONLINE→STALE by manipulating
  `set_hw_status`/`set_enabled`/`last_update_ms` directly. No real hardware needed.
- Measure: mutex contention under 10× write + 10× read in parallel tasks, queue
  send/receive latency for `g_ui_event_q`.

**Deliverables:**
- `firmware/docs/porting/DataBroker_2026-06-13_iv7.1.f0.0.md`
- Updated `firmware/components/data_broker/*.{c,h}` + CMakeLists.txt
- `firmware/test/test_data_broker.c`

---

### Module 2: cross_driver (Core 0 event bus)

**Source files:**
- `firmware/components/cross_driver/cross_driver.h`
- `firmware/components/cross_driver/cross_driver.c`
- `firmware/components/cross_driver/CMakeLists.txt`

**Code audit:**
- Mostly chip-agnostic. Comments still reference v5 chips (BME280/BME688, MAX30102/MAX30101).
- Event catalogue is forward-compatible but missing v7.2-specific events.

**Implementation scope:**
- Update the `XD_EVENT_ENV_UPDATED` comment: `(BME280/BME688, future)` → `(BME688)`.
- Update `XD_EVENT_HR_UPDATED` comment: `(MAX30101, future)` → `(MAX30101)`.
- Update `XD_EVENT_LIGHT_UPDATED` comment: `(BH1750, future)` → `(VEML6030)`.
- Add new event slots **at the end of the enum** (never reorder — ordinals are array
  indices) for the new v7.2 capabilities, even if no producer/consumer wires them yet:
  - `XD_EVENT_TMP_UPDATED`     — new skin-temp sample (TMP117). data = `broker_tmp_data_t*`
  - `XD_EVENT_CROWN_TURN`      — crown encoder rotation tick. data = `int8_t*` (signed delta)
  - `XD_EVENT_ALARM_FIRED`     — alarm task fired a slot. data = `uint8_t*` (slot index)
  - `XD_EVENT_BUTTON_LONG`     — boot button long-press (re-export from boot_power)
  - `XD_EVENT_SD_MOUNTED`      — SD card mount succeeded (Phase 5)
  - `XD_EVENT_MIC_FRAME_READY` — PDM frame available for consumers
  - `XD_EVENT_ECG_FRAME_READY` — Qvar ECG frame complete
- Extend `s_event_names[]` for every new ordinal — array uses designated initialisers,
  so order does not matter, but every new ordinal **must** be present or the lookup
  returns NULL and triggers `cross_driver_event_name` to print "UNNAMED".
- Verify `cross_driver_init()` is still called from `boot_logic` before any task.

**Test harness:**
- `firmware/test/test_cross_driver.c` — register N callbacks for each event, fire
  the event with a synthetic payload, assert each callback observed the payload.
- Measure: dispatch overhead for 0-listener events, 4-listener events; verify
  `configASSERT` triggers when over-registering (negative test, can be commented out).

**Deliverables:**
- `firmware/docs/porting/CrossDriver_2026-06-13_iv7.1.f0.0.md`
- Updated `firmware/components/cross_driver/*.{c,h}` + CMakeLists.txt
- `firmware/test/test_cross_driver.c`

---

### Module 3: app_logic (NVS + UI settings async save)

**Source files:**
- `firmware/components/app_logic/app_nvs.h`
- `firmware/components/app_logic/app_nvs.c`
- `firmware/components/app_logic/CMakeLists.txt`

**Code audit:**
- `CMakeLists.txt` still lists `app_battery.c` in SRCS — that file does not exist in
  the directory and battery now lives in `bq25619` (Phase 1). Build will fail.
- `esp_adc` is in REQUIRES but no longer needed (BQ25619 reads battery via I2C, not
  ADC). Drop it.
- Header comment block still says `"battery" -- lifetime min/max voltage (owned by
  app_battery.c directly)` — outdated; battery NVS keys now live inside `bq25619.c`.
- Magnetometer-cal NVS API stays — same key names work with LIS3MDLTR (it has hard-iron
  offsets in the same form). Verify that LIS3MDLTR's task_mag_cal_fn calls these.
- GPS-date NVS API stays — MAX-M10S seeds RTC the same way as TU10F.
- Height-reference API stays (commented future-use; activates when fusion adds baro).

**Implementation scope:**
- Drop `app_battery.c` from `CMakeLists.txt` SRCS.
- Drop `esp_adc` from `CMakeLists.txt` REQUIRES.
- Update header doc comment: remove the `"battery"` namespace mention.
- Verify the async save queue + `task_settings_saver_fn` flow still compiles — `ui_settings_t`
  lives in `ui_broker.h`, which is unchanged.
- Confirm `app_nvs_init()` is still called from main before any driver init.
- No new NVS keys in this phase — alarm slots use their own `"alarm_cfg"` namespace
  inside `alarm.c`; do not move them here.

**Test harness:**
- `firmware/test/test_app_nvs.c` — exercise mag-cal save/load round-trip (verify
  float-to-scaled-int32 quantisation is exact), GPS-date strictly-newer guard,
  height-reference save/load. Run with `nvs_flash_erase()` before each subtest.
- Measure: NVS write latency (mag-cal save = 5 floats + 1 u8), erase time, partition
  fill rate.

**Deliverables:**
- `firmware/docs/porting/AppNVS_2026-06-13_iv7.1.f0.0.md`
- Updated `firmware/components/app_logic/*.{c,h}` + CMakeLists.txt
- `firmware/test/test_app_nvs.c`

---

### Module 4: fusion (derived data — Tier 3)

**Source files:**
- `firmware/components/fusion/fusion.h`
- `firmware/components/fusion/fusion.c`
- `firmware/components/fusion/CMakeLists.txt`

**Code audit:**
- Header comment still says `BME280/BME688 driver lands` — BME688 has landed.
- The barometric-altitude branch is **commented out** because the broker did not
  expose `broker_env_read` at v5 time. With BME688 in the broker, uncomment and
  wire it up.
- `task_fusion_fn` reads only GPS today. After this phase it should read GPS + ENV
  and arbitrate altitude correctly (GPS → BARO → LAST).
- Activity classification is still a stub; leave it (do **not** add IMU+HR fusion
  in this phase — that's a v2 task).
- Cross-driver listener for `XD_EVENT_GPS_FIX_VALID` is a no-op; leave it as a
  registration scaffold for the future calibration path.

**Implementation scope:**
- Uncomment the BME688 read block in `task_fusion_fn` and adapt to current
  `broker_env_data_t` field names.
- Replace `(void)baro_altitude_m;` suppression with a live call.
- Verify the `altitude_arbitration` walks GPS → BARO → LAST → NONE correctly under
  unit-style mock data (GPS missing + BARO present, GPS missing + BARO missing +
  prev GPS present, etc.).
- Keep the 1 Hz rate, 2048-byte stack, priority 2, Core 0.
- Confirm `BROKER_FUSION_TIMEOUT_MS` = 3000 still fits with a 1 Hz producer.

**Test harness:**
- `firmware/test/test_fusion.c` — drive the broker with synthetic GPS / ENV writes,
  let `task_fusion_fn` run for a few ticks, assert `broker_fusion_read` returns the
  expected `altitude_m` + `altitude_source` for each scenario:
  (a) GPS fix → ALTITUDE_SRC_GPS, gps altitude.
  (b) No GPS, BME688 online with sane pressure → ALTITUDE_SRC_BARO, baro altitude.
  (c) No GPS, no ENV, previous GPS present → ALTITUDE_SRC_LAST, carried altitude.
  (d) Nothing → ALTITUDE_SRC_NONE.
- Measure: fusion task stack high-water, per-tick CPU cost.

**Deliverables:**
- `firmware/docs/porting/Fusion_2026-06-13_iv7.1.f0.0.md`
- Updated `firmware/components/fusion/*.{c,h}` + CMakeLists.txt
- `firmware/test/test_fusion.c`

---

### Module 5: alarm (vibration alarm + UI tile)

**Source files:**
- `firmware/components/alarm/alarm.h`
- `firmware/components/alarm/alarm.c`
- `firmware/components/alarm/alarm_tile.h`
- `firmware/components/alarm/alarm_tile.c`
- `firmware/components/alarm/CMakeLists.txt`

**Code audit:**
- The alarm task is broker-driven (RTC poll, DRV2605 fire, UI event queue). All three
  dependencies exist in v7.2. No chip-header swaps required.
- The `buzzer` slot field is a stale stub: v5 reserved GPIO42 as a piezo buzzer. In
  v7.2, **GPIO42 is the WS2812 status LED** — there is no piezo. Decide one of:
    (a) Remove the `buzzer` field entirely (NVS key `buzz` becomes unread; existing
        NVS values are tolerated by ignoring them).
    (b) Repurpose the field as `led_strobe` (boolean: when alarm fires, also strobe
        WS2812 red until dismissed). This is the cleaner story — keep the schema,
        change the semantics, rename in code.
  *Default recommendation:* (b). It re-uses the persisted bit and gives alarms a
  visible cue. See Open Questions.
- Haptic pattern IDs reference DRV2605 effect-library numbers. The driver chip is the
  same — patterns should still resolve. Verify against the v7.2 DRV2605 effect table.
- `alarm_tile.c` is a Core 1 LVGL screen — touch it only for theme and font changes
  driven by the broker, not by alarm chip changes.

**Implementation scope:**
- Apply Option (b) above OR Option (a) — settle in Open Questions first. **Do not
  guess.** Until decided, leave `buzzer` field in place but flag DEFECT-MED.
- Rename `buzzer` → `led_strobe` if (b); wire `task_alarm_fn` to call
  `ws2812_set_color(255,0,0)` for the strobe interval, restore previous colour on
  STOP / auto-dismiss / snooze (add WS2812 to alarm CMakeLists REQUIRES).
- Confirm `alarm_nvs_load_all` / `alarm_nvs_save_slot` keep working — the NVS schema
  bit-for-bit identical except the meaning of the `buzz` key changes.
- Re-verify the snooze-on-reboot path: `alarm_nvs_save_snooze` writes minute-of-day;
  on boot, `alarm_nvs_load_all` must restore it before `task_alarm_fn` starts ticking.

**Test harness:**
- `firmware/test/test_alarm.c` — instantiate broker + RTC stub + DRV2605 stub
  (mockable), arm a slot 1 minute in the future, advance the stub RTC, assert the
  alarm fires, then test SNOOZE / STOP / AUTO-DISMISS state transitions. Use the
  fakes folded into the broker for set_hw_status so the task does not crash on
  missing hardware.
- Measure: task_alarm_fn loop time, NVS save latency per edit, queue send latency.

**Deliverables:**
- `firmware/docs/porting/Alarm_2026-06-13_iv7.1.f0.0.md`
- Updated `firmware/components/alarm/*.{c,h}` + CMakeLists.txt
- `firmware/test/test_alarm.c`

---

### Module 6: lvgl_ui (UI orchestrator + tile registry)

**Source files:** (everything in `firmware/components/lvgl_ui/`)
- `lvgl_ui.{c,h}`
- `ui_broker.{c,h}`
- `ui_status_bar.{c,h}`
- `ui_lock_screen.{c,h}`
- `ui_shutdown_overlay.{c,h}`
- `ui_main_screen.{c,h}`
- `ui_settings_screen.{c,h}`
- `ui_navigation.{c,h}`
- `ui_notif_overlay.{c,h}`
- `tile_registry.{c,h}`
- `system_tile.{c,h}`
- `ui_theme_colors.h`
- `fa5_select_14.c` (LVGL font asset, no port work)
- `CMakeLists.txt`

**Code audit:**
- `CMakeLists.txt` REQUIRES `bh1750`, `tu10f`, `qmc5883p` — all dead. Need
  `veml6030`, `max_m10s`, `lis3mdl`, plus the rest of the v7.2 part list whose
  tile_desc symbols are referenced from `tile_registry.c`.
- `tile_registry.c` includes `light_tile.h`, `gps_tile.h`, `rtc_tile.h`,
  `compass_tile.h`, `imu_tile.h`, `env_tile.h`, `health_tile.h`, `haptic_tile.h`.
  Each of those headers now ships in the corresponding v7.2 driver folder:
    - `light_tile.h`   → `components/veml6030/`
    - `gps_tile.h`     → `components/max_m10s/`
    - `rtc_tile.h`     → `components/pcf85063/`
    - `compass_tile.h` → `components/lis3mdl/`
    - `imu_tile.h`     → `components/lsm6dsv16x/`
    - `env_tile.h`     → `components/bme688/`
    - `health_tile.h`  → `components/max30101/`
    - `haptic_tile.h`  → `components/drv2605/`
  So the include statements work as soon as the CMakeLists REQUIRES are correct.
- The comment block at the top of `tile_registry.c` still says
  `Col 5: Compass (QMC5883P)`, `Col 6: IMU (QMI8658)` — wording only.
- `ui_status_bar.c` LED strip is chip-agnostic (broker_xxx_get_status()) — leave
  alone but verify the broker calls compile after Module 1.
- No tile is wired for **TMP117 (skin temp)** or **Qvar ECG (ecg_tile)** yet. ECG
  has `qvar_ecg/ecg_tile.{c,h}` already; TMP117 has no tile file. Add to registry
  *only if* tile_desc exists — otherwise log a defect and defer.

**Implementation scope:**
- Swap CMakeLists REQUIRES (drop dead chips, add v7.2 chips).
- Fix the comment block in `tile_registry.c`.
- Add `&ecg_tile_desc` to the registry (extern from `qvar_ecg/ecg_tile.h`) if it
  exists and exports the `tile_desc_t` symbol. Otherwise file a defect against the
  qvar_ecg Phase-5 deliverable.
- If TMP117 tile is missing, add a minimal one in `components/tmp117/temp_tile.{c,h}`
  (label-only, "Skin: 36.4 °C" or similar). Treat as a Phase-6 extension, not a v5
  port — and document the scope.
- Update `lvgl_ui.c` log lines referencing `BH1750` / `QMI8658` to new chips
  (cosmetic).
- Verify `light_tile_create_overlay()` (used by auto-brightness) still resolves to
  the `veml6030` component. (The function is declared in `light_tile.h` so as long
  as the new driver keeps the same symbol it Just Works.)
- Verify `apply_ui_theme()` fan-out still walks every tile registered.

**Test harness:**
- `firmware/test/test_lvgl_ui.c` — boot LVGL on the real CO5300 display (or a
  software simulator if the display is not bring-up yet), build all screens, walk
  every tile via the registry, assert no NULL handles, no missing apply_theme
  callback. Drive synthetic broker writes; verify tile labels refresh within one
  refresh tick (200 ms).
- Measure: lvgl_port_lock latency, refresh tick duration with all tiles populated,
  LVGL heap usage with the full tile set, draw FPS while idle.

**Deliverables:**
- `firmware/docs/porting/LvglUI_2026-06-13_iv7.1.f0.0.md`
- Updated `firmware/components/lvgl_ui/*.{c,h}` + CMakeLists.txt
- `firmware/test/test_lvgl_ui.c`

---

## OUTPUT

You will produce **6 dated .md files** in `firmware/docs/porting/`:
- DataBroker_2026-06-13_iv7.1.f0.0.md
- CrossDriver_2026-06-13_iv7.1.f0.0.md
- AppNVS_2026-06-13_iv7.1.f0.0.md
- Fusion_2026-06-13_iv7.1.f0.0.md
- Alarm_2026-06-13_iv7.1.f0.0.md
- LvglUI_2026-06-13_iv7.1.f0.0.md

Plus updated component sources + 6 test harnesses in `firmware/test/`.

**Profiling template must be filled** for every module:
- Boot-time cost (broker_init, cross_driver_init, app_nvs_init, fusion_init, alarm
  task spawn, lvgl_ui_init).
- Per-operation cost (broker mutex acquire/release, XD dispatch, NVS read/write,
  fusion tick, alarm tick, LVGL refresh tick).
- Memory (broker_state_t footprint, XD listener table, ui_settings_t, alarm
  broker slot, fusion broker slot, LVGL heap with all tiles + buffers).
- Current draw (idle vs active for the alarm and UI tasks specifically — these
  run unconditionally and matter for the power budget).

**Defects logged** as `[DEFECT-NN]` in each .md.

**Commit strategy:** one commit per module (6 total) in dependency order.
Do not combine. Each commit follows the master format:
`[Module] Porting: <short>, iv7.1.f0.0, N issues noted`.

---

## DEPENDENCY GRAPH

```
data_broker  (Module 1 — must be first)
  ├── cross_driver       (Module 2)
  ├── app_logic          (Module 3)
  ├── fusion             (Module 4 — depends on broker + cross_driver)
  ├── alarm              (Module 5 — depends on broker + drv2605 + pcf85063 + ws2812)
  └── lvgl_ui            (Module 6 — depends on everyone above)
```

Do not start Module N until Module N-1 builds and the test harness passes (or its
failures are documented as Open Questions).

---

## OPEN QUESTIONS FOR IVAN

Resolve before opening Module 5 and Module 6:

1. **Alarm `buzzer` field.** Remove entirely (option A) or repurpose to
   WS2812 red-strobe on alarm fire (option B)? Recommendation: B. Persisted NVS
   schema preserved, alarms gain a visual cue, GPIO42 LED gets a job during
   sleep-bedside use.

2. **TMP117 tile.** Add a minimal skin-temp tile in Phase 6, or defer to a later
   v2 batch alongside body-temp fusion? Recommendation: defer. Scope creep risk
   inside Phase 6 is real; ship a clean infra port first.

3. **ECG tile in registry.** The `qvar_ecg/ecg_tile.{c,h}` deliverable exists.
   Add it to `tile_registry.c` now (Module 6) so the tileview includes an ECG
   screen, or hold until the Qvar register map is verified on real hardware?
   Recommendation: add now, label "Stub" until verified.

4. **Battery NVS namespace.** `app_battery.c` is gone; its "battery" NVS namespace
   (lifetime min/max voltage) is now expected to be owned by `bq25619.c`. Confirm
   BQ25619 took over the namespace, or migrate the keys here? Recommendation:
   audit `bq25619.c`; if it does not yet own the namespace, file a defect against
   the Phase 1 BQ25619 deliverable, do **not** silently move it into app_logic.

---

## DEPENDENCY NOTES

- `data_broker` REQUIRES (post-port): freertos, esp_timer, pcf85063, drv2605,
  max_m10s, lis3mdl, veml6030, lsm6dsv16x, bq25619, bme688, max30101, alarm,
  fusion. (`alarm` and `fusion` are forward references — define empty stub
  broker slots if needed to break the cycle; both modules ultimately #include
  `data_broker.h` themselves and the broker only needs their `broker_xxx_data_t`
  typedefs, which sit in `alarm.h` / `fusion.h`.)
- `cross_driver` REQUIRES: data_broker, freertos, esp_common, log.
- `app_logic` REQUIRES: driver, nvs_flash, data_broker, lvgl_ui (for `ui_settings_t`
  in `ui_broker.h`).
- `fusion` REQUIRES: data_broker, cross_driver, freertos, esp_timer, log.
- `alarm` REQUIRES: data_broker, drv2605, pcf85063, app_logic, lvgl, esp_lvgl_port,
  lvgl_ui, freertos, esp_timer, nvs_flash, and **ws2812** (new, if Option B above).
- `lvgl_ui` REQUIRES: lvgl, esp_lvgl_port, data_broker, app_logic, veml6030,
  pcf85063, max_m10s, lis3mdl, drv2605, lsm6dsv16x, bme688, max30101, alarm,
  qvar_ecg (if ECG tile lands), esp_timer, driver, nvs_flash. PRIV_REQUIRES:
  spi_flash, boot_logic.

---

## REMEMBER

- Reference `02_Kompic_Mk1_System_Instructions_v7.2.md` whenever a chip ↔ GPIO ↔ bus
  question comes up. No guessing.
- The broker contract is **non-negotiable** — single mutex, memcpy only inside the
  lock, no I/O under the lock, status derived on read. Do not "optimise" it.
- Cross-driver callbacks run on Core 0 only. Adding events does not relax this rule.
- NVS writes do not happen from Core 1 / LVGL context. Anything UI-driven goes
  through the `ui_settings_save_q`.
- For each module, the .md file is the record. Profile or note TBD; do not omit.
- Test harnesses are standalone and reproducible.
- Live in the .md. All work is visible.

After Phase 6 lands, the next step is the **datasheet pass**: collect the official
PDFs for every v7.2 chip and align the per-chip register/bit/timing constants in the
porting docs against the manufacturer's word. Any function the datasheet exposes
that the firmware does not use becomes an Opportunity entry — captured per chip,
prioritised for v2.

Go.
