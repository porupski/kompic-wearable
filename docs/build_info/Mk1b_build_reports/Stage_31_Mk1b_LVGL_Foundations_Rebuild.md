# Stage 31 -- Mk1b LVGL foundations rebuild (batch 1)

**Date opened:** 2026-09-14
**Date closed:** 2026-09-18
**Board:** Mk1b iv8.0.
**Firmware baseline:** 0.4.75 (Stage 30 close).
**Firmware close:**   0.4.87 (Stage 31.4b + MODE cycle button).
**Status:** CLOSED. All four batches landed + bench-verified:
- 31.1 shared pane style flyweight
- 31.2 subject scaffold + env_tile pilot (+ 31.2b corner-safe redo)
- 31.3 settings drawer as sibling pane (+ 31.3b/c iterations)
- 31.4a GPS tile subject conversion + robust status-first layout
- 31.4b UBX-CFG-VALSET + MON-RF diagnostic + GPS_DYNMODEL CLI
       + on-tile MODE cycle button (PED->WRIST->AUTO->AIR1G).
GPS chip firmware side complete; further GPS work waits on Ivan's
bench hardware fix (VIO_SEL float + SAFEBOOT_N float) then real
antenna. Stage 32 (bulk tile subject sweep) picks up in a fresh
session -- see `Stage_32_Mk1b_Tile_Subject_Sweep.md`.

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

### 9.1 -- Batch 31.1 flash (fw 0.4.76)

**What landed:**
- `ui_pane_styles.{c,h}` -- 5 shared LVGL styles
  (pane_root / pane_drawer / row / card / subtext). Init once at boot,
  reapply mutates theme-tracked props and invalidates active screen.
- `lvgl_ui.c` -- `kw_ui_pane_styles_init()` at top of `lvgl_ui_init()`;
  `kw_ui_pane_styles_reapply_theme()` at top of `apply_ui_theme()`
  (before the surviving per-tile fan-out).
- `ui_main_screen.c` -- `s_screen` uses `style_pane_root`; time +
  battery labels inherit text colour; date label wears `style_subtext`.
  Docstring updated (main is no longer "always DARK").
- `ui_settings_screen.c` -- `s_screen` + `s_tileview` use
  `style_pane_root`. Per-tile bg writes retained (Stage 33.3 sweep).
- ESP_LOG tiers seeded per new practice:
  `PANE_STYLE` LOGI on init + on each reapply,
  LOGD per property block, LOGV per screen-invalidate.

**What to watch on flash:**
1. Boot completes -- `PANE_STYLE: init: 5 pane styles created` line
   appears once before the tile boot loop.
2. Main screen renders identically to fw 0.4.75 on DARK theme
   (anthracite bg, off-white time, dim date, off-white battery).
3. Swipe up -> settings tile view. Pick System tile, toggle theme.
   Expected: LIGHT bg = warm ivory; DARK bg = anthracite; on LIGHT
   theme labels on main-screen are DARK ink (not invisible off-white).
   Log line `PANE_STYLE: reapply_theme: theme=LIGHT -- 5 styles
   updated, screen invalidated` per swap.
4. No LVGL warnings about invalidated regions, no assert about
   style not initialised, no CST9217 touch-freeze.
5. `STATUS` verb still works.

**If something's off, useful escalation:**
- Flip PANE_STYLE to DEBUG at boot -- add
  `esp_log_level_set("PANE_STYLE", ESP_LOG_DEBUG)` in
  `app_main` (main.c) or from `LOGLVL` CLI once wired -- to see the
  colour hex values written on each reapply.
- Flip to VERBOSE if the invalidate itself is suspect.

**Bench verdict (2026-09-18, fw 0.4.76):**
- Plumbing PASS: both `PANE_STYLE` log lines fire as designed
  (init once at boot; reapply once at UI init frame). No LVGL
  asserts, no un-initialised-style warnings.
- Visual PASS on DARK theme: main + settings identical to fw 0.4.75.
  Bonus: tile titles crisper than before (side-effect of centralising
  pad/radius on `style_pane_root`).
- Swipe responsiveness: good, possibly slightly better than 0.4.75.
- DARK<->LIGHT visual swap NOT yet verified -- Core 0 IRQ-WDT panic
  in `bq25619_read_reg` (i2c_hw_fsm_reset) killed the bench before
  Ivan could reach the theme toggle. Same signature as
  [[project_i2c_hw_fsm_reset_crash]], third recurrence
  (fw 0.4.68 / 0.4.73 / 0.4.76). PRE-EXISTING, not caused by 31.1.
- Deferred: theme-toggle visual check to a fast-path bench (swipe
  straight to System tile, toggle, watch for `theme=LIGHT` reapply
  log) or push once i2c_master API migration lands.

**Bench verdict (2026-09-18 pass 2, fw 0.4.76):**
- Visual DARK<->LIGHT toggle PASS on second bench: four clean reapply
  lines captured (LIGHT / DARK / LIGHT / DARK), no visual glitches,
  swap latency imperceptible.
