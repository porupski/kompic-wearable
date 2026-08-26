# Stage 17 — Firmware Polish + Display-Ready Prep (Mk1 bench, Mk1b in fab)

**Date:** 2026-08-26
**Board:** iv7.1 (Mk1 bench prototype). Mk1b PCB is in fabrication — screen work waits for it.
**Firmware baseline:** `iv7.1.f0.4.14` → this stage targets `0.4.15+`.
**Status:** OPEN.

---

## 1. Where we came from

- Stage 15 closed the ECG↔Kompic USB sync layer (`fc_sync.c`, NVS keys, `fc_sync_write_csv_headers()`, PING→PONG 407 µs min RTT).
- Stage 16 was the AMOLED receptacle-mirror post-mortem — display bring-up on iv7.1 is unrecoverable; Mk1b respin required.
- **Gap:** the combined PPG+BCG raw recording mode planned as the actual data-collection blocker never landed. `FCM_SKIN` today writes `millis,skin_c,bpm,finger,spo2` at 10 Hz — not the 200 Hz raw stream the Srceko-alignment protocol needs.

---

## 2. Session goals

**A. Unblock data collection.** Repurpose `FCM_SKIN` (already wakes MAX30101 + TMP117) into the combined PPG+BCG raw mode Stage 15 specified. Emit `pc_sync_*` header block, `t_local_us`/`t_pc_us` columns, interleaved `src=ppg|bcg` rows at 200 Hz. Ivan re-verifies `sync.py` end-to-end.

**B. Polish while Mk1b is en route.** Land the display-time infrastructure that we can build and test *now* against Mk1 without a screen, so Mk1b bring-up isn't blocked on it:

- Runtime `LOGLEVEL` CLI + quiet-on-battery default.
- `rgb_policy` layer — RGB stops being mode-owned, becomes broker/power-state-driven.
  - Mode-selection preview: LED shows the mode's colour for **~5 s** after selection, then goes dark unless wrist is up **and** user is interacting.
- Wrist-raise gesture wired from LSM6DSV16X 6D orientation → `g_imu_gesture` (already a broker stub). Feeds RGB policy now; feeds display wake once Mk1b lands.
- Spec (not code): mode→LVGL tile mapping for Mk1b bring-up.

**Not in scope:** display driver work (no panel to talk to), Mk1b hardware verification (not on bench yet).

---

## 3. Plan

### 3.1 Repurpose FCM_SKIN → FCM_PPG_BCG (combined raw mode)

Confirmed 2026-08-26: rename `FCM_SKIN` → `FCM_PPG_BCG` in place. Same enum ordinal (NVS boot-mode compat preserved). 7 call-sites to update:
- `field_capture.h:50` — enum entry.
- `fc_recording.c:192,223,253,268` — debug dump, needs_annot check, csv_open, csv_row.
- `fc_common.c:278` — mode dispatch.
- `nvs_cfg.h:121` — doc comment referencing skin mode by name.

Old `csv_row_skin()` gets replaced by the new PPG+BCG row writer; TMP117/HR aggregate at 1 Hz stays available via CLI status (not blocking for Mk1b).

Behaviour:
- 200 Hz tick loop (`fc_sync_get_step_us()`, default 5000 µs) — `vTaskDelayUntil` for cadence stability.
- Each tick reads LSM accel Z via `broker_imu_read()` and MAX30101 raw green. **Driver API check pending** — if today's `max30101.h` only exposes HR-filtered outputs, add a `max30101_read_raw_green()` accessor.
- Interleaved single-CSV rows: one per source per tick, `src=ppg` or `src=bcg`, so `pd.merge_asof(on=t_pc_us)` works without rename.
- CSV header via `fc_sync_write_csv_headers()` — this already emits `# pc_sync_applied=1|0`, offset, ref_iso, Kompic RTC, step_us, step_host_set. **Post-mortem question "did sync stick?" is answered by the header block.** No per-row `pc_synced` column (redundant / CSV bloat at 200 Hz).
- **Mode-start banner:** on entry, emit `[PPG_BCG] start: pc_sync=YES|NO offset_us=<n> step_us=<n>` so bench operator sees state at flash time.
- Column order (Stage 15 spec §CSV): `t_local_us,t_pc_us,t_ms,iso_utc,src,led_pa,raw,baseline,ac,bp,motion,beat,bpm_pk,bpm_ac,quality,stale`.
  - **Keep** `beat`, `bpm_pk`, `bpm_ac` — Ivan wants the real-time detector output alongside raw so we can eyeball detector quality on the same file (annotations, not gating).
