# Stage 31 -- Mk1b LVGL foundations rebuild (batch 1)

**Date opened:** 2026-09-14
**Board:** Mk1b iv8.0.
**Firmware baseline:** TBD -- picks up wherever Stage 30 closes
(target: `esp_lvgl_port_add_disp` landed, PARTIAL refresh back,
`lvgl_flush_cb`+byte-swap+esp_cache_msync deleted, sleep-race gone).
**Status:** OPEN, blocked by Stage 30 close.

**Companion refs:**
`docs/build_info/reference_files/lvgl_9/LVGL9_Kompic_Architecture.md`
(architecture spec derived from the LVGL 9 PDF + smartwatch demo +
current-code map). `docs/build_info/reference_files/lvgl_9/
LVGL9_Reference.md` (API cheat-sheet). Every batch below points into
the architecture spec by §.

---

## 1. Why this stage exists

Stage 30 fixes the plumbing (Espressif's port, DMA overlap, sane
partial-refresh again). That unlocks the *shape* work: our current
UI is a mixed-model patchwork -- three top-level screens with
`lv_scr_load_anim`, per-widget style calls, per-tile `apply_theme`
fan-out, a 200 ms polled `update()` per tile that reads a custom
broker. The LVGL 9 smartwatch demo shows a materially cleaner shape,
and LVGL 9 gives us first-class `lv_subject_t` widget bindings that
delete the poll loop entirely.

Analysis and target shape live in `LVGL9_Kompic_Architecture.md`.
That doc identifies **10 migration batches** spanning three stages:

| Stage | Batches | Theme |
|-------|---------|-------|
| **31 (this)** | 31.1 -- 31.4 | Foundations trio + first pilot tile |
| **32** | 32.1 -- 32.4 | Bulk tile -> subject conversion (7 tiles) |
| **33** | 33.1 -- 33.3 | Alarm drawer + overlay polish + apply_theme sweep + `ui_navigation.c` delete |

Each batch is Opus-sized (~one session's worth of code + one bench
flash cycle). Full rebuild wall-clock ~2-4 weeks.

**Nothing here changes hardware behavior.** All changes are UI-side.
Sensor drivers, CLI, boot, sleep -- untouched.

---

## 2. Stage 31 scope

Four batches, dependency-ordered. Each locks in one foundational
piece so later stages can lean on it.

- **31.1** -- shared pane style scaffold (theme lives in ONE place)
- **31.2** -- `ui_subjects.c` scaffold + Env tile as pilot
- **31.3** -- `ui_animations.c` + settings drawer as a sibling pane
- **31.4** -- GPS tile subject conversion (largest, worst-case proof)

31.1 and 31.2 are independent -- can flash in either order. 31.3
depends on 31.1 (drawer pane needs a shared style). 31.4 depends on
31.2 (subject infra must exist before we convert tiles).

Rationale + close-second alternatives per batch below.

---

## 3. Batch 31.1 -- Shared pane style + theme swap

**Goal:** replace per-widget theme-tracked style calls with one
`static lv_style_t` per pane; theme swap mutates the style once,
LVGL invalidates automatically. Architecture spec §2.2, §5.3.

### What lands

- New file: `firmware/esp-idf/components/lvgl_ui/ui_pane_styles.c/h`
  - Exposes `style_pane_root`, `style_pane_drawer`, `style_row`,
    `style_card` as `lv_style_t` module-scope objects.
  - `kw_ui_pane_styles_init()` -- idempotent one-shot init from
    `theme_bg()`, `theme_text()`, etc. (adds flex layout, radius,
    padding).
  - `kw_ui_pane_styles_reapply_theme()` -- mutates each style's
    theme-tracked props with the current `g_ui_theme` and calls
    `lv_obj_invalidate(lv_screen_active())`.
- Modified: `ui_main_screen.c` -- swap outer container to use
  `style_pane_root`; drop per-widget bg/text-color calls that duplicate
  what the style now carries.
- Modified: `lvgl_ui.c` `apply_ui_theme()` -- call
  `kw_ui_pane_styles_reapply_theme()` instead of per-tile fan-out.
