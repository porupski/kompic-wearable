# LOG -- loose-ends journal

**Purpose.** Append-only chronological journal for stray observations,
half-baked ideas, and side-quest asks that don't yet belong in a
Stage_NN doc or a memory note. Everything here is a breadcrumb --
promote to a proper venue (Stage_NN, memory/*, ADR, dedicated doc)
when the item crosses the "actually a work item" threshold, and
leave a `-> promoted: <link>` note behind so the trail stays intact.

**Format.** Reverse-chronological date headers. Under each date,
bullets with:
  - The observation / ask itself (one sentence).
  - `source:` where it came from (convo mid-Stage-NN / commit hash /
    fw version / bench log line).
  - `owner:` where it'll be resolved (Stage_NN, memory slug, "TBD").

**Split rule.** When this file passes ~500 lines, split by year
(`LOG_2026.md`, `LOG_2027.md`). Not before. Weekly splits add
friction (which bucket does the Sunday-night item go in?), and
`git grep` across one file is easier than across twelve.

**Not for.** Real work items, driver docs, stage plans, memory
notes. Those all have proper homes. This is the "I noticed X"
scratchpad.

---

## 2026-09-15

- **Boot ordering feels off.** Ivan wants a rebuild starting from
  core things -> per-device boots -> UI up first so the user sees
  progress -> drv-final buzz for finish. Not this stage.
  `source:` convo mid-Stage-30. `owner:` Stage 30.6 or Stage 31.
- **UI is a frankenstein.** Per-tile duplication + parallel
  broker/refresh pipelines. Rebuild scheduled.
  `source:` convo mid-Stage-30 2026-09-15.
  `owner:` Stage 31 -- [[project_lvgl_foundations_rebuild_planned]].
- **Encoder dead on iv8.0 unit.** `ENC` verb shows cw=0 ccw=0 net=0
  during active rotation; polled loop is alive; hardware suspect.
  Ivan flashing Arduino field-demo sketch to A/B hw-vs-fw next week.
  `source:` bench log fw 0.4.71-4.74.
  `owner:` Stage 30.5 -- [[project_encoder_dead_on_iv80]].
- **I2C hw_fsm_reset crash under rapid sleep/wake stress.** Core 0
  stuck in `i2c_hw_fsm_reset` -> `vPortExitCritical` during a
  `max17048_read16` after ~6+ double-tap cycles. ESP-IDF v5.5.2
  legacy I2C driver latent issue, not Stage 30.
  `source:` crash log fw 0.4.68 + fw 0.4.73.
  `owner:` i2c_master API migration (Stage 22b groundwork) --
  [[project_i2c_hw_fsm_reset_crash]].
- **Landscape via MADCTL + SW-swap is double-transformed.** Rebuild
  or use LVGL SW rotation.
  `source:` bench log fw 0.4.73 -- Stage_30 §4.3a.
  `owner:` Stage 30.3b (LVGL SW rotation, path A/B in §4.3a).
- **Touch may not reset 30 s idle-sleep.** Ivan flagged
  "feels like 30 s and its done". CST9217 press-edge already calls
  `field_capture_kick_activity()` but he hasn't retested after
  fw 0.4.74's mutex fix. Verify next flash.
  `source:` convo 2026-09-15. `owner:` next bench pass.
- **Diag pattern still ON.** 6 colored squares + white band on the
  main screen from Stage 30.2 §4.2g. Remove once landscape lands
  and rendering stays stable.
  `source:` fc 0.4.70 -- ui_main_screen.c `UI_MAIN_DIAG_PATTERN`.
  `owner:` next Stage 30 batch (after landscape decision).
- **BROKER: rtc_read: mutex timeout** intermittent warning during
  active navigation. Isolated occurrence during Stage 30 close-out
  bench; not blocking. Likely RTC read timing out against I2C
  contention window under UI activity.
  `source:` bench log fw 0.4.74, `data_broker/*` (tag `BROKER`).
  `owner:` monitor -- promote if recurring.
- **FLASH CLI verb landed (fw 0.4.75).** `FLASH [ON|OFF|<0..100>]`
  bypasses FCM_FLASHLIGHT mode-cycle so the LED works while the
  encoder is dead. Direct wrap around `flashlight_set_brightness`.
  Ivan's "least code" bridge to demo readiness.
  `source:` convo 2026-09-15. `owner:` shipped.
- **Log-tag legend landed.** `docs/build_info/LOG_TAGS.md` --
  canonical list of every ESP_LOG TAG in the tree, grouped by
  subsystem. New components add their tag there in the same
  commit. Rule: [[feedback-log-line-taxonomy]].
  `source:` convo 2026-09-15. Two known drifts flagged:
  `co5300_panel` + `co5300_pio` (lowercase), `XD` (mystery).
- **`XD` mystery tag.** Grep finds `static const char *TAG = "XD"`
  somewhere; origin unknown, unclear which C file. Hunt on next
  opportunistic pass through that area.
  `source:` LOG_TAGS.md build 2026-09-15. `owner:` TBD.

## 2026-09-16

- **Boot-mode force-hardcode landed as stopgap.** `field_capture.c`:
  right after `nvs_load()`, `s_mode = FCM_FLASHLIGHT; s_in_submenu =
  false;` -- does NOT call `nvs_save_mode()` so persisted mode is
  untouched. Only there because encoder is dead and there's no other
  way to select a mode. Delete this block once the encoder is fixed;
  device will resume remembering mode normally.
  `source:` convo 2026-09-16. `owner:` revert after
  [[project_encoder_dead_on_iv80]] resolves.

---
