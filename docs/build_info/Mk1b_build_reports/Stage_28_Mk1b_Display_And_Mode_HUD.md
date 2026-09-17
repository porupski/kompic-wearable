# Stage 28 -- Mk1b display polish + mode HUD reconciliation

**Date opened:** 2026-09-13
**Board:** Mk1b iv8.0 (assembled, panel mated, encoder + button + LRA + touch
+ SD all verified in Stage 27).
**Firmware baseline:** `0.4.50` (Stage 27 close: DISP verb, 15 s auto-
sleep, wake gates in place with known bugs, TS enabled, fuel-gauge as
`bat.voltage` + `bat.percentage`, MAX-M10S GPS still silent).
**Status:** OPEN.

---

## 1. Why this stage exists

Stage 27 closed with hardware bring-up done: every peripheral has a CLI
verb, the panel came up in portrait, the sensor stack + fuel gauge feed
the tiles, and the board runs headless-clean on a real cell. But the
display experience is still rough in four concrete ways that block
using the watch as a wearable rather than a bench object:

  1. **Render + landscape.** Panel is portrait, text looks "close but
     not truly correct" (color mixing / font AA off), and the correct
     wearing orientation is landscape rotated 90° CW per
     [[project_display_landscape_direction]]. Stage 27 §4.6 flagged
     LV_COLOR_DEPTH 16 -> 32 as the first thing to try before
     assuming panel calibration issues; Stage 25 Batch H tried MADCTL
     0x60 and got green bars, so landscape needs a different path
     than "just flip the panel bits".
  2. **Time on the main screen is tiny.** Currently Montserrat 28 in
     `ui_main_screen.c:69`. Ivan wants **at least 4× bigger** so the
     watch face is glanceable at arm's length. Kconfig already has
     Montserrat sizes up to 48 enabled.
  3. **Screen off/on isn't practical in the field.** Two Stage 27
     §4.10 bugs are still live:
       * Button press does NOT wake the panel (was supposed to per
         Batch 27.6).
       * Touch wakes on a SINGLE finger-down, not the intended
         500-ms double-tap.
     Also: Ivan can't confirm the 15-s auto-sleep is firing (he's
     been on USB power the whole time). Auto-sleep needs a
     bench-observable sanity check.
  4. **Encoder ↔ screen disconnect.** Encoder cycles the internal
     `s_mode` (FCM_*) + fires the RGB LED accordingly, but the screen
     shows no indication of which mode is selected. Sub-menus (LSM
     branch) are equally invisible. Ivan's spec: rotating the encoder
     should update an on-screen HUD showing the current mode name +
     submenu + RGB color mnemonic; pressing (button, since encoder
     has no click) also dumps mode info to the screen. **Robust
     before pretty** — this is a functional HUD first, aesthetics
     come after.

None of these need new hardware; all four are firmware polish on the
now-assembled Mk1b. Per [[feedback_flash_per_checkpoint]] we land one
batch per bench session and let Ivan verify before piling on.

---

## 2. Plan (concrete steps, per-item hypothesis + fix)

Ordered by "cheapest to try / most useful to verify first". Ivan flashes
after each numbered batch, we log the result in §4 in place, then move
to the next.

### 2.1 Batch 28.1 -- Big clock on the main screen (cheapest win)

**Symptom:** The time reads `HH:MM` in Montserrat 28 at the vertical
centre with `-30 px` offset, roughly ~45 px glyph height on a 502 px
tall panel. Too small.

**Hypothesis:** No hidden reason — the file just uses `UI_FONT_TITLE`
(Montserrat 28). Kconfig already has Montserrat 48 built in, so the
font is available without a Kconfig rebuild.

**Proposed fix (single file):**
  - `ui_theme_colors.h`: add a new `UI_FONT_TIME_XL` alias pointing at
    `&lv_font_montserrat_48` (roughly 4× the pixel area of 28) — a
    dedicated alias keeps other consumers of `UI_FONT_TITLE` (which
    the theme uses for tile titles) unchanged.
  - `ui_main_screen.c:69`: swap `UI_FONT_TITLE` -> `UI_FONT_TIME_XL`
    on the `s_lbl_time` label.
  - `ui_main_screen.c:71`: bump the vertical offset from `-30` to
    something like `-60` so the bigger glyph clears the date label
    below.
  - Consider bumping `UI_FONT_LABEL` (date) too if the ratio looks
    off after the time bump — but do the time first and eyeball
    proportions before touching the date.

**Success gate:** Ivan flashes, main screen shows `HH:MM` roughly 4×
its current size, date + battery labels don't overlap, other tiles
untouched.

**Bench flash:** independent of every other batch in this stage — land
this first so Ivan has something visible on the panel while we work on
the harder items.

---

### 2.2 Batch 28.2 -- Wake gate fixes (button + touch)

**Symptom A -- button doesn't wake:**
`fc_common.c:225` `button_poll()` currently gates wake at
`fc_common.c:291` on `if (event != 0)`. Trace: press → line 245 fires
`haptic_play(DRV_STRONG_CLICK)` immediately (this is Ivan's diagnostic
per Stage 27 §4.10) → sets `s_btn_state = BTN_PRESSED` but emits NO
event yet. Release → `s_btn_state = BTN_WAIT_DBL`, still no event.
Only after `BTN_DOUBLE_GAP_MS` timeout at line 280 does `event = 1`
land, and only THEN does line 291 check `boot_display_is_asleep()`.

**Hypothesis:** The event pipeline works in principle — the STRONG_CLICK
haptic on press-edge should fire on any real press. But by the time
the single-click `event = 1` lands (BTN_DOUBLE_GAP_MS = 250 ms per
`fc_common.c` header constants), Ivan may have already given up
waiting, OR the shutdown-watcher hold gate at line 235 is consuming
events on the press-edge before they reach line 291. Two possibilities;
first one to check is whether STRONG_CLICK fires at all on a press
while asleep.