- **Kept as-is:** per-tile `apply_theme()` callbacks. They still fire
  and set the same colors (no-op after the style change lands). Deletion
  happens in Stage 33.3 after every tile is migrated.

### Why this is the right choice

The demo pattern (§2.2) is unambiguous: every satellite pane holds a
`static lv_style_t main_style` that carries theme colors + layout +
radius. Any object added to the pane inherits without a single
per-widget style call. Adopting this pattern:

- **Deletes theme fan-out entirely** once every tile is migrated
  (Stage 33 finish line).
- **Cuts per-tile boilerplate** -- typical tile has 3-8 widgets each
  with an `lv_obj_set_style_text_color` + `_bg_color` in
  `apply_theme()`. Shared style: zero.
- **Fixes theme-switch races** -- current fan-out iterates tiles in
  registry order; if a tile is mid-update on Core 1 the writes may
  land out-of-order. Shared style + `lv_obj_invalidate` is one
  atomic op from the LVGL task.

**Close-second: skip and go straight to subjects (31.2 first).**
Rejected because the pilot in 31.2 will create new widgets that
should use the shared style from day one. Doing 31.1 first means
env_tile's rewrite in 31.2 lands in the correct final form.

**Close-second: adopt LVGL's default theme instead of a custom
palette.** Rejected because our palette (Space Grotesk + our dark/
light with named accents) is already good and matches Stage 29's
crispness work. LVGL's default theme is generic material and we'd
lose brand.

**Close-second: use `lv_theme_t` (a first-class theme object) instead
of shared styles.** Rejected because `lv_theme_t` is applied to a
whole display and re-applied on every widget creation -- deep, opaque,
harder to reason about mid-render. Shared styles are 20 lines and
transparent.

### Success gate

1. `apply_ui_theme(DARK)` and `apply_ui_theme(LIGHT)` both cause the
   main screen bg + text color to switch within one refresh.
2. Tile bodies still theme-track (via their surviving `apply_theme`
   cbs -- transitional).
3. No LVGL warnings or asserts about invalidated regions overlapping.
4. `STATUS` still readable; CLI theme verb still works.

### Effort

New file + 2 modified files. ~300-400 lines touched. One session.

---

## 4. Batch 31.2 -- Subject scaffold + Env tile pilot

**Goal:** stand up `lv_subject_t` infra + convert one tile to the
new pattern. Prove that widget bindings work under our threading
model. Architecture spec §1.6, §5.4, §6.

### What lands

- New file: `firmware/esp-idf/components/lvgl_ui/ui_subjects.c/h`
  - Subject singletons: initially `subj_temp_c` (int),
    `subj_rh_pct` (int), `subj_pressure_hpa` (int),
    `subj_iaq` (int), `subj_env_status` (int).
  - Publish queues (mirrors of what env producer already emits or
    creates them): `g_env_q` if not already present in bme688.
  - `kw_ui_subjects_init(void)` -- initialises every subject with
    a sentinel value ("--").
  - `kw_ui_subjects_start_drain(void)` -- `lv_timer_create(cb,
    200, NULL)` on the LVGL task; cb drains all queues under lock
    and calls `lv_subject_set_*`.
- Modified: `env_tile.c` -- `env_tile_init()` uses
  `lv_label_bind_text(lbl, &subj_temp_c, "%d °C")` etc.
  `env_tile_update()` **becomes no-op** (kept for registry
  compatibility until Stage 33 registry cleanup).
- Modified: `bme688` producer task (or wherever env samples are
  currently pushed to the broker) -- `xQueueOverwrite(g_env_q, &sample)`
  instead of / in addition to the old broker path.
- Modified: `lvgl_ui_display.c` `boot_screens()` -- calls
  `kw_ui_subjects_init()` + `kw_ui_subjects_start_drain()` after
  `ui_broker_init()`.

### Why this is the right choice

Two structural wins:

1. **Deletes `update()` on env_tile.** Widget stays live-updating
   without any per-tile polling code. Extended across all 11 tiles
   this deletes ~4.5k LOC of `update()` + `broker_*_read()` scaffolding.
