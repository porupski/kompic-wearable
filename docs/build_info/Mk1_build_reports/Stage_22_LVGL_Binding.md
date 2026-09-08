# Stage 22 -- LVGL port wiring + GPS photo view (bench-verifiable path)

**Date opened:** 2026-09-08
**Date closed:** 2026-09-08 (same day; large batch)
**Board:** iv7.1 (Mk1 bench, display dead). Mk1b PCB still in fab.
**Firmware baseline:** `iv7.1.f0.4.20` -> shipped at `0.4.24`.
**Status:** CLOSED — 4 batches landed (§4.1a+§4.5a, §4.1b, §4.3+§4.2, §4.6). §4.5c LVGL_SCREENSHOT deferred; encoder → tileview nav deferred; command-surface migration of the other 9 tiles deferred (Stage 22b).

---

## 0. Methodology notes (codified during Stage 22)

Four rules that emerged mid-stage and now govern all future work here.
The first three are behavioural; the fourth is a structural contract with
its own reference doc.

### 0.1 Working rules

Three rules that emerged mid-stage:

- **CLI-first testability.** Every driver, tile, and feature must be
  reachable through a `fc_cli.c` verb. Bench work is headless by default
  (iv7.1 has no working screen, Mk1b in fab, GPS module offline) — waiting
  for hardware to validate code is not acceptable. New broker accessor →
  new verb; new NVS flag → new verb; new tile → reachable via `TILE <name>`
  under `LVGL_FORCE ON`.
- **Group work by venue.** When touching a file / component / subsystem,
  bundle any other pending same-surface work into the same pass. Don't
  revisit `fc_cli.c` three times over three stages when one visit covers
  three verbs; don't split the `driver/i2c.h` migration across sensors
  when the same sensor also needs a new broker accessor and a new CLI
  verb. Bundle by nature, not opportunistically — see
  `feedback_group_work_by_venue.md`.
- **Flash-per-checkpoint cadence.** Ivan flashes + monitors after every
  sizable code batch, even when the "next" batch is trivially safe. Land
  work in flash-sized increments; each batch should be independently
  CLI-observable so `flash → monitor → verb → verdict` closes the loop.
  Don't chain three substeps into one push.

### 0.2 Self-contained module blueprint (new reference doc)

`docs/build_info/Module_Blueprint.md` — created 2026-09-08
mid-Stage 22. Codifies the architectural rule that emerged from Ivan's
observation "screen and CLI should be the same thing but in two different
formats":

- Each module exports a **command surface** `<module>_cmd.{c,h}` covering
  `enable`, `config`, `action`, `dump`, `status` categories.
- CLI verbs are thin parse-forward-print wrappers.
- Tile event handlers are thin LVGL-event → command-call wrappers.
- Same code path from typing the verb and tapping the button.
- Future outlets (preset button, BLE, scripted rigs) attach to the same
  surface with zero changes to the module core.
- Migration is opportunistic per `feedback_group_work_by_venue.md`; new
  modules ship compliant from day 1.

Priority migration targets (Stage 22b or later): `bq25619`, `veml6030`,
`pcf85063`, `drv2605`. Existing tile code (health, gps, env, imu,
compass, alarm, system, ecg, haptic, light) predates this blueprint and
still couples tile handlers to broker + NVS + driver directly — works
today, drifts eventually.

---

## 1. Why this stage exists

Stage 21 §4.1a landed the CO5300 QSPI driver, the CST9217 presence probe, and
`boot_display_is_present()`. The `lvgl_ui/` component tree turned out to be
already-built-out (25 files, 10 registered tiles, full screen / navigation /
overlay / theme fan-out / 200 ms refresh task); it just is not wired into
boot. The previous session's Stage 22 handoff described that tree as an
empty stub -- it is not. This plan is written against the codebase as it
actually is (audited 2026-09-08).

The Mk1b screen is not on the bench yet, so every deliverable in this
stage lands behind `boot_display_is_present() || nvs_cfg_sys_get_lvgl_force_on()`.
`iv7.1` default boot still returns headless. A `LVGL_FORCE ON` NVS flag
brings the LVGL side up in memory-only mode so we can bench-test the
tileview, tile updates, GPS photo view, and CLI hooks without a panel.

---

## 2. Codebase state (audit, 2026-09-08)

### 2.1 What already exists (verified present, no work needed)

