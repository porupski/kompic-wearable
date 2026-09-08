# Stage 20 -- Button Responsiveness Restore (Stage 17 UX regression fix)

**Date:** 2026-09-06
**Board:** iv7.1 (Mk1 bench). Mk1b still in fab.
**Firmware baseline:** `iv7.1.f0.4.24` -> target `0.4.25`.
**Status:** OPEN. Interrupts Stage 19 (power) for a critical UX bug.

---

## 1. Symptom (bench report, 2026-09-06)

Ivan on the bench: button feels laggy and unreliable versus everything
before AMOLED work.

- **Turn on:** press to enter a mode, "never sure if I've done it right --
  like there's a delay". No visible or tactile confirmation on the press
  edge. Mode entry happens later.
- **Turn off (4 s hold to ship):** "it would do the thing, I release,
  nothing, I keep holding, nothing, I start scrolling the encoder or
  something and oh now it shuts down". Ship-mode gesture appears to
  silently no-op on second and later attempts.
- Everything felt right pre-Stage-16 (i.e. before the Stage 17 button /
  shutdown rework).

---

## 2. Root cause

Stage 17 §4.3 and §4.5 landed three defensible click-vs-hold changes
that jointly stripped the press-edge feedback the user had been reading
as "we heard you":

1. **`BTN_LONG_PRESS_MS = 1000` swallow** in `button_poll` -- releases
   past 1 s no longer emit `event=1`. Correct fix for "held to ship,
   released too early, mode started anyway". Keep.
2. **`SHDN_COMMIT_MS = 1000` gate** in `task_shutdown_watcher_fn` --
   `g_shutdown_hold_active` stays false and the LED stays dark for
   sub-1 s presses. Correct for click-vs-hold arbitration. But the OLD
   watcher lit the LED red on the *first low sample*; the new one waits
   1 s. That was the "we heard you" cue and it is gone.
3. **`s_ship_mode_latched` per-boot latch** at `fc_shutdown.c:41`
   (`watcher_ship_mode` early-return). Ship-mode is intentionally
   idempotent per boot. On battery this never matters (BATFET drops on
   the first fire). On USB the first fire is a no-op, sets the latch,
   and every subsequent 4 s hold is silently swallowed inside
   `watcher_ship_mode()` -- no LED, no BQ call, no log. `LONG_BUZZ` still
   fires before the latch check, so the user feels the haptic and sees
   nothing else -- exactly Ivan's "keep holding, nothing" report.

The 350 ms `BTN_DOUBLE_GAP_MS` wait for single-click emit has always
been there; it was previously masked by the immediate LED-red from (2)
in the old code.

("Encoder scroll makes it shut down" cannot be reproduced from the
code paths. Most likely it is either the encoder-scroll preview LED
being read as visible ship confirmation, or that particular hold
happening to cross 4 s at the moment of the scroll.)

---

## 3. Plan

Three surgical fixes -- do not revert Stage 17.

### 3.1 Haptic tick on button press edge

`button_poll()` (`fc_common.c:211`) fires `haptic_play(DRV_STRONG_CLICK)`
on the leading-edge low transition. Symmetric to encoder detent haptic.
Fires regardless of eventual click / long-press classification. The
shutdown watcher already fires its own haptic pattern at the warn /
warm-up / fire thresholds and lives on a separate task, so an extra
click at press start does not compete with the ship sequence.

Rationale: press-edge feedback restored, LED semantics untouched, no
change to click-vs-hold logic.

### 3.2 Remove `s_ship_mode_latched`

Delete the latch at `fc_shutdown.c:29,41-42`. Every call to
`watcher_ship_mode()` runs the full sequence:

- red LED, 300 ms
- `bq25619_enter_ship_mode()` (with its own I2C-retry ladder)
- 500 ms delay
- USB-return warn + LED off

On battery: irrelevant, first fire kills VDD. On USB: every 4 s hold
gives the same visible feedback (red LED + warn log). Also makes the
15-min uptime-cap re-arm loop in `task_shutdown_watcher_fn` behave
predictably -- previously the second uptime-cap fire was silent.

### 3.3 Split visual LED-red ramp from commit

Currently the ramp only paints when `g_shutdown_hold_active` is true
(which flips at `SHDN_COMMIT_MS = 1000`). Split into:

- **200 ms .. 1000 ms:** paint a *dim* red (linear intensity from 0 up
  to about 25 % of WS_MAX). No commit, no click suppression, no
  haptic. If the user releases in this window, `button_poll()` still
  emits `event=1` on release + 350 ms gap.