2. **Formalises the threading contract.** Producers (any core, any
   task) push to a queue; the drain timer runs on the LVGL task and
   is the ONLY caller of `lv_subject_set_*`. Removes an entire class
   of thread-safety mistakes.

**Env is the correct pilot** because:

- Smallest sensor tile (367 LOC).
- Stable sensor (BME688 owned + working for months, no bring-up debt).
- All-numeric outputs (temp/RH/pressure/IAQ) -- exercises the
  `lv_label_bind_text(int_fmt)` binding fully.
- No user input, no toggles, no gestures -- pure display. Isolates
  the subject pattern from confounders.

**Close-second: battery_tile (176 LOC) as pilot.** Smallest of all
tiles. Rejected because battery pulls from MAX17048 which has some
Stage 30 flux (bq/mpf division per [[project_bq_no_vbat_adc]]);
touching it while the fuel-gauge story is still settling is asking
for a false-positive regression.

**Close-second: gps_tile as pilot (proves worst case first).**
Rejected because gps_tile is 680 LOC with toggles, gestures, sub-
tile, and photo-view. It IS the worst case, but that's the wrong
call for the pilot -- pilot's job is to isolate the pattern change
so any regression is unambiguously about subjects. GPS is Batch
31.4 (foundations-proof-under-load), which is the right slot for it.

**Close-second: replace the old broker in one shot.** Rejected.
The broker also handles settings save (async NVS queue) which is
orthogonal to sensor data flow. Keeping the broker while migrating
sensor flow means we can retire the sensor-broker functions
opportunistically per venue [[feedback_group_work_by_venue]] without
a stop-the-world rewrite.

**Close-second: publish direct-from-producer with `lvgl_port_lock`.**
Rejected. Producer tasks would spend most of their time waiting for
the LVGL mutex (LVGL flush can hold it for a full frame). The
queue + drain-timer pattern decouples the producer's rate from LVGL's
frame rate.

### Success gate

1. Env tile temp/RH/pressure/IAQ labels update every 200 ms with
   real sensor data.
2. No LVGL asserts about mutex ownership or invalidated observers.
3. Theme swap still recolors env_tile text (via style_pane_root
   inheritance from 31.1 + surviving `env_tile_apply_theme` no-op).
4. `STATUS TEMP` (or equivalent CLI verb) reads
   `lv_subject_get_int(&subj_temp_c)` and matches the on-screen value.
5. Sensor producer task doesn't block or slow down (bench: watch
   `TOP` CLI for cpu-per-task, compare pre/post).

### Effort

New file + 2-3 modified files. ~400-600 lines touched. One session.
The trickiest part is auditing the current env producer path so the
queue doesn't get double-pushed.

---

## 5. Batch 31.3 -- Animation module + settings sibling drawer

**Goal:** replace `lv_scr_load_anim(settings)` with a slide-down
sibling-pane drawer. Introduce the reusable animation module.
Architecture spec §5.2, §5.5, §6.5.

### What lands

- New file: `firmware/esp-idf/components/lvgl_ui/ui_animations.c/h`
  - `kw_ui_animate_slide_in(obj, lv_dir_t from, dur_ms, delay_ms)`
  - `kw_ui_animate_slide_out(obj, lv_dir_t to, dur_ms, delay_ms)`
  - `kw_ui_animate_opa(obj, target_opa, dur_ms, delay_ms)`
  - Path defaults to `lv_anim_path_ease_out` for in / `_ease_in`
    for out.
- Modified: `ui_settings_screen.c` -- pane is created ONCE at boot
  as a child of `lv_screen_active()`, positioned via
  `style_pane_drawer`'s `translate_y = -LCD_V_RES`. Its tileview is
  built as before; only the parent changes.
- Modified: `ui_navigation.c` -- the settings branch of the state
  machine deletes; replaced by:
  - `main_screen` gesture cb: on swipe-up, call
    `kw_ui_animate_slide_in(settings_pane, LV_DIR_BOTTOM, 400, 0)`.
  - `settings_pane` gesture cb: on swipe-down from the top-most
    tile, `kw_ui_animate_slide_out(settings_pane, LV_DIR_TOP, 400, 0)`.