| Piece                                              | File                                                       | Notes                                    |
|:---------------------------------------------------|:-----------------------------------------------------------|:-----------------------------------------|
| CO5300 QSPI panel driver                           | `components/co5300/co5300.c` (390 LOC)                     | `co5300_set_window`, `co5300_write_pixels`, brightness -- all real |
| CST9217 touch driver + task + queue                | `components/cst9217/cst9217.c` (263 LOC)                   | `cst9217_init`, `task_touch_fn`, `g_touch_q` (depth 1, `xQueueOverwrite`) |
| Presence probe + backlight facade                  | `components/boot_logic/boot_display.{c,h}`                 | `boot_display_is_present()` returns false on iv7.1 (CST9217 NAK) |
| LVGL UI orchestrator                               | `components/lvgl_ui/lvgl_ui.c` (239 LOC)                   | `lvgl_ui_init()` + `task_ui_refresh_fn` fully coded, never called |
| Screen framework                                   | `ui_main_screen.c`, `ui_settings_screen.c`, `ui_lock_screen.c`, `ui_shutdown_overlay.c`, `ui_notif_overlay.c`, `ui_navigation.c`, `ui_status_bar.c` | All present, all real |
| Tile registry                                      | `components/lvgl_ui/tile_registry.c` (81 LOC)              | 10 tiles registered, columns already assigned |
| Live tiles (9 of 10)                               | `health_tile.c`, `haptic_tile.c`, `light_tile.c`, `system_tile.c`, `gps_tile.c`, `rtc_tile.c`, `env_tile.c`, `compass_tile.c`, `imu_tile.c` | Real init + update + apply_theme + broker reads |
| GPS "raw NMEA" subtile                             | `components/max_m10s/gps_tile.c` (487 LOC)                 | Subtile fully coded, reads `max_m10s_get_debug_sentences()` |
| CLI dispatch pattern (20 verbs today)              | `components/field_capture/fc_cli.c`                        | `startswith_ci()` chain; easy insertion |
| NVS config pattern                                 | `components/nvs_cfg/nvs_cfg.{c,h}`                         | `nvs_cfg_sys_get/set_batt_test` template to copy for `lvgl_force` |
| `esp_lvgl_port` managed component                  | `managed_components/espressif__esp_lvgl_port/`             | Present. Only needs adding to `main REQUIRES` |

### 2.2 What is missing (the actual Stage 22 work)

- **Zero calls** to `lvgl_port_init`, `lvgl_port_add_disp`, `lvgl_port_add_touch`, `lv_display_create` in firmware source. (Only `lvgl_port_lock` is used, inside `task_ui_refresh_fn`.)
- **`esp_lvgl_port` not in `main/CMakeLists.txt REQUIRES`** (line 13, deliberate comment).
- **`lvgl_ui_init()` never called** -- TODO slot at `main.c:112-114`.
- **`task_ui_refresh_fn` and `task_touch_fn` externs commented out** at `boot_tasks.c:60-61` with note "display path down".
- **No CO5300 flush callback** -- the LCD driver has no `esp_lcd_panel_t` vtable; the flush cb has to call `co5300_set_window` + `co5300_write_pixels` directly, and needs access to the `co5300_handle_t` currently static in `boot_display.c`.
- **`cst9217_init(I2C_NUM_0)` never called** -- lives inside `cst9217.c` but has no boot-time caller yet.
- **No LVGL indev** registered against `g_touch_q`.
- **No NVS `lvgl_force_on` flag** for bench-forcing LVGL up without a panel.
- **No CLI verbs** for `LVGL_FORCE`, `TILE`, or `LVGL_SCREENSHOT`.
- **GPS photo view unwritten** -- only the NMEA debug subtile exists (Stage 18 §5.2b spec: black bg, huge white monospace lat/lon; encoder click toggles under-the-hood ↔ photo).

### 2.3 Known open questions to resolve during implementation

- **Draw buffer size + placement** — starting point: `LCD_H_RES * 40 * 3 = 55 KB` in PSRAM (partial refresh, single buffer). Confirm SRAM headroom vs PSRAM DMA latency during §4.1b.
- **LVGL display creation path** -- `lvgl_port_add_disp()` wants an `esp_lcd_panel_handle_t`. Two options:
  - (A) Wrap CO5300 in an `esp_lcd_panel_t` vtable (idiomatic, ~80 LOC of glue).
  - (B) `lvgl_port_init()` for the task/lock only, then `lv_display_create()` + `lv_display_set_flush_cb()` directly (bypasses `add_disp`, ~30 LOC).
  Decision made during §4.1b implementation; log the choice in §7.
- **Encoder → tile nav** -- `ui_navigation.c` (144 LOC) currently handles swipes. Whether it also handles encoder rotate/click over the tileview needs verifying during §4.3; add if missing.

### 2.4 Reference material to read first

- `docs/build_info/Mk1_build_reports/Stage_21_Screen_Bringup_Prep.md`
  §4 (what needs building) and §5 (Mk1b arrival bring-up sequence).
- `docs/build_info/Mk1_build_reports/Stage_18_Log_Audit_PCF_Tile_Spec.md`
  §5 (tile mapping spec: home tile + GPS two-mode view).
- `docs/build_info/reference_files/IV71_TO_MK1B_FIRMWARE_DELTA.md`
  §3, §5 (LVGL enable + tile registry sections).
- `components/lvgl_ui/tile_registry.{c,h}` -- descriptor schema (init/update/apply_theme/has_subtile/main_dirs).
- `firmware/arduino/15_amoled_touch_test/15_amoled_touch_test.ino` -- touch report parsing reference.