- **1000 ms .. 4000 ms:** existing behaviour -- commit, full ramp,
  warn buzz at 2 s, warm-up at 3.7 s, fire at 4 s.

Rationale: user gets visible proof the hold is being counted from ~0.2 s
while the click-vs-hold arbitration boundary stays at 1 s.

### 3.4 Not touched

- `BTN_DOUBLE_GAP_MS = 350` stays. Shrinking it changes double-click
  reliability across the whole codebase; not worth it once the press-
  edge haptic gives instant ack.
- `BTN_LONG_PRESS_MS = 1000` swallow stays. This is the Stage 17 fix
  that Ivan needs -- released-past-1 s does not fire a mode.
- Wrist gesture (Stage 18 §7.1) and rgb_policy (Stage 17 §4.10)
  behaviour unchanged.

---

## 4. Results

### 4.1 Press-edge haptic (fw 0.4.25)

`fc_common.c::button_poll()` now fires `haptic_play(DRV_STRONG_CLICK)`
on the leading-edge low transition (right where the state machine
sets `BTN_PRESSED` or `BTN_PRESSED_2`). Files touched:

- `fc_common.c` -- added `#include "haptic.h"` and the one-line
  `haptic_play()` call inside the `if (low)` branch.

The `haptic_play` path uses `xQueueSend` (non-blocking, thread-safe).
The shutdown watcher's own haptic sequence (warn / warm-up / fire)
still runs; the extra press-start click doesn't collide because they
target different moments in the hold ladder.

### 4.2 Removed `s_ship_mode_latched` (fw 0.4.25)

`fc_shutdown.c::watcher_ship_mode()` -- deleted the per-boot latch
and its early-return. Every call now runs the full sequence: red
LED, `bq25619_enter_ship_mode()`, USB-return warn, LED off. On
battery this changes nothing (BATFET drops on first fire and VDD
disappears). On USB every 4 s hold now paints the LED red and logs
"ship mode declined -- USB is powering VDD ... Unplug USB to
actually ship" -- the same feedback every time, no more silent no-op
after the first hold.

Also fixes the second-uptime-cap-fire silence: the 15 min autonomous
shutdown loop already re-arms `uptime_cap_deadline`, and now the
paired `watcher_ship_mode()` call also runs its visible portion.

### 4.3 Pre-commit LED ramp at SHDN_VISUAL_MS = 200 ms (fw 0.4.25)

`fc_shutdown.c::task_shutdown_watcher_fn` -- the LED ramp and the
click-vs-hold commit are now decoupled:

- New constant `SHDN_VISUAL_MS = 200`.
- New local `bool visual_owned` -- tracks whether the shutdown
  watcher currently owns the LED via `rgb_policy_pause()`.
- In the `low && hold_active` branch: at held >= SHDN_VISUAL_MS,
  `visual_owned = true` and `rgb_policy_pause()` runs once. From
  that point every 5 ms tick paints the linear ramp
  `intensity = (held * WS_MAX) / SHDN_FIRE_MS`. At 200 ms the LED
  is barely lit (~5 %), at 1000 ms it hits 25 %, at 4 s it's full red.
- All release paths (confirmed release, mid-bounce release-fire,
  and normal fire while held) call `rgb_policy_resume()` and clear
  `visual_owned` before exiting -- rgb_policy takes over the LED
  again cleanly.
- `g_shutdown_hold_active` continues to flip at SHDN_COMMIT_MS =
  1000 ms as before, so `button_poll()`'s hard gate and click
  swallow are unchanged. Sub-1 s presses still fire a click on
  release.

The fresh-press branch no longer paints `ws2812_set_color(0, 0, 0)`
-- the previous black-flash was pointless (rgb_policy re-painted
within 50 ms anyway) and now the transition to the dim red ramp is
smoother.

Also cleaned up the ladder comment above the constants to reflect
the new 0..200 / 200..1000 / 1000..2000 / ... phases.

### 4.4 Files touched (summary)

- `firmware/esp-idf/components/field_capture/firmware_version.h`
  -- `0.4.24` -> `0.4.25`.
- `firmware/esp-idf/components/field_capture/fc_common.c`
  -- press-edge haptic (+ include).
- `firmware/esp-idf/components/field_capture/fc_shutdown.c`
  -- removed `s_ship_mode_latched`, added `SHDN_VISUAL_MS` +
  `visual_owned` state, rgb_policy pause / resume dance, updated
  ladder comment.