- `t_local_us = esp_timer_get_time() - session_start_us`. `t_pc_us = t_local_us + fc_sync_get_offset_us()`.
- Stop on button click. No REC_DURATION timer this pass.

**Acceptance:** Kompic pushes rows to SD at 200 Hz, `sync.py` handshake still passes, CSV header includes the `pc_sync_*` block with `pc_sync_applied=1` when sync ran, mode-start banner prints correct offset, Srceko alignment via `pd.merge_asof(on=t_pc_us)` gives < 10 ms shift on a bench cross-check.

### 3.2 Runtime LOGLEVEL knob

- New CLI verb `LOGLEVEL <off|error|warn|info|debug|verbose>` in `fc_cli.c`.
- Boot-time default: `warn` if USB-Serial-JTAG not connected at boot, `info` if connected. (Detection via `usb_serial_jtag_ll_txfifo_writable()` heartbeat during first 200 ms, or `esp_reset_reason() == ESP_RST_USB` — pick simplest.)
- Persist last-set level in NVS (`sys:log_level`) so field debug sessions survive a reboot.

**Acceptance:** on-battery boot with no USB shows only `W`/`E` lines; plugging USB + `LOGLEVEL info` restores chatter.

### 3.3 `rgb_policy` component

New tiny component: `components/rgb_policy/`. Small state machine on Core 0, driven by 50 ms tick. Reads:
- `broker_battery_read()` → charging / charged / low state.
- `power_flags.h::g_display_sleep` → wrist-down proxy until 3.4 lands.
- `broker_imu_read()->gesture` → wrist raise/down (once 3.4 lands).
- Ivan-specified **mode-selection preview timer**: on `field_capture` mode change, `rgb_policy_show_mode_preview(mode)` starts a 5 s window during which the LED renders that mode's colour. After the window, LED goes off unless something else claims it.

Priority (high → low):
1. Alert (low batt, error) → red blink.
2. Notif pending → magenta blink.
3. Charging / charged → blue pulse / green steady.
4. Active recording (running mode) with wrist up → mode's own animation via `ws2812_set_color()` (unchanged escape hatch — `s_manual_override` already exists).
5. Mode-preview window (5 s post-selection, any wrist state) → mode colour.
6. Idle wrist-up → dim white.
7. Wrist-down → **off**.

Modes stop calling `ws2812_set_color()` directly for status hints; they call `rgb_policy_request_active_animation(mode)` and `rgb_policy_release()` instead. Legacy inline calls in `fc_modes.c` migrate opportunistically — don't force a big-bang refactor.

**Acceptance:** bench test with wrist-down proxy = LED goes off after 5 s of any mode selection; wrist-up = LED holds mode colour during active recording.

### 3.4 Wrist-raise gesture from LSM6DSV16X

LSM has hardware 6D orientation detection (registers `FUNC_CFG_ACCESS`, `WAKE_UP_THS`, `WAKE_UP_DUR`, `TAP_CFG*`, `SIXD_SRC`). Alternative: sample accel every ~100 ms and derive tilt in software.

Pick hardware where it works — lower CPU, wakes on interrupt. Feed decoded events to `broker_imu_data_t::gesture` = `IMU_GESTURE_WRIST_RAISE` / `IMU_GESTURE_WRIST_DOWN`. Publish `UI_EVENT_TAP_Z_SINGLE`-style events on the `g_ui_event_q` for future display-wake logic.

**Acceptance:** bench log confirms gesture transitions when Ivan tilts the board wrist-up / wrist-down.

### 3.5 Mode → LVGL tile mapping spec (Mk1b prep)

Text-only in this stage doc — do not touch `lvgl_ui/`. Deliverable is a table:

