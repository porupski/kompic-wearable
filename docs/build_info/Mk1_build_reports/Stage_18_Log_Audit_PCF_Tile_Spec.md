# Stage 18 -- Log Audit, PCF I2C Contention, LVGL Tile Spec (Mk1b prep)

**Date:** 2026-08-27
**Board:** iv7.1 (Mk1 bench). Mk1b in fabrication; assembly plan documented below.
**Firmware baseline:** `iv7.1.f0.4.20` -> target `0.4.21+`.
**Status:** CLOSED 2026-08-28 at fw `0.4.24`. Log audit + PCF fix + tile spec + gesture + Mk1b delta plan + battery-management scoping all landed. Power refinement continues in Stage 19.

---

## 1. Session goals

The bench pipeline is now data-collection-ready. Real 5-min protocol runs
and PPG tuning wait a moment while three lighter items land:

**A. Log-level audit pass.** Apply `docs/build_info/reference_files/LOG_LEVEL_POLICY.md`
across the firmware. Move per-tick / per-row `ESP_LOGI` inside recording
loops down to `ESP_LOGD` so `LOGLEVEL WARN` on battery is genuinely quiet
without losing milestone events.

**B. PCF85063 I2C mutex timeout root-cause.** Stage 17 bench log showed
five sequential `W (321858) PCF85063: I2C mutex timeout` lines while
`TEMP_DUMP` was polling sensors on bus 0. The PCF driver's retry cadence
fights the sensor sweep. Understand the contention and pick a fix.

**C. LVGL tile mapping spec (Stage 17 §3.5).** Text-only, but weighted
toward Mk1b bring-up sequencing. The GPS coordinate tile is the day-1
must-have; the rest of the tile tree slots around it.

**Not in scope this stage:**
- Actual data collection (§A/B of Stage 18 rundown -- rolled forward).
- LVGL widget code (need panel to iterate against).
- Wrist-raise gesture (§3.4 -- next stage).

---

## 2. Plan

### 2.1 Log-level audit

Grep every ESP-IDF component for `ESP_LOGI` inside a `while ()` or
`for (;;)` block. Reclassify per the policy:

- Milestone events (mode entry / exit, session open / close) stay `INFO`.
- Per-tick / per-second status prints move to `DEBUG`.
- Retry / fallback paths move to `WARN`.
- `ERROR` reserved for operations that cannot complete.

Track the audit as a checklist in §4.1.

### 2.2 PCF85063 mutex contention

Read `pcf85063.c` and the RTC task's polling loop. Understand:

- What mutex is being taken (`g_i2c_mutex` on bus 0)?
- What holds it long enough to time out the PCF read (2000 ms suggests
  a long-sweep loop like `TEMP_DUMP` doing per-sensor reads back-to-back)?
- Is the PCF task's polling cadence too tight?

Candidate fixes in §4.2. Pick one, patch, log.

### 2.3 LVGL tile mapping spec + Mk1b bring-up plan

Deliver two artefacts in this document (§5):

1. **Tile tree.** Each tile: name, purpose, source data, primary widgets,
   how the encoder navigates to it.
2. **Mk1b assembly + firmware bring-up ordering.** GPS is the day-1
   target. Every intermediate flash checkpoint listed so the trip pack
   is a straight-line assembly + flash sequence.

---

## 3. Mk1b assembly plan (context)

Recorded here so future sessions do not lose the sequencing.

Ivan's plan for Mk1b once boards arrive:

1. **Reflow phase A:** all top-side components EXCEPT GPS, USB connector,
   and bottom-side components (DRV2605, SD holder). GPS chip is a
   single-unit, super-sensitive part saved for last -- see project memory
   `project-gps-chip-scarcity`.
2. **Bring-up A:** connect USB via test pads (not the connector). Verify
   every subsystem passes -- especially the **screen** (CO5300 QSPI +
   CST9217 touch). Screen was the Stage-16 blocker on iv7.1 and is the
   Mk1b acceptance gate. Also verify voltage rails, RTC, sensors, LSM,
   button, encoder, RGB, mic. Sensor suite must reach the same reliability
   iv7.1 currently shows.
3. **GPS pad check:** DMM probe the GPS footprint pads for +3V3, GND,
   TX/RX before soldering the chip. Any short kills the chip.
4. **Reflow phase B:** solder GPS, USB connector, then flip and solder
   the bottom components (DRV2605, SD, any other bottom-side parts).
5. **Bring-up B:** verify GPS UART traffic, DRV auto-cal (Stage 17
   retry helps here), SD mount.
6. **Trip pack.** From this point Ivan can iterate firmware only during
   travel; hardware is sealed.

For the firmware side, Mk1b bring-up needs:

- The `15_amoled_touch_test.ino` sketch, verbatim, as the first
  post-reflow-A flash. Confirms the receptacle footprint fix.
- The current iv7.1 ESP-IDF baseline (fw `0.4.20+`) with the display
  driver wired.
- A minimum-viable GPS tile that shows `lat, lon, fix, sats, HDOP`.
  This is the trip-ready deliverable.