Not touched: `BTN_DOUBLE_GAP_MS = 350` (Ivan preferred the
press-edge haptic over shortening the double-click window),
`BTN_LONG_PRESS_MS = 1000`, `rgb_policy` itself, wrist gesture.

Compiles-clean on inspection; not yet flashed. Ivan flashes next.

---

## 5. Next steps

**Bench flash (0.4.25):**

1. `idf.py build` -- verify no regressions.
2. `idf.py flash monitor` on iv7.1.
3. Boot: expect the usual two boot buzzes; no new startup chatter.
4. **Turn-on responsiveness:** click the button in STANDBY. Expect
   a **firm click on press** (new -- press-edge haptic). Mode entry
   still happens ~350 ms later on release (BTN_DOUBLE_GAP_MS
   unchanged), but the press-edge ack should kill the "did that
   register?" feeling.
5. **Turn-off visible ramp:** press-and-hold. Expect the LED to
   start showing dim red at ~200 ms and ramp toward full red as
   the hold progresses. Release before 1 s: click still fires the
   mode as before. Release 1..2 s: no click, LED goes back to
   rgb_policy.
6. **Turn-off USB behaviour:** on USB, hold 4 s, feel the
   `LONG_BUZZ`, see red LED + warn log. Then hold again 4 s --
   expect the SAME behaviour again (LED, buzz, warn). Old code
   went silent on the second hold; new code doesn't.
7. **Turn-off battery behaviour:** on battery, hold 4 s -- BATFET
   drops as before. First (and only) fire behaves identically.

**If it feels right on bench, roll to Stage 19 (power)** -- the
button work does not block or interact with the Stage 19 park /
idle / off boot-policy rewrite.

**Archive on wrap:**

```
python docs/build_info/reference_files/make_archive.py stage-20-button-responsiveness
```

---

## 6. Bench flash (0.4.25) results + second batch (0.4.26)

Ivan flashed 0.4.25, reported: "not better. still doesn't turn on for
5 s after press, doesn't turn off 9 s after shutdown sequence."
Follow-up Q&A clarified the real symptoms are NOT the click-vs-hold
timing Stage 20 §3 addressed. Actual symptoms:

- **"Turn on":** device is OFF (BATFET shipped). Press-and-hold to
  power on takes ~5 s. Short press does nothing (BQ QON is only
  triggered by longer press). Was never about entering modes.
- **"Turn off":** LONG_BUZZ fires at 4 s (correct); then 9 s dead
  time before BATFET actually opens and the device dies.