- Each current `FCM_*` mode → tile name + parent group + primary widgets.
- Which modes become full-screen (recording) vs. thumbnail tile (status).
- How encoder maps to tile navigation once display exists.

Feeds directly into Mk1b bring-up plan — Ivan flashes screen sketch first, then this spec becomes the LVGL work.

---

## 4. Results

### 4.1 FCM_PPG_BCG landed (fw 0.4.15)

Files touched:
- `field_capture.h` — `FCM_SKIN` → `FCM_PPG_BCG` (enum ordinal preserved), driver version → `0.3.2`.
- `firmware_version.h` — `KOMPIC_FW_VERSION` → `0.4.15`.
- `fc_internal.h` — removed `csv_row_skin()` decl, added `run_ppg_bcg_mode()` decl.
- `fc_common.c` — `MODE_INFO` name `"skin"` → `"ppgbcg"` (same magenta palette 26/0/12); `wake_sensors_for_mode()` case rewritten to enable IMU only (HR stays parked so run_ppg_bcg_mode can drive the MAX30101 directly).
- `fc_recording.c` — deleted `csv_row_skin()`, dropped `FCM_SKIN` from `debug_dump_mode`, `needs_annot`, `csv_open` switch, and row-writing switch. Generic recording orchestrator no longer knows about the slot.
- `field_capture.c` — added `else if (s_mode == FCM_PPG_BCG)` branch that calls `run_ppg_bcg_mode()` directly (bypasses the generic 100 ms recording loop).
- `nvs_cfg/nvs_cfg.h` — updated `nvs_cfg_sys_get_rec_audio()` doc comment (FCM_PPG_BCG has its own start banner, no voice-annot pre-roll).
- `fc_modes_ppg_bcg.c` — NEW, ~250 lines.
- `field_capture/CMakeLists.txt` — added `fc_modes_ppg_bcg.c`.

`run_ppg_bcg_mode()` behaviour as landed:
- Wakes IMU broker only. Explicit `broker_hr_set_enabled(false)` to guarantee the HR task doesn't race for the MAX30101 FIFO.
- Grabs `g_i2c_mutex`, brings MAX30101 out of shutdown, configures MULTI_LED (Red=0x00, IR=0x00, Green=0x2A → ~8.4 mA), clears FIFO, releases mutex. Uses existing `max30101_setup_multi_led_mode()` — SR=100 Hz, SMP_AVE=8 → **~12.5 Hz effective PPG cadence in the FIFO**. First-pass values; tune after bench data.
- Opens CSV at `/sd/data/ppgbcg/s<boot>_r<rec>.csv`. Emits standard file header via `csv_open()`, then `fc_sync_write_csv_headers()` (the `# pc_sync_applied=…` block from Stage 15), then a `# ppg_led_pa_green=… bcg_hpf_alpha=…` config line.
- Column schema (deviates from Stage 15 spec — leaner for 200 Hz row cadence, keep beat annotations Ivan asked for): `t_local_us,t_pc_us,src,raw,filt,beat,bpm`. `src` = `bcg` or `ppg`. Empty `filt` cell for PPG rows.
- Mode-start `ESP_LOGI` banner: `[PPG_BCG] start: pc_sync=YES|NO offset_us=<n> step_us=<n> step_host_set=<0|1> pa_green=0x2A` — Ivan's "did the landing stick" check at flash time.
- Tick loop: `vTaskDelayUntil` at `fc_sync_get_step_us()` cadence (default 5 ms → 200 Hz). Every tick emits one BCG row (broker_imu_read, HPF+LPF+envelope+refractory+median-5 BPM — same algo as `run_bcg()`, inlined). Then opportunistically drains up to 8 MAX30101 FIFO samples inside a 0-timeout mutex take (skip tick if contended — no I2C blocking), emits one `src=ppg` row per drained sample with beat annotation from the built-in `max30101_check_for_beat()` helper.
- LED: 100 ms alternate magenta / off during recording. Bypasses `rgb_policy` for now (that layer is §3.3, not landed yet).
- Durability: `fflush() + fsync(fileno(csv))` every 1000 ms so a power-drop mid-session loses at most 1 s of rows (per `feedback_sd_write_durable`).
- Exit on button click. Cleanup: fclose CSV, MAX30101 back to shutdown, `park_all_modal_sensors()`, session summary log.