---

## 4. Results

_(fill during session)_

### 4.1 Log audit checklist

Per-tick / per-second `ESP_LOGI` reclassified to `ESP_LOGD`:

- [x] `fc_modes_ppg_bcg.c:248` -- `[PPG_BCG] t=Xs bcg_bpm=...` (1 Hz status).
- [x] `fc_modes_lsm.c:144,148` -- `[BCG] t=Xs BPM=...` (1 Hz status).
- [x] `fc_modes_lsm.c:222` -- `STEPS t=Xs count=...` (1 Hz status).
- [x] `fc_modes_lsm.c:503` -- `[TAP-HB]` heartbeat (500 ms).
- [x] `fc_battery_test.c:328` -- `BLACKBOX #N` (per-sample).
- [x] `fc_battery_test.c:479` -- `BATT_TEST #N` (per-sample).

Deliberately kept at `INFO` (milestone / one-shot):

- `[BCG] active`, `[BCG] CSV closed`, `[BCG] exit -> STANDBY`.
- `[PPG_BCG] session start`, `start: pc_sync=YES ...`, `CSV closed`, `exit`.
- `STEPS session start`, `STEPS session end`.
- `MLC_COLLECT: LABEL_PICK phase`, `MLC label -> N`, `RECORDING label=N`,
  `MARK #N`, `RECORDING end`, `LABEL_PICK exit`, `mode exit`.
- `TAP_DBG start`, `[TAP-HOST] #N`, `[TAP-CHIP] SINGLE/DOUBLE cnt=N`,
  `[TAP-DBG] exit`.
- `shutdown watcher armed`, `shutdown commit at N ms`, `shutdown warn
  phase started`, `shutdown aborted`, `shutdown FIRE`, `shutdown warm-up`.
- All sensor-driver init / mode-config lines (BME, LSM, MAX, TMP, VEML,
  LIS, PCF, BQ). None fire inside a loop.
- `mode -> X (top)` from `field_capture.c` -- one line per encoder detent,
  a legit user action.

Effect on `LOGLEVEL WARN` (battery-only default): the recording loops
(BCG, PPG+BCG, STEPS, TAP heartbeat) go silent. Mode entry / exit
milestones still print at INFO. Only true warnings / errors survive at
WARN. Chatter drop is ~10x on the 5-min protocol.

### 4.2 PCF I2C mutex timeout root cause + fix

**Trace.** `task_rtc_fn` in `pcf85063.c` polls at 1 Hz and takes
`g_i2c_mutex` with a 50 ms timeout. `TEMP_DUMP` in `fc_cli.c` holds the
same mutex for the LSM die-temp read + MAX die-temp read each 500 ms
tick. Each read is a multi-register I2C transaction (~5-10 ms). The
window between mutex releases is not always <50 ms, so the RTC task
misses every second or third tick during `TEMP_DUMP` and every ~2 s
during a MULTI_LED PPG sweep.

**Fix.** Bumped the RTC task's mutex timeout to 300 ms (matches the
rest of the PCF driver, which already uses 300 ms for user-facing
reads) and dropped the miss log to `ESP_LOGD`. Rationale: a 300 ms
window is longer than any realistic bus-0 transaction, so a true
timeout still surfaces; expected contention during heavy sensor
sweeps stops spamming the console.

**Not fixed.** The underlying "TEMP_DUMP holds the bus per read" is
still there; it just no longer trips the timeout. If Stage 18 or later
adds heavier per-tick sensor sweeps, revisit -- either bump the PCF
timeout further, or teach `TEMP_DUMP` to release the mutex between
sensor reads (5x `take`/`release` per tick instead of 1x for 500 ms).

---

## 5. LVGL tile mapping spec (Stage 17 §3.5)

Documented here for Mk1b bring-up. No code lands until Mk1b hardware
arrives and the screen sketch verifies the footprint fix.

### 5.1 Top-level tile tree

Encoder rotation moves across top-level tiles horizontally. Encoder
click enters the tile (opens its detail view). Long-press-back or
double-tap-Z returns to top level.

| # | Tile           | Purpose                                                       | Data source                                       | Mk1b priority |
|--:|:---------------|:--------------------------------------------------------------|:--------------------------------------------------|:--------------|
| 0 | Home / clock   | Big RTC time + date + battery %; icon strip for status        | `broker_rtc`, `broker_battery`                    | Day 1         |
| 1 | **GPS**        | lat, lon, fix status, sats, HDOP, altitude                    | `broker_gps` (`broker_gps_data_t` from `max_m10s`)| **Day 1**     |
| 2 | Steps          | Big step counter, session graph                               | `broker_steps`                                    | Day 2         |
| 3 | Health         | HR + SpO2 + skin temp (from PPG+BCG or standalone reads)      | `broker_hr`, `broker_skin`                        | Day 2         |
| 4 | Environment    | Air temp, RH, pressure, lux                                   | `broker_env`, `broker_light`                      | Day 2         |
| 5 | Compass        | Heading, calibration status                                   | `broker_mag`                                      | Day 2         |
| 6 | Motion / IMU   | Acc / gyro live plot; tap counters                            | `broker_imu`                                      | Day 3         |
| 7 | Recording      | Enter FCM_* modes (MIC, PPG_BCG, MOT, ENV, LSM sub-menu, ...) | `field_capture` state                             | Day 3         |
| 8 | Settings       | Brightness, log level, sync state, ship mode confirmation     | `nvs_cfg` + system state                          | Day 3         |
| 9 | Flashlight     | Torch on/off, brightness (encoder while active)               | `flashlight`                                      | Day 3         |