**Proposed fix:** Move the wake gate to the **press edge** (line 245),
before shutdown-watcher state can matter, so the wake happens
immediately on finger-down rather than 250 ms after finger-up. Concretely:
  - Inside the `if (low)` branch at `fc_common.c:245`, right after
    `haptic_play(DRV_STRONG_CLICK)`, check `boot_display_is_asleep()`.
    If yes: `boot_display_wake()`, set `s_btn_state = BTN_IDLE`,
    `s_btn_prev_low = low`, `s_last_activity_ms = now`, `return 0`.
    This kills the whole downstream state-machine for this press so
    the wake click doesn't also emit a single-click event 250 ms
    later.
  - Delete the old wake gate at lines 291-298 (or leave a comment
    stub — cleaner to delete per [[feedback_no_invented_hw_status]]
    equivalent for dead code).

**Diagnostic to include:** before landing the fix, add ONE line of
ESP_LOGI at the press-edge (line 245) that always logs
`"[BTN] press-edge, asleep=%d"`. If Ivan flashes and sees this line
on a press-while-asleep, we know the GPIO path is alive and the bug is
purely in the wake gate. If he sees no line at all, the bug is
upstream (ISR / GPIO / pull config) and we investigate that first.

**Symptom B -- touch wakes on single tap:**
`cst9217.c:295` currently maintains `s_prev_tap_us` — first tap sets
it, second tap within 500 ms wakes. Reading the code carefully:
  - First valid tap while asleep: line 305 → `boot_display_is_asleep()`
    true → line 308 `s_prev_tap_us == 0` so else branch → line 314
    stores timestamp → line 316 `continue` (report NOT queued).
  - **BUT**: `s_prev_tap_us` is `static int64_t`, initialized to 0.
    First tap after boot, or first tap after a wake, works correctly.
  - **Race candidate**: `s_prev_tap_us` isn't reset when panel wakes
    via ANOTHER path (button press). Then on later re-sleep, if
    `s_prev_tap_us` is stale but within 500 ms of a fresh tap
    (extremely unlikely — different sleep cycles are seconds apart),
    it could fire. This isn't the bug.
  - **More likely**: The CST9217 controller may be emitting **multiple
    reports per finger-down** — one report for finger-down, then a
    "still down" report a few ms later, then release. Two valid reports
    within 500 ms of each other, from ONE physical tap, would trigger
    the wake. Sketch 16 in Stage 24 sees only the down-event via ACK;
    but the ESP-IDF `task_touch_fn` polls at a faster cadence and may
    read the same physical touch twice before the controller's own
    debounce clears.

**Proposed fix (two parts):**
  1. **Require finger-lift between the two taps.** Add a `s_prev_tap_
     was_release` sentinel: after storing `s_prev_tap_us`, mark that
     we're now waiting for a release (`valid == false` with fingers=0
     from the controller) before the NEXT valid tap can count as the
     second half of a double-tap. Concretely: keep a
     `static bool s_awaiting_release_between_taps = false`. When we
     store the first tap, set it. On any invalid/release report while
     asleep, clear it. On a valid tap while asleep AND the sentinel
     is CLEAR, treat it as a fresh first-tap (reset the pair).
  2. **Sanity gate: minimum gap between the two taps.** Add
     `TOUCH_DBL_TAP_MIN_US = 80 * 1000` (80 ms) — no human can
     double-tap faster than ~80 ms, so any pair closer than that is
     the controller double-reporting one physical touch. If
     `(t_wake - s_prev_tap_us) < TOUCH_DBL_TAP_MIN_US`, treat as
     duplicate: refresh `s_prev_tap_us = t_wake` and continue.

If **either** of these makes single-tap-wake stop happening, we know
which failure mode was live. If neither helps, escape hatch per Stage
27 §4.10 note: "either accept single-tap officially and drop the
double-tap detector" — one-liner CLI-observable regression, log a
`[TOUCH] single-tap wake` and accept the UX.

**Success gate:**
  - Panel asleep. Ivan taps ONCE → panel STAYS asleep (no wake).
  - Ivan taps TWICE within ~500 ms → panel wakes. Neither tap
    dispatches a tile gesture.
  - Panel asleep. Ivan presses button ONCE → panel wakes. No tile /
    mode toggle from the wake press.

**Bench flash:** independent from 28.1 and 28.3; land after 28.1 so
Ivan can see the wake actually reveal the big new clock.

---

### 2.3 Batch 28.3 -- Auto-sleep sanity check + CLI observability

**Symptom:** Ivan doesn't know if the 15 s auto-sleep is firing because
the panel has been on USB power (and DISP OFF is CLI-manual).

**Hypothesis:** The code IS running per `field_capture.c:138-153` (15 s
constant + ST_STANDBY + activity-kick chain). But the only way to
observe it firing is a boot log line at sleep-transition, and Stage
27's boot_display.c may or may not log at INFO level on sleep.

**Proposed fix (observability only, no behavior change):**
  - `boot_display.c:125` (`boot_display_sleep`): ensure there's an
    `ESP_LOGI` line like `"[DISP] sleep -- panel to DISPOFF+SLPIN
    after %u ms idle"` where the caller passes the idle duration OR
    the sleep function reads a broker/global for it. Keep it terse
    per [[feedback_status_terse]].
  - `boot_display.c:141` (`boot_display_wake`): same, log the wake
    with source (`"[DISP] wake -- source=button"` or `source=touch`
    or `source=cli`). Requires the callers to pass a source enum
    (small refactor).
  - `fc_cli.c` STATUS verb: add a line
    `display: on|asleep (idle=<N>s / 15s)` so Ivan can `STATUS` any
    time and see how close to auto-sleep the panel is.

**Success gate:** Ivan runs STATUS, sees `display: on (idle=3s / 15s)`.
Waits 15 s without touching anything, boot log prints
`[DISP] sleep -- 15000 ms idle`. Touches once with double-tap → log
prints `[DISP] wake -- source=touch`.

