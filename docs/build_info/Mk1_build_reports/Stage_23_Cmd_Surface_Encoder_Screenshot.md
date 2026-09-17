# Stage 23 -- Command-surface migration + encoder nav + LVGL_SCREENSHOT

**Date opened:** 2026-09-08
**Board:** iv7.1 (Mk1 bench, display dead). Mk1b PCB still in fab.
**Firmware baseline:** `iv7.1.f0.4.24` -> target `0.4.25+`.
**Status:** OPEN.

## 1. Scope

Three Tier-1 items agreed at end of Stage 22. Full context in
`Stage_22_LVGL_Binding.md` and `Module_Blueprint.md`.

### 1.1 §2 -- Command-surface migration pass 1 + `driver/i2c_master.h` migration

Bundled per venue. Group-work-by-venue means one pass through each
component directory lands the command-surface extract AND kills the
boot-time `i2c: This driver is an old driver, please migrate...`
warning at the same time.

Target modules in bring-up order:

1. **`bq25619`** (charger, smallest surface) -- extract
   `bq25619_cmd.{c,h}` with `enable_set/get`, `batt_test_set/get`,
   `ship_mode`, `dump`, `status_summary`. Refold `BATT_TEST` +
   `SHIPMODE` CLI blocks into thin wrappers. Rewrite `battery_tile.c`
   handlers to call the same. Migrate I2C.
2. **`veml6030`** (ambient/backlight) -- extract `veml6030_cmd.{c,h}`
   covering enable, auto-brightness, brightness, blue-light-filter,
   dump. Add new CLI verb `LIGHT`. Migrate I2C.
3. **`pcf85063`** (RTC) -- extract `pcf85063_cmd.{c,h}` with
   time_get/set, dump_regs, sync_from_gps, status_summary. Refold
   `GET_TIME` / `SET_TIME` / `RTC_DUMP`. Migrate I2C.
4. **`drv2605`** (haptic) -- extract `drv2605_cmd.{c,h}` with enable,
   play_effect(n), calibrate, sweep_start/stop, dump. New CLI verb
   `HAPTIC`. Migrate I2C.

Per Module_Blueprint §7, each migration updates HELP + STATUS +
`nvs_cfg_boot_print` where the module owns NVS keys.

### 1.2 §3 -- Encoder → LVGL tileview nav

Encoder driver in `components/encoder/` is already polled-detent-rest
per `feedback_encoder_polling.md`.

- New `components/lvgl_ui/lvgl_ui_encoder.{c,h}` mirroring
  `lvgl_ui_display.c` structure.
- `lv_indev_create()` with `LV_INDEV_TYPE_ENCODER`; read cb pulls from
  the encoder event queue.
- `lv_group_t` covering the settings tileview; encoder indev drives it.
  Rotate steps tileview columns.
- Encoder click routes via new `tile_desc_t.on_click` field (nullable
  fn ptr). LVGL press event finds active tile via `tile_registry_get()`
  + tileview index, invokes `desc->on_click()`.
- **First consumer:** `gps_tile_desc.on_click = gps_tile_cmd_view_toggle`
  -- Ivan explicitly wants encoder click on the GPS tile to swap
  normal ↔ photo view without touching the screen.
- CLI verb `ENC <rotate n|click>` fakes events into the LVGL indev
  queue for headless verification.

### 1.3 §4 -- LVGL_SCREENSHOT (PNG)

- `lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB888)` -> PSRAM byte buffer.
- Encode as PNG. Options: hand-rolled minimal PNG (miniz for deflate)
  or `stb_image_write` header-only (~10 KB). Decide at implementation
  time.
- Write to `/sd/lvgl_fb/s<seq>_f<counter>.png` via new
  `sdcard_open_binary(dir, name, out_path)` helper mirroring
  `csv_open`.
- CLI verb `LVGL_SCREENSHOT [full|tile]` -- `full` = whole screen,
  `tile` = current tile only.
- Size sanity: 466×466×3 = 651 KB uncompressed, PNG ~200-300 KB.
  PSRAM handles it.

## 2. Order of work

Flash-per-checkpoint cadence per `feedback_flash_per_checkpoint.md`.