---

## 3. Working goal

Same discipline as Stage 21 §4.1a: one image, two behaviours. The image
brings the LVGL port up, registers the flush + indev callbacks, calls
`lvgl_ui_init()`, and starts `task_ui_refresh_fn` + `task_touch_fn`. On
iv7.1 the whole LVGL side is skipped unless `LVGL_FORCE ON` is set in
NVS. On Mk1b after reflow-A the same image lights up the panel and
opens the tileview with all 10 tiles available.

The Mk1b operator's first-boot experience: home (System) tile appears
by default; encoder rotate scrolls left/right through 10 tiles; encoder
click on the GPS tile toggles between under-the-hood and photo view;
touch works for the settings screen slider.

---

## 4. Plan

### 4.1a NVS `lvgl_force_on` flag

- Add `nvs_cfg_sys_get_lvgl_force_on()` / `nvs_cfg_sys_set_lvgl_force_on(bool)` to `components/nvs_cfg/nvs_cfg.{c,h}`, u8 in namespace `cfg_sys`, key `lvgl_force`. Copy the `batt_test` template exactly.
- Default: `false` (auto: honour `boot_display_is_present()`).
- No CLI wiring yet -- that lands in §4.5.

Acceptance: `nvs_cfg_boot_print(I2C_NUM_0)` at boot now prints the flag alongside the other cfg values.

### 4.1b LVGL port + CO5300 flush

- Uncomment `esp_lvgl_port` in `main/CMakeLists.txt REQUIRES`.
- Expose the CO5300 handle from `boot_display.c`: add `co5300_handle_t boot_display_get_co5300(void)` (returns `NULL` if panel absent). Or pass the handle through a new `boot_display_lvgl_start()` entry point.
- After `boot_display_init()` (or after the NVS force check) at `main.c:112-114`:
  - Allocate a partial-refresh buffer in PSRAM: `heap_caps_aligned_alloc(4, LCD_H_RES * 40 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)`.
  - `lvgl_port_init(&lvgl_cfg)` on Core 1, task priority 4, stack 6 KB.
  - Choose display path A or B (see §2.3), register the flush callback that:
    - Calls `co5300_set_window(x1, y1, x2, y2)`.
    - Calls `co5300_write_pixels(pixels, count)`.
    - When forced (`LVGL_FORCE ON` with no real panel), the CO5300 handle is `NULL`; the flush cb early-returns and calls `lv_display_flush_ready()` so LVGL keeps running.
- Extend `STATUS` (`fc_cli.c:174` block) with a new line: `lvgl = up (buf=Nkb)` / `off` / `forced (no panel)`.

Acceptance: iv7.1 default boot unchanged (no LVGL, boot log clean). `LVGL_FORCE ON` + reboot: STATUS shows `lvgl = forced (no panel)`, task list shows the LVGL port task, no crashes.

### 4.2 CST9217 init + LVGL indev

- Inside `boot_display_init()` after `co5300_init()` OK (or inside a new `boot_display_lvgl_start()` after `lvgl_port_init()`), call `cst9217_init(I2C_NUM_0)`. Log warning on failure but do not fail the display bring-up; touch is optional.
- Uncomment `task_touch_fn` extern at `boot_tasks.c:61` and add to `task_table[]` on Core 0 (I2C0 mutex contention), stack 3 KB, priority 4.
- Register LVGL indev via `lvgl_port_add_touch()` (if we take path A) or `lv_indev_create()` + `lv_indev_set_type(LV_INDEV_TYPE_POINTER)` + read callback (path B). Read cb: `xQueueReceive(g_touch_q, &pt, 0)`; return `pt.x / pt.y / LV_INDEV_STATE_PR` if `pt.fingers > 0`, else last-known-pos + `LV_INDEV_STATE_REL`.
- Gestures not needed Day-1 (encoder handles nav). Ignore `pt.gesture`.
- Under `LVGL_FORCE ON` with no real touch chip, `cst9217_init()` fails cleanly; the indev never fires but LVGL keeps running.

Acceptance: Mk1b bench (when it lands): tap on settings screen slider actually moves it. iv7.1 with `LVGL_FORCE ON`: LVGL up, touch queue empty (no chip), indev returns `RELEASED` forever, no crash.

### 4.3 lvgl_ui_init() + refresh task + encoder nav

- Uncomment `task_ui_refresh_fn` extern at `boot_tasks.c:60` and add to `task_table[]` on Core 1, stack 6 KB, priority 3. Guard the entry so it only starts when LVGL is up (either display present or forced).
- Call `lvgl_ui_init(&ui_cfg)` at `main.c:112-114` inside `lvgl_port_lock(portMAX_DELAY)` / `lvgl_port_unlock()`, immediately after LVGL port init.
- Registry stays as-is (all 10 tiles). Do not trim.
- Read `ui_navigation.c` to verify encoder rotate → `lv_tileview_set_tile` / encoder click → tile-scoped action. If missing, add: wire the existing encoder task (via `broker_encoder_*` events or the same queue field_capture uses) to a callback that adjusts the current tileview column.
- After first successful build, confirm each of the 10 tiles' `update()` runs at least once via a log line.

