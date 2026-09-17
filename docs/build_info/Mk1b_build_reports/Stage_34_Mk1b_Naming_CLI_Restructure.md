# Stage 34 -- Mk1b naming + CLI restructure + command-surface consolidation

**Date opened:** 2026-09-14
**Board:** Mk1b iv8.0.
**Firmware baseline:** TBD -- follows Stage 33 close (LVGL foundations
rebuild complete, `ui_navigation.c` deleted, per-tile `apply_theme` cbs
gone).
**Status:** OPEN, queued behind Stages 30 → 31 → 32 → 33.

**Companion refs:**
- `docs/build_info/Module_Blueprint.md` (existing) -- codifies the
  command-surface / two-outlet contract that this stage EXTENDS.
- `Stage_31_Mk1b_LVGL_Foundations_Rebuild.md` §1 -- 3-stage rebuild
  roadmap; this is the 4th stage appended after Ivan asked to fold
  naming + CLI cleanup into the same rewrite window.

---

## 1. Why this stage exists

The LVGL foundations rebuild (Stages 31-33) rewrites the display side of
the app. During that rewrite we necessarily touch many files. Two
long-standing structural debts naturally piggyback:

1. **`field_capture` is a legacy name.** The component was originally
   named for the "capture sensor readings during a field trip" mode. Its
   scope has drifted -- today it owns modes, activity/sleep, CLI dispatch,
   recording writers, RGB LED mode indication, and half the app's runtime
   state. The name is a landmine for new readers and for future-us. Naming
   it correctly is a one-time mechanical refactor; delaying it means every
   later stage keeps referencing an inaccurate module name.

2. **CLI lives inside `field_capture/fc_cli.c`.** That was expedient
   during Stage 15 when the CLI existed to test capture modes. Today the
   CLI reaches every module -- BQ / haptic / GPS / display / LSM / RTC /
   NVS / tiles. It is a **watch-wide surface**, not a field_capture
   subsystem. Living inside field_capture forces every CLI-touching
   change to import that component; forces every new module's CLI verbs
   to route through a component that has nothing to do with them; and
   pollutes the abstraction.

Both cleanups also unlock:

3. **HELP menu restructure per [[project_cli_submenus]]** -- top-level
   HELP is currently a wall of ~30 verbs. Wants to be ≤12 top-level +
   submenus (DEV / REC / UI / SYS). Cleanest done when the dispatcher
   moves anyway.

4. **Command-surface consolidation** -- Module_Blueprint.md already says
   "CLI verb + UI button both call the module's command surface". A few
   modules do this (GPS PHOTO view was the pilot). Most don't. Ivan's
   design intent: **as much as possible on both surfaces, so their
   tie-in is a single dispatch point**. This stage makes that the
   universal pattern, extending Module_Blueprint.md accordingly.

**Nothing here changes hardware behavior or LVGL shape.** All changes
are naming + code organization.

---

## 2. Stage 34 scope

Four batches, dependency-ordered.

- **34.1** -- rename `field_capture` -> `app_core` AND `boot_logic` ->
  `app_boot` (mass rename, both in one atomic pass)
- **34.2** -- lift CLI to top-level `components/app_cli/`
- **34.3** -- HELP menu top-level ≤12 + submenus per
  [[project_cli_submenus]]
- **34.4** -- command-surface consolidation sweep + Module_Blueprint
  update

34.1 is the biggest blast radius (dozens of files, mechanical) but
zero decision-making -- both renames ride the same sed pass and the
same green-build gate. 34.2 depends on 34.1 (CLI verbs reference
`app_core` / `app_boot` strings in HELP output). 34.3 lives naturally
inside 34.2's dispatcher. 34.4 sweeps the rest.

**Naming convention codified this stage:** app-layer top-level
components use the `app_*` prefix. Current family: `app_core`,
`app_nvs`, `app_cli`, `app_boot`. `lvgl_ui` deliberately keeps the
tech-first name -- Ivan wants LVGL visible up-front on that one.
Hardware drivers stay chip-named (`bq25619`, `co5300`, etc.). See
new memory [[project_app_naming_convention]].