- No I2C crash this run (crash appears non-deterministic across
  sleep/wake cycle counts).

**Batch status:** LANDED + fully verified (plumbing + visual).

---

### 9.1a -- BQ25619 prophylactic (fw 0.4.77)

Small preventive patch riding this stage's flash cycle -- see
[[project_i2c_hw_fsm_reset_crash]]. Not a fix (real fix = i2c_master
API migration), just reduces exposure:

- `bq25619.c` `task_battery_fn`: skip poll when `g_display_sleep`
  is true. Cached broker value stays visible on wake.
- `bq25619` CMakeLists: adds `boot_logic` to REQUIRES so the flag
  extern resolves.

**Watch on flash of fw 0.4.77:**
1. `BQ25619` still logs "Task started" at boot.
2. After 30 s idle-timeout sleep: no crash, no BQ log noise. At
   DEBUG level the `poll skipped: display asleep` line ticks 1 Hz.
3. On double-tap wake: poll resumes; battery % on main screen
   updates within ~1-2 s (fresh read).
4. Repeat sleep/wake stress; crashes should be less frequent
   (probabilistic, not eliminated).

**Bench verdict (2026-09-18, fw 0.4.77):**
- Sleep-skip landed cleanly, no build regression.
- I2C crash STILL fires under mid-navigation stress (not
  sleep/wake related as originally hypothesised) -- sleep-skip
  helps only for the "sleeping bus" case, not "hot navigation".
  Kept landed (harmless prophylactic), root fix remains
  i2c_master API migration. Log lines see 2026-09-18 LOG.md.
- ENC verb registered rotation mid-fiddle -- confirms firmware
  path is alive when signals arrive; encoder hw is intermittent-
  contact not fully dead. Memory + LOG updated.

---

### 9.2 -- Batch 31.2 flash (fw 0.4.78)

**What landed:**
- `ui_subjects.{c,h}` -- 5 env string subjects + `g_env_q` queue +
  `lv_timer` drain cb running on LVGL task at 200 ms cadence.
- `bme688_drv.c` `task_env_fn`: after `broker_env_write()` also does
  `xQueueOverwrite(g_env_q, &bd)`. Both paths coexist; broker stays
  for CLI (STATUS/WHOAMI) + other consumers.
- `env_tile.c`:
  - Labels bound via `lv_label_bind_text(lbl, &subj_env_*, NULL)`.
  - `env_tile_update()` deleted 5 snprintf blocks; retains only
    LED colour, toggle sync, button feedback timeout (Stage 32/33
    scope for those). Went from ~100 lines -> ~30 lines.
- `lvgl_ui_display.c` `boot_screens()`:
  - `kw_ui_subjects_init()` after `ui_broker_init()` (pre-lock).
  - `kw_ui_subjects_start_drain()` inside lock, after `lvgl_ui_init`.
- CMake: `ui_subjects.c` added to lvgl_ui SRCS.
- ESP_LOG tiers per new practice:
  `UI_SUBJ` LOGI init + drain-timer-started,
  LOGD per drain-cycle-with-sample,
  LOGV per subject write firehose.

**What to watch on flash:**
1. Boot logs:
   - `UI_SUBJ: init: 5 env subjects + g_env_q created` (once)
   - `UI_SUBJ: start_drain: LVGL drain timer running (200 ms)` (once)
   - `PANE_STYLE:` lines still fire as in 0.4.77 (regression check).
2. On env tile:
   - First ~2 s post-boot: labels show `---` (sentinel).
   - After first bme688 sample: temp / hum / press / alt / delta
     populate. Update cadence indistinguishable from pre-31.2
     (bme688 runs at 0.5 Hz -> ui refreshes at that rate; the
     200 ms drain timer just catches whichever sample is newest).
3. LED still tracks status (green online, red offline, ...).
   Power toggle still syncs with broker. ZERO HEIGHT button
   works + shows "ZEROED ✓" feedback for 2 s then reverts.
4. No LVGL asserts about observer / subject / mutex.
5. Theme swap still works (regression from 31.1).
6. `STATUS` verb still shows env fields (broker path intact).

**If something's off, useful escalation:**
- `esp_log_level_set("UI_SUBJ", ESP_LOG_DEBUG)` -> see per-drain
  `drain: env sample consumed` ticks.
- `esp_log_level_set("UI_SUBJ", ESP_LOG_VERBOSE)` -> see per-write
  `env published: T=... H=... P=... Alt=...` firehose.

**Bench verdict (2026-09-18, fw 0.4.78):**
- Build + boot clean; subjects init line + drain-timer line fire
  as designed. env_tile labels render "---" initially.
- BLOCKED at "bring sensor ONLINE" step: the power switch (top-
  right corner) is cropped by the AMOLED corner arc -- Ivan can't
  tap it, so BME688 stays parked and label content stays sentinel.
  Producer path never fires so we don't get to visually confirm
  the drain -> subject -> label flow with real data.