Acceptance: `LVGL_FORCE ON` + reboot: `LVGL_UI: Initial tile status update complete (10 tiles)` appears in the boot log, `task_ui_refresh_fn` present in FreeRTOS task list, no watchdog trips.

### 4.5 CLI verbs (`LVGL_FORCE`, `TILE`, optionally `LVGL_SCREENSHOT`)

- Insert three `if (startswith_ci(line, "..."))` blocks in the `fc_cli.c` dispatcher.
- `LVGL_FORCE <on|off|auto>`: parse arg, write via `nvs_cfg_sys_set_lvgl_force_on()`, print current value. Takes effect on next boot.
- `TILE <name>`: look up tile by symbolic name (system, gps, rtc, ...), call `lv_tileview_set_tile(tv, entry->handle, LV_ANIM_ON)`. Guard on `boot_display_is_present() || forced`.
- `LVGL_SCREENSHOT` (optional this stage): dump the raw draw buffer (or capture via `lv_snapshot_take(main_scr, LV_COLOR_FORMAT_RGB888)`) into `/sd/lvgl_fb/s<seq>.rgb`. Use the `csv_open()` session pattern (session rotation, mkdir-on-create). ~651 KB per shot; only useful for verifying tile layout without a panel.
- Add each verb to the `HELP` printout at `fc_cli.c:126`.

Acceptance: `HELP` lists the new verbs, `LVGL_FORCE ON` persists across reboot, `TILE gps` jumps to column 3.

### 4.6 GPS photo view

- Add a `s_photo_view` flag in `gps_tile.c` (module-static bool).
- Second layout inside `gps_tile_init()` -- create a hidden container (`lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN)`) at black bg with two large monospace labels (lat, lon) plus a small time+date row above. Use `LV_FONT_MONO_36` or the largest available; grow FA/mono asset if needed (already have `fa5_select_14.c` -- may need a bigger mono variant; check LVGL built-ins first).
- `gps_tile_update()` -- when `s_photo_view == true`, populate the photo labels with `broker_gps_read()` result, hide the under-the-hood widgets, show the photo widgets. Toggle back on encoder click.
- Encoder click handler -- register a per-tile click callback via `tile_registry.h` (if the descriptor has no click hook, add one). On click while GPS tile is active, `s_photo_view = !s_photo_view; gps_tile_update();`.
- 1 Hz refresh (the existing 200 ms refresh task already covers this by calling `gps_tile_desc.update`); no separate timer.
- Testable without Mk1b: `LVGL_FORCE ON` + `TILE gps` + encoder click via CLI `GESTURE` verb (if it stubs a click) or a temporary `TILE gps photo` variant.

Acceptance: bench with `LVGL_FORCE ON`, jump to gps tile, log line confirms photo-view toggled; on Mk1b, physical encoder click swaps the display.

---

## 5. Order of work

Each step ends in `idf.py build` clean on iv7.1 and a flash + boot log
verification. Bench criterion: `LVGL_FORCE OFF` boot regresses nothing
(sensors, CLI, shutdown, charging all identical to fw 0.4.20); once
`LVGL_FORCE ON` is set, LVGL side comes up in memory and stays there
across reboot without crashing.

1. **§4.1a** NVS `lvgl_force_on` flag. Compile-only check; `NVS_PRINT` shows the new key.
2. **§4.5 partial** -- Land `LVGL_FORCE` CLI verb now (so we can flip the flag before §4.1b even exists). `TILE` and `LVGL_SCREENSHOT` come later.
3. **§4.1b** LVGL port + flush stub. First LVGL-on-force reboot. STATUS shows `lvgl = forced (no panel)`. Nothing regresses on iv7.1 default.
4. **§4.3** `lvgl_ui_init()` + refresh task + encoder nav. Boot log confirms all 10 tiles run their `update()` at least once.
5. **§4.5 remainder** -- Land `TILE` verb, then `LVGL_SCREENSHOT` if time permits.
6. **§4.6** GPS photo view. Toggle-verifiable via CLI on the bench without Mk1b.
7. **§4.2** CST9217 init + LVGL indev. Compile + boot clean on iv7.1; only real-world verify happens on Mk1b.

Ivan explicitly said "we got tokens to burn" -- push through as much of 1-7
as fits. Everything sits behind the LVGL gate, iv7.1 default boot stays
boring throughout.

---

## 6. Testing strategy without Mk1b hardware