**New name:** `app_core` (locked in 2026-09-14). Reads as the sibling
of `app_nvs`; describes the component as the app's runtime core;
prefix `app_core_*` reads cleanly for functions, `APP_CORE_*` for
enums, tag `"APP"` for logs. All `<app>` placeholders in the rest of
this doc mean `app_core`.

**Shortlist that was considered (for the record):**

| Candidate | Fit | Vibe |
|-----------|-----|------|
| **`app_core`** (chosen) | Sibling of `app_nvs`; describes it as the app's runtime core. | Idiomatic. |
| `session` | "Watch is awake and running a mode." Wearable metaphor. | Poetic. |
| `runtime` | Technical, unambiguous. `runtime_kick_activity()` reads. | Neutral. |
| `watch_ctrl` / `wctl` | Explicit watch-level controller. | Descriptive. |
| `activity` | Wearable-industry vocab (Fitbit/Apple). But could collide with LSM activity events. | Risky. |

---

## 3. Batch 34.1 -- Rename `field_capture` -> `app_core` + `boot_logic` -> `app_boot`

**Goal:** mechanical rename across the tree, both components in one
atomic pass. Zero behavioral change. Bundled because both are grep-
and-sed operations that share the same green-build gate, and doing
them separately would double the "no half-migrated tree in git" cost.

### What lands -- `field_capture` -> `app_core`

- Directory rename: `firmware/esp-idf/components/field_capture/` ->
  `firmware/esp-idf/components/app_core/`.
- File renames within:
  - `field_capture.c/.h` -> `app_core.c/.h`
  - `fc_common.c` -> `app_core_common.c`
  - `fc_modes.c` -> `app_core_modes.c`
  - `fc_modes_lsm.c` -> `app_core_modes_lsm.c`
  - `fc_battery_test.c` -> `app_core_battery_test.c`
  - `fc_internal.h` -> `app_core_internal.h`
  - `fc_cli.c` -> stays here for 34.1 (moves in 34.2)
  - `firmware_version.h` -> stays.
- Symbol renames across the WHOLE tree:
  - `field_capture_*` -> `app_core_*` (functions + types)
  - `FIELD_CAPTURE_*` -> `APP_CORE_*` (macros + defines)
  - `fc_*` -> `app_core_*` (internal helpers)
  - `FC_*` -> `APP_CORE_*` (mode enum values, etc.) -- **carefully**
    since some `FC_MODE_*` names might read chatty under `APP_CORE_MODE_*`;
    audit per-name and either accept the length or rename the enum family
    to something shorter (e.g. `MODE_TEMP` alone if unambiguous in scope).
- `CMakeLists.txt` component name updates in every consumer.
- `#include "field_capture.h"` -> `#include "app_core.h"` in every consumer.
- Log tags: `TAG = "FIELD"` (or wherever) -> `TAG = "APP"`. Keep the
  short-word discipline (bench logs get parsed by eye).
- CLI verb strings: e.g. `FC MODE X` -> pick a new short verb; typically
  drop the prefix entirely since the CLI is being lifted anyway (34.2).

### What lands -- `boot_logic` -> `app_boot`

- Directory rename: `firmware/esp-idf/components/boot_logic/` ->
  `firmware/esp-idf/components/app_boot/`.
- File renames within:
  - `boot_display.c/.h` -> `app_boot_display.c/.h`
  - `boot_hw_init.c` -> `app_boot_hw_init.c`
  - `boot_tasks.c` -> `app_boot_tasks.c`
  - Any additional `boot_*` files pick up the same `app_boot_*` prefix.
- Symbol renames across the WHOLE tree:
  - `boot_display_*` / `boot_hw_init_*` / `boot_tasks_*` public API ->
    keep the short name, DO NOT bloat to `app_boot_display_*` unless
    grep collides. Audit: is there any other `boot_display_*` symbol
    outside this component? If not, leave the short name; the
    component-name prefix is enough for identification.
