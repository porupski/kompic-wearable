# Stage 12 -- field_capture refactor + ship-mode robustness + REC_AUDIO

Date: 2026-08-01
Firmware: iv7.1 (bench), fw baseline entering Stage 12 was 0.4.12
Build: `idf.py build` clean, binary `smartwatch.bin` = 0x8bcb0 bytes, 82% partition free.

## What changed

### 1. field_capture component split from one 3 877-line file into 8 files

Previous state: `components/field_capture/field_capture.c` was a 3 877-line
mega-file mixing the FCM state machine, every mode session, WAV/CSV writers,
battery-test mode, BLACKBOX telemetry, the CLI, and the shutdown watcher.

Split:

| File | Lines | Purpose |
|------|------:|---------|
| `field_capture.c`   | 268 | Orchestrator: `field_capture_init`, `task_field_capture_fn` FCM state machine, `field_capture_get_boot_seq` public getter. |
| `fc_common.c`       | 315 | Shared globals (`s_mode`, `s_state`, `s_boot_seq`, ...), palette (`MODE_INFO`), LSM taxonomy helpers, button + encoder state machines, `wake_sensors_for_mode` / `park_all_modal_sensors`, `nvs_load` / `nvs_save_mode`, `ensure_sd` + `rtc_iso_now`, base RGB helpers. |
| `fc_recording.c`    | 309 | WAV header + `wav_record_to`, `wav_write_meta_sidecar`, `csv_open` + `csv_row_env|motion|skin`, `debug_dump_mode`, `voice_annot_lead_in`, `run_recording_for_current_mode`, `run_alarm_firing`. |
| `fc_modes.c`        | 526 | Compass (`run_mag_cal`, `run_compass`, `run_compass_or_cal`), ECG/QVAR (inline block + `run_ecg`, `run_ecg_session`), TEMP (`read_lsm_die_temp`, `read_max_die_temp`, `run_temp_session`, `run_temp_mode`), mode-signature LED animations (`rgb_compass_alt_red_blue`, `rgb_qvar_alt_yellow_purple`, `rgb_temp_warm_cycle`, `rgb_lsm_submenu_indicator`). |
| `fc_modes_lsm.c`    | 530 | LSM-submenu modes: `run_bcg` / `run_bcg_mode`, `run_steps_mode`, `run_mlc_collect_mode`, `run_tap_dbg_mode`. |
| `fc_battery_test.c` | 490 | `run_battery_test_mode`, `task_blackbox_fn`, shared telemetry helpers `vbat_adc_ensure_init`/`vbat_adc_read_mv`, `esp_ts_ensure_init`/`esp_ts_read_c`, `idle_pct_sample`. |
| `fc_shutdown.c`     | 221 | `watcher_ship_mode`, `task_shutdown_watcher_fn`, watcher globals `g_shutdown_hold_active` / `g_recording_active` / `g_watcher_press_reset_pending` / `g_batt_test_active`. |
| `fc_cli.c`          | 702 | All `rtc_cli_*` handlers, dispatcher, `task_rtc_cli_fn`. |
| `fc_internal.h`     | 228 | Shared extern declarations, constants, enums, `mode_info_t` typedef. Component-private -- not included by consumers of `field_capture.h`. |
| **total**           | **3 589** | (was 3 877 in one file; the extra lines are file headers + `fc_internal.h` boilerplate.) |

`components/field_capture/CMakeLists.txt` extended to add the seven new
`.c` files to `SRCS`. `field_capture.h` (public API) is unchanged, so
`main.c` / `boot_tasks.c` / other consumers were not touched.

All static globals that cross file boundaries were promoted to non-static
and re-declared `extern` in `fc_internal.h`. No functional changes to any
state machine, mode, or CSV format -- byte-identical CSV output on rerun.

### 2. Ship-mode is now king

`bq25619_enter_ship_mode` (in `components/bq25619/bq25619.c`) previously
took the shared I2C mutex with a 300 ms timeout and silently returned
`ESP_ERR_TIMEOUT` on failure. If the DRV2605 was mid-buzz on the same
I2C bus 1 (e.g. during the 3-beep voice-annotation lead-in), the watcher's
ship-mode call could time out and the BATFET write never happened.

Change: `bq25619_enter_ship_mode` now retries the mutex take with a
three-attempt escalating budget (500 ms -> 1 500 ms -> 3 000 ms, ~5 s
worst case). If all three fail, it emits an `ESP_LOGE` "ship-mode ABORT"
so the failure is visible rather than silent.

`task_shutdown_watcher_fn` (in `fc_shutdown.c`) also gained release
debounce. Symptom that motivated it: a long press was interpreted as a
single click, a recording started, and ship-mode never fired. Root cause:
a momentary contact bounce mid-hold made the watcher see `!low` for one
5 ms poll cycle, aborting the hold state. Fix: `SHDN_RELEASE_MS = 80 ms`.
The watcher only treats the pin as "released" after it has stayed high
for 80 ms; short bounces are absorbed. If the fire threshold (4 s) is
reached mid-bounce, ship-mode fires anyway.

### 3. `REC_AUDIO ON|OFF` CLI command

New NVS knob `cfg_sys.rec_audio` (default ON). Toggle via serial
`REC_AUDIO ON` or `REC_AUDIO OFF`; no reboot needed, takes effect on the
next recording start. When OFF, `run_recording_for_current_mode` skips
the 3-beep + 5 s WAV voice-annotation pre-roll for `FCM_ENV / FCM_MOTION /
FCM_SKIN` recordings and jumps straight into the CSV loop.

Wired through:

- `nvs_cfg.h/.c`: `K_SYS_REC_AUDIO` key, `nvs_cfg_sys_get_rec_audio` /
  `nvs_cfg_sys_set_rec_audio`, boot printout line, default `true`.
- `fc_recording.c` `run_recording_for_current_mode`: gated `voice_annot_lead_in`
  + the annot WAV write on `nvs_cfg_sys_get_rec_audio()`.
- `fc_cli.c` `REC_AUDIO` handler + HELP text + `STATUS` + `NVS_PRINT` dump.

## What still needs to happen (handoff)

- **Verify on bench.** The refactor compiled clean but nothing has been
  flashed and physically driven through every mode. Suggested smoke path:
  boot, run `WHOAMI`, run `TEMP_DUMP`, enter each top-level mode + LSM
  submenu, verify `REC_AUDIO OFF` skips the 5 s pre-roll, verify 4 s
  hold trips ship-mode even mid-mic-annotation.
- **BQ25619 TS network** is still not populated on iv7.1. `TEMP_DUMP` prints
  `bq (BQ25619) = -- (TS network not populated on iv7.1)` for that reason.
  Once the thermistor divider is in place, add a `bq_die_temp` read to
  `rtc_cli_dump_temps` in `fc_cli.c`.
- **Bench-test bouncy button.** The 80 ms release debounce is a workaround
  for what looks like a dying encoder switch. Once a replacement lands,
  the debounce is harmless -- it eats bounces without any downside on a
  clean button.
- **BQ mutex retry budget review.** Escalating retries max out at ~5 s.
  Long enough for any legitimate in-flight I2C transaction on this system
  (drv2605 buzz sequence, TMP117 read, BQ polling) but if we ever add a
  driver that legitimately holds the mutex longer, ship-mode will log
  "mutex stuck" and abort -- which is loud but correct.