1. §1.1 pilot -- `bq25619` command-surface + i2c_master. Verify
   `BATT_TEST` / `SHIPMODE` + battery tile behave identically.
2. §1.1 repeat -- veml6030, pcf85063, drv2605. One batch each, or
   bundled if they compile without cross-touching (they don't share
   files).
3. §1.2 encoder indev + `ENC` CLI verb (rotate first).
4. §1.2 encoder click + `tile_desc_t.on_click` + GPS toggle wiring.
5. §1.3 `sdcard_open_binary` + PNG writer + `LVGL_SCREENSHOT full`.
6. §1.3 `LVGL_SCREENSHOT tile` variant + cleanup pass.

**Cleanup, opportunistic batch:** delete `components/qvar_ecg/`,
`ecg_tile.{c,h}`, tile_registry ECG entry. NO ECG on Kompic per
`feedback_no_ecg_on_kompic.md`.

## 3. Testing strategy

- All new CLI verbs (`BQ`, `LIGHT`, `HAPTIC`, `RTC` extensions, `ENC`,
  `LVGL_SCREENSHOT`) reachable + provide state readout under
  `LVGL_FORCE ON` on iv7.1. Bench criterion.
- Command-surface migrations are refactors -- existing verbs must
  round-trip identically to Stage 22 behaviour. No STATUS field values
  change.
- `LVGL_SCREENSHOT` under `LVGL_FORCE ON` produces a PNG on SD that
  opens on the laptop -- first bench visual verification of tile
  layouts pre-Mk1b.
- Grow `Screen_Test_Checklist.md` with encoder + screenshot rows.

## 4. Tier 2 -- battery drain baseline (Ivan-run, parallel)

Setup:

```
1. Charge to full (green LED / STATUS soc = 100).
2. LVGL_FORCE OFF   (keeps display path skipped even on Mk1b).
3. BATT_TEST ON      (persists; parks sensors, enters batt_test next boot).
4. REBOOT            (writes /sd/data/battery/batt_<seq>.csv every 10 s).
5. Unplug USB.
6. Leave overnight until BATFET cuts.
7. Plug USB back in. VBUS-detected boot auto-skips batt_test for
   recovery. `BATT_TEST OFF` if you want normal mode back.
8. FS_LS /sd/data/battery/          -> find CSV.
9. FS_CAT </sd/path>  (or plug SD into laptop).
```

## 5. Results

### 5.1 Batch 1a -- bq25619_cmd + drv2605_cmd (fw 0.4.25)

Two new command surfaces landed, no driver internals touched. Deprecation
warning `i2c: This driver is an old driver...` still present (killed in
Batch 1b). Ready for flash + verdict.

**New files:**
- `components/bq25619/bq25619_cmd.{c,h}` -- surface: enable/boost/ship_mode/
  dump/status_summary.
- `components/drv2605/drv2605_cmd.{c,h}` -- thin veneer over existing
  `haptic.h`. Surface: enable/play_effect/calibrate/sweep_start/stop/
  ui_effect_get/set/dump/status_summary.

**Modified:**
- `components/field_capture/fc_cli.c` -- new `BQ` + `HAPTIC` verb blocks,
  HELP lines added. `BATT_TEST` + `SHIPMODE` untouched (still route through
  nvs_cfg / watcher_ship_mode -- semantically fc-owned, not charger-owned).
- `components/drv2605/haptic_tile.c` -- `cb_power` swapped from
  `broker_haptic_set_enabled()` (blueprint anti-pattern) to
  `drv2605_cmd_enable_set()`.
- `components/bq25619/CMakeLists.txt` -- added `bq25619_cmd.c`, marked
  `WHOLE_ARCHIVE` (nothing intra-archive references bq_cmd_*, so linker
  needs the force-include to expose the new surface to fc_cli).
- `components/drv2605/CMakeLists.txt` -- added `drv2605_cmd.c`
  (haptic_tile intra-archive reference already pulls the object in).

**New CLI verbs (bench-test recipe):**