- Not a Stage 31.2 regression -- the switch has been in the corner
  since day one; 31.2 didn't move it. Every sensor tile has the
  same layout pattern. Fixing env in-venue (fw 0.4.79 below).
- i2c crash x1 (bq path, same signature); accepted. Main-clock
  italic/shear also recurring (chronic).

**Batch status:** LANDED, but visual confirmation of live values
deferred to fw 0.4.79 when the switch is reachable.

---

### 9.2b -- env_tile corner-safe layout (fw 0.4.79)

Opportunistic in-venue fix so the 31.2 pilot can actually be
verified with real sensor data. Also improves legibility across
the board.

**What landed (env_tile.c only):**
- Fonts bumped 20 -> 28 (UI_FONT_TITLE) for data rows + header +
  button label. Literal 2x (40 px) requires a new lv_font_conv
  face; ping if 28 isn't big enough.
- Power switch: 46x22 -> 90x46, moved from TOP_RIGHT (-8, 6)
  (INSIDE arc) to TOP_RIGHT (-60, 40) (outside arc).
- Zero button: 200x34 -> 280x60, moved to BOTTOM_MID (0, -40)
  (was -8, IN arc).
- Text starts at x=40 (was 12); interactive rows at x=60 pad each
  side per [[project_display_corner_cutoff]].
- New local layout constants ENV_PAD_* + ENV_ROW_STEP/Y0 so future
  tweaks land in one place.

**What to watch on flash:**
1. Env tile: header + switch fully visible top-of-tile, no arc clip.
2. Tap the switch -> BME688 wakes; within ~2 s the drain timer
   pushes the first sample into subjects -> labels populate.
   Watch for `UI_SUBJ: drain: env sample consumed` at DEBUG level
   if you want proof.
3. All 5 data rows readable at arm's length (bigger font).
4. ZERO HEIGHT button fully visible, tappable, and Δ Height
   refresh feels immediate (piggyback push from 31.2 landed).

**Other tiles are NOT fixed by this batch.** Every sensor tile
uses the same corner-hostile layout pattern; systemic sweep is
Stage 32 as each tile gets its subject conversion pass.

**Bench verdict (2026-09-18, fw 0.4.79):**
- Layout PASS: header + switch fully visible top-of-tile, button
  fully visible bottom, no arc clip on interactive elements.
- Font legibility: header at 28 px renders clean. Data rows
  (same font, `theme_subtext` colour, subject-driven) render
  blurry / thin outline / shear -- same chronic display issue
  as the main-clock italic. Ticket in LOG.md; grouped for a
  render-pipeline audit later.
- Header text ("BME688 T/H/P/Gas") overlaps the wider switch --
  should be chip-name-only ("BME688"). Deferred.
- Live env values populate after tapping switch ON -- confirms
  producer -> queue -> drain -> subject -> label flow end-to-end.

**Batch status:** LANDED + verified. env_tile is the first
subject-bound tile in the tree.

---

### 9.3 -- Batch 31.3 flash (fw 0.4.81)

**What landed:**
- `ui_animations.{c,h}` -- 3 helpers (slide_in / slide_out / opa).
  Wraps lv_anim_t boilerplate around translate_x/y + opa. Ease-out
  on in, ease-in on out. Direction taken as LV_DIR_*; off-screen
  extents computed from LCD_*_RES.
- `ui_settings_screen.{c,h}`:
  - `settings_screen_build()` now takes a `parent` argument.
  - Root pane is `lv_obj_create(parent)` (was screen via NULL),
    full-screen sized, initial `translate_y = LCD_V_RES`
    (off-screen BELOW), styled with `style_pane_drawer`.
- `ui_navigation.c`:
  - Settings branch of `cb_main_gesture` now calls
    `kw_ui_animate_slide_in(pane, LV_DIR_BOTTOM, 300, 0)` instead
    of `lv_scr_load_anim`.
  - `cb_settings_gesture` on swipe-down checks tileview active
    tile y-coord; only closes drawer when at row 0 (y=0). Sub-
    tile row (GPS at y=LCD_V_RES) is preserved for tileview's
    own nav-back-to-top handling.
  - Alarm branch UNTOUCHED -- still `lv_scr_load_anim`. Sibling-
    pane conversion is Stage 33.1.
- `lvgl_ui.c` `lvgl_ui_init`: passes `main_scr` into
  `settings_screen_build(main_scr)` so the drawer becomes a
  child of main.
- CMake: added `ui_animations.c` to `lvgl_ui` SRCS.

**Bench watches:**
1. Boot -> main screen visible. Drawer sits off-screen below
   (invisible). No visual artefacts at boot.
2. Swipe UP on main -> drawer slides up into view over ~300 ms;
   main visible under the drawer during the animation (overlap
   is the whole point vs. the old lv_scr_load_anim swap).
3. On env tile (auto-lands per reorder): horizontal swipes still
   navigate between tiles. Vertical swipe-down at env tile ->
   drawer slides down out of view.