**Status:** compiles-clean on inspection; not yet flashed. Ivan reflashes next.

### 4.2 First bench-flash surfaces (2026-08-26)

Ivan flashed `0.4.15`, encoder-scrolled to `ppgbcg`, clicked to record.

**Observed (log excerpt):**
- `DRV2605: DRV2605 auto-cal FAIL in 50 ms (STATUS=0xEC)` — cal reports fast fail (DIAG bit set); playback path still works.
- No boot buzz — the LRA moved slightly during autocal but nothing definite.
- `csv fopen /sd/data/ppgbcg/s0008_r0001.csv failed: No such file or directory (errno=2)` — folder never got pre-created (ensure_sd() only makes the hardcoded classic set).
- Mode ran to completion but `rows=0` because the CSV never opened; serial telemetry showed BCG beats being detected on the LSM Z-axis anyway.
- Two log lines (`temperature_sensor: Range …` and `FC_BATT: Vbat ADC armed …`) appeared not at boot but on first STATUS invocation ~19 s in — lazy-init side effect.
- Long-press-to-ship sometimes registers as single click and starts a recording, sometimes fires shutdown but *after* the mode acted on the phantom click. Two-task race on the same GPIO with no coordination.

### 4.3 Follow-up fixes landed (still fw 0.4.15 → 0.4.16 pending)

**csv_open auto-mkdir** (`fc_recording.c`) — every `csv_open("<name>", …)` call now does `try_mkdir("/sd/data/<name>")` first. `EEXIST` is silently OK. Fixes `ppgbcg` and any future mode name. Documented as a general rule in memory [[new-module-folders]] for modes that bypass `csv_open()` and roll their own `fopen()` (e.g. `run_steps_mode()`).

**Boot buzz + DRV last** (`boot_hw_init.c`) —
- Reordered so `haptic_init()` is the very last step in `boot_hw_init`, after ws2812/flashlight/sd/mic and the sensor enable policy.
- After `haptic_init()` returns OK, queue `haptic_play_forced(DRV_STRONG_CLICK)` **twice**. The queue drains once the haptic task starts (during `boot_tasks_start()`), so the two clicks fire audibly separated. If the LRA moves at boot, everything upstream ran clean.
- No change to `drv2605_init()` itself — its 1500 ms poll window is already generous; the observed 50 ms exit is the DRV chip finishing fast with `DIAG=FAIL`, not a Claude-side timeout. Playback still works via INTTRIG.

**Long-press click swallow** (`fc_internal.h`, `fc_common.c`) —
- New `BTN_LONG_PRESS_MS = 1000` constant.
- `button_poll()` now tracks `s_btn_press_start_ms` on the transition to `BTN_PRESSED`. On release, if `held ≥ BTN_LONG_PRESS_MS` the state jumps to `BTN_IDLE` with no queued event. Kills the "queued click fires after shutdown" bug — the shutdown watcher owns any press ≥ 1 s.

**Shutdown ladder rework** (`fc_shutdown.c`) —
- New commit threshold `SHDN_COMMIT_MS = 1000`. `g_shutdown_hold_active` no longer flips on press-start; it flips only once the press crosses 1 s. Sub-1 s presses stay ambiguous → click. Once committed, LED countdown starts and the button_poll swallow (above) has already kicked in.
- Warn phase still 2000–3700 ms with 500 ms STRONG_CLICKs.
- **New warm-up burst 3700–3950 ms**: 3× `DRV_STRONG_CLICK` at 80 ms spacing so the LRA is definitely awake for the ship-time `DRV_LONG_BUZZ`. One-shot per press via `warm_up_fired` flag.
- FIRE at 4000 ms while held: unchanged (LONG_BUZZ + `watcher_ship_mode()`).
- **Release safety net**: if the user releases *after* the 4 s mark and FIRE somehow didn't run (task jitter), the release path fires ship unconditionally. Codifies Ivan's "REGARDLESS OF EVERYTHING" clause.