```
BQ                            -> status_summary + full reg dump
BQ EN [ON|OFF]                -> charger enable via REG_POC.CHG_CONFIG
BQ BOOST [ON|OFF]             -> PMID 5V (mutually exclusive with charge)
BQ SHIPMODE                   -> alias for existing SHIPMODE
HAPTIC                        -> status_summary + reg dump
HAPTIC EN [ON|OFF]            -> haptic feedback master switch
HAPTIC PLAY <1..123>          -> enqueue effect (respects EN gate)
HAPTIC CAL                    -> DRV auto-cal (diagnostic; fails on Taptic)
HAPTIC SWEEP START|STOP       -> LRA period sweep + latch step
HAPTIC UI [<1..123>]          -> get/set NVS-persisted UI feedback effect
```

**Batt_test placement decision:** stays in `nvs_cfg` / `fc_cli` (see
§1 note). Will migrate when field_capture gets its own cmd-surface.

**Verdict criteria for Batch 1a:**
- Deprecation warning still on boot (expected).
- `BQ` prints reg dump + status summary. `BQ EN OFF` disables charging
  (BATFET stays gated by VBUS so device keeps running while plugged in).
- `HAPTIC PLAY 14` fires the LRA. `HAPTIC EN OFF` silences UI feedback.
- `SHIPMODE` + `BATT_TEST` still work identically to fw 0.4.24.
- Battery tile renders identically.

**Batch 1a verdict (2026-09-09):** ✓ verified on bench. `BQ EN OFF`
drops charge current visibly; `HAPTIC EN OFF` silences LRA; `HAPTIC
EN ON` re-arms; `SHIPMODE` still fires `watcher_ship_mode` + BATFET
drop unchanged. Deprecation warning still on boot (expected -- Batch
1b target). Ivan-noted follow-ups:
- Command name polish (later).
- CLI submenu restructure -- see [[project_cli_submenus]] (post §1.1).
- WHOLE_ARCHIVE gotcha captured in
  [[feedback_whole_archive_cmd_extract]] for future extracts.

### 5.2 Batch 1b -- I2C_NUM_1 to driver/i2c_master.h
_(TBD)_

### 5.3 Batch 2 -- veml6030_cmd + LIGHT verb (fw 0.4.26)

Cmd-surface extract only; I2C migration deferred to its own stage (see
Next steps). Battery pilot (§5.8) exposed that the deprecation warning
covers all 10 drivers on both buses; migrating just I2C_NUM_1 wouldn't
kill it, so scope was moved out of Stage 23.

**New files:**
- `components/veml6030/veml6030_cmd.{c,h}` -- surface: enable, auto/manual
  brightness, blue-light filter, dump, status_summary.