- Modified: `ui_main_screen.c` + `ui_settings_screen.c` gesture cbs
  wired directly on the panes (not centrally in ui_navigation).
- **NOT DONE this stage:** the alarm branch of `lv_scr_load_anim`
  stays. Alarm becomes a sibling pane in Stage 33.1. `ui_navigation.c`
  survives (shrunk) until then.

### Why this is the right choice

**The demo's sibling-pane pattern is objectively better for drawers**
(§2.1, §2.3):

- **Overlap during transition** -- home stays visible while settings
  slides down over it. `lv_scr_load_anim` swaps the entire active
  screen; you can't see the source and destination simultaneously.
  For a "drawer opens" affordance that's exactly wrong.
- **No screen reload cost** -- the tileview + all tile children stay
  built. Reopening the drawer is instant; today it's a full-screen
  reload every time.
- **Simpler cancellation** -- swipe-down mid-open just reverses the
  animation; screen-load doesn't cancel.

**Settings goes first (not alarm) even though alarm is smaller**
because settings exercises `LV_OBJ_FLAG_GESTURE_BUBBLE` interaction
with the tileview -- vertical swipes must escape the tileview to
reach the pane's cb for drawer-close. This is the tricky bit; getting
it right on settings makes alarm trivial. Alarm has no tileview so
it's a strictly-easier case; deferring it to Stage 33.1 loses nothing.

**Close-second: keep `lv_scr_load_anim` and just polish the
transition timing.** Rejected -- see "overlap during transition"
above. The affordance mismatch isn't a polish issue, it's structural.

**Close-second: use `lv_tileview` at the root with home / settings /
alarm as tiles.** Rejected. Settings is *itself* a tileview; nested
tileviews confuse gesture routing. Also alarm isn't gesture-reachable
in the target design (it comes from an event, not a swipe).

**Close-second: build the pane on-demand instead of at boot.**
Rejected -- reopen latency goes back up. RAM cost of always-resident
tileview + 9 tiles is ~100 KB, well within budget on PSRAM.

**Close-second: land ui_animations.c in a separate batch first.**
Considered. The reason to bundle: 31.3 is the first venue that
needs the module. Landing an unused module is dead weight until it
has a consumer.

### Success gate

1. Swipe-up on main -> settings drawer slides down; both visible
   mid-animation.
2. Swipe-down at settings root (top tile, y=0) -> drawer slides
   back up; home visible underneath as it moves.
3. Tileview horizontal swipes still navigate between tiles
   (bubble flag correct).
4. Vertical swipes on non-top tiles do NOT close the drawer (tileview
   consumes them for tile-nav, which is correct).
5. Alarm swipe-right path (unchanged this stage) still works via
   surviving `ui_navigation.c` code.

### Effort

1 new file + 3 modified. ~500-800 lines touched. One session. Bubble-
flag debug may add a bench cycle if we get the flag wrong first try.

---

## 6. Batch 31.4 -- GPS tile subject conversion (worst-case)

**Goal:** prove the subject pattern under the hardest tile. If GPS
converts cleanly, every other tile is easier. Architecture spec §5.7.

### What lands

- Extend `ui_subjects.c/h` -- add:
  - `subj_gps_sats` (int), `subj_gps_hdop_10x` (int),
    `subj_gps_utc_str` (string), `subj_gps_date_str` (string),
    `subj_gps_lat_str` (string), `subj_gps_lon_str` (string),
    `subj_gps_alt_m` (int), `subj_gps_speed_kmh_10x` (int),
    `subj_gps_status` (int), `subj_gps_power` (int, two-way toggle).
  - Drain timer already exists from 31.2; add a GPS branch to its cb.
- Modified: `gps_tile.c` -- `gps_tile_init()` binds labels + LED +
  power switch via `lv_label_bind_text` + `lv_obj_bind_flag_if_eq` +
  `lv_obj_bind_checked`. `gps_tile_update()` becomes no-op.
  Sub-tile (raw NMEA) similarly bound.
  Photo-view: `gps_tile_cmd_view_set()` stays as CLI/button surface;
  internal impl uses `lv_obj_bind_flag_if_eq` on
  `LV_OBJ_FLAG_HIDDEN` against a new `subj_gps_photo_view` int
  subject.