**Eager telemetry init** (`main.c`) — `vbat_adc_ensure_init()` + `esp_ts_ensure_init()` moved to run right after `boot_hw_init()`, so their `[FC_BATT]` and `[temperature_sensor]` log lines land in the correct boot-order slot instead of squirting out mid-CLI on the first STATUS.

**Archive workflow rule** (memory only) — codified: on stage-doc close, snapshot via `python docs/build_info/reference_files/make_archive.py <kebab-tag>` (e.g. `stage-17-ppgbcg`). Script already exists and excludes the heavy regeneratable stuff (build/, managed_components/, .git, datasheets/). Result goes to `hardware/Reflow_info/reference_files/archives/`. Rule lives in [[stage-log-workflow]] step 6.

### 4.4 Runtime LOGLEVEL knob (§3.2) — not started this batch.

### 4.5 `rgb_policy` skeleton (§3.3) — not started this batch.

### 4.6 Wrist-raise gesture (§3.4) — not started this batch.

### 4.7 Mode → tile mapping spec (§3.5) — not started this batch.

---

## 5. Next steps

**Second flash (0.4.16 pending version bump before you flash):**
1. Bump `KOMPIC_FW_VERSION` → `0.4.16` before build.
2. Boot: expect **two firm haptic clicks** during boot. If they fire, everything upstream ran clean.
3. On first `STATUS`: `[FC_BATT]` + `temperature_sensor` log lines should now appear during boot (before the CLI banner), not mid-STATUS.
4. Select `ppgbcg`, click to record: expect no `errno=2` this time; `[PPG_BCG] start:` banner + growing `rows=` counter.
5. Long-press to shutdown: hold past 1 s and release → **no phantom recording**. Hold past 4 s → LRA warm-up burst at 3.7 s, LONG_BUZZ + ship at 4 s.
6. If short-click still starts modes and long-press still ships → the two-task race is gone.

**Immediate (Ivan bench, original checklist):**
1. `idf.py build` — verify no compile regressions from the FCM_SKIN removal cascade.
2. `idf.py flash monitor` on the iv7.1 bench unit.
3. Encoder-scroll to the `ppgbcg` slot (was previously the pink `skin` slot — same position, same magenta colour).
4. Optional but recommended: run `python sync.py` first (both Kompic + Srceko plugged in, no serial monitors open on either port). Confirms the sync landing then feeds the mode-start banner.
5. Click to start recording. Expect `[PPG_BCG] start: pc_sync=YES offset_us=+…` banner in the serial log.
6. Put on wrist, quiet 1–2 min bench check. Click to stop.
7. Pull SD → inspect `/sd/data/ppgbcg/s<boot>_r<rec>.csv`:
   - Header block contains `# pc_sync_applied=1` if sync ran.
   - Row density: ~200 BCG rows/s + ~12 PPG rows/s.
   - Beat markers on BCG track sensible HR; PPG beats appear only after finger/wrist detection settles.

**If bench looks OK:**
- 5-minute protocol run with Srceko to validate `pd.merge_asof(on='t_pc_us')` alignment < 10 ms.
- Then move to §3.2 (LOGLEVEL) → §3.3 (rgb_policy) → §3.4 (wrist-raise) → §3.5 (tile spec).

**Known first-pass caveats to keep on the radar:**
- MAX30101 configured via existing `max30101_setup_multi_led_mode()` which forces SMP_AVE=8. That gives 12.5 Hz effective PPG in the FIFO. If Ivan wants raw 100 Hz PPG, we need a new `max30101_setup_ppg_raw_mode()` helper with SMP_AVE=1.
- `PPG_GREEN_PA=0x2A` is a first-pass guess. Bench feedback will drive the tune.
- The `iso_utc` column from Stage 15 spec is not emitted per-row (redundant with `# rtc_start=…` header + timestamps). Revisit if downstream tooling wants it.
- BCG timestamps use `millis_u32()` which wraps at ~71 min uptime — same limitation `run_bcg()` already carries. Not a problem for 5-min sessions.