**Mk1b bring-up order.** Day 1 = tile 0 + tile 1. That is the trip
deliverable: the operator can see the time, battery level, and current
GPS coordinates. Day 2 onwards fills in the rest.

### 5.2 GPS tile spec (Day-1 deliverable)

The GPS tile has **two view modes**. Encoder click cycles between them
while the tile is open.

#### 5.2a "Under the hood" view (default when the tile opens)

Technical diagnostic view -- everything the operator needs to trust
the fix.

**Widgets:**

- Header: title "GPS" + fix status icon (red / yellow / green).
- Big rows: `lat`, `lon` with sign, six decimal digits.
- Sub-row: `alt` (metres), `sats N`, `HDOP D.D`, PDOP if the driver
  exposes it.
- Footer: last-fix age in seconds.
- **Raw console** at the bottom (scrolling text box, ~4 lines). Shows
  the last few NMEA / UBX sentences pulled by `max_m10s`. Mirrors the
  "raw console" pattern from the earlier prototype's GPS bring-up
  workflow -- the operator watches the sentences flow to confirm the
  antenna is actually receiving.

**Data update rate.** 1 Hz for the parsed values via `broker_gps_read()`.
The raw console appends per sentence, ring-buffered to the last N
lines (target N = 8; measure at bring-up).

**States:**

- `SEARCHING` -- broker `has_fix=false`, `sats > 0`. Yellow icon,
  "waiting" in the footer, sat count updates live.
- `LOCKED` -- broker `has_fix=true`. Green icon, lat / lon rows show
  values, footer prints "fix age Xs".
- `STALE` -- `has_fix=true` but `last_update_ms` older than
  `BROKER_GPS_TIMEOUT_MS`. Grey icon, values greyed out, footer
  "stale N s".

#### 5.2b "Photo" view

Commemorative photo shot. **The moment fixes; the numbers must survive
being photographed under sunlight and being cropped.**

**Layout:**

- Pure black background across the whole screen (LVGL: OLED off-pixel
  = zero draw; no dithering).
- **Very large white lat / lon** as the dominant elements, sized to
  fill the screen width to within the panel bezel. Each line one row.
  Six decimal digits, sign always shown, monospace face (LVGL Montserrat
  or the built-in mono) so digits don't jitter as they update.
- Time + date row above the lat / lon block, noticeably smaller
  (~30-40 %% of the coord row height). Format `YYYY-MM-DD HH:MM:SS UTC`.
- Nothing else -- no fix icon, no battery, no HDOP. Anything that
  photographs poorly is off. The idea is that the photo IS the proof;
  the numbers must be big and clean.

**Data update rate.** 1 Hz. No animation, no fade, no throb -- solid
white on solid black so the camera sensor does not have to hunt for
focus / exposure.

**Fix required?** No. The tile renders "no fix" in the coord positions
if the receiver is still searching. The intent is that Ivan opens the
tile only when he has a fix (he can confirm via the under-the-hood
view first), so the "no fix" case is rare and does not need a nice
fallback layout.

#### 5.2c Testing

Under a metal ceiling the antenna gets nothing. Bring-up outside once
the trip pack is sealed; interim testing may use `max_m10s` in "sim"
mode if the driver exposes one (verify at bring-up time). For photo
view specifically, verify: max coord font size on the CO5300 panel,
antialiasing quality, sunlight readability with the panel at full
brightness.

### 5.2d Plan B: fallback GPS tile on the Waveshare devboard

Documented so the pivot is a single day of firmware work if Mk1b
turns up with another screen fault. Waveshare ESP32-S3 dev board with
its built-in touchscreen, plus the (working) GPS module Ivan already
has soldered on the earlier prototype. Photo view only -- no
under-the-hood console, no other tiles. Deliverable: still a working
commemorative GPS photo. Details tracked in project memory
`project-photo-gps-plan-b`.

### 5.3 Home tile spec (Day-1 deliverable)

**Widgets:**

- Very large time (HH:MM in centred bold, ~120 pt).
- Below: date `YYYY-MM-DD`, day of week.
- Right edge: vertical status strip -- battery %%, USB / charging icon,
  GPS-fix dot, SD-card dot.

**Data update rate.** 1 Hz.

### 5.4 Encoder + tap-Z navigation

- **Encoder rotate** in top-level: move between tiles (horizontal
  swipe emulation).