- **iv7.1 default boot** (`LVGL_FORCE OFF`) -- CST9217 probe NAKs, `boot_display_is_present()` returns false, LVGL side skipped entirely. Bench continues to test recording modes, CLI, shutdown, charging.
- **iv7.1 with `LVGL_FORCE ON`** -- LVGL port up, flush cb is a no-op (CO5300 handle NULL), indev never fires. Tile transitions, event handlers, GPS photo layout all exercise. `LVGL_SCREENSHOT` (if built) captures the rendered buffer to SD; open the .rgb dump on a laptop to verify layout.
- **Mk1b after reflow-A** -- probe succeeds, `boot_display_is_present()` true, full LVGL + panel + touch. No firmware change from the iv7.1 image; `LVGL_FORCE OFF` (default) is the shipping mode.

---

## 7. Results

### 7.0 Outcome summary

| Substep | Outcome  | Bench-verified                                       |
|:--------|:---------|:-----------------------------------------------------|
| §4.1a NVS `lvgl_force_on`           | landed | `NVS_PRINT` shows the flag |
| §4.5a `LVGL_FORCE` CLI verb         | landed | flag round-trips, persists across reboot |
| §4.1b LVGL port + CO5300 flush      | landed | iv7.1 default = headless, LVGL_FORCE ON = up in forced mode, STATUS shows `lvgl = forced (no panel)  buf=54 KB` |
| §4.3 `lvgl_ui_init` + refresh task  | landed | 10-tile registry lives, task_ui_refresh on Core 1, no watchdog trips |
| §4.5b `TILE` CLI verb               | landed | `TILE list` + `TILE gps` + `TILE 4` all round-trip |
| §4.2 CST9217 init + LVGL indev      | landed (compile-clean only) | iv7.1 has no touch to actually verify; Mk1b will validate |
| §4.6 GPS photo view + `GPS_VIEW`    | landed | `GPS_VIEW PHOTO / NORMAL / TOGGLE` all round-trip |
| §4.5c `LVGL_SCREENSHOT`             | **deferred** | own batch — needs SD-write + `lv_snapshot_take` |
| Encoder → tileview nav              | **deferred** | own batch — Stage 22b or later |

Firmware version bumped `0.4.20` → `0.4.24`. Binary size grew 890 KB → 1.24 MB (all 10 LVGL tiles now reachable) → 1.28 MB (final, after photo view + Montserrat_30). Still 59 % partition free.

Ivan flashed after every batch; no regressions on the iv7.1 default-headless path across the whole stage.

### 7.1 §4.1a NVS `lvgl_force_on` flag

Landed. `components/nvs_cfg/nvs_cfg.{c,h}`:
- Added key `K_SYS_LVGL_FORCE = "lvgl_force"` (u8, ns `cfg_sys`).
- Added `nvs_cfg_sys_get_lvgl_force_on()` / `nvs_cfg_sys_set_lvgl_force_on()` copying the `batt_test` template exactly.
- Added a line to `nvs_cfg_boot_print()` so boot printout shows the current flag alongside `batt_test`, `blackbox`, `rec_audio`.
- `NVS_CFG_DRIVER_VERSION` bumped `0.3.1` → `0.3.2`.

Default is `false` — iv7.1 default boot unchanged. No CLI writer wired yet (that lands in §4.5); flag is settable only from other code paths until then.

### 7.2 §4.5a LVGL_FORCE CLI verb

Landed. `components/field_capture/fc_cli.c`:
- New `LVGL_FORCE [ON|OFF]` dispatch block (after `BATT_TEST`); no-arg prints current state.
- Added to HELP printout and to STATUS's `NVS:` line so a `STATUS` call shows the flag.

TILE + LVGL_SCREENSHOT verbs held for §4.5b — they need `lvgl_ui_init` up first.

### 7.3 §4.1b LVGL port + CO5300 flush

Landed. Six files touched, one new module.

- New `components/lvgl_ui/lvgl_ui_display.{c,h}` exports `lvgl_ui_display_setup(bool force_no_panel)`, `lvgl_ui_display_is_up()`, `lvgl_ui_display_is_forced()`, `lvgl_ui_display_buf_bytes()`.
  - Calls `lvgl_port_init` (task Core -1, prio 4, stack 7 KB, timer 5 ms).
  - Allocates a 55 920 B (466 × 40 × 3) partial-refresh buffer via `heap_caps_aligned_alloc(4, ..., MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`.
  - `lv_display_create(466, 466)` + `lv_display_set_color_format(LV_COLOR_FORMAT_RGB888)` + partial-refresh buffer + flush callback.
  - Flush cb: on real panel, calls `co5300_set_window` then `co5300_write_pixels`; under force mode (CO5300 handle NULL) it just calls `lv_display_flush_ready` so LVGL keeps advancing.