- Same behaviour on USB and on battery.
- **Stage 20 press-tick DOES fire when the device is on** ("Yes -- I
  feel a firm click the moment I press"). So the press-edge haptic
  landed fine; it just doesn't help the power-on case because the
  ESP isn't running yet.

Diagnosed two real BQ25619 bugs in `bq25619.c`:

### 6.1 Wrong I2C mutex (bus 0 vs bus 1)

`bq25619.c` locked `g_i2c_mutex` (bus 0) on every I2C access -- but
BQ25619 is on I2C bus 1 (`I2C_NUM_1`) with the DRV2605. Bus 1's
mutex is `g_i2c2_mutex` (already used correctly by drv2605/haptic).
This meant:

- `bq25619_init`, `bq25619_set_boost`, `bq25619_enter_ship_mode` (both
  retry ladder and release), and `task_battery_fn` all took the wrong
  lock.
- No mutual exclusion between BQ and DRV bus-1 transactions.
- Ship-mode's retry ladder waited up to ~5 s on bus 0's mutex, which
  is contended by the bus-0 sensor sweep (LSM / BME / PCF / MAX /
  TMP / LIS / VEML). Adds jitter to Ivan's "9 s dead time" reading.

Fix: `extern SemaphoreHandle_t g_i2c2_mutex;` (was g_i2c_mutex),
every `xSemaphoreTake/Give` swapped throughout the file. Header
comment in `bq25619.c` updated. `boot_hw_init.h:9,39` already
documents `g_i2c2_mutex` as the DRV2605 + BQ25619 bus lock -- this
was a straight-up latent bug.

### 6.2 BATFET_DLY not cleared in ship-mode -- the 9 s

The BQ25619's `REG_MISC` (0x07) has a `BATFET_DLY` bit (1 << 3):

- `= 1` (datasheet default): BATFET disconnect delayed ~10 s after
  ship-mode write.
- `= 0`: immediate disconnect.

Previous `bq25619_enter_ship_mode` only OR'd `BATFET_DIS` (bit 5),
leaving `BATFET_DLY` at its default. That is **literally** Ivan's
"9 s dead time between LONG_BUZZ and actual death."

Also missing:
- `BATFET_RST_WVBUS` (bit 4) should be set so BATFET auto-re-enables
  on a VBUS unplug -- prevents "device dead after ship + USB replug"
  which was Stage 3 §8.1 / Stage 6 lore.
- `BATFET_RST_EN` (bit 2) should be cleared so a fresh VBUS insert
  doesn't autonomously re-enable BATFET before the operator wants it.

Fix mirrors the Arduino reference exactly
(`firmware/arduino/12_lsm_full/hw.ino:226-232`):

```c
misc_new |=  (BATFET_DIS | BATFET_RST_WVBUS);
misc_new &= ~(BATFET_DLY | BATFET_RST_EN);
```

New bit defines added to `bq25619.h`:

- `BQ25619_MISC_BATFET_RST_WVBUS` (1 << 4)
- `BQ25619_MISC_BATFET_DLY`       (1 << 3)
- `BQ25619_MISC_BATFET_RST_EN`    (1 << 2)

`bq25619_init`'s existing "clear BATFET_DIS on boot" path uses a
local `MISC_BATFET_DIS` macro; left untouched (correct, just
stylistically duplicating the header define).

### 6.3 "5 s to turn on" -- not fixed this stage

Root cause is the BQ25619 QON wake-from-ship-mode timing plus ESP32
cold-boot + firmware init (DRV auto-cal up to 1.5 s, sensor probes,
SD mount). All of that runs before the LED / haptic can ack. No
firmware-only path shortens the QON portion. Possible wins later:

- Verify DRV auto-cal retry actually skips on cold boot when a
  saved period is in NVS (Stage 17 §4.5).
- Time-slice sensor probes with an early "boot OK" LED blink so the
  user sees life before the SD mount finishes.
- Defer non-critical inits until after first UI event.

Parked -- not a Stage 20 target.

### 6.4 Files touched (batch 2)

- `firmware/esp-idf/components/bq25619/bq25619.c` -- extern to
  `g_i2c2_mutex`, all take/give swapped, top-of-file comment
  updated, ship-mode write pattern rewritten to match the Arduino
  reference with per-write REG_MISC old->new log line.
- `firmware/esp-idf/components/bq25619/bq25619.h` -- three new
  `BQ25619_MISC_BATFET_*` bit defines + block comment naming the
  Arduino reference.
- `firmware/esp-idf/components/field_capture/firmware_version.h`
  -- `0.4.25` -> `0.4.26`.

### 6.5 Bench checklist (0.4.26)

1. `idf.py build` -- if the extern swap resolves cleanly across
   both compile units the mutex fix is done.
2. **Ship-mode timing:** hold 4 s on battery -- expect LONG_BUZZ
   then device dies within ~1 s (was ~9 s). Serial log will print
   `ship-mode REG_MISC 0xXX -> 0xYY` right before the write.
3. **Ship-mode on USB:** same 4 s hold -- LED red + warn ("Unplug
   USB to actually ship") should still fire, and unplugging USB
   should now drop VDD promptly instead of ~9 s later.
4. **Re-plug USB after ship:** BATFET_RST_WVBUS should let charging
   resume without a full VBUS-removal drama.
5. **DRV vs BQ contention:** during heavy DRV activity (rapid
   button clicks with new press-tick haptic), the BQ 1 Hz poll
   should still land -- watch for any `BQ25619` error logs
   suggesting bus 1 collisions.

The 5 s cold-boot "turn on" is not fixed this stage; separate
followup once we have real DRV-cal / SD-mount timings.

---

## References

- `docs/build_info/Mk1_build_reports/Stage_17_Firmware_Polish_Prep.md`
  §4.3 (long-press click swallow), §4.5 (button hard gate + ship-mode
  restart on USB) -- the Stage 17 code this fix builds on top of.
- `firmware/esp-idf/components/field_capture/fc_common.c`
  `button_poll()` -- press-edge haptic added.
- `firmware/esp-idf/components/field_capture/fc_shutdown.c`
  `watcher_ship_mode()` (latch removed), `task_shutdown_watcher_fn()`
  (visual pre-commit LED ramp added).