- Modified: `max_m10s` producer -- push `gps_sample_t` to `g_gps_q`;
  drain cb formats the strings.
- **Kept:** the `g_gps_sync_requested` flag pattern for the ATOMIC
  SYNC button -- that's a fire-and-forget command to Core 0, not
  observable state. Button `LV_EVENT_CLICKED` cb writes the flag.

### Why this is the right choice

GPS tile is the biggest (680 LOC) and hits every subject-binding
edge case:

- **9 read-only value labels** -- exercises int, string, and formatted-
  int label bindings.
- **1 LED bound to a multi-state status** -- exercises
  `lv_obj_bind_flag_if_eq` with reference values.
- **1 two-way toggle switch** -- exercises `lv_obj_bind_checked`
  with a subject that both the UI (user tap) and a producer
  (driver state confirmation) can publish. This is where the
  re-entrancy guard `s_syncing` in gps_tile.c today lives; the
  bind_checked pattern replaces it structurally.
- **1 conditional-visibility container** (photo view) -- exercises
  `lv_obj_bind_flag_if_eq` on `LV_OBJ_FLAG_HIDDEN`.
- **1 fire-and-forget command button** -- shows the correct pattern
  for "not an observable, a command": stay with event cb + flag.

If GPS converts cleanly under these five patterns, IMU / compass /
health / haptic / light / battery / system / RTC / alarm are all
subsets or trivial variants. The mesh-of-patterns audit here is
what makes Stage 32 low-risk.

**Close-second: convert IMU next (369 LOC, similar sensor shape).**
Rejected. IMU tile is similar to env -- lots of labels, no toggles,
no photo-view. Converting IMU proves nothing env didn't. GPS
stress-tests toggle re-entrancy which is the class of subtle bug
worth catching before doing it 7 more times.

**Close-second: convert compass_tile (387 LOC).** Rejected for the
same reason. Compass has one clever bit (bearing arc) but no
toggles.

**Close-second: convert system_tile (290 LOC).** Rejected because
system_tile pulls from NVS and uptime -- neither is producer-task-
driven so the drain pattern isn't exercised the same way. Not a
representative test.

### Success gate

1. GPS tile: all 9 value labels track live NMEA (visible with sky-
   view; static "--" indoors).
2. Power toggle: tap on-screen -> driver enable succeeds -> switch
   stays checked (no re-entrancy oscillation). CLI `GPS_POWER OFF`
   -> subject flips to 0 -> switch un-checks on next drain.
3. Status LED color tracks acquiring / online / offline correctly.
4. Photo view toggle via both the on-tile button and
   `GPS_VIEW photo` CLI verb -- both flip `subj_gps_photo_view` and
   the container's HIDDEN flag follows.
5. ATOMIC SYNC button still triggers Core 0 NTP sync (flag path
   unchanged).

### Effort

1 modified subjects file + 1 modified 680-line tile + 1 modified
producer. ~700-900 lines touched. One session -- possibly two if
the `lv_obj_bind_checked` two-way path has a subtle bug we don't
catch on bench first try.

---

## 7. Non-goals for Stage 31

- Converting IMU / compass / health / haptic / light / battery / system /
  RTC to subjects -- Stage 32 territory.
- Alarm as sibling pane / deleting `ui_navigation.c` -- Stage 33.1.
- Deleting per-tile `apply_theme()` cbs -- Stage 33.3 (once all
  tiles migrated).
- Replacing shutdown overlay with `lv_msgbox` -- Stage 33.2.
- Encoder tile redesign per [[project_encoder_tile_redesign]] -- still
  its own future stage.
- CLI submenu restructure per [[project_cli_submenus]] -- unrelated
  surface.
- Recording writer datetime migration per
  [[feedback_recording_datetime_opportunistic]] -- unrelated surface.

---

## 8. Definition of done for Stage 31

1. `ui_pane_styles.c` in place; `apply_ui_theme()` mutates ONE style
   set and calls `lv_obj_invalidate`; main-screen theme swap works
   without per-tile fan-out.
2. `ui_subjects.c` in place; drain timer running on LVGL task;
   env_tile fully subject-bound with `env_tile_update()` a no-op.