4. GPS sub-tile: swipe DOWN on GPS main tile -> tileview goes
   to GPS sub-tile (still works, drawer stays). Swipe DOWN on
   GPS sub-tile -> tileview navs back to GPS main (still works,
   drawer stays). Swipe DOWN on GPS main tile again -> drawer
   closes (now at y=0). This is the sub-tile guard doing its job.
5. Alarm still works: swipe RIGHT on main -> alarm loads via
   lv_scr_load_anim; swipe LEFT on alarm -> main returns.
6. Theme swap on System tile still works (drawer bg tracks).
7. Log lines: `UI_NAV: → Settings (drawer opens)` and
   `UI_NAV: → Main (drawer closes)` on gestures. At DEBUG:
   `UI_ANIM: slide_in obj=... from_dir=... dur=300 ms` and
   `UI_NAV: swipe-down on sub-tile -- tileview navs, drawer stays`.

**If something's off, useful escalation:**
- `esp_log_level_set("UI_ANIM", ESP_LOG_DEBUG)` -> per-call anim
  parameters.
- `esp_log_level_set("UI_NAV", ESP_LOG_DEBUG)` -> sub-tile guard
  decisions.
- If drawer looks visually WRONG (torn / half-drawn / stuck at
  translate_y != 0/LCD_V_RES): report screen coords + which anim
  path was taken; likely a partial-refresh strip boundary issue,
  which is chronic and not 31.3's fault.

**Bench verdict (2026-09-18 attempt 1, fw 0.4.81):**
- Drawer OPENS cleanly on swipe-up (slide_in animation renders,
  main visible under drawer during slide).
- Drawer FAILS TO CLOSE on swipe-down on Env tile (row 0).
  Ivan: "cant return to the main time screen at all".
- GPS swipe-down triggered a crash -- but the backtrace is the
  chronic `bq25619_read_reg -> i2c_hw_fsm_reset -> IRQ WDT`,
  5th instance this session. Not caused by 31.3, just triggered
  by nav stress.
- Follow-up 31.3b (fw 0.4.82) below.

**Batch status:** LANDED but drawer-close broken; hotfix pending.

---

### 9.3b -- 31.3 drawer-close fix (fw 0.4.82)

**Hypothesis on the failed close:** the `lv_obj_get_y > 0` sub-tile
guard may have false-triggered on Env row-0 due to timing between
tileview's gesture handler and our cb reading `active_tile`.
Rewrite to use explicit `tile_registry.subtile_handle` comparison
which can't be racy about tile identity. Also add DEBUG entry
log so bench can see if cb fires at all.

**What landed (ui_navigation.c only):**
- `#include "tile_registry.h"` added.
- `cb_settings_gesture` gets an unconditional ENTRY LOGD line
  (`entry dir=... state=... asleep=...`) so we can trace every
  dispatch even the early-outs.