- `components/boot_logic/boot_display.{c,h}`: new `boot_display_get_co5300()` accessor so the flush cb can reach the driver handle.
- `components/co5300/co5300.c`: bumped `CO5300_MAX_TRANSFER_BYTES` from `4096 + 8` to `64 * 1024` so the SPI DMA descriptor pool covers the 55 KB strip. `CO5300_DRIVER_VERSION` → `0.2.1`.
- `main/CMakeLists.txt`: added `lvgl_ui` to `REQUIRES` (transitively pulls `esp_lvgl_port`); dropped the "intentionally omitted" comment.
- `main.c`: after `boot_display_init()`, block `6b` calls `lvgl_ui_display_setup(!present && forced)` when the panel is present OR when `nvs_cfg_sys_get_lvgl_force_on()` is set. Force mode reported explicitly in the log.
- `components/field_capture/fc_cli.c`: STATUS gained a `lvgl = up (CO5300)|forced (no panel)|off  buf=NN KB` line.
- `components/field_capture/firmware_version.h`: `KOMPIC_FW_VERSION` → `0.4.21`.

**Build**: clean, `smartwatch.bin = 0xd94f0` bytes, 72 % partition free. Not flashed yet — needs a bench run to observe the STATUS line under `LVGL_FORCE ON` and to confirm LVGL boot log lines appear.

**Known open items** for a later step:
- Byte order (`LV_COLOR_FORMAT_RGB888` in-memory layout vs CO5300 wire order) is unverified — no panel on the bench. If Mk1b bring-up shows swapped red/blue, add a byte-swap loop in `lvgl_flush_cb` before the pixel write.
- Buffer size is a starting-point guess (40 rows). Confirm no fragmentation issues once we have panel + tile activity together.

### 7.4 §4.3 lvgl_ui_init + refresh task + encoder nav

Landed. Wired the fully-built `lvgl_ui_init()` into boot and spawned the refresh + settings-saver tasks.

- `components/lvgl_ui/lvgl_ui_display.{c,h}` grew two entry points:
  - `lvgl_ui_display_boot_screens()` — loads `ui_settings_t` from NVS (`app_nvs_load_ui_settings`, defaults on first boot), calls `ui_broker_init()` to create the save queue, then wraps `lvgl_ui_init(&cfg)` in `lvgl_port_lock`. Applies the persisted backlight brightness on the real panel.
  - `lvgl_ui_display_start_tasks()` — spawns `task_ui_refresh_fn` (Core 1, prio 3, 6 KB) and `task_settings_saver_fn` (unpinned, prio 2, 3 KB). No-op when the LVGL side isn't up.
- `main.c` — after `lvgl_ui_display_setup()` succeeds, calls `boot_screens()` then `start_tasks()`.
- **`components/max_m10s/max_m10s.c`** — added the missing `volatile bool g_gps_sync_requested = false;` definition. `gps_tile.c` had an `extern` reference (ATOMIC SYNC button) that was never satisfied; the linker previously dead-stripped the tile code, so this only surfaced now that `lvgl_ui_init` iterates the tile registry and drags every tile in as reachable. GPS task doesn't consume the flag yet (module offline on iv7.1); wiring lands whenever the GPS driver goes back online.
- `components/boot_logic/boot_tasks.c` — kept the LVGL task extern block out of boot_logic to avoid a `boot_logic` ↔ `lvgl_ui` component-dep cycle (lvgl_ui already `PRIV_REQUIRES boot_logic`). Task creation lives in `lvgl_ui_display_start_tasks()` instead.

**Encoder → tileview nav**: `ui_navigation.c` currently registers touch-swipe gesture callbacks only; there is no encoder-driven tileview navigation. Left as-is for now — touch works on Mk1b via the CST9217 indev (§4.2). Adding encoder rotate/click will land alongside the GPS photo view (§4.6) since the click-to-toggle spec directly requires it.

**Build**: clean. `smartwatch.bin = 0x1304d0` bytes (1.24 MB, 60 % partition free) — expected jump from 890 KB now that all 10 tiles + refresh task are reachable code.

### 7.5 §4.5b TILE verb (LVGL_SCREENSHOT deferred)

Landed. New `TILE list | <n> | <name>` CLI verb.

- `components/lvgl_ui/lvgl_ui_display.{c,h}`: exports `lvgl_ui_display_tile_count()` + `lvgl_ui_display_jump_tile(int col)` — port-locked wrapper around `lv_tileview_set_tile()` reading from `tile_registry_get()` / `settings_screen_get_tileview()`.
- `components/field_capture/fc_cli.c`: `TILE` dispatch block, accepts:
  - `TILE list` (or no arg) — dumps `col N  <name>` for all 10 tiles.
  - `TILE <n>` — jumps to column n.
  - `TILE <name>` — case-insensitive name lookup against hardcoded `TILE_NAMES[]` (health / haptic / light / system / gps / rtc / env / compass / imu / ecg). Table lives in `fc_cli.c` rather than on `tile_desc_t` to avoid touching all 10 widget files this pass — natural bundle target the next time `tile_desc_t` gets extended.
- HELP printout includes the verb.
- Guarded with `lvgl_ui_display_is_up()` — prints a friendly hint pointing to `LVGL_FORCE ON` when LVGL is off.