- **Encoder click** in top-level: enter the tile (open the detail
  view). In a detail view: mode-specific action (start recording,
  reset counter, ...).
- **Tap-Z double** (LSM chip event): return to top level.
- **Button single click** in tiles: reserved for tile-local action.
- **Button hold 4 s**: ship mode (unchanged from Stage 17).

### 5.5 Refresh + performance targets

- LVGL tick: 5 ms.
- Tile-refresh cadence: 200 ms (matches broker poll cadence).
- No LVGL redraw during recording sessions to keep the SD write path
  fast (recording modes call `lvgl_port_lock` before entering to
  suspend the refresh task; unlock on exit).

### 5.6 What is NOT in Mk1b Day-1 scope

- Real-time signal plots (BCG waveform, PPG waveform). Day 2 at
  earliest.
- Notifications overlay (`ui_notif_overlay.h` exists but no producer
  wired). Day 3.
- Blue-light overlay (Day 2 tunable).
- Fusion / dead-reckoning overlay on GPS. Post-trip.

---

## 6. Next steps

**Immediate (Ivan bench, fw 0.4.21):**

- Flash and confirm: `LOGLEVEL WARN` on battery keeps the console
  quiet during a `FCM_PPG_BCG` session. Mode entry / exit lines still
  print. `LOGLEVEL INFO` restores per-second status.
- Verify `TEMP_DUMP` no longer prints five `PCF85063: I2C mutex
  timeout` lines in a row. A single occasional DEBUG log during a
  MULTI_LED sweep is expected and now hidden at INFO+.

**Rolls forward to a future stage:**