- `CMakeLists.txt` component name in every consumer (`REQUIRES boot_logic`
  -> `REQUIRES app_boot`).
- `#include "boot_display.h"` etc. -- paths change if headers moved,
  but names stay stable to minimize churn.
- Log tag: whatever it currently is -> `"BOOT"` (already short and good).

The `boot_*` short symbol prefix inside the renamed component is a
deliberate exception to the `app_boot_*` convention -- boot symbols
are used widely and adding the `app_` prefix everywhere they appear
would make bench log lines unnecessarily long. The component name
carries the `app_` prefix; the symbols carry the shorter `boot_`
prefix. Documented as an exception in
[[project_app_naming_convention]].

### What does NOT change

- FreeRTOS task names -- keep whatever they are; renaming is optional
  and doesn't add value.
- The mode enum VALUES -- only the enum NAMES + type name. This keeps
  the RGB LED mnemonic table untouched.
- Any binary-persisted data (NVS keys, PCF RAM byte codes). Bench units
  in the wild would fail to boot if we changed those.

### Why now

- **Blast radius shrinks over time.** Every new tile / mode / stage
  that references `field_capture_kick_activity()` is one more find-and-
  replace after the rename. Doing it now, before Stage 32 / 33 add
  more consumers via the LVGL rewrite, keeps the sed pass finite.
- **Grep-quality improvements.** `grep field_capture` today returns
  ~250 hits. Post-rename, everything named `<app>` describes the actual
  responsibility. Future onboarding is materially easier.

### Close-second: piecewise rename over multiple stages

Rejected. A mid-rename tree is confusing to work in (some files use
old name, some new) and slows every unrelated bench cycle. One-shot
mechanical rename with a green build gate is safer.

### Close-second: rename in a separate branch, merge at end

Considered. Per our workflow ([[feedback_commits]]) Ivan drives commits,
and we work on `main`. Long-lived branches slow iteration on other
stages. Do it in-place with clear commit boundaries.

### Success gate

1. `idf.py build` clean.
2. `grep -r field_capture firmware/` returns 0 matches (in code; docs
   deliberately keep the historical name for context).
3. `grep -r "fc_" firmware/` also 0 (or only false positives on
   unrelated identifiers).
4. `grep -r boot_logic firmware/` returns 0.
5. Bench boot + flash: STATUS + HELP still print; a couple of common
   CLI verbs still work; boot sequence still runs to completion with
   the same log-line count as pre-rename.

### Effort

50-80 files touched, all mechanical. One session (both renames together).
The audit for `fc_*` false-positives is the only manual bit; `boot_*`
symbols already have enough context in-name that they don't need
per-name review.

---

## 4. Batch 34.2 -- Lift CLI to `components/app_cli/`

**Goal:** CLI dispatcher becomes a watch-level component. `app_core`
becomes one CLI consumer among many, not the CLI's home. Named
`app_cli` to fit the `app_*` family established in 34.1.

### What lands

- New component: `firmware/esp-idf/components/app_cli/`
  - `app_cli.c/h` -- top-level dispatcher, UART/USB-CDC reader,
    prompt + history + backspace handling.
  - `app_cli_verb.h` -- verb registration API:
    ```c
    typedef struct {
        const char *name;               /* e.g. "STATUS" */
        const char *submenu;            /* NULL for top-level, else "DEV"/"REC"/"UI"/"SYS" */
        const char *summary;            /* one-liner for HELP */
        app_cli_err_t (*handler)(int argc, char **argv, app_cli_out_t *out);
    } app_cli_verb_t;
    void app_cli_register(const app_cli_verb_t *verb);
    ```
  - `app_cli_out.h` -- output abstraction so UI buttons can dispatch
    the same verbs and capture output for a screen widget (see 34.4).
- Modified: `app_core` (renamed field_capture) -- keeps its own verbs
  (`MODE`, `RECORD`, `KICK`, ...), just calls `cli_register()` from
  its init.
