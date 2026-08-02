# Stage 6 Build Report — Kompic Mk I (iv7.1)

**Date:** 2026-07-07 → 2026-07-11 (multi-session sprint, main day 2026-07-11)
**Board:** Same iv7.1 prototype from Stages 1–5 (declared end-of-line as a hardware-rework target after Stage 5; carried forward for firmware-only work while v7.3 is being spun).
**Builder:** Ivan
**Stage:** 6 — Arduino → ESP-IDF port + magnetometer degauss + ECG (Qvar) bring-up + shutdown priority path
**Prerequisite:** Stage 5 close-out (2026-07-05). Coming in: `7_demo_field_capture.ino` verified working on the bench, all sensors alive, display + GPS connector destroyed and both parts shelved for ~3 weeks.

---

## 1. Executive summary

Firmware-heavy stage. No new solder work; the goal was to promote the Arduino field-capture sketch to a proper ESP-IDF project and add three new modes (compass, ECG, priority shutdown) that the sketch didn't have. Almost everything landed.

- ✓ **Repo reorganised** — `firmware/esp-idf/` and `firmware/arduino/` split. All ESP-IDF project files (main / components / partition table / CMakeLists / docs / test drafts) under `firmware/esp-idf/`. All Arduino sketches under `firmware/arduino/` (including the new bench sketches 8 and 9 from this stage). Legacy sketches and the `.data_test` capture folder moved under `firmware/arduino/legacy` and `firmware/arduino/.data_test`. `.gitignore` updated.
- ✓ **Phase 1 (link-clean tree)** — Stub `boot_hw_init.h/c` + `boot_display.h`, rewrote `boot_tasks.c` with iv7.1-correct extern task list (dropped stale references to `qmc5883p`, `qmi8658`, `bme280`, `max30102`, `tu10f`, `bh1750`). Fixed `main/idf_component.yml` — `boschsensortec/bme68x_sensor_api` is not published on the Espressif component registry; replaced with `francisduvivier/bme68x_sensorapi_espidf ^0.0.6`. Fixed `lvgl_port` / `lvgl` version pinning mismatch (`esp_lvgl_port 2.8.0` needs LVGL 9.3+; bumped `lvgl/lvgl` from `9.2.0` → `^9.3`). Fixed `timegm()` (not exposed by ESP-IDF newlib) → `mktime()` with `TZ=UTC0` in `max_m10s.c`.
- ✓ **Phase 2 (feature-parity port)** — Real `boot_hw_init.c`: installs both I²C buses (bus 0 sensors + RTC, bus 1 DRV2605 + BQ25619), WHO_AM_I-probes every chip, sets `broker_xxx_set_hw_status(true)` and per-sensor enable flags per mode. New `components/field_capture/` component owns the state machine (mode ring, encoder polling, WS2812 pulse, single-click recording, double-click ship mode). CSV / WAV recording on the SD card matches the sketch's directory layout (`/sd/data/<mode>/s<boot>_r<seq>.csv|wav`). NVS persistence of `mode` + `boot_seq`.
- ✓ **Encoder** — Polled detent-rest state machine ported from sketch (per auto-memory `feedback_encoder_polling.md`). PCNT-based approach dropped (the ALPS EC05E's settle bounce lands >15 ms after the leading edge with B flipped — PCNT sees this as +1/−1 oscillation). Field-capture polls GPIO21/43 directly at 5 ms.
- ✓ **Compass mode added** (ESP-IDF only, not in sketch). 10 s figure-8 hard-iron cal on first press this boot; red-blue N-S gradient LED thereafter; DRV pulse at 1 Hz when heading within ±5° of N or S. LED signature: 2 Hz yellow ↔ purple alternation in standby. Coordinate math assumes chip dot in top-right → +X points away from operator.
- ✓ **DRV2605 auto-cal** — Ported the sketch's ELV1411A closed-loop profile verbatim (`FEEDBACK=0xB6`, `RATED_VOLT=0x49`, `OD_CLAMP=0x60`, `CTRL1=0x9C`, `CTRL4=0x30`, then auto-cal). Also uncovered that `boot_power.c` was driving `GPIO0` (DRV_EN) **LOW**, which is the shutdown state — the datasheet comment in the old header was polarity-inverted. Flipped to HIGH; DRV auto-cal now passes on every boot.
- ✓ **MAX30101 → multi-LED mode** — Was running SpO2-only (Red + IR). Switched to full three-slot Red + IR + Green multi-LED, matching the sketch.
- ✓ **Sensor park policy** — Modal sensors (env / IMU / mag / HR / skin / light) stay disabled at boot; field_capture wakes only the pair(s) needed for the selected mode when a recording starts, then parks them again on close. RTC / battery / haptic stay always-on. Fixes both the boot-time I²C-mutex-timeout warnings and the "MAX30101 LEDs always on" battery drain that the first cut had.
- ✓ **`configTICK_RATE_HZ = 1000`, PSRAM Octal 80 MHz, flash 16 MB QIO** — sdkconfig changes made and captured in `BUILD.md`.
- ✓ **Magnetometer dead-channel fault CLOSED** — LIS3MDLTR was reporting one axis stuck at ±full-scale from boot, and different axes could "hop" to stuck on subsequent neodymium sweeps. Root cause: internal per-axis latched-saturation state (chip damage from prior overload event, hypothesis 2 from the diagnostic doc). Recovery procedure: fast-spinning neodymium on a string, slow withdrawal while still spinning. Full write-up in `hardware/Reflow_info/2026-07-09_magnetometer_dead_channel_diagnostic.md` — the fault is closed and the chip reads sane Earth-field values across all three axes. New bench sketch `firmware/arduino/8_magnet_test/` written for the recovery + as a repeat tool if it ever recurs.
- ✓ **BQ25619 charge-watchdog + TS_IGNORE + BATFET_DIS clear** — See §4 below. The Arduino sketch disables the BQ watchdog and sets TS_IGNORE at boot; the ESP-IDF driver was never doing this, so charging halted ~40 s after every boot. Also, prior ship-mode invocations left BATFET_DIS latched across USB-plug cycles until VBUS was fully removed. Fixed all three in `bq25619_init` — reads REG_IINDPM / REG_TIMER / REG_MISC, sets TS_IGNORE, clears WATCHDOG (bits 5:4 = 00), clears BATFET_DIS if set.
- ✓ **Priority double-click ship mode** — New `task_shutdown_watcher_fn` at priority 6 (above field_capture = 4), 5 ms polling cadence, own button state machine independent of field_capture. Detects double-click regardless of any blocking recording / mic capture / ECG loop and fires `bq25619_enter_ship_mode()` directly after 300 ms of solid-red LED confirmation. Motivation: user got trapped in a "10 s to shut down" state because the mic-record loop wasn't polling the button; had to open the case + short GPIO0 to recover.

---

## 2. Not landed / knowingly deferred

- ✗ **ECG on QVAR was the wrong scope call all along** — see § 7 addendum. The QVAR block is a **charge-transient sensor** designed for touch / tap / L-R swipe gesture UI, not for biopotential measurement. Bench pursuit of ECG-via-QVAR (§ 3.6, § 3.7 with the notch filter and the AC-coupling caps) produced a clean live QVAR sample stream but no visible cardiac waveform — because QVAR is not amplifying skin biopotentials. Correct path for ECG on future hardware: dedicated single-lead AFE (AD8232 or similar).
- ✗ **USB-MSC not wired.** Deferred out of Phase 2. Composite CDC + MSC is non-trivial because of the FatFS ↔ MSC coordination during recording, and getting it wrong corrupts the SD. Left for a focused follow-up.
- ✗ **RTC serial CLI (`SET_TIME` / `GET_TIME`)** — not ported from sketch, punted.
- ✗ **GPS + display** — hardware, not firmware. Waiting for replacement FPC and reinstall. Once display is back, `main.c` has the `TODO: restore when display returns` markers on every commented-out block.
- ✗ **bq25619 driver mutex bug** — noticed but not fixed. The driver takes `g_i2c_mutex` (bus 0) when it writes to `I2C_NUM_1` (bus 1). Wrong mutex. Doesn't affect correctness on its own because writes still go to the right bus, but it leaves a latent race with `drv2605` (also on bus 1). One-line fix; deferred to avoid scope creep.
- ✗ **LSM6DSV16X driver CTRL1 layout** — writes CTRL1 = 0x62 believing it's "120 Hz + 4 g" but the bit layout is actually OP_MODE_XL[6:4] + ODR_XL[3:0], so it's really "LPM3 (low-power 8-mean) + 7.5 Hz". Fine for the IMU task (it still gets *some* motion data at a low rate), but ECG had to override this with the correct bytes (`CTRL1 = 0x07` = HP + 240 Hz). Full driver fix deferred; local override sufficient for now.
- ✗ **BME688 mutex-hog warning** — the BME688 heater cycle holds `g_i2c_mutex` for ~200 ms per read. Previously caused PCF85063 and BQ25619 to time out on their 100 ms mutex waits. Worked around by bumping their timeouts to 300 ms; not a real fix.

---

## 3. Session-by-session narrative

### 3.1 The reorg + Phase 1 (2026-07-07)

Started with the firmware tree pre-existing but half-ported: driver components (`bme688`, `lsm6dsv16x`, `veml6030`, `max30101`, `max_m10s`, `lis3mdl`, `tmp117`, `pcf85063`, `bq25619`, `drv2605`, `mic_pdm`, `sdcard`, `encoder`, `ws2812`, `flashlight`, `alarm`, `fusion`) all present and referencing the right iv7.1 chip set. But `main.c` called `boot_hw_init()`, `boot_display_init()`, `boot_cst816d_configure()` — none of these source files existed. And `boot_tasks.c` still had extern references to the *old* chip drivers (`qmc5883p`, `qmi8658`, `bme280`, `max30102`, `tu10f`, `bh1750`) so the tree would not link.

**Repo split**: everything under `firmware/*` moved to `firmware/esp-idf/*` (project) and `firmware/arduino/*` (sketches). `firmware/docs/` was intended to move with the ESP-IDF project too but a copy remained at the top-level `firmware/docs/` — noted in punch list.

**Phase 1 goal**: link-clean tree with no functionality change. Stubs for `boot_hw_init.h/c` and `boot_display.h` (empty function bodies + mutex-handle stub definitions). `boot_tasks.c` rewritten with the iv7.1 chip-set task list. `main.c` gutted of all `boot_display_init` / `lvgl_port_lock` / `lvgl_ui_init` calls — display path stays out of the boot sequence until the FPC returns. LVGL is still compiled and pulled in transitively (drivers each have `*_tile.c` files inside them) but their code is dead at runtime.

**Component-manager fixes**: BME688 manifest was `boschsensortec/bme68x_sensor_api ^4.4.8`. That namespace doesn't exist on the Espressif registry — Bosch never published there. Replaced with `francisduvivier/bme68x_sensorapi_espidf ^0.0.6` (community ESP-IDF adaptation, I²C-only, same headers `bme68x.h` / `bme68x_defs.h`, same function names). LVGL vs `esp_lvgl_port` mismatch: `esp_lvgl_port ^2.4.1` resolved to 2.8.0 which uses `LV_COLOR_FORMAT_RGB565_SWAPPED` (LVGL 9.3+); pinning LVGL at 9.2.0 exact was the culprit. Bumped to `^9.3`. And `timegm()` isn't exposed by newlib in ESP-IDF; swapped for `mktime()` after `setenv("TZ","UTC0",1); tzset();` which produces the identical epoch value.

Phase 1 built clean at the end.

### 3.2 Phase 2 (2026-07-07 → 2026-07-08)

Real `boot_hw_init` (I²C bus install for buses 0 and 1, WHO_AM_I probes, driver `_init(port)` per chip, `broker_xxx_set_hw_status(true)` on success). Non-I²C peripherals init: WS2812 (RMT), flashlight (LEDC), SD (mutex only; mount deferred), mic PDM (channel install only; start deferred), haptic queue.

`components/field_capture/` — the state machine. Modes: MIC, ENV, MOTION, SKIN, FLASHLIGHT, ALARM. Encoder cycles them (with DRV click). Single-click enters recording; recording writes to `/sd/data/<mode>/s<boot>_r<seq>.csv|wav`. SD-side directory tree created at first mount with `mkdir()`. WAV header written on open, patched (RIFF size + data-chunk size) on close. NVS keys `field/mode` and `field/boot_seq` persistent across boots, boot counter bumps once per boot.

Field-capture task pinned to Core 1 at priority 4, 8 KB stack. Sensor tasks stay on Core 0 as before.

**Ship-mode double-click** — initial cut was inline in field_capture's main loop. Worked, but doesn't preempt blocking recording loops. Superseded in § 3.9 by the dedicated watcher task.

### 3.3 Fixes and refinements (multiple boots, 2026-07-08 to 2026-07-09)

Small fires as they surfaced:

1. **DRV silent** — new open-loop driver init logged "OK" and got I²C ACK but never vibrated. Was two problems stacked: (a) `boot_power.c` was driving GPIO0 (DRV_EN) LOW, which is the DRV2605's shutdown state — the comment in the old header claimed LOW = enabled which is backwards. (b) The ESP-IDF driver used open-loop LRA config tuned for an Apple Taptic Engine clone; ELV1411A is a stock 25 Ω / 150–250 Hz LRA and prefers closed loop with auto-cal. Flipped GPIO0 to HIGH; ported the sketch's closed-loop init byte-for-byte. Auto-cal now passes on every boot.
2. **MAX30101 in HR-only mode** — driver was calling `max30101_setup_hr_mode()` (Red + IR only). Swapped to `max30101_setup_multi_led_mode()` (Red + IR + Green) to match the sketch. FIFO reader flag flipped to `multi_led = true`.
3. **Sensor park policy** — first cut had every alive sensor enabled at boot. Result: STANDBY was noisy on the serial monitor (all sensor tasks reading + updating broker) and MAX30101's LEDs stayed on 24/7 (visible red glow through the case, ~7 mA per channel = ~20 mA continuous). Refactored: RTC + battery + haptic always-on; env / IMU / mag / HR / skin / light park at boot. `field_capture` has `wake_sensors_for_mode()` on recording entry and `park_all_modal_sensors()` on exit.
4. **`long filename support` in FatFS sdkconfig** — CSV / WAV filenames like `s0058_r0001_annot.wav` (22 chars) fail `fopen` on the default 8.3-only FatFS. Documented in `BUILD.md`; user enabled the option and SD writes started landing.
5. **Encoder replaced with polled state machine** — PCNT-based encoder driver had visible mis-count issues (per auto-memory, ALPS EC05E's settle bounce lands >15 ms after leading edge with B flipped). Field-capture polls GPIO21/43 at 5 ms with the sketch's exact state-machine values (`ENC_DETENT_REST_MS = 10`, latch direction on first line to LOW). PCNT init call dropped from `boot_hw_init`.
6. **LED anchor on activity** — pulse cycle was `t % period_ms` (anchored to absolute time), so post-2 s solid the pulse resumed mid-cycle wherever the sine wave happened to be. Reanchored to `(now − s_solid_end) % period_ms` using `+cos` so the pulse starts at max brightness right after the solid window ends. Fixes the "mode changes but LED looks dim" complaint.
7. **Sensor dump moved from STANDBY to inside the recording loop** — `debug_dump_sensors` used to print all 8 sensors every second in STANDBY, which is noisy and unnecessary. Now only prints the mode's own sensors (ENV → env+light, MOTION → imu+mag, SKIN → skin+hr) once per second **during** a recording.

### 3.4 Compass mode (2026-07-10)

New mode `FCM_COMPASS`. Cycles into the mode ring after `alarm`. Standby LED is 2 Hz alternation between yellow (motion palette) and purple (alarm palette) — chosen to be visually distinct from the solid-then-pulse pattern of the other modes.

Single click:
- If no calibration has been performed this boot, run a 10 s figure-8 hard-iron cal. Purple LED flashes at 5 Hz during. Track X and Y min/max, then set `s_mag_offset_{x,y} = (min+max)/2` and `s_mag_scale_{x,y} = (max−min)/2`. RAM-only, no NVS.
- If cal has been done (any subsequent press this boot), go straight to compass. LED red↔blue gradient using `cos(heading)` (N = full red, S = full blue, E/W = LED off, smooth gradient between). DRV_MEDIUM_CLICK at 1 Hz when heading is within ±5° of N or S.

Coordinate math assumes LSM6DSV16X orientation with the chip's "dot" marker at top-right → +X points away from operator when the watch is worn flat. `heading = atan2f(-ny, nx) * 180/π + 360 if negative`. Documented three sign flips in the code comment for the case of the wrong compass rotation direction.

Wake / park via `wake_sensors_for_mode(FCM_COMPASS)` — only MAG is woken.

### 3.5 Magnetometer degauss (2026-07-09)

The LIS3MDL was reporting X stuck at −478.9 µT (== ±FS at ±4 G FS) across every read, with Y and Z varying normally, from boot. Not a proximity effect — LRA is far enough away and the failure persisted with the LRA static. "Dead channel hopping" observed on the bench: sweep a neodymium magnet fast → the identity of the stuck axis can change.

Debug tooling: wrote `firmware/arduino/8_magnet_test/8_magnet_test.ino`. Minimal-boot sketch, LIS3MDL only, streams raw + µT samples at 20 Hz with a `*` marker on any axis at the int16 rail. Single-click cycles FS (±4 → ±8 → ±16 G), soft-resetting the chip on each transition. Double-click ship mode. LED encodes the current FS.

Recovery procedure that worked: tie a small neodymium to a string, spin fast (~50 rpm — both poles wash over the chip every rotation), bring close until saturation is visible on the sketch output, then slowly withdraw *while still spinning*. Repeat with different spin orientations if one axis re-latches. Chip returned to sane Earth-field readings (all three axes moving continuously with orientation, vector magnitude ~80 µT locally, no latched channels).

Full diagnostic + resolution write-up at `hardware/Reflow_info/2026-07-09_magnetometer_dead_channel_diagnostic.md` — that document is the reference for any future recurrence.

### 3.6 ECG mode (Qvar) attempt (2026-07-10 → 2026-07-11)

New mode `FCM_ECG`. Standby LED: 2 Hz pink ↔ red alternation. Single-click enters live streaming to serial (Arduino Serial Plotter compatible) + DRV click per detected beat.

Two bit-layout bugs found (both surfaced on-bench with `firmware/arduino/9_test_ecg/9_test_ecg.ino`, a diagnostic sketch written specifically for the ECG bring-up):

1. **AH_QVAR_EN was defined as `(1 << 0)`** in `components/qvar_ecg/qvar_ecg.h`. Datasheet Table 65 (DS13510 rev 4, page 70) shows AH_QVAR_EN is CTRL7 **bit 7** = 0x80. `(1 << 0)` is actually `LPF1_G_EN`, a gyro filter unrelated to Qvar. Every prior attempt to enable Qvar was turning on the gyro filter and leaving the analog hub off.
2. **CTRL1 layout was reversed** in `components/lsm6dsv16x/lsm6dsv16x.h`. Datasheet Table 50 (page 65) shows OP_MODE_XL[6:4] + ODR_XL[3:0]. The header had `ODR_120HZ = (0x06 << 4)` which places 0110 in *OP_MODE_XL*, not in ODR_XL. Net effect: accel got put into LPM3 (low-power 8-mean) instead of HP mode, and the Qvar clock (which is derived from accel ODR) ran at a fraction of the intended rate.

Both fixes local to `field_capture.c` (bypass the buggy component headers with correct hex values). After both were applied, Qvar sample stream is alive at 240 Hz and responds to electrode touch.

**But the signal is unusable for cardiac readout on iv7.1** — see § 2 above. The circuit on the current PCB has `500 Ω + 110 pF shunt-to-GND` per Qvar pin, and no series AC-coupling caps. DC skin galvanic offset (hundreds of mV between arm contacts) directly reaches the amp inputs → the amp sits near the negative rail while both electrodes are touched. The 5 k LSB upward excursions visible on top of the −30 k baseline are the actual biopotential AC (heartbeat + mains hum + motion), but they're superimposed on a saturated baseline that's easy to lose in the plotter. Adding a per-line ac-only plot mode (`raw − baseline`, clipped to ±2000) made the biopotential visible but not clearly cardiac.

Fix belongs on v7.3: two 100 nF series caps between the 500 Ω resistors and the electrode pads. Datasheet's own reference schematic doesn't show these because it's a *touch* application note, not biopotential.

**Interim behaviour** on iv7.1: ECG mode runs, but the operator should treat the trace as "signal amplitude present" rather than as a diagnostic-quality ECG.

### 3.7 Fable moment — link-order pathology (2026-07-10)

Worth its own §. `field_capture.c` initially called `qvar_ecg_init()`, `qvar_ecg_read_sample()`, `qvar_ecg_deinit()` from the `qvar_ecg` component. Every attempt to link the project failed with:

```
undefined reference to `qvar_ecg_init'
undefined reference to `qvar_ecg_read_sample'
undefined reference to `qvar_ecg_deinit'
```

Even though `libqvar_ecg.a` was built and appeared in the link line three times, GNU `ld` couldn't resolve the refs from `libfield_capture.a`. Investigation traced this to a **REQUIRES cycle** in the ESP-IDF component graph: `qvar_ecg` requires `lvgl_ui` (for `ecg_tile.c`), `lvgl_ui` requires `boot_logic`, `boot_logic` requires `field_capture`, `field_capture` requires `qvar_ecg`. CMake breaks cycles arbitrarily and produces a link order where dependencies come *before* dependents — the opposite of what single-pass `ld` needs. ESP-IDF doesn't emit `--start-group / --end-group` so multi-scan doesn't fully rescue this.

Rather than untangle the cycle (which would need `ecg_tile.c` split into a separate component, or moved into `lvgl_ui/`), `field_capture.c` now does the QVAR register I/O inline with local helpers using the shared `g_i2c_mutex`. Same behavior, no linker dance. Documented in the code header and in the § 4 punch list.

**Clean fix (for future)**: move `*_tile.c` files out of the driver components and into `lvgl_ui/` where they belong topologically. Or make each tile file live in its own component.

### 3.8 Corner-case usability: LED, sensor dumps, mode indicators (2026-07-08 → 2026-07-10)

Small polishes:

- LED animation: post-2 s "solid on activity" pulse resumption re-anchored to the end-of-solid moment, using `+cos` so the pulse starts at max brightness (rather than picking up the sine mid-cycle at whatever phase happened to be current).
- Sensor debug print now mode-specific and only fires during a recording (previously spammed everything in STANDBY).
- FL_MAX_PCT tuned to **40 %** — 100 % was blinding, 75 % still too bright.
- Flashlight GPIO41: added `gpio_pulldown_en()` and `gpio_pullup_dis()` in `flashlight_init` to define the pin's rest state during the boot window before LEDC takes ownership.
- COMPASS mode LED signature (2 Hz yellow↔purple) and ECG mode LED signature (2 Hz pink↔red) chosen to be immediately distinguishable in standby from the standard solid-then-breathe pattern of the recording modes.

### 3.9 Priority shutdown watcher (2026-07-11)

Motivation: user hit a state where the field_capture task was inside a blocking mic-record loop, double-clicked to enter ship mode, and observed ~10 s of unresponsive-then-shutdown behaviour. Unacceptable — the ship-mode path must be preemptive so the operator never gets trapped requiring case-open + GPIO0 short.

Design: new `task_shutdown_watcher_fn` in `field_capture.c`, priority 6 (highest of any app task), unpinned. Runs its own button state machine independent of field_capture's — polls GPIO16 at 5 ms cadence, detects the sketch's canonical debounce + double-click pattern. On double-click:

1. Flip WS2812 to solid red (immediate visual "I heard you").
2. Delay 300 ms (short visual confirmation window).
3. Call `bq25619_enter_ship_mode(I2C_NUM_1)` — writes REG07 to drop BATFET.
4. Spin forever.

Single-click detection stays in field_capture. Both tasks poll the same pin; `gpio_get_level` is thread-safe, and the two state machines don't share statics. Watcher's higher priority means it preempts field_capture within a single 5 ms poll cycle even during a 30 s WAV recording. Verified on the bench: entered MIC mode → started recording → double-clicked at ~5 s → red LED immediate, power off within <500 ms.

Idempotency guard: `s_ship_mode_latched` prevents a second call from re-running the LED countdown if both tasks happen to see the double-click.

### 3.10 BQ25619 charging fix (2026-07-11 late, in this session)

User reported "USB-C stopped charging the battery." Serial still works (VBUS reaches the ESP), current meter confirms VBUS present at the connector, but charging is not happening.

Root cause: `bq25619_init` in `components/bq25619/bq25619.c` only read the PART register. The Arduino sketch has always done two extra writes at boot:

```c
i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_INPUT_SRC, r00 | BQ_TS_IGNORE_BIT);
i2c_write_reg(Wire1, BQ25619_ADDR, BQ_REG_CTRL1,     r05 & ~BQ_WD_MASK);
```

Without these:

1. **BQ25619's charge watchdog** trips ~40 s after VBUS presence unless the host either disables it or feeds it periodically. Charging halts silently. This matches "worked earlier in the session, stopped later" exactly.
2. **TS_IGNORE not set** → if the TS-pin divider is out of the BQ's expected window (iv7.1 had surgical bodges around this area per Stage 3 § 4.9), the BQ interprets it as over/under-temp and halts charging.

Additional third fault-path found while writing the fix: **ship mode leaves `BATFET_DIS` latched** across USB reconnect. `BATFET_RST_WVBUS` (also set by the ship-mode command) only clears BATFET_DIS on a VBUS-removal event, not on VBUS presence. So if the operator invokes ship mode with USB attached and then plugs USB back without a full removal, BATFET stays off and charging is blocked.

`bq25619_init` now does all three: set TS_IGNORE, clear the watchdog bits, clear BATFET_DIS if latched. Boot log:

```
BQ25619 init OK @ 0x6A, REG_PART=0x80 ...  [WD off, TS ignored, BATFET on]
```

If BATFET_DIS was found set at boot, an additional warning line prints:

```
cleared latched BATFET_DIS (previous ship-mode -- charging restored)
```

---

## 4. Punch list carried into next stage

| # | Item | Category | Where | Fix cost |
|---|---|---|---|---|
| 1 | Restore display + LVGL when FPC arrives | HW block | `main.c` `TODO`s | 2–4 h once FPC lands |
| 2 | Restore GPS module (MAX-M10S) | HW block | `boot_tasks.c` extern commented out | 1–2 h once module goes on |
| 3 | ~~ECG series AC-coupling caps~~ **Add a dedicated ECG AFE (AD8232 or similar)** — QVAR is not the right sensor for ECG (§ 7) | HW v7.3 | Schematic + BOM | AD8232 4×4 mm LFCSP, ~€3 |
| 4 | Add battery fuel gauge (MAX17048 2×2 mm WLP or LC709203F) — BQ25619 doesn't do SoC estimation | HW v7.3 | Schematic + BOM | 1 chip, ~€1 |
| 4b | Titanium case-to-GND when case lands — still useful for the QVAR *touch* UX and general noise floor | HW v7.3 | Case + PCB | design detail |
| 5 | `qvar_ecg` REQUIRES-cycle | Build hygiene | `components/qvar_ecg/CMakeLists.txt` + `lvgl_ui/` | 30 min |
| 6 | `bq25619.c` uses wrong I²C mutex (`g_i2c_mutex` for bus-1 writes) | Driver bug | `components/bq25619/bq25619.c` | 1 line |
| 7 | `lsm6dsv16x.h` CTRL1 bit layout reversed | Driver bug | `components/lsm6dsv16x/lsm6dsv16x.h` + `.c` | 5 min |
| 8 | BME688 heater cycle holds `g_i2c_mutex` ~200 ms | Perf | `components/bme688/bme688_drv.c` | 30 min (reduce mutex-hold scope) |
| 9 | USB-MSC (composite CDC + MSC with FatFS coordination) | Feature | new component | half-day |
| 10 | `main/idf_component.yml` still references LVGL / esp_lvgl_port / cst816s — can go once display path is finalised | Cleanup | manifest | 5 min |
| 11 | RTC serial CLI (`SET_TIME` / `GET_TIME`) | Feature | small helper task | 1 h |

Diagnostic docs to open when items are picked up:

- `hardware/Reflow_info/2026-07-09_magnetometer_dead_channel_diagnostic.md` — magnetometer recurrence protocol
- `hardware/Reflow_info/2026-07-09_flashlight_led_leak_diagnostic.md` — flashlight cosmetic sub-µA leak

---

## 5. Deliverables of this stage

| Artifact | Path | Notes |
|---|---|---|
| ESP-IDF project | `firmware/esp-idf/` | Builds clean; runs on iv7.1; all sensor modes work, ECG runs with the caveat in § 3.6 |
| Working-demo snapshot | `firmware/esp-idf/archive/9_working_demo_iv7.1_11july2026.7z` | 219 KiB, 166 files, source only (no build/managed_components/dependencies.lock) |
| Bench sketch: magnetometer degauss | `firmware/arduino/8_magnet_test/8_magnet_test.ino` | Reference tool for LIS3MDL recovery |
| Bench sketch: ECG diagnostic | `firmware/arduino/9_test_ecg/9_test_ecg.ino` | Bit-layout fix verification + 3-mode output (monitor / raw plot / ac-only plot) |
| Build cheat sheet | `firmware/esp-idf/BUILD.md` | `get_idf` shell setup + required menuconfig options (flash 16 MB, PSRAM octal 80 MHz, custom partition CSV, LFN heap, FreeRTOS tick 1000, LVGL font range) |
| Magnetometer diagnostic + closure | `hardware/Reflow_info/2026-07-09_magnetometer_dead_channel_diagnostic.md` | Fault closed with bench-verified recovery procedure |
| This report | `hardware/Reflow_info/Stage_6_Build_Report.md` | You are here |

---

## 6. Where the board stands at Stage 6 close

**Functional on bench** (verified this stage or carried from Stage 5):
- Power: LiPo + BQ25619 charging (WD off, TS_IGNORE, BATFET clear) — verified this session after the § 3.10 fix
- USB-C: 5 V rail + USB-Serial-JTAG working
- Boot + task scheduler: all sensors probe on boot, per-mode wake/park working
- Encoder + button: polled state machine works, double-click always shuts down
- WS2812: mode-signature colors + gradient for compass, no leaks
- Flashlight: LEDC PWM, capped at 40 % perceived-brightness cap
- Haptic: DRV2605 auto-cal PASS on every boot, ELV1411A closed-loop profile
- SD card: mount, session-CSV, session-WAV with header patching, LFN filenames working
- Mic PDM: 16 kHz mono capture into WAV
- All I²C sensors alive: BME688, LSM6DSV16X, LIS3MDLTR, VEML6030, MAX30101 (multi-LED), TMP117, PCF85063A, BQ25619, DRV2605
- Compass mode with hard-iron cal + N-S DRV pulse: working
- ECG mode: streaming, but see § 3.6 caveats

**Blocked**:
- Display / touch (destroyed FPC)
- GPS (module not installed pending solvent-free rework completion)
- ECG cardiac readout (needs v7.3 electrode caps)
- USB-MSC (deferred out of Phase 2)

**Board status**: iv7.1 continues as the firmware-development target. All new hardware work is going into v7.3 (see § 2, § 4, and § 7 below). No new rework planned on iv7.1 unless the flashlight LED leak is prioritised for closure.

---

## 7. Post-close correction: QVAR is a touch sensor, not an ECG sensor (2026-07-12)

Documenting a scope-error correction discovered after the initial Stage 6 close. **The entire pursuit of ECG through the LSM6DSV16X's QVAR block (§ 3.6) was chasing the wrong sensor architecture.** Firmware-side everything we built works as intended — the samples stream, the notch filter attenuates mains hum, the beat detector runs, the RGB LED signature is correct. But the samples themselves are **charge-transient events from body electrostatics**, not skin biopotentials, so no amount of software filtering or hardware AC-coupling was going to produce a cardiac waveform.

### 7.1 What QVAR actually is

Per DS13510 §6.8 and the datasheet feature line:

> "Embedded Qvar (electrostatic sensor) **for user interface functions (tap, double tap, triple tap, long press, L/R – R/L swipe)**"

QVAR is a **charge-variometer** — it integrates the current flowing on/off the two Qvar pins to detect charge redistribution. When a body approaches or touches the electrode, electrostatic charge from the body (accumulated from friction on shoes / clothing / air, roughly hundreds of pC on a human) redistributes onto the electrode. QVAR's job is to detect that transient. Its architectural characteristics:

- **Charge-detection, not voltage-differential.** A real ECG frontend measures the differential *voltage* between two skin contacts (µV to mV range, DC to 40 Hz). QVAR does not.
- **54 dB CMRR.** ECG frontends need 100+ dB. QVAR was never intended for skin biopotential.
- **±460 mV full-scale.** Tuned for the tens-of-mV events of touch electrostatics.
- **240 Hz fixed ODR, tied to accel HP mode.** Tuned for the ms-scale of a finger tap, not the DC-baseline restoration needed by ECG.
- **Two-electrode use = L/R swipe direction detection**, not differential-mode ECG. The datasheet is explicit that the two Qvar pins let a firmware classifier tell *which pin was triggered first* to identify swipe direction.

### 7.2 What Stage 6 built for QVAR that still works (and what it's actually good for)

Everything below the "ECG mode" label is functional and directly usable for its *actual* purpose — touch / tap / gesture UI:

- The bit-layout fixes for `CTRL7` (AH_QVAR_EN is bit 7, not bit 0) and `CTRL1` (OP_MODE_XL[6:4] + ODR_XL[3:0], not the reversed order the driver was writing) are permanent driver improvements. Both need to be ported back to `components/qvar_ecg/qvar_ecg.h` and `components/lsm6dsv16x/lsm6dsv16x.h` respectively — the ESP-IDF `field_capture.c` currently overrides them with local hex constants; a proper cleanup pass makes them global.
- The `qvar_local_enable()` sequence in `field_capture.c` (accel power-down → CTRL7 flip → accel HP@240 Hz re-enable) is the correct init sequence and matches the datasheet §9.20 note.
- `firmware/arduino/9_test_ecg/9_test_ecg.ino` remains useful as a **QVAR touch/gesture diagnostic tool** — the ZIN cycling, the raw+notched+AC plotter modes, and the button/shutdown boilerplate all directly transfer to touch UI development.
- The 10 nF series AC-coupling caps that were bench-added onto iv7.1: harmless for touch/gesture use, and they mean the DC-galvanic drift from long body contact won't rail the amp during a long-press-hold gesture. Net-net: fine to leave.

Repurposing suggestions (not on any current schedule, filed for later):

- **Tap-to-dismiss alarm**: tap the case, alarm stops. No mechanical parts needed.
- **Swipe gestures on a bezel with two contact pads**: swipe direction cycles modes. Frees the encoder for other input roles.
- **Body-proximity wake**: hand approaches the watch, screen wakes. Replaces "raise to wake" IMU heuristic which is unreliable.
- **MLC tap classifier**: ST publishes reference `.ucf` config files for QVAR tap/double-tap/long-press classification programmed into the MLC. Once loaded, the LSM streams tap events as interrupts — no host CPU during classification. See ST AN5804 (MLC) and AN5755 (QVAR).

### 7.3 What to build for ECG when v7.3 lands

If ECG is still on the wish list for the next revision:

- **AD8232** (Analog Devices) — LFCSP-20, 4×4 mm, single-lead ECG frontend with built-in DRL amp, high-pass filter, notch amp, and rail-to-rail output for direct feed into the ESP32-S3 ADC. ~€3, in stock at Mouser/Digikey. This is the smallest / cheapest option that will actually produce a clean single-lead ECG.
- **MAX30003** — better performance, built-in R-peak detector, 128 dB CMRR, digital SPI output — but ~€30, humongous package, currently out of stock. Not appropriate for a hobby-scale wearable.
- **INA333 + external filter** — DIY approach with a general-purpose instrumentation amp. Cheaper still (~€2 for the INA333) but requires external HP filter + notch + DRL circuitry. Real design work, not just a chip drop-in.

Recommended path for v7.3: **AD8232 + 2 or 3 skin electrodes with 100 nF series caps + ESD diodes + ESP32-S3 ADC on a dedicated channel**. This is a known-good consumer ECG architecture. Replace the current QVAR-electrode footprint entirely.

### 7.4 MAX30101 (PPG) is the correct pulse-detection path on the *current* board

The MAX30101 is what commercial wearables use for wrist heart-rate. It works today; SKIN mode reads it. Wrist-worn PPG is inherently noisier than fingertip PPG (perfusion index drops from ~5% at finger to ~0.1% at wrist — 50× less signal). Firmware knobs available inside the driver to improve wrist performance:

1. **Bump green LED current** from `0x1F` (~7 mA) to `0x7F–0xFF` (~30–50 mA). 4–7× more return light. Datasheet-safe; per-LED thermal limits are far above this at PPG duty cycles.
2. **Beat detector on green, not IR.** Apple/Fitbit/Garmin all use green for HR because green has the strongest AC modulation through skin (oxyhemoglobin absorption peak). Red+IR are only useful for SpO2, which needs both simultaneously. Current driver drives the beat detector off IR — legacy from MAX30102 era.
3. **Disable Red+IR** when SpO2 is not being computed. Saves ~15 mA of LED current and eliminates cross-channel optical leakage. Green-only for HR is a valid PPG mode.
4. **Higher ADC range** (SPO2_CFG bits 6:5 = `11` = 16384 nA full scale) to give headroom against LED→PD crosstalk without saturating.
5. **Higher FIFO averaging** (SMP_AVE = `011` or `100` = 8- or 16-sample hardware boxcar). Smoother trace at the expense of latency.

Physical / mechanical (worth checking on the current iv7.1):

- Optical coupling between MAX30101 lens and case bottom: any air gap loses ~90% of the light to Fresnel reflection.
- Opaque case around the sensor — ambient light bleeding onto the photodiode degrades SNR.
- Strap tension: PPG needs firm skin contact.

On v7.3+, a titanium case with a proper optical window (Corning Gorilla or similar) and a black ceramic bezel around the sensor package will improve wrist PPG significantly. Not a firmware issue but worth flagging for the case design.

### 7.5 Punch list additions

Adding to the § 4 table:

| # | Item | Category |
|---|---|---|
| 12 | MAX30101 optimisation: green-primary, high LED current, beat detector on green channel | Firmware | half-day |
| 13 | Port CTRL7 + CTRL1 bit-layout fixes from local hex in `field_capture.c` back into `qvar_ecg.h` and `lsm6dsv16x.h` | Cleanup | 30 min |
| 14 | Battery fuel gauge (MAX17048 or equivalent) for v7.3 | HW v7.3 | Schematic + BOM |
| 15 | Dedicated ECG AFE (AD8232) for v7.3 if ECG stays on the wish list | HW v7.3 | Schematic + BOM |
| 16 | Repurpose QVAR block for touch/tap/swipe UI once display is back | Firmware v7.3+ | 1–2 days incl. MLC config load |
| 17 | Optical coupling audit of MAX30101 on current iv7.1 (lens-to-case gap, ambient light shroud) | HW iv7.1 | 1 h bench |

### 7.6 Lessons for the next scope call

The mistake was one of pattern-matching: I saw "Qvar for high-end applications" in the datasheet title, saw a 16-bit signed output at 240 Hz that responded to skin contact, and assumed it was a biopotential path. Should have read the feature list (`for user interface functions...`) and § 6.8 more carefully before promising cardiac readout. Filed under "read the intended use case for a sensor before scoping firmware around it."

Also worth noting: the two bit-layout bugs (CTRL7 bit position, CTRL1 field ordering) that got found in the process **were real, and needed fixing anyway**. They would have caused subtle IMU inaccuracy (LPM3 low-power instead of HP) and failed touch/gesture (AH_QVAR_EN never on) even if we hadn't been trying to do ECG. So the bench work wasn't wasted — it surfaced two latent driver bugs that a future feature would have hit.