3. `ui_animations.c` in place; settings drawer is a root-sibling
   pane; swipe-up opens it with over-home animation; swipe-down
   closes it.
4. gps_tile fully subject-bound (labels + LED + power toggle +
   photo view); `gps_tile_update()` is a no-op.
5. No regression in: boot, sensor sampling, sleep, charging, CLI,
   theme swap, alarm ring, notification popup, tile navigation.

---

## 9. Bench log (fill in as each batch lands)

*(populated as we go, per [[feedback_stage_log_workflow]])*

---

## 10. Next-steps hook -- Stage 32 + 33 scope

**Stage 32 -- bulk tile -> subject conversion** (~4 batches):
- 32.1: imu_tile (369 LOC) + compass_tile (387) -- similar-shape
  sensor tiles, batched by likeness.
- 32.2: haptic_tile (471) + light_tile (537) -- both have UI-driven
  parameters (haptic waveform pick, brightness slider); exercises
  the widget->subject two-way pattern beyond gps power toggle.
- 32.3: health_tile (367) + battery_tile (176) -- health has PPG/BCG
  formatted strings; battery is smallest and might land as a bonus
  during 32.2's bench cycle.
- 32.4: system_tile (290) + rtc_tile (300) -- NVS-driven values,
  different producer shape; validates that non-sensor tiles fit the
  drain pattern.

**Stage 33 -- structural finish** (~3 batches):
- 33.1: alarm as sibling pane; delete the `alarm` branch of
  `ui_navigation.c`; delete `ui_navigation.c` entirely if empty.
- 33.2: shutdown overlay -> `lv_msgbox` on `lv_layer_top()`;
  audit lock overlay + notification overlay layer usage.
- 33.3: delete every tile's `apply_theme()` cb; delete
  `apply_ui_theme` fan-out loop; delete `broker_*_read()` sensor
  accessors (queue is now private to drain timer); delete tile
  `update` cbs from registry contract.

Total remaining after Stage 31 close: ~7 batches, ~4-6 weeks.

---

## 11. References

- `docs/build_info/reference_files/lvgl_9/LVGL9_Kompic_Architecture.md`
  -- authoritative shape spec; every batch cites its §.
- `docs/build_info/reference_files/lvgl_9/LVGL9_Reference.md`
  -- API cheat-sheet.
- `docs/build_info/reference_files/lvgl_9/lv_demos-master/src/
  smartwatch/` -- reference implementation:
  - `lv_demo_smartwatch_control.c:75` -- static-style-per-pane
    (canonical for 31.1).
  - `lv_demo_smartwatch.c:486` -- gesture-cb-per-pane
    (canonical for 31.3).
  - `lv_demo_smartwatch.c:280+` -- animate helpers (canonical for
    31.3 `ui_animations.c` API).
- LVGL 9 PDF `LVGL_Library.pdf`:
  - p. 328-381 (styles/themes) -- style flyweight semantics for 31.1.
  - p. 909+ (observer/subject) -- binding surface for 31.2/31.4.
  - p. 486-506 (anim) -- for 31.3 animation module.
  - p. 439-441 (screens/layers) -- for 31.3 sibling-pane semantics.
- `managed_components/lvgl__lvgl/src/others/observer/lv_observer.h`
  -- authoritative binding API (already read; every `_bind_*`
  helper used in this stage is listed there).
- Stage 30 (`Stage_30_Mk1b_Display_Port_Migration.md`) -- must land
  first; this stage assumes port + PARTIAL refresh.
- Auto-memory:
  [[project_kompic_mk1]],
  [[project_display_landscape_direction]],
  [[project_display_text_crispness]],
  [[project_mk1b_arrived]],
  [[feedback_lvgl_kconfig_authoritative]],
  [[feedback_lvgl_heap_too_small_for_pixels]],
  [[feedback_lv_font_conv_flags]],
  [[feedback_flash_per_checkpoint]],
  [[feedback_group_work_by_venue]],
  [[feedback_cli_first_testability]],
  [[feedback_stage_log_workflow]],
  [[feedback_mk1b_stage_naming]].