**Bench flash:** land alongside 28.2 (same file surface: fc_common.c,
boot_display.c, fc_cli.c) per [[feedback_group_work_by_venue]].

---

### 2.4 Batch 28.4 -- Encoder ↔ screen HUD (functional, not pretty)

**Symptom:** Encoder + RGB LED work; screen is oblivious to which mode
is selected. Explored file layout: `lvgl_ui/` has NO encoder consumer
at all — `encoder.c:48-62` forward-declares `UI_EVENT_CROWN_CW/CCW`
and `g_ui_event_q`, but nothing subscribes. `field_capture.c:186-196`
consumes encoder detents in the state machine but writes only to
NVS + RGB + haptic, never to the broker or any LVGL surface.

**Hypothesis:** The two subsystems were built in parallel and never
had a common "current mode" broker channel. The fix isn't rocket
science — it's a broker field + a HUD label that reads it.

**Proposed fix (multi-file, deliberately minimal):**

Step 1 — **broker channel for current mode:**
  - Add a `broker_mode_data_t` (or extend an existing one) with:
    `uint8_t mode_id` (FCM_* enum), `char mode_name[16]`,
    `char submenu_name[16]` (empty string when top-level),
    `uint8_t rgb_r, rgb_g, rgb_b` (mnemonic for the LED),
    `uint32_t seq` (monotonic, so the UI can debounce redraws).
  - Broker writer: `field_capture.c` after any `s_mode` change (both
    top-level and LSM submenu). One line per branch.
  - Broker reader: LVGL UI code, polled during `main_screen_update()`
    tick.

Step 2 — **HUD widget on the main screen (functional first):**
  - `ui_main_screen.c`: add `s_lbl_mode` label near the bottom of the
    screen. Position: `LV_ALIGN_BOTTOM_MID, 0, -20` initially, we
    can move it later.
  - Font: `UI_FONT_LABEL` (Montserrat 22) — big enough to read at a
    glance, no fancy formatting.
  - Content: `"MODE: <mode_name>"` when top-level;
    `"MODE: <mode_name> / <submenu_name>"` when in a submenu.
    Suffix with the RGB LED color mnemonic in parens:
    `"MODE: LSM (yellow)"`, `"MODE: LSM / STEPS (yellow→cyan)"`.
    Color code from broker `rgb_*`.
  - Update trigger: existing `main_screen_update()` tick already runs
    at ~10 Hz (per lvgl_ui refresh task); read broker mode struct,
    only `lv_label_set_text()` if `seq` changed.
  - No animations. No color-coded text. No fancy transitions. Just a
    label that always shows the truth.

Step 3 — **"Press dumps to screen" spec:**
  - Ivan wants a button press (which is the confirm/toggle button —
    encoder has no click) to also dump the current mode info to the
    screen. Two interpretations:
      (a) Show a transient overlay "MODE X CONFIRMED" for ~2 s, then
          fade back. Adds animation state — deferred until aesthetics
          pass.
      (b) Guarantee the HUD label is up-to-date on any button press
          (i.e., force a broker seq bump on button press). Simpler,
          no new UI widgets.
  - **Take (b) for this batch.** It's functional, robust, and matches
    "not pretty first". The label always shows current mode; a press
    that toggles recording / enters submenu will change the broker
    payload, which the tick will pick up. If Ivan wants a transient
    overlay later, that's Stage 29+ aesthetics.

Step 4 — **Sanity CLI verb:**
  - Add `MODE` (or extend `STATUS`) to print the same broker payload
    Ivan sees on screen: `mode = LSM (yellow) / submenu = STEPS`.
    Cross-check between CLI and screen makes the HUD trustworthy per
    [[feedback_cli_first_testability]].

**Success gate:**
  - Rotate encoder CW → RGB LED changes color AND main screen's mode
    label updates within one tick.
  - Enter LSM branch (whatever the current gesture is — likely a
    button press per `fc_modes_lsm.c`) → label updates to
    `MODE: LSM / <first_submenu>`.
  - Rotate within LSM branch → submenu name updates.
  - Exit LSM branch → label back to `MODE: LSM` top-level (or
    whichever mode).
  - `MODE` CLI verb agrees with the on-screen label byte-for-byte.

**Bench flash:** land last in this stage — depends on 28.2 (touch
wake) working so Ivan can actually see the HUD after auto-sleep. Big
diff (broker + UI + writer), so this deserves its own flash-and-
verify cycle per [[feedback_flash_per_checkpoint]].

**Deferred out of this batch:**
  - Encoder tile redesign per [[project_encoder_tile_redesign]] — this
    HUD makes encoder visible on the MAIN screen, not the dedicated
    encoder tile. The tile redesign is a separate follow-up.
  - Transient "mode changed" overlay animation — aesthetics, Stage
    29+.
  - Font/color polish on the HUD label — same reason.
  - Removing the RGB LED entirely in favor of on-screen color — LED
    stays as the always-on indicator even when the screen is asleep.

---

### 2.5 Batch 28.5 -- Landscape + color depth (deferred within stage)

**Symptom:** Portrait; text looks "close but not truly correct" per
Stage 27 §4.6.

**Hypothesis A -- color depth:** `LV_COLOR_DEPTH_16` per sdkconfig +
`CONFIG_LV_CONF_SKIP=y` per [[feedback_lvgl_kconfig_authoritative]].
LVGL runs RGB565 internally; the CO5300 flush cb converts to RGB888
per output. Font antialiasing on RGB565 has known artifacts because
the 5:6:5 quantization loses subpixel gradient info. Bumping to
`LV_COLOR_DEPTH_32` (ARGB8888) gives per-pixel alpha for glyph edges
and fixes the "off" color mixing.

**Hypothesis B -- landscape:** Stage 25 Batch H tried MADCTL 0x60 and
got green bars. The `rotate90` config field in `co5300.c:274-284` is
still present. Stage 27 §4.6 noted the working hypothesis: LVGL matrix
rotation OR manual pixel transpose, NOT another MADCTL retry.