- Modified: every module that currently has verbs baked into
  `fc_cli.c` -- moves ITS verbs INTO its own component's `foo_cli.c`
  file, which calls `app_cli_register()` from `foo_init()`.
  - e.g. `components/bq25619/bq25619_cli.c` owns `BAT`, `SHIPMODE`,
    `CHARGE` verbs.
  - `components/max_m10s/gps_cli.c` owns `GPS_*` verbs.
  - `components/co5300/display_cli.c` owns `TILE`, `TOUCH`, `LVGL_FORCE`,
    `GPS_VIEW`.
  - `components/app_nvs/nvs_cli.c` owns `NVS_GET`, `NVS_SET`, `NVS_LIST`
    (new -- see §6).
- Modified: `app_boot` (renamed boot_logic) / `main` -- calls
  `app_cli_init()` once; each module's init call is unchanged
  (register happens as a side effect of module init).

### Close-second: keep CLI in `app_core`, just factor its dispatcher

Rejected -- doesn't fix the "watch-wide CLI lives in a subsystem"
inversion. Also doesn't let per-module CLI files sit next to their
drivers.

### Why this shape

- **Each module owns its own CLI surface.** Adding a new sensor: put
  its verbs in `newsensor_cli.c`, call `cli_register()` in its init.
  Nothing in a central dispatcher to edit.
- **Verb registration is a runtime call, not a table.** Avoids the
  ugly "central switch statement" and the WHOLE_ARCHIVE trick per
  [[feedback_whole_archive_cmd_extract]] for archive-scoped verbs --
  the module's `_init()` is already reachable so the registration
  runs naturally.
- **`cli_out_t` abstraction** decouples the dispatcher from the
  output sink. Today: UART line printer. Tomorrow: UI overlay panel
  that shows CLI output on screen when a UI button dispatches a
  verb.

### Close-second: replace the CLI runtime with a MicroPython REPL

Rejected -- outsized. Nice thought experiment for Mk2+.

### Success gate

1. `app_cli` component builds standalone.
2. Every current verb still works from `HELP` and typed at the prompt.
3. Removing `app_core` from the build (hypothetical) leaves `app_cli`
   + sensor-verbs still functional -- proves the decoupling.
4. STATUS + HELP + REBOOT + SHIPMODE at the prompt behave identically
   to pre-34.2 (bench regression).

### Effort