**Modified:**
- `components/veml6030/light_tile.c` -- five event handlers rewrapped
  through cmd surface (kills the "tile writes broker + globals + save
  directly" anti-pattern for the LIGHT column). Theme dark/light stays
  tile-local (belongs to a future ui_settings_cmd).
- `components/veml6030/CMakeLists.txt` -- `veml6030_cmd.c` added; no
  WHOLE_ARCHIVE needed since light_tile.c pulls the cmd object in.
- `components/field_capture/fc_cli.c` -- new `LIGHT` verb block + HELP.
- Firmware version bump `0.4.25 -> 0.4.26`.

**New CLI verbs:**
```
LIGHT                       -> status_summary + reg dump
LIGHT EN [ON|OFF]           -> sensor on/off (broker enable)
LIGHT AUTO [ON|OFF]         -> auto-brightness (lux LUT -> backlight)
LIGHT BR [<1..100>]         -> manual brightness (ignored while AUTO on)
LIGHT BLUE [ON|OFF]         -> amber blue-light-filter overlay
```

**Verdict criteria for Batch 2:**
- `LIGHT` dumps VEML6030 regs + gain/it/lux/count factor.
- `LIGHT AUTO OFF` + `LIGHT BR 30` visibly dims backlight; `BR 100`
  brightens. `LIGHT AUTO ON` restarts lux-driven behaviour.
- `LIGHT BLUE ON` flips amber overlay (only visible on Mk1b with panel;
  bench-invisible on iv7.1 headless).
- Persisted values survive reboot (`AUTO`, `BR`, `BLUE` all live in
  ui_settings via async NVS queue).
- Light tile controls still work identically when running LVGL_FORCE.

**Bug found + fixed after Batch 2 verify (fw 0.4.27):** `AUTO` toggle
saved to NVS but didn't restore on reboot because `lvgl_ui_init()`
applied `theme`, `blue_light`, and `brightness` from the loaded
`ui_settings_t` but silently skipped `auto_brightness`. Pre-dated
Stage 23; only surfaced now because Ivan's Batch 2 recipe tested
persistence explicitly. One-line fix in
`components/lvgl_ui/lvgl_ui.c` (`g_auto_brightness = cfg->auto_brightness`).

### 5.4 Batch 3 -- pcf85063_cmd + datetime filenames (fw 0.4.28)

Bundled RTC cmd surface with the datetime-filename scheme (same venue
per group-work-by-venue): the RTC-derived timestamp helper is what
lets the SD writers switch away from boot_seq numbering.

**New files:**
- `components/pcf85063/pcf85063_cmd.{c,h}` -- surface: time_get/set,
  sync_utc, iso_now (replaces old rtc_iso_now), filename_stamp, dump,
  status_summary.

**Modified:**
- `components/pcf85063/CMakeLists.txt` -- adds `pcf85063_cmd.c` + marks
  `WHOLE_ARCHIVE` (rtc_tile is read-only, no intra-archive caller;
  same gotcha as bq25619).
- `components/field_capture/fc_common.c` -- `rtc_iso_now()` now delegates
  to `pcf85063_cmd_iso_now()`; new `fc_dated_path(dir, suffix, out, n)`
  helper builds `kompic_YYYY-MM-DD_HH-MM-SS_<suffix>.csv` with
  `<suffix>_boot<NNNN>.csv` fallback if RTC not synced at open time.
- `components/field_capture/fc_battery_test.c` -- both writer sites
  (`batt_path`, `bb_path`) now use `fc_dated_path()`. Kills the
  boot_seq-derived filename gaps Ivan complained about (Stage 23 §5.9).
- `components/field_capture/fc_cli.c` -- `SET_TIME` routes through
  `pcf85063_cmd_sync_utc()`; `RTC_DUMP` routes through
  `pcf85063_cmd_dump()`; new module-scoped `RTC` verb (summary + dump);
  local `rtc_cli_dump_pcf_regs` deleted (moved into cmd surface with
  named registers + OS-bit warning preserved). HELP updated.
- `components/field_capture/fc_internal.h` -- exports `fc_dated_path`.
- Firmware version bump `0.4.27 -> 0.4.28`.

**Scope decision -- recording filenames untouched:**
Recording writers (env, mot, qvar, ppgbcg, bcg, steps, mlc_train) use
`s%04lu_r%04lu` scheme (session=boot_seq + record counter). The
session grouping is intentional -- all recordings from one boot share
a prefix. Migrating them to datetime is a separate call: it changes
grouping semantics. Left as-is; add as a follow-up if Ivan wants.

**New CLI verb:**
```
RTC             -> status_summary + reg dump (aggregator).
                   Existing GET_TIME/SET_TIME/RTC_DUMP verbs unchanged
                   (they now route through pcf85063_cmd_ internally).
```

**Filename scheme:**
```
/sd/data/battery/kompic_2026-09-10_18-30-45_batt.csv        # RTC valid
/sd/data/blackbox/kompic_2026-09-10_18-30-45_blackbox.csv   # RTC valid
/sd/data/battery/batt_boot0077.csv                          # fallback
/sd/data/blackbox/blackbox_boot0077.csv                     # fallback
```

**Verdict criteria for Batch 3:**
- `RTC` prints summary + full named reg dump + oscstop warning if OS bit.
- `SET_TIME 2026-09-10T18:30:00` + `GET_TIME` still round-trip identically.
- `BATT_TEST ON` + `REBOOT` produces `kompic_<datetime>_batt.csv` (no
  more boot_seq gaps).
- `BLACKBOX ON` + `REBOOT` produces `kompic_<datetime>_blackbox.csv`.
- Existing GET_TIME/SET_TIME/RTC_DUMP output unchanged (they call cmd
  surface but formatting is preserved).

### 5.5 Encoder LVGL indev
_(Deferred + redesigned per [[project_encoder_tile_redesign]]:
encoder becomes its own device+tile with a "test-ground" layout
(rotation speed, click count, button responses). Swipes stay for
tile-to-tile nav; encoder is on-tile interaction only. Not in Stage 23
scope any more.)_

### 5.6 Batch 4 -- LVGL_SCREENSHOT (fw 0.4.29)

Streaming uncompressed-PNG encoder writes the active LVGL screen to
`/sd/lvgl_fb/kompic_<datetime>_screenshot.png`. Runs under
`LVGL_FORCE ON` even when the panel is absent (snapshot renders via
the SW draw pipeline, doesn't touch the flush cb). First bench visual
verification tool for the pre-Mk1b window.

**New files:**
- `components/lvgl_ui/lvgl_ui_screenshot.{c,h}` -- snapshot capture +
  PNG encoder. ~250 lines. Zero external deps (deflate stored blocks,
  hand-rolled CRC32 + adler32).

**Modified:**
- `sdkconfig` + `sdkconfig.defaults` -- `CONFIG_LV_USE_SNAPSHOT=y`
  (LVGL's snapshot API is Kconfig-gated in the managed-component
  build; setting only lv_conf.h isn't enough).
- `main/lv_conf.h` -- `LV_USE_SNAPSHOT 1` for consistency (belt +
  suspenders).
- `components/lvgl_ui/CMakeLists.txt` -- adds `lvgl_ui_screenshot.c`,
  adds `sdcard` to REQUIRES, marks `WHOLE_ARCHIVE` (nothing intra-
  archive references `lvgl_ui_screenshot_write`; same gotcha as
  bq25619 + pcf85063).
- `components/field_capture/fc_cli.c` -- `LVGL_SCREENSHOT` verb block
  + HELP entry.
- Firmware version bump `0.4.28 -> 0.4.29`. Flash: +~2.4 KB.

**Encoder design:** one deflate "stored" block per scanline, so no
back-patching needed for block headers. IDAT length is computed
up-front from `h * (5 + 1 + w*3)`. CRC32 (chunks) + adler32 (zlib
stream) streamed byte-by-byte. Output PNG is ~640 KB for 466x466
RGB888 -- fine for bench use, wasteful for production. If shipping
this becomes a real feature later, swap in miniz for real compression.

**Filename scheme:**
```
/sd/lvgl_fb/kompic_2026-09-10_18-30-45_screenshot.png   # RTC valid
/sd/lvgl_fb/screenshot_up<uptime_ms>.png                # fallback
```

**New CLI verb:**
```
LVGL_SCREENSHOT   -> lv_snapshot_take(lv_scr_act(), RGB888) -> PNG on SD.
                     Requires LVGL up (LVGL_FORCE ON if no panel).
                     Mounts SD if needed, unmounts on exit.
```

**Verdict criteria for Batch 4:**
- `LVGL_FORCE ON` + reboot + `LVGL_SCREENSHOT` writes a file and
  echoes the path.
- Pull SD -> file opens on laptop as a valid PNG.
- With no panel present (headless bench), the PNG still contains real
  rendered pixels (the LVGL SW renderer draws into the snapshot
  buffer regardless of flush).
- `TILE 3` (or any tile switch) followed by `LVGL_SCREENSHOT`
  produces a different image showing that tile.

**Bug found + fixed after first Batch 4 flash (fw 0.4.30):**
`lv_snapshot_take()` returned NULL under `LVGL_FORCE ON` because it
allocates the pixel buffer via LVGL's internal heap (LV_MEM_SIZE =
64 KB, per `lv_conf.h`). A 466x466 RGB888 snapshot is ~650 KB and
won't fit. Fix: allocate the buffer in PSRAM via `heap_caps_malloc`
and drive `lv_snapshot_take_to_draw_buf()` with a caller-supplied
`lv_draw_buf_t` pointing at it. Same recipe LVGL's own deprecated
`lv_snapshot_take_to_buf` uses internally. See
[[feedback-lvgl-heap-too-small-for-pixels]].

**Second bug found + fixed after 0.4.30 flash (fw 0.4.31):** stack
overflow in `task_rtccli` (3 KB stack) whenever `LVGL_SCREENSHOT`
runs -- the SW draw pipeline
(`lv_snapshot_take_to_draw_buf` -> label rendering, glyph
rasterisation) eats several KB of stack. Bumped `task_rtccli` to
8 KB in `boot_tasks.c`. Same crash regardless of `LVGL_FORCE` state
because both entry paths call the snapshot function inline.

**Batch 4 verdict (2026-09-10):** ✓ verified on bench. Screenshots
land at `/sd/lvgl_fb/kompic_<datetime>_screenshot.png` and open on
laptop. Take ~5 s each (SW render + SDMMC 1-bit write, roughly equal
contributors).

**Known limitations of the bench flow (not bugs, follow-ups for
Stage 24+):**
- All screenshots capture the main screen because there's no touch
  input under `LVGL_FORCE ON` to navigate to the settings tileview.
  `TILE`, `GPS_VIEW`, `LIGHT BLUE` all mutate state on non-active
  screens/tiles. Fix: add `SCREEN [main|settings]` verb, or make
  `TILE` implicitly switch screens.
- Screen dimensions in firmware are 466x466 but the panel is
  actually 410x502 landscape. Hardcoded portrait orientation.
  Fix: `SCREEN ROTATE` verb + configurable orientation.
- File size ~640 KB (uncompressed PNG). Real deflate via miniz would
  drop it to ~50-200 KB and halve total time. JPEG would be smaller
  still but needs an encoder library. Not urgent.

### 5.7 ECG cleanup
_(Not landed in Stage 23. Deferred to Stage 24 opportunistic bundle
when tile_registry gets touched for Mk1b bring-up.)_

### 5.8 Battery drain baseline (fw 0.4.24, batt_0124.csv)

**Run:** 2026-09-07T01:06 → 03:47 UTC (Ivan-copied + renamed
`batt_0070.csv` -> `batt_0124.csv` for chronological ordering).

**Result:** unchanged from Stage 17 baseline. No fw revision since
has moved the needle.

| metric                | value       |
|-----------------------|-------------|
| discharge duration    | ~2 h 41 m   |
| vbat start / end      | 4206 → 2556 mV |
| delivered (@106 mA)   | ~284 mAh    |
| SoC temp mean         | ~36 °C      |
| idle_pct              | 99 % nominal |

Aggregate for fw 0.4.24 across n=4 discharge sessions: mean 149.6 min,
mean 264 mAh delivered. Nominally-400 mAh cell measures as ~300 mAh
effective under 100 mA load down to 2.8 V UVLO -- so drain math =
~106 mA average → ~2h50m theoretical, we're hitting it.

**PM lock inventory (pre-BATT_TEST boot, uptime 14 s):**

| lock             | type          | active | time% |
|------------------|---------------|--------|-------|
| `sdmmc`          | APB_FREQ_MAX  | 1      | 79 %  |
| `rtos1`          | CPU_FREQ_MAX  | 1      | 9 %   |
| `rtos0`          | CPU_FREQ_MAX  | 0      | 15 %  |

Modes: `SLEEP` 18 %, `APB_MAX` 58 %, `CPU_MAX` 23 %.

**Under BATT_TEST (uptime 42 s):** `batt_test_vbus NO_LIGHT_SLEEP`
holds 93 % -- expected, we pin serial alive while VBUS is present.
SDMMC lock correctly cycles per sample (mount-write-unmount). Real
overnight run without VBUS will release both.

**Culprit hierarchy for Stage 24 investigation:**

1. **`sdmmc APB_FREQ_MAX` permanent hold** during normal operation
   (79 %). Should only be held around mount/unmount, not persistently.
   Suspect: mount API is holding without matching release in the
   normal (non-batt-test) code path. Check
   `components/sdcard/*` mount/unmount balance.
2. **rtos1 CPU_FREQ_MAX** — LVGL refresh task on Core 1 keeps CPU
   pinned. Options: (a) drop LVGL refresh cadence, (b) yield with
   longer vTaskDelay between frames, (c) put LVGL task on Core 0 and
   let ADC/telemetry drive Core 1 idle windows.
3. **rtos0 CPU_MAX 15 %** — collateral, chase after (1) + (2).

### 5.9 Regressions / follow-ups from Ivan's Sept-9 bench session

- **RGB WS2812 not lighting up on iv7.1.** Ivan can't tell what menu
  he's on. Boot log shows `RGB` CLI clears manual override + firmware
  animations "resume" -- so firmware thinks it's driving the LED, but
  no light output. **Diagnose:** (a) probe WS2812 data pin with scope
  for the 1-wire waveform; (b) `RGB 255 0 0` manual full-red should
  latch immediately; (c) is WS2812 shared with a display-cascade pin
  that got isolated post-display-dead diagnosis? Cross-check
  `Master_Pinout.md`. Not blocking Stage 23 §1 (all CLI-verifiable).
  Add to Stage 24 diagnostic queue.

- **Filename convention: switch to date-time.** Ivan wants all
  SD-written data files to be
  `device_YYYY-MM-DD_HH-MM-SS_<suffix>.csv` from now on. Boot_seq
  naming produces "gaps" that look like data loss (batt_0070 landed
  after a bunch of non-batt-test boots). Files affected:
  `batt_<seq>.csv`, `blackbox_<seq>.csv`, any other `csv_open`-based
  writers. Implement in `components/sdcard/csv_open.c` -- pull RTC via
  `pcf_read_utc()`, format `<device>_YYYY-MM-DD_HH-MM-SS_<name>.csv`.
  Fallback to `<name>_boot<seq>.csv` if RTC not yet synced at
  open-time. **Land as Stage 23 §1.4** (bundle it into the pcf85063
  cmd-surface pass -- same venue for RTC access).

## 6. Stage 23 close-out (2026-09-10)

**Landed (all bench-verified on iv7.1):**
- §1.1 -- four cmd surfaces: `bq25619_cmd`, `drv2605_cmd`,
  `veml6030_cmd`, `pcf85063_cmd`. Plus BQ / HAPTIC / LIGHT / RTC
  CLI verbs.
- §1.3 -- `LVGL_SCREENSHOT` verb, PNG output on SD.
- Datetime filenames for batt / blackbox writers.
- Auto-brightness NVS restore bug fixed (pre-existing, surfaced
  during §1.1 verify).

**Deferred at close:**
- §1.2 encoder LVGL nav -- redesigned per
  [[project-encoder-tile-redesign]] (encoder gets its own tile),
  moves to Stage 24+.
- I2C_NUM_0/1 migration to `driver/i2c_master.h` -- dedicated stage
  (all 10 drivers together).
- ECG cleanup -- opportunistic during Stage 24 tile_registry work.
- Recording writers datetime migration -- opportunistic per module
  ([[feedback-recording-datetime-opportunistic]]).

**Follow-ups tracked for Stage 24:**
1. Mk1b PCB assembly + first-boot (highest priority -- board arrived
   2026-09-10).
2. RGB WS2812 hardware diagnosis (dark at boot on iv7.1; may also
   affect Mk1b -- verify during bring-up).
3. HELP verb output sorted alphabetically (crowded list; auto-sort
   at print time).
4. `SCREEN [main|settings|ROTATE]` verb (screenshot always shows
   main screen; panel is landscape 410x502 mounted portrait).
5. Dual-time feature (local + CET via GPS location LUT, toggleable).
6. Time-zone GPS sync (currently `SET_TIME` just persists UTC; the
   +1 CET offset should be applied at display time, not stored).
7. SDMMC PM lock chase (§5.8 -- 79% APB_FREQ_MAX hold).
8. PNG compression (miniz deflate) to shrink screenshots + shorten
   capture time.
9. Encoder tile + cmd surface (per redesign memory).

Next stage: `Mk1b_build_reports/Stage_24_Mk1b_Bringup.md`.

## References

- `Stage_22_LVGL_Binding.md`
- `Module_Blueprint.md`
- `Screen_Test_Checklist.md`
- `reference_files/IV71_TO_MK1B_FIRMWARE_DELTA.md`