**Proposed fix (two-part, land depth-bump first, test, THEN landscape):**

Part A — `LV_COLOR_DEPTH_16 -> _32`:
  - `sdkconfig` (via menuconfig or direct edit) — set
    `CONFIG_LV_COLOR_DEPTH_32=y` and clear the 16 flag.
  - Draw buffer bump: `lvgl_ui_display.c:162-164` currently allocs
    `410 × 40 × 3 = 49200 B`. ARGB8888 is 4 B/pixel → `410 × 40 × 4
    = 65600 B`. Still fits in the current MALLOC_CAP_INTERNAL
    allocation (checked against §4.6 boot-log values). If it doesn't,
    reduce `LVGL_STRIP_ROWS` from 40 to 30 (410 × 30 × 4 = 49200 B).
  - Flush cb: adjust the RGB888 conversion in `co5300_write_pixels`
    if the input format assumption changes. Verify CO5300 pixel
    format is still RGB888 on wire — it is (24-bit color mode set at
    init).

Part B — landscape (only after Part A confirmed working):
  - Try LVGL 9's `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_
    90)` first — this is the software matrix rotation, no MADCTL
    changes, no draw-buf reshape. Direction matters: per
    [[project_display_landscape_direction]] we need 90° CW from
    portrait, which is `LV_DISPLAY_ROTATION_90` in LVGL 9's convention
    (verify sign on first flash — if it goes CCW, we swap for 270).
  - Keep `rotate90 = false` in the CO5300 config; the panel stays in
    its native portrait orientation, LVGL does the transpose.
  - IF matrix rotation is too slow (visible tearing / frame drops on
    the tile-swipe), fall back to manual pixel transpose in the
    flush cb. But try matrix first — cheap to enable, reversible in
    one line.

**Success gate for Part A:** Text looks crisp, colors match Ivan's
expectation. Boot log shows the new buffer size and no OOM.

**Success gate for Part B:** All tiles readable in landscape, top-of-
portrait now on the right side of landscape. No green bars, no
tearing, no missing rows.

**Bench flash:** LAST in the stage — biggest visual change, needs Ivan
to eyeball proportions and rotation direction on the real panel. If
Part A regresses anything (crashes at boot, tiles unrenderable), we
back out and ship Stage 28 without the depth bump — the earlier
batches are the higher-value wins.

---

## 3. Non-goals for Stage 28

  - GPS silence investigation (still parked per Stage 27 §4.2; needs
    bench iron time, not firmware).
  - CLI submenu restructure per [[project_cli_submenus]] — deferred
    until Stage 28 verbs settle.
  - Encoder tile redesign per [[project_encoder_tile_redesign]] — the
    main-screen HUD in 28.4 is deliberately NOT the encoder tile. The
    tile redesign is Stage 29+.
  - Battery broker channel polish / MAX17048 1 Hz task — Stage 27
    already put fuel VCELL/pct into `broker_battery_data_t`; more
    channels wait for cell soak data.
  - Recording writer datetime migration per
    [[feedback_recording_datetime_opportunistic]] — opportunistic,
    lands as those modules get touched.
  - `[TOUCH] I2C mutex timeout` noise from Stage 27 §4.6 — bench-log
    noise, not user-visible, not a Stage 28 concern.
  - `spi_master ... polling transaction not terminated` one-shot from
    Stage 27 §4.6 — same reason, one-shot at init, non-fatal.

---

## 4. Bench log (fill in as each batch lands)

### §4.1 Batch 28.1 -- Big clock on main screen (fw 0.4.51)

**Landed:**

  - `ui_theme_colors.h`: new `UI_FONT_TIME_XL` alias pointing at
    `&lv_font_montserrat_48`. Kept `UI_FONT_TITLE` at Montserrat 28
    unchanged so every other tile that uses TITLE (tile headers,
    section titles) is untouched.
  - `ui_main_screen.c:65-72`: `s_lbl_time` font swapped
    `UI_FONT_TITLE` -> `UI_FONT_TIME_XL`; vertical offset bumped
    `-30` -> `-50` so the taller 48-px glyph doesn't crash into the
    date label sitting at centre.
  - `firmware_version.h` -> 0.4.51.

**Not touched:** date label + battery label + status bar all still
at their Stage 27 sizes. Bump those in a follow-up if the ratio
looks off on the real panel after Ivan flashes.

**Success gate:** flash + boot into the main screen. Clock should
read at roughly 4x the pixel area of Stage 27's 28-px version.
Date + battery labels should still be visible below / above without
overlap.

**RETURN POINT -- clock needs to be another 2x bigger AND crisper.**
Ivan's follow-up: stock Montserrat 48 is the Kconfig ceiling; going
to ~96 px needs a custom-generated LVGL font C file. Tooling gap:
`lv_font_conv` is Node.js and Node isn't installed on this box (see
precedent `fa5_select_14.c` header comment for the original tool
invocation). Ivan will decide the Node install route (conda vs apt)
in a later session. Font source when we resume:

```
/home/ivan/Projekti/Elektronika/Kompic-Wearable/kompic-wearable/
docs/build_info/reference_files/Space_Grotesk/
SpaceGrotesk-VariableFont_wght.ttf
```

Space Grotesk instead of Montserrat -- Ivan's choice, gives the
watch face a distinct look vs the tile UI which stays on Montserrat.
When we resume: pick a target size (~96 px baseline), pick a weight
axis (Medium is a good default for a variable font), generate
`space_grotesk_96.c` via lv_font_conv with symbol set `0123456789:`
(digits + colon only -- the clock never renders letters, so keeping
the glyph table minimal keeps flash cost tiny), drop it into
`lvgl_ui/`, add a `LV_FONT_DECLARE` in `ui_theme_colors.h`, swap
`UI_FONT_TIME_XL` to point at the new font.

### §4.2 Batch 28.2 -- Wake gates + LV_COLOR_DEPTH 16→32 (fw 0.4.52)

Ivan folded the "but better" crispness ask into the wake-gate batch so
one flash covers glyph AA + button-wake + touch-wake. Stage 28.5 Part
A (color depth) is now spent here; Part B (landscape) still deferred.

**Landed:**

  - **Button wake -- moved to press-edge.** `fc_common.c::button_poll()`
    now checks `boot_display_is_asleep()` inside the `if (low)` branch
    right after debounce (line ~245). If asleep: wake, swallow the
    whole click sequence (reset state to BTN_IDLE, kick activity,
    haptic ack, return 0). The old release-edge gate at line 291-298
    was deleted. Root cause per plan §2.2 was that single-click event
    only fires ~250 ms after finger-up (after BTN_DOUBLE_GAP_MS), by
    which time the shutdown-watcher hold state or another consumer
    could eat the event. Waking on press matches user expectation.
  - **Diagnostic log line on every press:**
    `ESP_LOGI("[BTN] press-edge, asleep=%d")`. Ivan can now tell from
    the boot log whether GPIO16 is producing edges at all -- if the
    line is missing when the button is pressed, the bug is upstream
    (ISR / GPIO / pull config), not the wake gate.
  - **Touch wake -- controller-repeat guards added.** `cst9217.c
    task_touch_fn` rewrite of the double-tap detector at line ~295:
      * `TOUCH_DBL_TAP_MIN_US = 80 ms` floor -- no human double-taps
        faster than 80 ms, so any pair inside that window is a
        controller repeat of one physical touch (refresh timestamp,
        drop as duplicate, don't count as second tap).
      * `s_awaiting_release` sentinel -- after storing the first tap,
        require a non-valid (finger-up) report before a subsequent
        valid finger-down can count as the second half of a pair.
      * `TOUCH_DBL_TAP_MAX_US = 500 ms` outer window unchanged.
      * Log line `[TOUCH] double-tap wake (gap=X ms)` on wake so Ivan
        can see the timing that landed.
  - **LV_COLOR_DEPTH 16 -> 32.** `sdkconfig` + `sdkconfig.defaults`
    both updated (defaults so it persists a menuconfig regen).
    `lvgl_ui_display.c` flush cb comment updated. Draw buffer stays
    RGB888 at 49 200 B -- LVGL renders internally at 32-bit but the
    display format is unchanged, so LVGL does the down-conversion on
    flush. No memory pressure change.
  - `firmware_version.h` -> 0.4.52.

**Not touched:**
  - Encoder wake (still intentionally excluded per Stage 27 §4.8).
  - LVGL refresh-task pause during sleep (still queued as future
    power win if the ~32 mA baseline doesn't drop enough).
  - Landscape rotation (Stage 28 §2.5 Part B, still deferred).
  - Custom 96-px Space Grotesk clock font (return point at end of
    §4.1).

**Success gate for tonight's flash:**
  1. Panel asleep. Press button once → panel wakes within one debounce
     tick. Boot log shows `[BTN] press-edge, asleep=1` then
     `[DISP] wake` (already present per Stage 27 §4.7).
  2. Panel asleep. Single tap → panel STAYS asleep (no `[TOUCH]
     double-tap wake` line, and screen remains off).
  3. Panel asleep. Fast double-tap (< 500 ms gap) → panel wakes,
     boot log shows `[TOUCH] double-tap wake (gap=X ms)` with X
     between ~100 and ~500.
  4. Text on any tile looks visibly crisper than pre-flash (font AA
     no longer quantized at RGB565).
  5. No regression on peripherals, no OOM at boot.

### §4.3 Batch 28.3 -- Sleep-side gestures + logs + AA baseline reset (fw 0.4.53)

Bench feedback on 0.4.52 (2026-09-13, second flash):
  - Double-tap wakes ✓
  - Single button press wakes ✓
  - **Nothing puts screen to sleep** (double-tap, double-press, or
    manual gesture) -- was never implemented; only auto-sleep at 15 s
    idle worked.
  - Auto-sleep IS firing (screen actually goes off after ~10-15 s)
    but no serial line marks the transition.
  - **Text AA still fuzzy** -- the LV_COLOR_DEPTH 16 -> 32 bump was
    not the cause. Ivan pointed at the old WS-Touch-LCD-1.69
    smartwatch project (crisp text on ST7789 at LV_COLOR_DEPTH_16
    via Espressif's `esp_lvgl_port_add_disp`).

**Landed:**

  - **Double-click-to-sleep (button).** `field_capture.c:155` main-
    loop check: `btn == 2 && !s_in_submenu && s_state == ST_STANDBY`
    -> `boot_display_sleep()` + swallow. Coexists with the existing
    "btn==2 while in_submenu -> LSM exit" gesture; the two branches
    are mutually exclusive on `s_in_submenu`.
  - **Double-tap-to-sleep (touch).** `cst9217.c task_touch_fn` --
    mirror of the wake detector. File-scope `s_sleep_prev_tap_us` +
    `s_sleep_awaiting_release` state, same 80 ms min + 500 ms max
    window as the wake path. On fire: `boot_display_sleep()` +
    swallow the pair (`continue` before the queue post) so the
    tile/widget under the tap doesn't also react. First-tap-of-pair
    falls through so single-tap widget interactions still work
    normally.
  - **Sleep/wake transition logs at the boundary.**
    `boot_display_sleep()` and `boot_display_wake()` each now
    `ESP_LOGI` when the state actually flips, so bench observers
    always see the transition even if the caller forgets a reason
    line. Caller-side reason lines added at every entrypoint:
      * `field_capture.c` auto-sleep: `[DISP] reason=idle-timeout`
      * `field_capture.c` btn-double: `[DISP] reason=btn-double`
      * `cst9217.c` touch wake: `[DISP] reason=touch-double wake`
      * `cst9217.c` touch sleep: `[DISP] reason=touch-double sleep`
      * `fc_common.c` button wake: `[DISP] reason=btn-press wake`
      * `fc_cli.c` DISP OFF/ON: `[DISP] reason=cli DISP ...`
  - **LV_COLOR_DEPTH reverted 32 -> 16.** Bench-observed no crispness
    improvement in 28.2, and the old WS-Touch-LCD-1.69 project
    renders crisp at DEPTH_16 -- so 16 is a known-good baseline for
    LVGL 9. Both `sdkconfig` + `sdkconfig.defaults` reverted; comment
    in defaults tags the real suspect for a follow-up batch (LVGL 9
    emits RGB888 bytes as `[B, G, R]` per
    `lv_draw_sw_blend_to_rgb888.c` while CO5300 COLMOD 0x77 expects
    `[R, G, B]` on the wire -- MADCTL BGR bit toggle is the A/B).
  - `firmware_version.h` -> 0.4.53.

**Not touched (deliberate defer):**

  - **RGB byte-order A/B test.** Wants its own batch with a
    non-destructive CLI toggle (e.g. `RGBSWAP` verb that flips MADCTL
    bit 3 at runtime) so Ivan can flash once and bench-compare on/off
    without reflashing. Root-cause candidate #1 for the AA/color
    weirdness per Stage 28 research spike.
  - **esp_cache_msync on flush.** Research spike ranked this #1, but
    our draw buffer is `MALLOC_CAP_INTERNAL` (internal SRAM on
    ESP32-S3 is uncached; the cache lives over PSRAM). Adding
    `esp_cache_msync` is a no-op for internal-SRAM DMA. Leaving out
    to avoid useless boilerplate; only relevant if we ever move the
    strip buffer to PSRAM.
  - **Espressif esp_lvgl_port migration.** The old smartwatch project
    uses `esp_lcd_panel_*` + `lvgl_port_add_disp` which manages all
    the SPI/DMA/cache details automatically. Migrating away from our
    homegrown CO5300 flush cb would be a full-stage rewrite -- out
    of Stage 28 scope, queued for later if the RGB swap doesn't
    resolve crispness.

**Known trade-off:** touch double-tap-to-sleep swallows the pair, so
any LVGL widget that relies on native "double-clicked" events won't
fire on the awake path. No such widget currently exists in the
tree -- the sleep gesture is universal per Ivan's spec.

**Success gate:**
  1. Double-click button (with panel awake, on main tile, not in
     submenu, not recording) → panel sleeps. Boot log shows
     `[DISP] reason=btn-double` then `sleep -- panel to DISPOFF+SLPIN`.
  2. Double-tap touch (panel awake) → panel sleeps. Boot log shows
     `[DISP] reason=touch-double sleep (gap=X ms)` then the boundary
     log.
  3. Auto-sleep at 15 s idle logs
     `[DISP] reason=idle-timeout (15000 ms idle)`.
  4. Single-tap on awake screen still dispatches to LVGL widgets
     (tile swipes / button clicks still work).
  5. Text on tiles at least as crisp as pre-28.2 (LVGL back to
     DEPTH_16 baseline).

**Bench observations flagged for later (no fix in this batch):**

  1. **TEMP_DUMP triggers spurious LSM WRIST_DOWN gesture.** Every
     run of the TEMP_DUMP CLI verb prints
     `LSM6DSV16X: [GESTURE] NONE -> WRIST_DOWN (az_lpf=+0.98 g)` in
     the middle of the sensor poll. `+0.98 g` on az with the board
     stationary on the bench is physically consistent (gravity), so
     the gesture detector is seeing what it should -- but the fact
     that it only fires DURING temp_dump suggests either the sensor
     wake path taps into the gesture pipeline before it should, or
     the settling window skews the LPF baseline. Investigation
     queued.
  2. **New `spi_master` error at LVGL runtime:**
     `E (1487616) spi_master: spi_device_polling_start(1406): Cannot
     send polling transaction while the previous polling transaction
     is not terminated.` Not seen in prior batches. One-shot in the
     captured log so far; may correlate with the LVGL flush racing
     against a concurrent CO5300 sleep/wake command via QSPI (both
     go through the same SPI2_HOST). If it recurs mid-session, add
     a `xSemaphoreTake` around every co5300_* call that shares a
     bus with the flush cb.

### §4.4 Batch 28.4 -- Back-gesture unification + RGB swap + softer double-tap (fw 0.4.54)

Bench feedback on 0.4.53 (2026-09-13, third flash):

  - Double-tap-to-sleep + double-tap-to-wake both fired -- but the
    detector was **too grabby**. Log excerpt: three sleep/wake cycles
    inside 10 s of normal use because swipe-then-tap combos counted
    as "two valid finger-down reports within 500 ms" and triggered
    the sleep pair.
  - Ivan clarified the desired semantics: **double-tap should exit
    the menu (go up a level), and if already at the top level, lock
    the screen. Universal gesture -- anywhere, anytime.** Button
    double-click, LSM tap-double, and touch double-tap all follow
    the same rule.
  - Two extra observations logged but not fixed here: LSM WRIST_DOWN
    firing during TEMP_DUMP, spi_master polling error at LVGL init.

Ivan also pointed at `firmware/arduino/16_amoled_touch_test_mk1b` --
that Arduino sketch renders colors correctly on the same CO5300
silicon, so use its recipe as the reference for the AA / colour fix
instead of adding a runtime `RGBSWAP` toggle.

**Landed:**

  - **Softer touch double-tap (press-edge + hold-time filter).**
    `cst9217.c task_touch_fn` rewritten to be edge-triggered:
      * **Press edge** (previous report invalid, this one valid) --
        record `s_touch_press_us`, kick activity, check if the last
        release-timestamp is inside `[DBL_TAP_MIN_US=150ms ..
        DBL_TAP_MAX_US=500ms]`. If yes -> fire double-tap and swallow
        this report.
      * **Release edge** (previous report valid, this one invalid) --
        compute hold time. If `<= TAP_MAX_HOLD_US=300ms`, record as
        eligible first-tap for a future pair. If longer, it was a
        swipe / hold -- reset the tap chain.
      * **Continuous valid** (touch dragging) -- no state change; the
        tap chain is only touched at edges. Kills the swipe-then-tap
        false-fire path from Batch 28.3.
      * Min gap raised 80ms -> 150ms (still below the human
        double-tap minimum).
      * Old file-scope statics deleted; replaced with
        `s_touch_press_us / s_prev_valid / s_last_tap_end_us`.
  - **Universal back-gesture dispatch (Ivan's semantics).** New
    `field_capture_back_gesture(const char *reason)` in
    `field_capture.h` -- atomic-counter cross-core signal, callable
    from any thread. Main loop drains the counter each tick and
    dispatches:
      * panel asleep         -> wake
      * awake + in submenu   -> exit submenu (up one level)
      * awake + top-level    -> sleep (lock)
    Small ring-buffer stashes the last few reason strings so the
    dispatch log can name whichever source fired even if two land
    in the same tick.
  - **All three back sources funneled into the same pipeline:**
      * `field_capture.c` main loop: btn==2 in ST_STANDBY -> call
        `field_capture_back_gesture("btn-double")`. LSM tap-double
        counter delta -> `field_capture_back_gesture("lsm-tap-double")`.
      * `cst9217.c` press-edge detector -> `field_capture_back_gesture(
        "touch-double")`.
      * Old direct calls to `boot_display_sleep()` from cst9217.c
        deleted. Old `btn==2 -> boot_display_sleep()` branch in
        field_capture.c deleted (was in Batch 28.3, superseded).
  - **RGB byte-swap in `lvgl_flush_cb`.** Verified against Sketch 16:
    LVGL 9's `LV_COLOR_FORMAT_RGB888` lays pixels as `[B, G, R]` in
    memory (source-verified in
    `lv_draw_sw_blend_to_rgb888.c`: `res[0] = src.blue`); CO5300 with
    COLMOD 0x77 + MADCTL 0x00 expects `[R, G, B]` on wire; the
    sketch sends `[R, G, B]` manually and colors render fine. Fix:
    in-place per-pixel swap of bytes 0 and 2 in the flush cb before
    `co5300_write_pixels`. Cost ~50 us per full strip; safe because
    LVGL doesn't reuse `px_map` after `flush_ready`. File-header
    byte-order comment corrected (previous version claimed
    `[R, G, B]` -- that assumption is what caused the fuzzy-AA
    symptom Ivan flagged in Stages 27 §4.6, 28 §4.2, and 28 §4.3).
    No MADCTL BGR-bit toggle (would depend on panel honouring bit 3
    of MADCTL, not datasheet-confirmed on this silicon).
  - `firmware_version.h` -> 0.4.54.

**Not touched:**
  - MADCTL BGR-bit path (deferred -- per-pixel swap is guaranteed
    correct; MADCTL is an optimisation candidate for a future batch
    if the ~50 us/flush cost shows up in profiling).
  - The two §4.3 bench observations (TEMP_DUMP WRIST_DOWN spurious,
    spi_master polling error) remain queued.
  - Encoder wake (still intentionally excluded per Stage 27 §4.8).

**Behaviour matrix after this flash:**

| Input                          | State  | Action                       |
|--------------------------------|--------|------------------------------|
| btn single-press               | asleep | wake (fc_common.c press-edge)|
| btn single-press               | awake  | mode-specific (unchanged)    |
| btn double-press               | asleep | wake                         |
| btn double-press               | awake, in submenu | exit submenu     |
| btn double-press               | awake, top-level | LOCK              |
| touch single-tap               | asleep | ignored (needs double)       |
| touch single-tap               | awake  | LVGL widget dispatch         |
| touch double-tap               | asleep | wake                         |
| touch double-tap               | awake, in submenu | exit submenu     |
| touch double-tap               | awake, top-level | LOCK              |
| LSM tap-double                 | awake, in submenu | exit submenu     |
| LSM tap-double                 | awake, top-level | LOCK              |
| touch swipe (>300ms hold)      | any    | never triggers double-tap    |

**Success gate:**
  1. Text on tiles renders with correct colour (no visible R↔B
     swap; test: any coloured tile background / red status LED
     highlight looks right).
  2. Text edges look crisp -- fuzzy AA gone once channels are
     rendered correctly.
  3. Single-tap on awake screen still fires LVGL widgets normally
     (tile swipes / button clicks work).
  4. Double-tap on awake main tile -> panel sleeps.
  5. Double-tap on submenu tile -> submenu exits (returns to
     top-level LSM), does NOT sleep on the same gesture.
  6. Swipe on awake screen (fast finger move across the tile) does
     NOT trigger sleep, even if immediately followed by a tap.
  7. Double-tap on asleep panel -> wakes.
  8. All transitions logged with `[BACK] reason=...` or
     `[DISP] back-gesture reason=...`.

### §4.5 Batch 28.5 -- Narrow lock to main screen (Stage 28 wrap) (fw 0.4.55)

Bench feedback on 0.4.54 (2026-09-13, fourth flash):

  - **RGB fix confirmed** -- colors render correctly.
  - **Text still buggy blurry.** The R↔B swap wasn't the only cause of
    the crispness gripe; a second bug remains. Deferred to Stage 29.
  - **Double-press-to-sleep works.** Double-tap wake + sleep both
    fire.
  - **Double-tap still too grabby while browsing** -- Ivan noted
    accidental locks while double-tapping menu tiles / settings
    controls. New scope: lock ONLY on the main clock face, not on any
    other screen or tile.
  - **LSM tap-double and touch double-tap overlap** -- log shows
    both fire on one gesture (~1 ms apart). Ivan wants one detector
    at a time; touch-alive should mask LSM. Parked as an
    observation, not fixed in this batch.
  - **New bug:** DRV auto-cal IMU sweep needs LSM enabled first;
    fails silently if user runs the sweep with LSM off. Parked too.

**Landed:**

  - **`ui_navigation_is_on_main()` public predicate** in
    `ui_navigation.c/.h` -- returns
    `s_current == UI_SCREEN_MAIN`. Cross-core safe (single aligned
    int, writer-side is the LVGL task on Core 1).
  - **Back-gesture dispatch narrowed.** The lock branch in
    `field_capture.c` now requires `ui_navigation_is_on_main()` in
    addition to the existing awake + top-level guards. Any back
    gesture that lands on Settings, Alarm, or any tile inside the
    Settings tileview logs
    `[BACK] reason=... -> ignored (not on main)` and does nothing.
    Swipe-down remains the intended nav-back path.
  - `firmware_version.h` -> 0.4.55.

**Semantics after this flash:**

| Location on flash        | back gesture      | outcome                    |
|--------------------------|-------------------|----------------------------|
| Panel asleep             | any               | wake                       |
| Main screen (clock face) | any               | LOCK                       |
| Settings screen          | any               | ignored (use swipe-down)   |
| Alarm screen             | any               | ignored (use swipe-left)   |
| Any tile in Settings     | any               | ignored (use swipe-down)   |
| LSM submenu              | any               | submenu exit (unchanged)   |

**Parked observations for Stage 29+ (no fix in Stage 28):**

  3. **LSM tap-double + touch double-tap redundant.** Both fire on
     one gesture -- log excerpt from fourth-flash bench:
     ```
     I (380534) FIELD: [BACK] reason=lsm-tap-double -> submenu exit
     I (380535) CST9217: [TOUCH] double-tap detected (gap=164 ms)
     I (380542) FIELD: [DISP] back-gesture reason=touch-double -> sleep
     ```
     Ivan's rule: exactly one detector armed at a time. If touch
     subsystem is alive, LSM tap-double is muted for back-gesture
     purposes. Button always available as fallback. Needs a
     "touch-recently-alive" heuristic + guard in the LSM tap-double
     branch of the back dispatcher.
  4. **DRV auto-cal IMU sweep requires LSM enabled.** Sweep prints
     `Z=0.000 m/s²` on every step when LSM is off, then reports
     `Sweep complete -- no frequency latched`. User has to enable
     LSM manually (mode nav + click) before the sweep produces
     valid data. Auto-enable LSM at sweep start, or gate the sweep
     command on LSM ready. Not critical -- workaround is documented.

### §5 Stage 28 close-out

**Definition of done (§5) revisited:**

  1. ✓ Main screen time bigger (Montserrat 28 -> 48 in §4.1, still
     wants another 2x in Stage 29).
  2. ✓ Panel wakes on single button press (fc_common.c press-edge).
  3. ✓ Panel wakes on double-tap only (single tap ignored while
     asleep after §4.4 press-edge rewrite).
  4. ✓ Auto-sleep firing is CLI + log observable (§4.3 boundary
     logs).
  5. ✗ Mode HUD -- deferred to Stage 29 as new batch (was Batch
     28.5 in the original plan; renumbered when 28.5 became the
     lock-narrow wrap).
  6. ✓ No regression on peripherals stable at Stage 27 close.

**Landscape rotation (originally §2.5 Part B) and text-crispness
(originally Part A revert) both stay open for Stage 29.**

Stage 28 = **CLOSED** on fw 0.4.55.

---

## 5. Definition of done for Stage 28

  1. Main screen time is ≥ 4× its Stage 27 pixel area — glanceable at
     arm's length.
  2. Panel wakes on a single button press within one poll cycle
     (~5 ms + debounce).
  3. Panel wakes on double-tap only (a single stray tap does NOT
     wake).
  4. Auto-sleep firing is CLI-observable via STATUS and boot log lines
     mark every sleep/wake transition with source.
  5. Main screen shows current mode name + submenu (when active) + RGB
     color mnemonic. Rotating the encoder updates the label within
     one tick. `MODE` CLI verb matches the on-screen label.
  6. No regression on peripherals already stable at Stage 27 close.

Landscape + color-depth bump (28.5) are STRETCH goals for this stage —
if they don't land cleanly, we ship Stage 28 with the four functional
wins and defer the visual polish to Stage 29.

---

## 6. Next-steps hook -- Stage 29+ scope

  - Encoder tile redesign per [[project_encoder_tile_redesign]] — the
    dedicated encoder "test ground" tile with rotation speed / click
    count.
  - HUD aesthetics: color-coded mode label, transient "mode changed"
    overlay, animations.
  - `MODE / brightness / theme` in a proper settings tile once the
    HUD is proven robust.
  - GPS revive attempt with fresh bench iron time (still §4.2 of
    Stage 27, standing open item).
  - Recording writer datetime migration opportunistic sweep.
  - Kill the `spi_master polling transaction not terminated` log line
    if it starts recurring in a real session (currently one-shot).

---

## References

  - `Stage_27_Mk1b_Peripheral_Bringup.md` -- previous stage. §4.6,
    §4.7, §4.8, §4.9, §4.10 all directly feed this stage.
  - `Screen_Test_Checklist.md` -- visual verification list.
  - `Module_Blueprint.md` -- CLI + screen = two outlets over one
    command surface. The mode HUD in 28.4 is a canonical example.
  - Auto-memory `MEMORY.md` -- especially
    [[project_display_landscape_direction]],
    [[project_display_text_crispness]],
    [[project_mode_restructure]],
    [[project_encoder_tile_redesign]],
    [[feedback_lvgl_heap_too_small_for_pixels]],
    [[feedback_lvgl_kconfig_authoritative]],
    [[feedback_encoder_polling]],
    [[feedback_cli_first_testability]],
    [[feedback_flash_per_checkpoint]],
    [[feedback_group_work_by_venue]],
    [[feedback_status_terse]].