New component + ~10 files touched to migrate verbs. One session,
maybe two if we do the full migration in one shot -- can be split
into 34.2a (framework + `app_core`'s verbs) and 34.2b (each other
module's verbs, opportunistically per venue) if it feels big.

---

## 5. Batch 34.3 -- HELP menu top-level ≤12 + submenus

**Goal:** first `HELP` a user sees is ≤12 items, most of them submenus.
Per [[project_cli_submenus]].

### Target top-level menu

Exactly 12 items, in this order:

```
HELP        show this help; HELP <submenu> for details
STATUS      one-line status of every subsystem
VER         firmware + hardware version banner
REBOOT      soft reboot
SHUTDOWN    long-press-equivalent shutdown flow
SHIPMODE    BQ ship-mode; battery is permanently attached
TIME        show current time; TIME SET <iso> to set
SD          mount / unmount / list SD card
TILE        list / jump tiles
THEME       DARK / LIGHT / next
MODE        show / switch capture mode
LOG         log level get/set/dump
```

### Submenus

Typed as `HELP DEV`, `HELP REC`, etc.

- **DEV** -- per-device verbs: `GPS_*`, `BAT`, `RTC_*`, `HAPTIC_*`,
  `LIGHT_*`, `ENV_*`, `IMU_*`, `MAG_*`, `PPG_*`.
- **REC** -- recording verbs: `RECORD_START`, `RECORD_STOP`,
  `RECORD_STATUS`.
- **UI** -- UI-facing verbs: `LVGL_FORCE`, `TOUCH`, `GPS_VIEW`,
  `TILE_LIST`, `THEME_*`.
- **SYS** -- system verbs: `NVS_*`, `TASKS`, `HEAP`, `MEM`, `PART`.

### Why this shape

- **≤12 fits on-screen without scroll** at a typical serial console
  width. Ivan can eyeball the whole thing at a glance.
- **Submenus preserve discoverability.** `HELP DEV` and `HELP SYS`
  are one word each; the tree is shallow.
- **STATUS + HELP + REBOOT + SHIPMODE stay top-level** because those
  are the four verbs used most often; asking Ivan to type
  `SYS SHIPMODE` on a live bench would be user-hostile.

### Close-second: alphabetical flat list, larger terminal

Rejected -- Ivan's serial console setup is what it is, and the wall-of-
verbs slows onboarding.

### Close-second: fuzzy verb matching / auto-complete

Considered. Nice but not first. Add after 34.3 lands if it feels worth
the complexity.

### Success gate

1. `HELP` at the prompt shows exactly the 12 top-level items above.
2. `HELP DEV` etc. show only the submenu's verbs.
3. Every previously-working typed verb still dispatches (submenus are
   organizational; verbs are not renamed just because they moved).

### Effort

Trivial addition to 34.2's dispatcher. One session, likely rolls in
with 34.2's bench cycle.

---

## 6. Batch 34.4 -- Command-surface consolidation + UI-as-CLI-dispatch

**Goal:** extend Module_Blueprint.md so that CLI verbs and UI buttons
are structurally the same call, not just "share a helper function".

### Design intent (Ivan, this session)

> "I want most of the CLI action to be buttons as well, CLI primarily,
> buttons another way to do it instead of typing. Some UI will be UI
> only, some CLI will be CLI only, but in general, I want as much as
> possible on both -- ultimate control. So their tie-in should be as
> clean and light as possible, so not to clutter with excess, like
> pressing a specific button literally just executes the commands to
> the CLI for us."

### Two options for the tie-in mechanism

Both land the same UX. Pick per-venue based on cost.

**Option A -- Direct handler dispatch (fast path):**

```c
/* Module registers ONE handler surface used by CLI + UI. */
cli_err_t bq_cmd_shipmode(int argc, char **argv, cli_out_t *out);

/* CLI verb (in bq25619_cli.c): */
static const cli_verb_t bq_verbs[] = {
    { "SHIPMODE", NULL, "enter BQ ship mode", bq_cmd_shipmode },
    /* ... */
};

/* UI button (in battery_tile.c): */
static void ship_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bq_cmd_shipmode(0, NULL, cli_out_null());   /* discard output */
}
```

Simplest, fastest, no string parsing on the UI side.

**Option B -- UI button emits a CLI string (uniform path):**

```c
static void ship_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cli_dispatch_line("SHIPMODE", cli_out_ui_overlay(g_output_overlay));
}
```

Slower (parser roundtrip) but has the beautiful property that **every
UI action leaves a traceable CLI-verb footprint** -- useful for logging,
for scripting, for macro recording. Output can be captured into an
on-screen overlay panel so the user sees exactly what happened.

### Recommended pattern

**Direct handler dispatch (Option A) by default.** Option B available
for verbose / discoverable actions where showing the CLI verb is a
feature (e.g. an on-screen "console" panel that logs every button
press as a CLI line). Ivan can toggle overlay-visible-command as a UI
setting.

### Ancillary work in this batch

- **`app_nvs` gets a full CLI surface:** `NVS_GET <key>`, `NVS_SET <key>
  <value>`, `NVS_LIST`, `NVS_DELETE <key>`, `NVS_ERASE`. Every field
  in `ui_settings_t` and every mode-persistent value is readable +
  settable from CLI. Per Ivan's directive this session.
- **Every existing tile audits** whether its buttons dispatch through
  the command surface or hand-roll a call. Migrate as encountered.
- **Module_Blueprint.md** updated with a new §: "Two-way rule: any
  action reachable from a UI element MUST also be reachable from a CLI
  verb, and vice versa, EXCEPT for UI-only affordances (gestures,
  brightness slider live drag) and CLI-only affordances (verbose
  hex dumps, one-shot diagnostics)."

### Success gate

1. `NVS_LIST` returns every persisted key + value.
2. `NVS_SET theme LIGHT` -> theme swap on screen within one refresh
   (via the subject-drain from Stage 31.2).
3. At least three tile buttons demonstrably invoke `foo_cmd_*` and the
   CLI verbs invoke the SAME function (audit via grep for duplicated
   logic).
4. Module_Blueprint.md updated with the two-way rule + a code snippet
   showing the tie-in.

### Effort

Small dispatcher change + ~5-10 tile audits + doc update. One session.
The Module_Blueprint.md update is a natural home for lessons from this
whole rewrite window.

---

## 7. Non-goals for Stage 34

- Rewriting mode logic. Mode semantics stay identical; only the
  containing component's name changes.
- BLE / WiFi commands -- radios are stubbed on Mk1b per
  [[project_radios_stubbed_until_mk2]]. Add when the radio wakes.
- Macro / script recording. Attractive follow-on to Option B in 34.4;
  not this stage.
- Auto-complete / fuzzy verb match -- 34.3 non-goal per §5.
- CLI over BLE. When radio comes back, another output sink for `cli.c`.

---

## 8. Definition of done for Stage 34

1. `field_capture` fully renamed to `app_core`; grep -r returns 0.
2. `components/cli/` exists; every module registers its own verbs via
   `cli_register()`; `app_core` is one consumer, not the host.
3. `HELP` at the prompt shows exactly 12 top-level items; submenus
   populated per §5.
4. At least the batteries + display + NVS modules demonstrably wire
   their UI buttons to their `_cmd_*` handlers (proving the pattern
   for the rest to follow opportunistically).
5. Module_Blueprint.md updated with the two-way rule.
6. No regression in: boot, sensor sampling, sleep, charging, tile
   navigation, theme swap, alarm ring, subject-bound tiles from
   Stage 32.

---

## 9. Bench log (fill in as each batch lands)

*(populated as we go, per [[feedback_stage_log_workflow]])*

---

## 10. Next-steps hook -- Stage 35+

- **BLE bring-up** if antenna ever lands on Mk1b (unlikely) -- new
  output sink for `cli.c`, new subject publisher for connection state.
- **Macro / script recording** via Option B in §6 -- log every CLI
  dispatch to a ring buffer, replayable.
- **Full docs pass** during Mk2 PCB fab wait -- collapse this + every
  stage doc + Mk1_build_reports + Mk1b_build_reports + Mk2_intel/*
  into ~100 pp readable Mk2 design brief (per Ivan's plan this session).
- **Fuzzy verb match / auto-complete** if we're feeling luxurious.
- **Encoder tile redesign** per [[project_encoder_tile_redesign]] --
  probably lands as its own stage since the encoder is under-used and
  Ivan has design intent to develop.

---

## 11. References

- `docs/build_info/Module_Blueprint.md` -- existing two-outlet contract;
  extended by 34.4.
- `Stage_30_Mk1b_Display_Port_Migration.md` -- foundation port work.
- `Stage_31_Mk1b_LVGL_Foundations_Rebuild.md` -- LVGL architecture
  rebuild; this stage completes the rewrite window.
- `docs/build_info/Mk2_intel/mk1_mk1b_experiences_for_mk2.md` -- Mk2
  intel jotting.
- Auto-memory:
  [[project_kompic_mk1]],
  [[project_cli_submenus]],
  [[feedback_cli_first_testability]],
  [[feedback_group_work_by_venue]],
  [[feedback_flash_per_checkpoint]],
  [[feedback_stage_log_workflow]],
  [[feedback_mk1b_stage_naming]],
  [[feedback_whole_archive_cmd_extract]],
  [[feedback_commits]],
  [[project_radios_stubbed_until_mk2]] (new this session),
  [[project_display_corner_cutoff]] (new this session),
  [[project_cli_ui_two_way_pattern]] (new this session).