**LVGL_SCREENSHOT deferred** to its own batch (needs SD-write plumbing + `lv_snapshot_take` or draw-buffer capture; higher complexity than TILE, lower Day-1 value).

**Build**: clean, +928 bytes → `smartwatch.bin = 0x130870`. `KOMPIC_FW_VERSION` → `0.4.22`.

**Bench criterion (headless via LVGL_FORCE):** `LVGL_FORCE ON` → reboot → `TILE list` (see all 10) → `TILE gps` (log line confirms jump) → `TILE 3` (jump to system).

### 7.6 §4.6 GPS photo view

Landed. Photo view built as a second layout inside the same tile parent. Toggle via in-tile PHOTO button or `GPS_VIEW` CLI verb -- same code path, per the new Module_Blueprint.

- `components/max_m10s/gps_tile.h`: added `gps_tile_view_t` enum + first pass of the command surface (`gps_tile_cmd_view_get`, `gps_tile_cmd_view_set`, `gps_tile_cmd_view_toggle`). This is the pilot use of the pattern documented in `docs/build_info/Module_Blueprint.md`.
- `components/max_m10s/gps_tile.c`:
  - Photo container built hidden at init: `s_photo` (LV_OPA_COVER black), `s_photo_time` (Montserrat_16 header row), `s_photo_lat` + `s_photo_lon` (Montserrat_30, largest available today — proportional; true-monospace font is a Stage 25 upgrade), `s_photo_alt` (Montserrat_14), `s_photo_hint` ("tap to exit").
  - Photo container is `LV_OBJ_FLAG_CLICKABLE`; tap anywhere fires `cb_photo_container_tap` → `gps_tile_cmd_view_set(NORMAL)`.
  - New "PHOTO" button in normal view (bottom-left, tiny, Montserrat_10) → `cb_photo_btn` → `gps_tile_cmd_view_set(PHOTO)`.
  - `set_normal_widgets_hidden()` helper toggles the entire normal-view widget set at once when switching modes.
  - `gps_tile_update()` extended: when photo view is active, also formats and pushes `d.utc_*` + `d.latitude/longitude` (5 decimals: `46.05110° N`) + altitude + sat count into the photo labels. Same broker snapshot as the normal view — no extra broker read.
- `components/field_capture/fc_cli.c`:
  - New `GPS_VIEW [normal|photo|toggle]` verb. No-arg / STATUS / GET all print the current state.
  - Takes `lvgl_port_lock` before calling the setter (in-tile callbacks fire inside the LVGL task which already holds the mutex — CLI runs on a different task so it must acquire).
  - Gated on `lvgl_ui_display_is_up()`.
  - HELP printout includes the verb.
- `components/field_capture/firmware_version.h`: `KOMPIC_FW_VERSION` → `0.4.24`.

**Build**: clean, +43 KB → `smartwatch.bin = 0x13c7f0` (59 % partition free). ~40 KB of that is the `lv_font_montserrat_30` glyph table being pulled in as reachable code.

**Bench criterion (headless via LVGL_FORCE)**:
- `LVGL_FORCE ON` → REBOOT → `GPS_VIEW` prints `view = NORMAL`.
- `GPS_VIEW PHOTO` → `[GPS_TILE] view → PHOTO` log line + `view = PHOTO`.
- `GPS_VIEW TOGGLE` → alternates.
- `GPS_VIEW NORMAL` → back to telemetry.
- `TILE gps` still jumps to the tile regardless of view mode.

**Known limits (deferred, not blockers):**
- Montserrat_30 is proportional; decimal alignment across lat/lon relies on right-margin, not true monospace. Enabling a real mono font (`lv_font_unscii_16` or an imported JetBrainsMono) is a Stage 25 candidate.
- Encoder click → toggle wiring is not there yet (encoder → tileview nav is itself deferred). Touch-tap and CLI are the two current outlets.
- No touch-hardware verification possible on iv7.1; Mk1b will validate the tap paths.

### 7.7 §4.2 CST9217 init + LVGL indev + TOUCH verb

Landed. Full touch path from ISR → queue → LVGL indev, plus a headless bench debug verb.

- `components/boot_logic/boot_display.{c,h}`:
  - New `s_touch_present` flag + `boot_display_touch_is_present()` accessor.
  - `boot_display_init()`: after `co5300_init()` succeeds, calls `cst9217_init(I2C_NUM_0)`. On failure, logs a warning and continues with `s_touch_present=false` -- panel bring-up is not blocked by a bad touch chip. On iv7.1 the presence probe NAKs earlier so we never reach this call; under `LVGL_FORCE ON` the touch stays absent because the probe still fails.