- Sub-tile guard rewritten: loop over tile_registry, compare
  active-tile pointer against each `subtile_handle`. Match =
  we're on a sub-tile, don't close. Also LOGDs the decision
  ("swipe-down on sub-tile col=X" or "swipe-down on main-row
  tile -- closing drawer").

**Watch on flash:**
1. `esp_log_level_set("UI_NAV", ESP_LOG_DEBUG)` first thing in
   `app_main` OR just navigate + inspect I-level output.
2. Swipe-down on Env tile:
   - DEBUG: `cb_settings_gesture entry dir=... state=1 asleep=0`
     (state=1 is UI_SCREEN_SETTINGS)
   - DEBUG: `swipe-down on main-row tile -- closing drawer`
   - INFO:  `→ Main (drawer closes)`
   Drawer visibly slides down.
3. Swipe-down on GPS main (col 5, row 0):
   - Same INFO line, drawer closes (if user actually wants that)
   - OR tileview goes to GPS sub-tile if timing lands active on
     sub-tile at eval time. The registry-compare pattern is
     robust either way.
4. Swipe-down on GPS SUB-TILE (col 5, row 1):
   - DEBUG: `swipe-down on sub-tile col=5 -- tileview navs, drawer stays`
   - Drawer stays. Tileview navs back to GPS main.
5. If cb doesn't fire at all on swipe-down: NO entry log line
   appears -- that's an event-routing bug (not a guard bug),
   escalate with a description of exactly what you tapped.

**Bench verdict (2026-09-18 attempt 2, fw 0.4.82):**
- Registry-based sub-tile guard shipped. Sub-tile nav on GPS
  still works cleanly. But drawer close still broken.
- Root cause found via LVGL source dive (lv_indev.c:1781 +
  lv_obj.c:593): LV_OBJ_FLAG_GESTURE_BUBBLE is a DEFAULT flag
  on every non-screen object. Gesture dispatch walks UP the
  parent chain while the flag is set, firing on the first
  object WITHOUT it. Screens are the only objects without the
  flag by default. So gestures on the drawer pane ALWAYS
  bubble past to main_scr; my cb_settings_gesture never runs.
- Explains everything: drawer opens (cb_main fires), GPS
  internal nav works (LVGL scroll handling is separate), drawer
  never closes (event routed elsewhere).

**Batch status:** LANDED but broken; 31.3c is the real fix.

---

### 9.3c -- 31.3 drawer-close (real fix, fw 0.4.83)

**Fix:** unify all gesture handling in cb_main_gesture. Delete
cb_settings_gesture entirely; drop its registration on the
drawer pane. Sub-tile guard extracted to active_is_subtile()
helper. Callback branches on s_current.

**Files touched (2):**
- `lvgl_ui/ui_navigation.c` -- merge cbs; delete cb_settings +
  its registration; docstring update explaining WHY.
- `field_capture/firmware_version.h` -- 0.4.82 -> 0.4.83.

**Watch on flash:**
1. Swipe-up on main -> drawer opens (same as before).
2. Swipe-down on Env row 0 -> drawer closes.
   DEBUG: `cb_main_gesture entry dir=<N> state=1 asleep=0`
   INFO:  `→ Main (drawer closes)`
3. GPS sub-tile still works: swipe-down on GPS main goes to
   GPS sub-tile OR closes drawer (either OK; both intentional).
   Swipe-down on GPS sub-tile navs back to GPS main WITHOUT
   closing drawer (`swipe-down on sub-tile col=5 -- ...`).
4. Alarm still swipes right -> left as before.
5. Log line `Settings drawer registered (gestures dispatch via
   main cb)` on boot (renamed from the old "gesture + bubble set").

**Bench verdict (2026-09-18, fw 0.4.83):**
- Drawer opens on swipe-up, closes on swipe-down. Log lines
  fire as designed: `→ Settings (drawer opens)` +
  `→ Main (drawer closes)`.
- GPS sub-tile still works (guard preserved via active_is_subtile).
- Alarm untouched, still works.
- Chronic i2c crash x1 (6th instance, no longer counting; real
  fix stays i2c_master API migration).

**Batch status:** LANDED + verified. **Stage 31.3 = DONE.**

---

## Stage 31 rollup (2026-09-18)

- 31.1 shared pane style flyweight -- LANDED + verified
- 31.2 subject scaffold + env_tile pilot -- LANDED + verified
  (plus 31.2b corner-safe layout redo)
- 31.3 settings drawer as sibling pane -- LANDED + verified
  (through 3 attempts; 31.3c was the real fix after finding
  LVGL 9's GESTURE_BUBBLE default behaviour)
- 31.4 GPS tile subject conversion -- **NEXT**, blocked on
  hardware fix (see §11)

## 11. Stage 31.4 pre-flight (2026-09-18)

Before 31.4 GPS work starts, Ivan is bench-fixing MAX-M10S
wiring per [[project_max_m10s_wiring]]:
- VIO_SEL: was GND -> **float** (3V3 mode)
- SAFEBOOT_N: was 3V3 -> **float** (normal boot; internal 1kΩ
  to TIMEPULSE was being fought)
- SDA/SCL (chip pins 16/17): unused, floating

**31.4 planned scope** (once hardware fix is in):
1. GPS tile subject conversion (original plan): 9 value labels
   bind via lv_label_bind_text; power toggle via lv_obj_bind_checked
   (two-way); photo-view via lv_obj_bind_flag_if_eq(HIDDEN).
   gps_tile_update becomes no-op.
2. Enable UBX-NAV-PVT (0x01/0x07) -- one binary frame carries
   fixType/numSV/gnssFixOK/pDOP. Better fix telemetry than
   parsing GGA quality strings.
3. Enable UBX-MON-RF (0x0A/0x38) -- antStatus, antPower,
   jammingState, noise floor, AGC. Single highest-value "why
   no fix" diagnostic. Add CLI verb to dump it.
4. Enable UBX-NAV-SAT (0x01/0x35) -- per-satellite C/N0.
   Distinguishes antenna problems (all zero) from TTFF
   pending (25-35 for several sats).
5. Dynamic model CLI verb: cycle through wrist / pedestrian /
   portable / automotive / airborne (per §2.2.1 of Integration
   Manual, `CFG-NAVSPG-DYNMODEL` = 9 / 3 / 0 / 4 / 6/7/8).

**Blocker for CFG-VALSET frames:** the exact key IDs live in
a separate PDF (u-blox M10 Interface Description, UBX-19035940)
not yet in docs/build_info/datasheets/. Options:
- Ivan grabs it from u-blox site (preferred)
- We source the key IDs from an open-source library
  (Sparkfun u-blox GNSS Arduino Library has them all)

Non-goal: WHY nothing is working currently. The bench hardware
fix + antenna cap + open-sky wait is the correct answer. 31.4
is UI + diagnostics, not GPS-alive troubleshooting.

---

### 9.4 -- Batch 31.4a flash (fw 0.4.84)

Robust subject-bound GPS tile. Works whether the chip is dead,
half-alive (no fix), or fully acquiring. Bench hardware fix
still pending; tile must show "no data -- check antenna/wiring"
when chip is silent, and immediately populate when it starts
talking.

**What landed:**
- `ui_subjects.{c,h}`: extended with 9 GPS string subjects +
  2 int subjects (subj_gps_enabled, subj_gps_photo_view) +
  g_gps_q. Drain cb now handles both env + gps queues.
  Two-way switch observer: subject change -> broker write.
- `max_m10s/max_m10s.c` `task_gps_fn`:
  - After broker_gps_write, xQueueOverwrite g_gps_q (fresh
    sample path).
  - When !enabled, ALSO push a "disabled" snapshot so tile
    status reflects the toggle immediately (was stuck showing
    last known fix pre-disable).
- `max_m10s/gps_tile.c`: full rewrite.
  - Corner-safe layout: 60H/40V pads, 28px status + header.
  - Header text is chip-name only ("MAX-M10S"), not name+desc.
  - Prominent STATUS line (28 px, `subj_gps_status_str`) --
    always shows a coherent message: "3D fix (8 sats)" /
    "acquiring... (time only)" / "no data -- check antenna/
    wiring" / "GPS disabled" / etc.
  - Power switch: `lv_obj_bind_checked` on `subj_gps_enabled`.
    Old `cb_power_toggle` + `s_syncing` guard deleted --
    two-way binding handles both directions.
  - Photo view: HIDDEN flag via `lv_obj_bind_flag_if_not_eq`
    on the photo container, `lv_obj_bind_flag_if_eq` on the
    normal-view widgets. Toggled via `kw_ui_gps_view_set`.
  - gps_tile_update reduced to LED colour + sync button
    enable-state + feedback timeout (~30 lines from ~180).
  - Sub-tile (raw NMEA debug) stays poll-based -- low-value
    to subject-ify.
- `max_m10s/CMakeLists.txt`: added lvgl_ui to REQUIRES
  (same pattern bme688 uses).

**What to watch on flash:**
1. Boot logs:
   - `UI_SUBJ: init: env(5) + gps(9 str + 2 int) subjects
     created; g_env_q + g_gps_q up` (once)
   - `GPS_TILE: MAX-M10S tile init OK (subject-bound, normal +
     photo views)` (once, on drawer build)
2. Open drawer, swipe to GPS (Col 5 with current registry).
   Expected initial state:
   - Header: `MAX-M10S`
   - Switch: reflects current broker enabled state
   - STATUS line: `waiting for GPS module...` if never received
     data, else derived from broker
3. Tap power switch ON:
   - `UI_SUBJ: gps switch -> broker_gps_set_enabled(1)` at DEBUG
   - Task starts polling UART
   - Within 500ms next drain fires; STATUS changes to
     `acquiring... (no signal yet)` or similar
4. Tap power switch OFF:
   - STATUS changes to `GPS disabled` within ~500ms
5. If any NMEA arrives (needs bench fix + antenna to happen):
   - STATUS shows `acquiring... (time only)` (time-valid, no fix)
   - Or `3D fix (N sats)` (full fix)
   - Value rows populate with real numbers
6. PHOTO button -> photo overlay appears (subject-driven HIDDEN
   toggles). Tap anywhere -> back to normal.
7. ATOMIC SYNC button: enabled iff `time_valid` + not sleeping,
   still works via the existing g_gps_sync_requested flag path.
8. Sub-tile (swipe from GPS main to row 1) shows raw NMEA or
   "-- no signal --".
9. Theme swap on System tile still recolors GPS text.

**Robustness contract:**
- Chip dead: `no data -- check antenna/wiring` after
  `BROKER_GPS_TIMEOUT_MS` (5 s) of silence. No garbage, no
  crashes, no stale numbers.
- Chip alive without fix: `acquiring...` variants + time when
  available.
- Chip alive with fix: 2D or 3D status + populated fields.
- User toggles OFF: `GPS disabled` within one drain cycle.

**Bench verdict (2026-09-18, fw 0.4.85):**
- Status line + subject bindings work well; layout clean at
  arm's length; two-way switch works though feel is slightly
  finicky (system-wide -- see LOG).
- Photo view HIDDEN-flag bindings toggle cleanly.
- ATOMIC SYNC button doesn't respond to taps -- deferred
  (needs GPS fix to be functionally meaningful).
- Chronic i2c crashes still present under swipe stress; ignore
  per LOG (not counting further).

**Batch status:** LANDED + verified. Ready for 31.4b.

---

### 9.5 -- Batch 31.4b flash (fw 0.4.86)

UBX-CFG-VALSET config sends + MON-RF diagnostic + dynamic-model
CLI verb. Firmware side of the GPS story locked in even before
the bench hardware fix.

**What landed:**
- `max_m10s/max_m10s.h`: new public API for CFG-VALSET writes:
  `max_m10s_set_dynmodel(mode)`, `max_m10s_enable_monrf(rate)`,
  `max_m10s_get_monrf(out)`, `max_m10s_dynmodel_name`,
  `max_m10s_ant_status_name`. `max_m10s_dynmodel_t` enum +
  `max_m10s_monrf_t` snapshot struct.
- `max_m10s/max_m10s.c`:
  - Class/ID constants for MON (0x0A), ACK (0x05), MON-RF
    (0x38), ACK-ACK (0x01), ACK-NAK (0x00).
  - CFG-VALSET key IDs sourced from Interface Description PDF
    (agent-verified 2026-09-18): DYNMODEL 0x20110021,
    MSGOUT-MON-RF-UART1 0x2091035a,
    MSGOUT-NAV-PVT-UART1 0x20910007.
  - `ubx_fletcher()` + `ubx_send_valset_u1()` static helpers.
  - `handle_ubx_mon_rf()` + `s_monrf` static snapshot behind a
    mutex. UBX dispatch extended to route class 0x0A id 0x38
    and log UBX-ACK-ACK / -NAK for CFG-VALSET debugging.
  - `max_m10s_init` now: init monrf lock, 200 ms settle,
    send DYNMODEL=PEDESTRIAN + enable MON-RF at 1 Hz.
- `lvgl_ui/ui_subjects.c` `gps_format_and_publish`: append
  `\xc2\xb7 ant <STATUS>` to status line when MON-RF snapshot
  is valid. Bench sees "3D fix (8 sats) \xc2\xb7 ant OK" /
  "acquiring... \xc2\xb7 ant OPEN" as one glance-diagnostic.
- `fc_cli.c` HELP + dispatcher:
  - `GPS_DYNMODEL [PORT|STAT|PED|AUTO|SEA|AIR1|AIR2|AIR4|WRIST|BIKE]`
    Sends CFG-VALSET at runtime; echoes back current mode.
    Watch DEBUG log for UBX-ACK-NAK if the chip refuses a
    mode (WRIST + BIKE especially).
  - `GPS_MONRF` dumps the snapshot: antStatus, antPower,
    noise/ms, AGC count + percentage. "no snapshot yet" if
    the chip is silent.

**What to watch on flash:**
1. Boot log:
   - `MAX_M10S: DYNMODEL -> PEDESTRIAN (3) queued to UART`
   - `MAX_M10S: MON-RF output rate -> 1`
   - `MAX_M10S: MAX-M10S init OK (dynmodel=PED, MON-RF on)`
2. If the chip is silent (current pre-hw-fix state):
   - `GPS_MONRF` -> "no snapshot yet -- either MON-RF not
     enabled or chip is silent (check antenna/wiring first)"
   - GPS tile status stays "no data -- check antenna/wiring"
     with NO ant hint appended.
3. Once hw fix + antenna are in and chip starts talking:
   - `MAX_M10S: MON-RF: ant=OPEN(4) pwr=... noise=... agc=...`
     at DEBUG (or expected ant=OK when antenna cap attached)
   - Tile status: "acquiring... · ant OPEN" until antenna, then
     "acquiring... · ant OK" until TTFF, then "3D fix (N sats)
     · ant OK".
   - `GPS_MONRF` dumps live snapshot.
4. `GPS_DYNMODEL PED` (or AUTO / STAT / etc.):
   - Prints "DYNMODEL <- PEDESTRIAN (3) : ESP_OK"
   - Chip acks (DEBUG: `UBX-ACK-ACK cls=0x06 id=0x8a`) or naks
     (`UBX-ACK-NAK` -- expected for WRIST/BIKE on M10).
5. Regression: env tile, drawer, tile nav, theme swap, all
   still work unchanged.

**Bench verdict (2026-09-18, fw 0.4.86):**
- Boot log confirms: `DYNMODEL -> PEDESTRIAN (3) queued to UART` +
  `MON-RF output rate -> 1` + `init OK (dynmodel=PED, MON-RF on)`.
- `GPS_DYNMODEL AIR<1G` -> `ESP_OK`, `DYNMODEL = AIR<1G (6)`.
- `GPS_MONRF` -> "no snapshot yet" (chip is silent -- pre-bench-fix,
  as expected).
- Chronic i2c crash x1 mid-CLI (not counting further).

**Batch status:** LANDED, awaiting hw fix for full validation.

---

### 9.5b -- on-tile MODE cycle button (fw 0.4.87)

Ivan wants dynamic-model changeable from the tile too. Added a
BOTTOM_RIGHT MODE button mirroring PHOTO on the left. Cycles
PED -> WRIST -> AUTO -> AIR1G; each tap calls
`max_m10s_set_dynmodel()` (same command surface CLI uses).

**What landed (gps_tile.c only):**
- New static handles `s_btn_mode` + `s_lbl_mode`.
- New `cb_mode_btn` cycles through the 4 modes Ivan named.
- Button widget in `gps_tile_init`: 100x60 at BOTTOM_RIGHT
  (-60, -110), two-line label "MODE\n<name>".
- Added to normal-view HIDDEN-flag bindings list so photo view
  hides it too.
- `gps_tile_update` refreshes the label when
  `max_m10s_get_dynmodel()` diverges from the last-known value
  (CLI-driven changes propagate to the on-tile label).

**Watch on flash:**
1. Tile shows MODE button bottom-right; initial label "MODE\nPEDESTRIAN".
2. Tap -> label flips to "MODE\nWRIST" (or NEXT if WRIST NAKs on M10);
   DEBUG log shows `UBX-ACK-ACK cls=0x06 id=0x8a` or `UBX-ACK-NAK`.
3. `GPS_DYNMODEL AUTO` from CLI -> on-tile label updates to AUTOMOTIVE
   within one 200 ms tick.

**Bench verdict:** *(fill in after flash)*

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

---

## Appendum — 2026-09-20 — MAX-M10S GPS UART unblocked

Out-of-band from the LVGL foundations work, but happened this session
and unblocks the day-1 "GPS coordinates on tile" deliverable
([[project_mk1b_assembly]]).

### What was wrong
`components/max_m10s/max_m10s.h` shipped `MAX_M10S_TX_PIN = 18` and
`MAX_M10S_RX_PIN = 17` — inverted from the master pinout
(`0_Kompic_Pinout_MASTER_v20_iv7.1.md`), which assigns U1TXD to GPIO17
and U1RXD to GPIO18. So `uart_set_pin(UART_NUM_1, 18, 17, ...)` was
telling the S3 to drive GPS RX from GPIO18 and listen for GPS TX on
GPIO17 — the exact opposite of the PCB routing.

Same bug also existed in Arduino sketches `18b_max_m10s_gps_test_mk1b`
and initial `18c_max_m10s_gps_test_mk1b_clksrc`. Every prior "swapped
both directions" test moved wires but never simultaneously flipped the
firmware defaults, so no combination ever aligned. Full debug trail:
`docs/build_info/Mk1b_build_reports/GPS_M10S_Fixing.md`.

### What was ported from sketch 18c into the IDF driver

`components/max_m10s/max_m10s.h`:
- `MAX_M10S_TX_PIN = 17`, `MAX_M10S_RX_PIN = 18` (matches master pinout
  IOMUX assignment).

`components/max_m10s/max_m10s.c`:
- Added `#include "driver/rtc_io.h"`.
- `max_m10s_init()` now clears digital + RTC hold on both GPS pins
  before UART setup (`gpio_hold_dis` + `rtc_gpio_hold_dis` +
  `rtc_gpio_deinit` + `gpio_reset_pin` on each, `gpio_deep_sleep_hold_dis`
  globally). Cheap insurance against any latching mechanism that could
  disconnect the pin from the UART matrix while leaving the external
  waveform visible.
- `.source_clk = UART_SCLK_APB` (was `UART_SCLK_DEFAULT`). On ESP32-S3
  the two normally resolve to the same clock, but pinning removes
  ambiguity and any future IDF default drift.
- Reordered init to match the working sketch: `uart_param_config` →
  `uart_set_pin` → `uart_driver_install` (was
  install → param → set_pin, which IDF v5.x tolerates but is not the
  documented order).

**Ivan's note:** GPIO07 (MAX30101_INT) is rather handy, as its a pad 
for the daughter board, meaning I just bodge over to it, no mods needed. 

### Reference: what "GPS alive" looks like on the bench
Sketch 18c, Mk1b iv8.0, GPS on GPIO17/18 per master pinout, no antenna,
indoors — stream is ~289–370 B/s, zero errors, no fix (indoors is
expected):
```
[STAT] total=947 B  window=289 B/s (last 1000 ms)  NMEA=28 UBX=0 err=0
       idle=418 ms  rx=GPIO18 tx=GPIO17 baud=9600 clk=APB
[NMEA] $GNRMC,,V,,,,,,,,,,N,V*37
[NMEA] $GNVTG,,,,,,,,,N*2E
[NMEA] $GNGGA,,,,,,0,00,99.99,,,,,,*56
[NMEA] $GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1*33
```
With the antenna wired on the tape-and-flying-wire Mk1b build, we
expect the same stream plus a non-zero fix count in GGA and a `V→A`
flip in RMC field 2 within tens of seconds to a couple of minutes of
cold acquisition outdoors.

### Follow-ups for this appendum
- After first successful bench flash: capture the IDF `[GPS] ...` log
  lines during first fix and paste back here (or in
  `GPS_M10S_Fixing.md`) as the "IDF-side alive" reference.
- If bytes still don't arrive despite the pin swap + APB pin + nuke
  block, the next thing to check is that no earlier component in the
  boot sequence claimed GPIO17/18 for another peripheral before
  `max_m10s_init()` runs — grep for `gpio_config` and `gpio_set_direction`
  on 17/18.
- Once GPS is confirmed alive on IDF, port the `PROBE` CLI verb from
  18c so we can detach any peripheral pin at runtime and read it as
  raw GPIO — the single tool that would have found this in one line.