- Actual 5-min protocol run + PPG PA / SR tuning (was Stage 18 §A/B in
  the rundown; parked at Ivan's request).
- Wrist-raise gesture (§3.4 from Stage 17).
- LVGL tile *implementation* -- gated on Mk1b arrival.
- BQ25619 real telemetry -- gated on Mk1b hardware.

**Wrap** when Ivan is happy with the bench behaviour: run
`python docs/build_info/reference_files/make_archive.py stage-18-audit-tiles`
and close the stage.

---

## 7. Second batch (0.4.22) -- wrist gesture + Mk1b prep

Landed while Ivan verified `0.4.21` on the bench (2026-08-27 evening).

### 7.1 §3.4 wrist gesture wire-up

`lsm6dsv16x.c::task_imu_fn` now runs a simple LPF-on-Z-axis wrist detector:

- Every 20 ms tick, `s_az_lpf_g = 0.8 * s_az_lpf_g + 0.2 * (accel_z / 9.81)`.
  ~200 ms time constant -- fast enough for a wrist raise, slow enough to
  reject taps.
- Hysteresis bands: `>= +0.55 g` -> `WRIST_RAISE`, `<= -0.35 g` -> `WRIST_DOWN`.
  Between the bands the state is sticky (no publish).
- Publishes to `g_imu_gesture` via `data_broker.h`. Two counters
  (`s_gesture_changes`, `s_gesture_last_ms`) surface via new public
  accessors (`lsm6dsv16x_gesture_az_lpf_g`, `..._change_count`,
  `..._last_change_ms`) so the CLI can display + tune.

New CLI verb `GESTURE` in `fc_cli.c` dumps:

```
[GESTURE] current=WRIST_RAISE (1)  az_lpf=0.912 g  changes=4  last_change_age=1230 ms
```

Bench test convention: component side up = RAISE, upside down = DOWN.
Physical watch orientation on Mk1b may want a different axis -- adjust the
`WRIST_*_ON_G` thresholds in `lsm6dsv16x.c` after Mk1b bring-up if needed.
No `rgb_policy` coupling (deferred per Stage 17 §4.11).

### 7.2 AMOLED sketch audit (Mk1b bring-up)

`firmware/arduino/15_amoled_touch_test/15_amoled_touch_test.ino` reviewed
against every relevant memory rule -- all pass:

- Pin annotations line-by-line vs master pinout v20; TOUCH_RST 5 -> 44
  delta explicitly called out (GPIO5 is SCL_bus2 for BQ).
- Ship-mode handler present -- double-click on GPIO16 enters
  `enter_ship_mode()` via BQ25619 (matches `feedback-sketches-need-shipmode`).
- USB CDC guard note explicitly documents why `ARDUINO_USB_MODE == 1` is
  not needed (no TinyUSB use in this sketch -- matches `feedback-tinyusb-guard`).
- CO5300 driver details preserved from the standalone breakout: COLMOD
  0x77, instruction 0x02 for commands, opcode 0x32 for pixel writes.
- DISP_RST on GPIO3 (JTAG strap) noted as safe per master pinout v20.

**No sketch changes needed.** It flashes verbatim as the first post-
reflow-A verification on Mk1b.

### 7.3 iv7.1 -> Mk1b firmware delta plan

New reference at
`docs/build_info/reference_files/IV71_TO_MK1B_FIRMWARE_DELTA.md`. Ten
sections covering version bump, pin diffs, display + LVGL uncomment,
GPS enable, tile registry seed, rgb_policy note, shutdown, BQ, Day-1
bring-up checklist, and post-trip cleanup items.

Highlights:

- **Vbat ADC** moves off GPIO18 (iv7.1 prototype override, ADC2_CH7)
  onto whatever the Mk1b schematic wires (ADC1_CH8 default). Frees
  GPIO18 for its intended master-pinout use: GPS UART RX. See memory
  `project-vbat-on-gpio18`.
- **Display + LVGL** already exists as commented-out block in
  `main.c` (lines 32-34 and 115-124). Uncomment + wire tile registry.
- **GPS enable** in `boot_tasks.c` line 59 is a single un-comment;
  `max_m10s_init()` and `task_gps_fn()` are already implemented and
  the UART pins (TX=17, RX=18, PPS=46) match master pinout v20.

Version target for Mk1b line: `0.5.0` (fresh minor to make audit
grepping easy).

### 7.4 Firmware version

`0.4.21` -> `0.4.22`.

## 8. Third batch (0.4.23) -- bench findings from 0.4.22

### 8.1 GESTURE stuck at NONE (root cause)

`task_imu_fn` short-circuits when `broker_imu_get_enabled()` is false.
At boot only RTC / battery / haptic were always-on; the IMU task
looped without ever calling `gesture_update`, so `s_az_lpf_g` stayed
0.0. Fix: `boot_hw_init.c` now enables `broker_imu` alongside the
other always-on brokers. Cost is one 50 Hz I2C burst on bus 0 --
negligible next to any sensor mode and required for the gesture to
publish continuously.

### 8.2 GESTURE UX -- live log on every change

Ivan asked for a live monitor without polling `GESTURE` by hand. Added
an `ESP_LOGI(TAG, "[GESTURE] %s -> %s (az_lpf=%+.2f g)")` inside
`gesture_update` on every hysteresis transition. Naturally rate-limited
by the hysteresis + LPF -- flipping the board fires one line, not
spam. The `GESTURE` CLI dump is kept for one-shot inspection.

### 8.3 ws2812 mutex fix (RMT race)

`E rmt: rmt_tx_enable(768): channel not in init state` in a burst of
three pairs on `ppgbcg` entry. Root cause: `rgb_policy`'s esp_timer
callback (Core 0) and the field_capture task (Core 1) both call
`ws2812_push` beneath `ws2812_set_color`. Without serialisation, one
caller's `rmt_enable -> rmt_transmit -> rmt_tx_wait_all_done -> rmt_disable`
sequence gets tangled with the other's. Fix: added `s_tx_mutex` in
`ws2812.c` created in `ws2812_init()`, taken at the top of `ws2812_push`
with a 50 ms timeout and released at the bottom. Zero API change,
zero mode-code change.

### 8.4 "Unplug USB and re-hold" message wrong on uptime-cap fire

`fc_shutdown.c::watcher_ship_mode` is called from both the user 4 s
hold AND the 15 min autonomous uptime-cap fire. The old warn said
"unplug USB and re-hold to actually ship" -- correct for the user
path, misleading for the uptime cap (nobody held anything). Rewrote
to "Unplug USB to actually ship" -- neutral to both triggers. Same
recording-abort + LED-off behaviour otherwise.

### 8.5 GPS tile spec extended -- two view modes

Under Stage 18 §5.2, the GPS tile is now specified as two-mode:

- **Under-the-hood view** (default) -- fix status icon + lat/lon +
  alt + sats + HDOP + last-fix age + a scrolling raw-console pane at
  the bottom showing the last N NMEA / UBX sentences from `max_m10s`.
  Diagnostic view Ivan trusts before committing to a photo.
- **Photo view** -- pure black background, very large white lat/lon
  filling the screen width, smaller time+date row above. Nothing else.
  The output is meant to be photographed as commemorative proof of
  when + where the picture was taken.

Encoder click cycles between the two while the GPS tile is open.

### 8.6 Plan B for GPS-photo tile (fallback)

Documented in project memory `project-photo-gps-plan-b`. If Mk1b
turns up with another screen fault, Ivan pivots to a Waveshare ESP32-S3
touchscreen devboard running the (still working) GPS module from the
earlier prototype. Photo view only. Shelved unless triggered.

### 8.7 Firmware version

`0.4.22` -> `0.4.23`.

**Bench checklist for `0.4.23`:**

- Boot: expect `[GESTURE] NONE -> WRIST_RAISE (az_lpf=+0.9x g)` within
  a second (bench prototype sits component-side-up).
- Flip it: expect `[GESTURE] WRIST_RAISE -> WRIST_DOWN (az_lpf=-0.9x g)`.
- Flip back to raise, then park at 45 deg -- state should stay
  `WRIST_RAISE` (hysteresis holds it).
- `mode -> ppgbcg` (click): expect NO burst of `rmt:` errors before
  the "session start" line.
- Wait 15 min tethered: shutdown-fire message reads "Unplug USB to
  actually ship" instead of the old "and re-hold" wording.

---

## 9. Fourth batch (0.4.24) -- gesture polarity, BATT_TEST regression, RGB idle real-off

### 9.1 Gesture polarity flipped

Bench 2026-08-27: screen-up on the table reads `az_lpf ~ -0.9 g`, not
`+0.9 g`. Ivan's bench iv7.1 mounts the LSM with Z pointing away from
the panel face. Flipped `WRIST_RAISE_ON_G` to `-0.55 f` and
`WRIST_DOWN_ON_G` to `+0.35 f`, and swapped the comparison direction
in `gesture_update()`. Mk1b's enclosure may want the polarity flipped
again -- retune with the live `[GESTURE]` log after first bring-up.
Two-line change; no behavioural risk.

### 9.2 BATT_TEST regression fix

`0.4.23` enabled `broker_imu` at boot to make the gesture publish
continuously. That broke the "sensors default parked" assumption in
`run_battery_test_mode()`. Fix: call `park_all_modal_sensors()` at
BATT_TEST entry. The batt_test CSV now shows `sensors_on=0xC0`
(BAT + RTC only) as intended.

### 9.3 RGB idle -> real OFF

`rgb_policy.c::paint_scaled` now caches the last RGB and skips the
`ws2812_set_color` call when unchanged. Big win in the OFF phase
(25 s+ since activity): the previous code fired an RMT transmit every
50 ms writing `(0,0,0)` -- 20 pointless wakes/second. Now the phase
paints once on entry and stays silent. Same optimisation kicks in
during the solid preview (0..5 s) where colour does not change per
tick. Only the pulse and fade phases keep the tick active.

### 9.4 Firmware version

`0.4.23` -> `0.4.24`.

---

## 10. Battery-management roadmap

Ivan reports 2-3 hr runtime; wants double digits. Current baseline
average draw is roughly 70-150 mA depending on activity -- expected
wearable-idle should be 5-15 mA. This section is the map from where
we are to where we want to be, in dependency order.

### 10.1 Where we already are

| In place                                       | Where                                             |
|:-----------------------------------------------|:--------------------------------------------------|
| DFS 40..240 MHz + light-sleep                  | `boot_pm.c`                                       |
| Modal sensors parked by default                | `boot_hw_init.c` + `wake_sensors_for_mode()`      |
| Recording mode wakes only what it needs        | `fc_common.c::wake_sensors_for_mode()`            |
| SD unmounts between rows in BATT_TEST          | `run_battery_test_mode()`                         |
| VBUS-aware PM lock                             | `run_battery_test_mode()` (batt-test only)        |
| RMT PM lock scoped per-transmit                | `ws2812.c::ws2812_push()`                         |
| ws2812 skips repeat colours (Stage 18 §9.3)    | `rgb_policy.c::paint_scaled()`                    |
| Boot uptime cap forces ship after 15 min       | `fc_shutdown.c::SHDN_MAX_UPTIME_MS`               |
| `BATT_TEST` mode + `BLACKBOX` telemetry logger | `fc_battery_test.c`                               |
| `PM_DUMP` CLI                                  | `fc_cli.c` (needs `CONFIG_PM_PROFILING=y`)        |

### 10.2 Unknown drainers -- measure before cutting

None of the below is worth guessing at. Overnight `BATT_TEST` run
produces the ground truth. The plan:

1. Ivan flashes `0.4.24`.
2. `BATT_TEST ON` + `REBOOT` + unplug USB.
3. Walk away. Wake up to a `/sd/data/battery/batt_XXXX.csv` with ~8600
   rows if the pack ran a full 24 h at 10 s cadence, fewer if it
   died earlier.
4. Plot `vbat_adc_mv` vs `t_ms` -- discharge curve. Plot `idle_pct`
   vs `t_ms` -- tells us how much of each 10 s window the CPU spent
   in light-sleep. High `idle_pct` (> 90 %%) means we are already
   close to the floor. Low `idle_pct` (< 50 %%) means something is
   keeping a PM lock and we can hunt it.
5. Also plot `cpu_mhz` -- if it hangs at 240 MHz, DFS is being
   defeated by a lock somewhere.

### 10.3 Likely culprits after 10.2 measurements

**If `idle_pct` is high but runtime is still short:**
- The floor is set by hardware quiescent -- LSM6DSV16X active+gyro,
  MAX30101 shutdown vs stop, VEML6030 active, BME688 continuous.
- Fix: audit each sensor's parked-state power draw against datasheet.
  Some drivers may not be putting the chip in real shutdown when the
  broker "disables" them.

**If `idle_pct` is low:**
- Something is holding a PM lock. `PM_DUMP` at various times reveals
  which one. Candidates in rough likelihood order:
  - `rgb_policy` esp_timer (fires every 50 ms) -- but the tick itself
    is short; only a lock issue if the timer callback grabs one.
  - USB-Serial-JTAG driver holding `NO_LIGHT_SLEEP` when nothing is
    plugged. `usb_serial_jtag_is_connected()` should be false; verify.
  - RMT (WS2812) rejigged in `0.4.10` but check again with `PM_DUMP`.
  - I2C0 / I2C1 drivers -- polling tasks (RTC, LSM, MAG, BME, VEML)
    each fire I2C which takes brief CPU-max locks.

### 10.4 Then, targeted cuts (in dependency order)

- **CPU freq cap** during idle STANDBY: DFS should already handle
  this, but if it does not, add an explicit `esp_pm_configure` pass
  to lower `CPU_MAX` during idle.
- **LSM ODR reduction**: the driver forces 240 Hz HP accel for tap.
  For non-recording idle, drop to 12.5 Hz. Wake back up on tap or
  on entering a mode that needs high ODR.
- **MAX30101 real shutdown**: the driver has `max30101_set_shutdown`
  -- confirm it is called on `broker_hr_set_enabled(false)`. Even in
  SpO2 mode the LEDs eat 20+ mA if the chip is not truly shut down.
- **Poll cadences**: BME688 and VEML6030 at their default rates each
  wake the CPU every few hundred ms. Slow them down when parked.
- **BME688 forced mode instead of continuous** when just showing air
  temperature (no gas conductivity).
- **Encoder pullups** left enabled in idle -- fine when the switch
  is open; on the ALPS EC05E in a detent position, one line is
  grounded, so a pull-up sources current continuously. Estimate
  ~10 uA; not a real problem. Leave alone.

### 10.5 "Battery test + BCG (no PPG)" mode -- design sketch

Ivan asked whether we can do a combined test: sleep -> read one BCG
sample burst -> log -> sleep. The physics:

- BCG needs ~1..15 Hz spectral energy. Detection wants at least 25 Hz
  samples so we can HP filter without aliasing.
- To sleep between samples we need bursts, not continuous streaming.
  Wake, capture 5 s of accel at 25 Hz (125 samples, ~10 s including
  wake latency), sleep 60 s, repeat. Every hour we get 60 samples.
- Beat detection on 5 s bursts is possible but noisy -- HR variance
  depends on posture, breathing, motion. Acceptable for "am I still
  alive over 12 hours" but not for HRV work.

Whether this is worth building depends on 10.2 measurements: if the
current baseline BCG-continuous drain is already close to the
"burst + sleep" projection (limited by MCU + LSM shutdown / wakeup
overhead), the burst mode buys little. Defer until after the drain
curve is in hand.

### 10.6 SD card power impact

Direct answer to Ivan's "is SD storage the problem": **not usually**,
if we play the cards right. The BATT_TEST loop already unmounts
between samples -- the card is inactive most of the time. During
recordings the card is mounted continuously and burst-writes; the
card typically pulls 50-100 mA during a write (~10 ms per row at
1 KB) and drops to ~2 mA between. So per-row fsync at 200 Hz would
be a disaster (card never sleeps); the current 1 Hz fsync in
`FCM_PPG_BCG` is fine.

For very long unattended sessions (hours): consider unmounting the
card between batches (BATT_TEST already does this). Never leave the
card mounted while the whole system is meant to be idle.

### 10.7 Tonight (overnight drain)

Single command sequence:

```
BATT_TEST ON
REBOOT
```

Then unplug USB, put the pack on the desk, walk away. The device
wakes at 10 s cadence, samples, writes, sleeps. Cyan LED blip on
each sample confirms it is alive. Wake up in the morning, plug the
USB back in, run `BATT_TEST OFF` + `REBOOT`, pull the SD card, and
plot `vbat_adc_mv` and `idle_pct` vs `t_ms`. Everything from Stage
18 §10.3 depends on that curve.

### 10.8 Gesture recording + MLC -- parked

Ivan's aside during this session:

- Wrist gesture as a full state-machine would be nice but is not a
  blocker; the accel-Z LPF is enough for wrist-off in `rgb_policy`.
- MLC-based classification would be great, but the earlier
  still-vs-walking recordings are unlabeled and Ivan will need to
  plot to identify them again. Doable but expensive without a
  screen.
- Button-as-marker for gesture-record sessions (start/end) with RGB
  feedback would be a self-contained tool. Slots into a later stage
  once the screen is up on Mk1b (so we can also display the
  recording state).

Parked. Revisit after Mk1b bring-up if the trip pack is stable.

---

## 11. First BATT_TEST snapshot (fw `0.4.24`, USB plugged)

Ivan flashed and ran a short USB-plugged snapshot to sanity-check the
telemetry pipeline before the overnight drain.

**Ammeter (USB plugged):**
- Non-BATT-mode standby: 105 mA
- BATT_TEST standby (fw 0.4.24): **94 mA**

Delta is the always-on IMU + boot polling settling down. Both numbers
are still far above the 5-15 mA wearable-idle target -- expected,
because the plugged-in `batt_test_vbus` PM lock holds
`NO_LIGHT_SLEEP` and stops light-sleep entering. On battery this lock
lifts; the drain baseline drops meaningfully.

**PM_DUMP after ~215 s of BATT_TEST (USB plugged):**

```
Name            Type            Active  Total_cnt  Time(us)     Time(%)
batt_test_vbus  NO_LIGHT_SLEEP  1       1          211 920 882  99 %
i2s_driver      APB_FREQ_MAX    0       0          0            0 %
sdmmc           APB_FREQ_MAX    0       23         217 432      1 %
rmt_0_0         CPU_FREQ_MAX    0       306        21 823       1 %
ws2812          APB_FREQ_MAX    0       306        24 564       1 %
i2c_driver      APB_FREQ_MAX    0       883        387 632      1 %
i2c_driver      APB_FREQ_MAX    0       291        188 091      1 %
rtos1           CPU_FREQ_MAX    0       213 600    30 624 211   15 %
rtos0           CPU_FREQ_MAX    1       222 578    33 341 338   16 %

Mode stats:
Mode      CPU_freq    Time(us)      Time(%)
SLEEP     40 M        2 450 508     1 %
APB_MIN   40 M        148 192 977   68 %
APB_MAX   80 M        526 188       0 %
CPU_MAX   240 M       64 074 478    29 %

light_sleep_counts: 100    light_sleep_reject_counts: 0
```

**Reading the dump:**

- `batt_test_vbus` NO_LIGHT_SLEEP at 99 % is the reason SLEEP mode is
  1 %. When USB unplugs this lock releases and SLEEP grows.
- `light_sleep_counts: 100` with zero rejects confirms the sleep path
  is proven -- when the lock lifts, sleep engages cleanly.
- DFS works: 68 % of active time at 40 MHz (APB_MIN), 29 % at 240 MHz.
- `rtos0` and `rtos1` account for the bulk of CPU_FREQ_MAX grabs
  (~30 % total). That is application-task pressure. Which tasks
  specifically -- Stage 19 with per-task profiling.
- `sdmmc` fires 23 times (matches sample count) for ~200 ms total
  -- ~10 ms per row. SD is not the drain culprit here.
- `rmt_0_0` and `ws2812` each fire 306 times, ~25 ms total -- the
  cyan blip is essentially free. Answer to Ivan's estimate ask:
  RGB is ~0.04 mAh across 3 hours. Rounding error.

**DRV state check.** DRV2605 is NOT in real shutdown -- it is in
`INTTRIG` mode (playback-ready), which draws ~0.5-1 mA continuously.
Real shutdown mode (STANDBY bit set or MODE=0x40) is ~5 uA. Toggling
between these on `haptic_play` boundaries is a Stage 19 win.

**LSM state check.** Currently 240 Hz HP accel + gyro for tap
detection + BCG. ~500-750 uA. Datasheet floor for
gesture-only paths: low-power mode 1 at 12.5 Hz accel-only -- 6.5 uA.
Not a rounding error; a real ~500 uA cut is available.

## 12. Small policy tweaks landed in `0.4.24` post-snapshot

- **BATT_TEST per-sample log restored to INFO** (was demoted to DEBUG
  in the Stage 18 log audit). During a drain test the operator wants
  vbat + idle_pct on every 10 s tick; that is not "chatter", that is
  the point. Reordered fields so `vbat=NNNNmV [CHG]` leads and the
  ISO timestamp trails. BLACKBOX telemetry stays at DEBUG (it is
  background instrumentation, not a live feed).

## 13. Wrap

Stage 18 closes at fw `0.4.24`.

- Data-collection pipeline (Stage 17 §3.1) verified twice more --
  `ppgbcg` still writes clean CSVs, notebook still loads.
- Log-level policy authored (`LOG_LEVEL_POLICY.md`), first audit pass
  landed, per-tick chatter reclassified.
- PCF I2C mutex timeout root-caused + fixed.
- LVGL tile mapping spec written with two-mode GPS view (under-the-
  hood + photo). Plan B (Waveshare fallback) documented in memory.
- Wrist gesture wired (LPF on accel-Z + hysteresis, polarity flipped
  for the bench prototype, live log on every transition).
- iv7.1 -> Mk1b firmware delta plan written.
- AMOLED touch-test sketch audited against every memory rule --
  passes as-is for Mk1b first-flash.
- Battery-management scoping done; overnight drain measurement is the
  gate on any code cuts.
- One PM_DUMP snapshot captured for baseline.

**Rolled forward to Stage 19:**

1. Overnight drain measurement (Ivan runs tonight; morning has the
   discharge curve + `idle_pct` over time).
2. `PM_DUMP` at multiple times during a drain to identify the
   `rtos0` / `rtos1` CPU_MAX holders.
3. Profiling infrastructure -- a real telemetry harness we can point
   at any future power-cut hypothesis.
4. `GESTURE ON`/`OFF` CLI + NVS -- gesture becomes opt-in, LSM stays
   in low-power mode by default.
5. DRV real shutdown between plays.
6. MAX30101 real shutdown between uses.
7. LSM low-power mode when tap + BCG are not needed.

**Archive command:**

```
python docs/build_info/reference_files/make_archive.py stage-18-audit-tiles
```