- `components/lvgl_ui/lvgl_ui_display.{c,h}`:
  - New `lvgl_touch_read_cb()` — non-blocking `xQueuePeek(g_touch_q)`, returns `PRESSED` + point when `fingers > 0`, else `RELEASED` at last-known coords (avoids the LVGL cursor teleporting to 0,0 between touches).
  - `setup()`: after the display is created, gated on `boot_display_touch_is_present()`, does `lv_indev_create()` + `lv_indev_set_type(LV_INDEV_TYPE_POINTER)` + `lv_indev_set_display(s_lv_disp)` + `lv_indev_set_read_cb(lvgl_touch_read_cb)`.
  - `start_tasks()`: spawns `task_touch_fn` on Core 0 (prio 4, 3 KB stack) when touch present. I2C0 mutex locality with the other sensor tasks.
  - New `lvgl_ui_display_touch_indev_ready()` + `lvgl_ui_display_touch_snapshot(x, y, events, pressed)` for the TOUCH verb.
- `components/field_capture/fc_cli.c`:
  - New `TOUCH` CLI verb -- prints indev status, last-seen coords, monotonic PRESSED event count from the read cb, current queue state (PRESSED/released). Guarded on `boot_display_touch_is_present()` with a hint pointing at iv7.1 vs Mk1b.
  - HELP printout includes it.
  - STATUS line updated: `display = ...  touch = CST9217 up | off` combined on one row.
- `components/field_capture/firmware_version.h`: `KOMPIC_FW_VERSION` → `0.4.23`.

**Build**: clean, +5 200 bytes → `smartwatch.bin = 0x131ac0`.

**Bench criterion (iv7.1)**: default boot still headless -- `STATUS` shows `display = absent (headless)  touch = off`, `TOUCH` prints the "CST9217 absent" hint. `LVGL_FORCE ON` + reboot: STATUS still shows `touch = off` (probe NAKs regardless of force flag -- correct behaviour, force is a display-side override only), `TOUCH` still prints the hint. Nothing hangs or crashes on the indev-less LVGL path.

**Mk1b path (untested until PCB is back)**: probe ACKs → `s_touch_present=true` → task spawns, indev binds, taps register in `TOUCH` output.

---

## 8. Next steps

_(fresh instance fills at wrap)_

Reasonable Stage 23 candidates once Stage 22 closes:

- **Stage 22b -- Command-surface migration pass 1 + i2c_master migration.**
  Extract `bq25619_cmd.{c,h}`, `veml6030_cmd.{c,h}`, `pcf85063_cmd.{c,h}`,
  `drv2605_cmd.{c,h}` per `docs/build_info/Module_Blueprint.md`.
  Bundle with the `driver/i2c.h` → `driver/i2c_master.h` migration for the
  same modules (group work by venue). Kills the deprecation warning and
  aligns those four modules with the two-outlet contract in one pass.
- Stage 23 -- Mk1b bring-up when the PCB returns.
- Stage 24 -- Boot log drop root-cause + fix (`esp_log_set_vprintf`, buffer sizing, task priorities).
- Stage 25 -- Day-2 tile polish (ECG waveform actual, Phase 20 broker_ecg_* definitions). Migrate remaining tile modules (health / env / imu / compass / alarm / system / ecg / haptic / gps) to command surface.
- **`LVGL_SCREENSHOT` (§4.5c)** — deferred from Stage 22 §4.5b. Needs SD-write plumbing + `lv_snapshot_take`.
- Battery drain overnight measurement at fw 0.4.20 baseline before Stage 23 (comparable curve).

---

## 9. References

- `docs/build_info/Mk1_build_reports/Stage_21_Screen_Bringup_Prep.md`
  -- CO5300 SPI plumbing + touch-probe rationale + §4.1b hooks.
- `docs/build_info/Mk1_build_reports/Stage_18_Log_Audit_PCF_Tile_Spec.md`
  §5 -- tile spec (home + GPS two-mode).
- `docs/build_info/reference_files/IV71_TO_MK1B_FIRMWARE_DELTA.md`
  §3, §5 -- LVGL enable + tile registry seed sections.
- `docs/Kompic_Mk1_User_Manual.md` -- current UI manual; add tile
  navigation section once LVGL lands.
- `firmware/esp-idf/components/co5300/co5300.{c,h}` -- pixel + window primitives ready for the flush callback.
- `firmware/esp-idf/components/cst9217/cst9217.{c,h}` -- init + task + `g_touch_q` ready for indev binding.
- `firmware/esp-idf/components/lvgl_ui/tile_registry.{c,h}` -- registry API + descriptor schema (10 tiles already registered).
- `firmware/esp-idf/components/lvgl_ui/lvgl_ui.{c,h}` -- fully-built orchestrator waiting for its first caller.
- `firmware/esp-idf/components/max_m10s/gps_tile.c` -- under-the-hood view + NMEA debug subtile. Add photo view here.
- `firmware/arduino/15_amoled_touch_test/15_amoled_touch_test.ino` -- touch report parsing reference for §4.2.

**Archive baseline for rollback:**
`hardware/Reflow_info/reference_files/archives/kompic_snapshot_2026-09-08_1852_iv71-fw-0-4-18-pre-mk1b-baseline.7z`
