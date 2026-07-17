# Stage 9 Firmware Report — Kompic Mk I

**Date:** 2026-07-18
**Hardware target:** iv7.1 (assembled prototype, bench-modified)
**Firmware trees:** `firmware/arduino/` (bench-first sketches) + `firmware/esp-idf/` (production port)
**Author:** written from Stage 7 field session + Stage 9 refinements

---

## 0. Purpose of this document

Snapshot of what the firmware does *right now*, per mode, per file, with the
threshold values and filter coefficients as they stand. This is the reference
for the next Mk II hardware pass (iv8.0) so that any changed sensor / axis
convention / footprint rotation can be replayed against firmware that's
known to have worked on iv7.1.

---

## 1. Executive summary

- iv7.1 boots to a working ESP-IDF firmware on every reset (Stage 5 fixes held).
- **Ten modes** in the ESP-IDF encoder-driven dispatcher: MIC, ENV,
  MOTION, SKIN, FLASHLIGHT, ALARM, COMPASS, QVAR, TEMP, BCG.
- **Single button:** click = enter mode / trigger action / exit recording
  session; 4-second hold = BQ25619 ship-mode (via a priority-6 watchdog
  task independent of the mode loop).
- **Damage-control hard cap: 15-minute uptime = automatic ship-mode**,
  regardless of state. Protects the battery if the button dies or the
  operator forgets to shut down.
- **All sessions record to SD** (`/sd/data/<mode>/s%04d_r%04d.csv|wav`)
  except FLASHLIGHT / ALARM. **No fixed session duration** — recordings
  run until the button is clicked again (or the 15-min cap fires).
- **Every SD file carries a provenance header** with RTC timestamp, hardware
  revision (`iv7.1`), and firmware version (`KOMPIC_FW_VERSION`). WAV files
  get a matching `.meta.txt` sidecar since WAV lacks a text-comment field.
- **Every driver component carries a `<NAME>_DRIVER_VERSION` macro** at the
  top of its main header. Current baseline: `"0.2.0"` across the board.
  Ivan bumps MINOR on feature adds; Claude bumps PATCH on any code change;
  MAJOR bumps on release quality (beta / RC / GA).
- QVAR and BCG each have a **compile-time plotter/monitor toggle** for
  Arduino-Serial-Plotter debugging without editing the plot output paths.
- Compass is in a **reverted, simple 2D form** (mag X/Y only, red-N /
  blue-S cosine gradient). Tilt-comp attempted in Stage 7 didn't align
  with the LSM6DSV16X axis convention on this PCB — postponed until Mk II
  where the footprint rotations can be captured cleanly.

---

## 2. Repo layout — the two firmware trees

```
firmware/
├── arduino/                     bench sketches, one hardware area each
│   ├── 1_smoke_stage1_reflow1/  post-reflow smoke test after Stage 1
│   ├── 2_smoke_stage2_hotair/   after hot-air rework
│   ├── 3_smoke_stage3_full/     all-sensors-alive smoke on the full stack
│   ├── 4_demo_flashlight/       flashlight-LED PWM demo
│   ├── 5_demo_sensor_logger/    Arduino-side sensor logger to Serial
│   ├── 6_test_sd_full/          SDMMC bring-up + card format probe
│   ├── 7_demo_field_capture/    the "reference" firmware sketch (ported to ESP-IDF)
│   ├── 8_magnet_test/           LIS3MDL degauss + hard-iron cal isolation
│   ├── 9_test_ecg/              QVAR bench (mis-scoped as ECG, retained as touch/gesture tool)
│   ├── 10_test_max30101/        MAX30101 wrist-PPG diagnostic
│   └── legacy/                  older sketches, kept for reference
└── esp-idf/
    ├── main/                    application entry, boot task launcher
    ├── components/              per-driver + system components (25 dirs, see §3.1)
    ├── build/                   CMake / ninja output (gitignored)
    └── CMakeLists.txt / sdkconfig
```

**Rule of thumb**: any new hardware feature is proven in an Arduino sketch
first (fast iteration, no build system overhead), then ported to ESP-IDF
as a real component. Stage 6 § 7 is the correction of the one time this
rule was bypassed (QVAR as ECG) — cost several bench sessions.

---

## 3. Arduino sketches — status matrix

| # | Sketch | Purpose | State (2026-07-18) |
|---|--------|---------|--------------------|
| 1 | `1_smoke_stage1_reflow1` | ESP32-S3 alive, blink | ✅ pass |
| 2 | `2_smoke_stage2_hotair` | I2C bus + BME688 handshake | ✅ pass |
| 3 | `3_smoke_stage3_full` | All 8 I2C sensors respond | ✅ pass |
| 4 | `4_demo_flashlight` | Flashlight LED PWM ladder | ✅ pass |
| 5 | `5_demo_sensor_logger` | Serial CSV of every sensor | ✅ pass |
| 6 | `6_test_sd_full` | SDMMC mount + read/write | ✅ pass (requires MSDOS-formatted card) |
| 7 | `7_demo_field_capture` | Encoder-mode-selector + recorder | ✅ pass; ported to ESP-IDF as the baseline |
| 8 | `8_magnet_test` | LIS3MDL cal + degauss diagnostic | ✅ pass; degauss procedure documented in `2026-07-09_magnetometer_dead_channel_diagnostic.md` |
| 9 | `9_test_ecg` | (mis-named) QVAR bench | ✅ QVAR streams cleanly; retained as touch/gesture debug tool. **Not for ECG** — see Stage 6 § 7 |
| 10 | `10_test_max30101` | Wrist-PPG SNR probe | ✅ confirms MAX30101 optical limit at wrist |
| 11 | `11_test_bcg` | Ballistocardiography on LSM accel | ⏳ **planned**, not yet written. BCG logic in the ESP-IDF firmware works; a standalone sketch for pure bench iteration would help with threshold tuning without full-stack rebuild |

**Ship-mode boilerplate** (double-click BQ25619 BATFET drop) is present in
every sketch flashed to the iv7.1 prototype, per auto-memory
`feedback_sketches_need_shipmode.md` — the battery is permanently attached
on this unit and any sketch left running will drain it.

---

## 4. ESP-IDF project — architecture

### 4.1 Component graph

25 components. The critical dependency chain (from `main` upward):

```
main
 └── boot_logic  (kicks off tasks, holds hw init, ship-mode watchdog)
      ├── data_broker      (thread-safe pub/sub for every sensor)
      ├── field_capture    (mode dispatcher, LED, encoder+button state machine)
      ├── lvgl_ui          (display path — parked on iv7.1, no display fitted)
      ├── app_logic        (auxiliary tasks: alarms, RTC time-set CLI)
      └── cross_driver     (mutex + I2C helpers shared across sensors)
```

Sensor drivers (own components): `bme688`, `bq25619`, `co5300`, `cst9217`,
`drv2605`, `encoder`, `flashlight`, `lis3mdl`, `lsm6dsv16x`, `max30101`,
`max_m10s`, `mic_pdm`, `pcf85063`, `qvar_ecg`, `sdcard`, `tmp117`,
`veml6030`, `ws2812`.

Support: `alarm` (wake-buzz sequences), `fusion` (complementary filter,
currently linked but iv7.1 uses raw accel via LSM component directly).

### 4.2 Threading model

Priority ladder (higher = wins scheduler):

| Task | Priority | Core | Owns |
|---|---|---|---|
| **task_shutdown_watcher** | 6 | Core 1 | GPIO16 button poll, 4 s hold → BQ ship-mode, LED override during warn |
| Sensor tasks (LSM, LIS, BME, MAX, TMP, ENV, VEML) | 5 | Core 0 | I2C polling at per-driver ODR, publish to data_broker |
| **task_field_capture** | 4 | Core 1 | encoder / short-press / mode dispatch / LED |
| lvgl_ui task | 3 | Core 1 | (idle on iv7.1) |
| Idle | 0 | both | — |

**Ship-mode watcher runs above field_capture** deliberately: no matter what
mode you're in — even during a 30-s BCG recording — a 4-second hold
guarantees the BATFET drops. Warn buzz at 2 s, LED goes solid red with
0→WS_MAX fade-in over the full 4 s (Stage 7).

`g_shutdown_hold_active` flag suppresses mode-owned LED writes while the
watcher owns the LED.

### 4.2b Recording session semantics (Stage 9 change)

**No fixed duration on any mode.** All CSV / WAV recording sessions run
until the user clicks the button. The `RECORDING_MS` constant and the
per-mode 30-second timers (BCG_SESSION_DURATION_MS, QVAR_SESSION_DURATION_MS)
were removed in Stage 9 — the file just keeps growing until you click.

The two safety nets that remain:

1. **Button click** — user exit, natural termination.
2. **15-minute global uptime cap** (`SHDN_MAX_UPTIME_MS`) — if the firmware
   has been running for 15 minutes total, `task_shutdown_watcher_fn` fires
   `watcher_ship_mode()` unconditionally. Damage control for stuck buttons,
   forgetful operators, or any state where the mode loop won't exit. On
   trigger: long DRV buzz, LED goes solid red, BATFET drops.

`s_recording_early_end` is set by the button poll and propagates into
`wav_record_to()` (which now polls the button internally so MIC WAV can
also exit on click). The WAV header is patched with the actual written
size at fclose, so any duration is valid — no fixed-size assumption in
the recording code.

`VOICE_ANNOT_MS = 5000` still exists — that's the fixed-length voice
annotation prelude for the CSV modes (ENV / MOTION / SKIN), not a data
recording limit.

### 4.2c Provenance headers on every SD file (Stage 9 addition)

Every CSV opened by `csv_open()` starts with:

```
# rtc_start=<ISO-8601> hw=iv7.1 fw=0.2.0 boot=<n> seq=<n> mode=<dir>
<column-header-row>
<data rows...>
```

Every WAV file gets a matching sidecar at `<same-stem>.meta.txt`:

```
rtc_start=<ISO-8601> hw=iv7.1 fw=0.2.0 boot=<n> seq=<n> tag=<mic|annot>
```

The `hw=` and `fw=` values come from `components/field_capture/firmware_version.h`:

```c
#define KOMPIC_HW_VERSION   "iv7.1"
#define KOMPIC_FW_VERSION   "0.2.0"
```

Bump `KOMPIC_FW_VERSION` whenever any driver's `<NAME>_DRIVER_VERSION`
bumps. That way any recording in the field can be traced back to the exact
firmware source-tree state that wrote it — no ambiguity about which
threshold / filter / register-layout version was in effect.

### 4.2d Per-driver versioning (Stage 9 addition)

Every ESP-IDF component's primary header now carries:

```c
// Driver version: MAJOR.MINOR.PATCH -- bump PATCH on any change here,
// MINOR on feature adds, MAJOR on release quality (beta / RC / GA).
#define <NAME>_DRIVER_VERSION  "0.2.0"
```

Full list of the 26 macros seeded at baseline `"0.2.0"` in Stage 9:

| Component | Macro |
|---|---|
| alarm | `ALARM_DRIVER_VERSION` |
| app_logic | `APP_LOGIC_DRIVER_VERSION` |
| bme688 | `BME688_DRIVER_VERSION` |
| boot_logic | `BOOT_LOGIC_DRIVER_VERSION` |
| bq25619 | `BQ25619_DRIVER_VERSION` |
| co5300 | `CO5300_DRIVER_VERSION` |
| cross_driver | `CROSS_DRIVER_DRIVER_VERSION` *(name reads awkward — cosmetic)* |
| cst9217 | `CST9217_DRIVER_VERSION` |
| data_broker | `DATA_BROKER_DRIVER_VERSION` |
| drv2605 | `DRV2605_DRIVER_VERSION` |
| encoder | `ENCODER_DRIVER_VERSION` |
| field_capture | `FIELD_CAPTURE_DRIVER_VERSION` |
| flashlight | `FLASHLIGHT_DRIVER_VERSION` |
| fusion | `FUSION_DRIVER_VERSION` |
| lis3mdl | `LIS3MDL_DRIVER_VERSION` |
| lsm6dsv16x | `LSM6DSV16X_DRIVER_VERSION` |
| lvgl_ui | `LVGL_UI_DRIVER_VERSION` |
| max30101 | `MAX30101_DRIVER_VERSION` |
| max_m10s | `MAX_M10S_DRIVER_VERSION` |
| mic_pdm | `MIC_PDM_DRIVER_VERSION` |
| pcf85063 | `PCF85063_DRIVER_VERSION` |
| qvar_ecg | `QVAR_ECG_DRIVER_VERSION` |
| sdcard | `SDCARD_DRIVER_VERSION` |
| tmp117 | `TMP117_DRIVER_VERSION` |
| veml6030 | `VEML6030_DRIVER_VERSION` |
| ws2812 | `WS2812_DRIVER_VERSION` |

**Rules going forward:**
- Any driver code touch → PATCH bump on THAT driver's macro + on
  `KOMPIC_FW_VERSION`.
- Feature add (new sensor, new mode, new API surface) → MINOR bump on the
  affected macro(s) + on `KOMPIC_FW_VERSION`.
- Release readiness (beta / RC / GA) → MAJOR bump on `KOMPIC_FW_VERSION`;
  driver macros can lag if unchanged.

Currently these macros are informational only — they aren't compared
across component boundaries or embedded per-driver in the file header.
Room to expand later (e.g., emit a full driver-version dump into the file
header, or fail-fast on mismatched versions between paired components).

### 4.3 SD data layout

```
/sd/data/
├── mic/    s0001_r0001.wav + s0001_r0001_annot.wav (voice-annotation prelude)
├── env/    s0001_r0001.csv (BME688 + VEML6030 @ 2 Hz)
├── mot/    s0001_r0001.csv (LSM6DSV16X + LIS3MDL @ 10 Hz, 9 axes)
├── skin/   s0001_r0001.csv (TMP117 + MAX30101 HR @ 10 Hz)
├── bcg/    s0001_r0001.csv (LSM accel_z @ 240 Hz, 30 s window)  [new]
└── qvar/   s0001_r0001.csv (QVAR raw+HPF @ 250 Hz, 30 s window) [new]
```

- `s%04d` = boot sequence (NVS-persistent, monotonic across power cycles).
- `r%04d` = recording sequence (resets per boot, increments per session).
- All dirs are `mkdir`-created lazily by `ensure_sd()`; card must be
  MSDOS-formatted (Kodak SDXC cards died on us in Stage 4 — auto-memory
  `feedback_suspect_cheap_sd_cards.md`).

---

## 5. Mode-by-mode reference

Encoder cycles between modes; single click enters the selected mode.
LED signatures below are the **standby** rendering (idle in the mode
before entering it) — the **session** rendering is described in each
mode's paragraph.

| Mode | Standby LED | Session behaviour |
|---|---|---|
| MIC | red solid+pulse | 30 s WAV @ 16 kHz PDM |
| ENV | green | CSV @ 2 Hz, BME688 + VEML6030 |
| MOTION | yellow-green | CSV @ 10 Hz, LSM6DSV16X + LIS3MDL |
| SKIN | pink | CSV @ 10 Hz, TMP117 + MAX30101 |
| FLASHLIGHT | white | LED on, encoder = brightness ladder |
| ALARM | purple | 15 s DRV buzz pattern |
| COMPASS | red↔blue 2 Hz alt | live heading, `cos(θ)` gradient |
| QVAR | yellow↔purple 2 Hz alt | 30 s SD record + touch classifier |
| TEMP | red→orange→yellow slow cycle | 5-source thermal map, 1 Hz print |
| BCG | yellow↔red 2 Hz alt | 30 s SD record + BPM estimate |

### 5.1 MIC (FCM_MIC)

- WAV @ 16 kHz mono, 30 s, saved to `/sd/data/mic/`.
- Voice-annotation prelude: 3 beeps + WAV lead-in file (`_annot.wav`).
- LED: red solid during record, pulsing 0.5 Hz thereafter.
- Uses PDM mic on GPIOs from `cross_driver`.

### 5.2 ENV (FCM_ENV)

- BME688 die+air+humidity+pressure+gas_resistance
- VEML6030 lux
- CSV row: `time_ms, temp_C, humidity_pct, pressure_hPa, gas_ohm, lux`
- Cadence: 2 Hz (500 ms per row) for 30 s. Cadence tunable via
  `row_period_ms` in `run_recording_for_current_mode`.

### 5.3 MOTION (FCM_MOTION)

- LSM6DSV16X (accel + gyro, both ±4 g / ±2000 dps, ODR 240 Hz internally)
- LIS3MDL (3-axis mag, ODR 80 Hz)
- CSV row: `time_ms, ax, ay, az, gx, gy, gz, mx_uT, my_uT, mz_uT`
- Cadence: 10 Hz (100 ms per row).
- Sensor sample rates are much higher than the CSV cadence — broker
  publishes the latest available.

### 5.4 SKIN (FCM_SKIN)

- TMP117 skin-temp (± 0.1 °C).
- MAX30101 PPG (green-only slot config on iv7.1 — no on-chip DC-cancel DAC,
  so wrist-side SNR is limited; see Stage 6 § 3.5).
- CSV row: `time_ms, skin_temp_C, bpm, finger_detected, spo2_pct`.
- Cadence: 10 Hz.
- **HR estimate is unreliable on wrist** because MAX30101 wasn't designed
  for it — kept for logging, Mk II swap to a proper PPG AFE tracked in
  Stage 8 § 4b.

### 5.5 FLASHLIGHT (FCM_FLASHLIGHT)

- Single click: LED on. Encoder while on: brightness ladder (`FL_LEVELS`).
- Single click again: off.
- No SD recording.

### 5.6 ALARM (FCM_ALARM)

- Fires a 15 s DRV2605 wake-buzz pattern (STRONG_CLICK + LONG_BUZZ mix).
- Single click during: interrupt.
- No SD recording.

### 5.7 COMPASS (FCM_COMPASS)

- **Current: simple 2D** — heading = `atan2(+ny, nx)` where
  `nx, ny = (raw − offset) / scale` from figure-8 hard-iron cal.
- First press per boot runs the 10-second figure-8 cal
  (`run_mag_cal`, 5 Hz purple flash during cal).
- Subsequent presses skip cal, enter compass directly.
- Cal outlier gate: ±500 µT per axis (Stage 7 § 9).
- Rejects the whole cal if <50 accepted samples (3-click haptic to say
  "start over").
- LED: `rgb_compass_gradient(heading)`:
  - N (heading 0°) → pure red
  - S (heading 180°) → pure blue
  - E/W (heading 90/270°) → LED off (cos = 0)
  - continuous cos-based blend in between
- Haptic MEDIUM_CLICK once per second when within ±5 ° of N or S
  (via `heading_on_ns`).
- Log line at 2 Hz: `heading, raw(x,y) uT`.
- Sensors woken for this mode: **mag only** (IMU dropped after tilt-comp
  revert).

**Deferred / removed:** tilt-compensated heading, tilt-hue overlay
(yellow/green up/down). Reverted this session — Stage 7's attempt fell
apart on the LSM6DSV16X ↔ LIS3MDL axis-alignment question. Both chips
sit in the same PCB plane but their footprint rotations relative to each
other haven't been captured in code. Next steps notated at line-level
`TODO(iv8.0)` in `field_capture.c`.

### 5.8 QVAR (FCM_ECG in the code — display name "qvar")

- **NOT ECG.** QVAR is a charge-variometer for touch/gesture UI
  (Stage 6 § 7.1 correction, DS13510 Rev 4 § 6.8).
- LED: yellow↔purple 2 Hz alt, both standby and session.
- Sensor wake: **IMU is force-parked** at session entry — QVAR and the
  IMU share LSM CTRL1 / CTRL7, so we own the chip config for the whole
  session via `qvar_local_enable()`.
- Signal chain:
  - Read `OUT_AH_L/H` at ~250 Hz (driver ODR 240 Hz)
  - 1st-order HPF @ ~2 Hz cutoff, `alpha = 0.952`
  - Envelope of HPF output, `decay = 0.985 per sample` (~66 ms τ)
  - Slow raw mean EMA, `alpha = 0.02` (~50-sample smoothing)
- State classifier:
  | State | Condition |
  |---|---|
  | one-electrode (Qvar1 or Qvar2) | `envelope > 400` |
  | both electrodes | `\|raw_mean\| > 15000` AND `envelope < 150` |
  | no touch | else |
- Sign polarity from last big AC excursion (`abs_y > 800`) determines
  Qvar1 vs Qvar2.
- 30 s SD recording, columns `time_ms, raw, hpf, state`.
- Haptic MEDIUM_CLICK on rising edge into any touch state.
- Compile-time flag `QVAR_PLOTTER_MODE` (default 0 = monitor) — flip to 1
  for Arduino Serial Plotter tab-separated output.

**Known issue (2026-07-18 bench):** state classifier flaps rapidly
between Qvar1 / Qvar2 / both when touching. Envelope goes 17 k–21 k
during a single-electrode contact — well over the `> 400` "one-touch"
threshold but the sign latch inverts every ~10 ms, so we get
`Qvar1 → Qvar2 → Qvar1 → …` at 100 Hz effective. Fix TBD; likely wants
sign-hysteresis (require |y| > 2× onset threshold for opposite polarity
to take over) or a longer sign-hold window.

### 5.9 TEMP (FCM_TEMP)

- Aggregates every temperature source on the board:
  - **TMP117** — skin temp (~1 SPS default, first valid ~5 s after wake)
  - **BME688** — die + air (≠ same reading; BME688 has both)
  - **LSM6DSV16X** — on-die temp (register OUT_TEMP_L/H, 256 LSB/°C,
    offset +25 °C)
  - **MAX30101** — die temp via one-shot `TEMP_EN` (wake with LEDs
    forced 0, trigger, wait 35 ms, read TINT/TFRAC, back to shutdown)
  - **ESP32-S3** — SoC junction temp via `driver/temperature_sensor.h`
- Print cadence: 1 Hz to serial console (no SD recording — kept as
  monitor mode; add if you want time-series later).
- LED: red → orange → yellow slow cycle (~0.5 Hz).
- **MAX30101 gotcha (fixed this session):** was inadvertently kicking the
  MAX out of shutdown into PPG mode → green LED lit + local heating.
  `read_max_die_temp()` now zeros LED_PA[1..3] then wakes into HR mode
  with 0 mA current, triggers the temp conversion, and returns to
  shutdown. No LED activity, ~45 ms MAX-active window per read.

**Field observations (Stage 7):**
- LSM reads ~2 °C above ESP32-S3 SoC when on desk. Suspected PCB thermal
  spread + I²C bus activity heating the chip. LSM is adjacent to ESP on
  the left; BME+mic on the right; LIS+RGB below. Corrected the earlier
  hypothesis that MAX was next-door — the MAX is on a separate PCB entirely.
- TMP117 shows 0.00 °C for the first ~5 s of TEMP mode because the
  driver waits for the first averaged sample (default: 8-sample average
  at 1 SPS = ~1 s cycle + broker publish latency). Not blocking; could
  be sped up with a driver-side write to AVG=1.

### 5.10 BCG (FCM_BCG) — Stage 8 addition

- Ballistocardiography via LSM6DSV16X accelerometer Z-axis.
- Signal chain:
  - Read `accel_z` from broker (LSM publishes at ~240 Hz)
  - Normalise to g (`/ 9.81`)
  - HPF @ 1 Hz, `alpha = 0.9745`
  - LPF @ 15 Hz, `alpha = 0.2822`
  - → `filtered` (g), `filt_mg = filtered * 1000`
- Peak detector:
  - Envelope on `|filtered|`, `decay = 0.998 per sample` (~2 s half-life)
  - Adaptive threshold: `envelope * 0.4`, floor `0.003 g` (3 mg)
  - **Absolute peak** (catches both up-peaks and down-troughs — wrist
    BCG is biphasic)
  - Refractory: 400 ms (caps at 150 BPM)
- BPM: median-of-5 rolling window over the last 5 beat intervals.
  Meaningful BPM available ~5 heartbeats after entry.
- 30 s SD recording, columns `time_ms, accel_z_g, filt_mg, beat`.
- LED: yellow↔red 2 Hz alt, both standby and session.
- No haptic on beat (Stage 7 request — silent operation).

**Field observations (Stage 7, 2026-07-18):**
- On-wrist rest gives 70–90 BPM. Consistent with real HR.
- Motion inflates BPM to 100–140 (peak detector fires on motion
  artifact). Not a bug — expected physical limitation of wrist BCG.
- Signal amplitude at rest: 5–13 mg peak, occasional 20 mg. Threshold
  floor was 10 mg — many real beats got gated. Dropped to 3 mg this
  session, plus flipped peak detection to `|filtered|` local max.
- First-beat BPM=1 artifact (fixed this session): initial
  `last_beat_ms = 0` produced an "interval" of `now` for the first beat.
  Now skipped: interval logged from the 2nd beat onward.

- Compile-time flag `BCG_PLOTTER_MODE` (default 0 = monitor). Session
  runs for 30 s regardless; button click exits early.

---

## 6. Non-mode systems

### 6.1 Encoder — polled detent-rest state machine

Per auto-memory `feedback_encoder_polling.md`, the ALPS EC05E's settle
bounce lands >15 ms after the leading edge with B flipped. An ISR
approach oscillates ±1. The polled state machine in `s_enc` waits for
both lines to reach the rest state, latches direction from the first
line to go LOW on the next motion, and emits exactly one click per
full rest → motion → rest cycle.

### 6.2 Shutdown watcher — 4 s hold, ship mode, + 15-min uptime cap

`task_shutdown_watcher_fn` at priority 6, Core 1. Polls GPIO16 raw
level, tracks contiguous LOW time. Also checks total uptime each cycle.

**Button-hold ladder (user-initiated shutdown):**
- 2000 ms → BUZZ warn (500 ms MEDIUM_CLICK stack)
- 4000 ms → BQ25619 BATFET_DIS ← 1 (BATFET drops, board off)
- LED during hold: solid red, brightness = `(held_ms * WS_MAX) / 4000`
  linear fade-in
- `g_shutdown_hold_active` blocks the mode LED writer during hold

**Uptime-cap ladder (damage control):** `SHDN_MAX_UPTIME_MS = 15 * 60 * 1000`.
- On every 5 ms watcher tick: `if (millis_u32() >= SHDN_MAX_UPTIME_MS)` →
  log WARN, LED solid red, LONG_BUZZ, `watcher_ship_mode()`.
- Runs regardless of button state. Guarantees no runaway session can
  drain the battery beyond 15 minutes of firmware runtime.

If a recording is in progress, `s_recording_early_end` is set so the
active session tears down cleanly (fflush + fclose or WAV header patch)
before BATFET drops. Best-effort — no guarantee the flush completes
before the BATFET switch fires.

### 6.3 NVS

Two persistent keys:
- `mode` — current `fc_mode_t` (uint8), so mode selection survives boots
- `boot_seq` — monotonic uint32, incremented on every boot, used in SD
  filenames

### 6.4 RTC (PCF85063A)

- Time is set via a USB-Serial-JTAG console CLI (`SET_TIME` / `GET_TIME`
  in `app_logic`).
- RTC produces the `rtc_iso_now(...)` timestamp embedded as the first
  comment line of every CSV.
- **Known warning noise:** `PCF85063: I2C mutex timeout` in TEMP mode
  because the bus is contended with LSM+BME+MAX+TMP all polling at high
  rates. Not a data-loss event (RTC is autonomous), just log clutter.
  Bumping the RTC mutex-wait timeout would silence it.

### 6.5 Battery + charging (BQ25619)

- `boot_hw_init` clears BATFET_DIS, disables watchdog, sets TS_IGNORE.
  Any one of these off caused the "USB-C plugged in, no charge current"
  fault we chased in Stage 4.
- Fuel-gauge is *not* on iv7.1. Stage 8 § 3 tracks MAX17048 addition
  for Mk II.

---

## 7. Known issues / TODO carry-forward

Copy-paste of the standing punch list, updated with Stage 9 status:

| # | Item | Status | Owner |
|---|---|---|---|
| 1 | QVAR touch classifier flapping between Qvar1/Qvar2 | **open** — needs sign hysteresis | firmware |
| 2 | Compass tilt-comp axis alignment | **deferred to Mk II** — capture chip rotation in firmware | firmware + KiCad |
| 3 | Wrist PPG SNR poor (MAX30101 architectural limit) | Mk II swap to dedicated AFE (Stage 8 § 4b) | HW |
| 4 | Fuel gauge missing (no SoC readout) | Mk II add MAX17048 | HW |
| 5 | USB-C VBUS fatigue on iv7.1 unit | Mk II reinforced footprint (Stage 8 § 2) | HW |
| 6 | Case-to-GND bond point | Mk II layout addition (Stage 8 § 2) | HW / mech |
| 7 | LSM 47 °C on desk — 2 °C above ESP32-S3 | **open** — investigate on Mk II with thermal camera | HW |
| 8 | TMP117 first-reading delay ~5 s | **open** — driver-side AVG=1 config would drop to ~15 ms | firmware |
| 9 | PCF85063 mutex-timeout log noise in TEMP mode | cosmetic, bump wait timeout on RTC task | firmware |
| 10 | AD8232 (proper ECG AFE) | Mk II (Stage 6 § 7, Stage 8 § 3) | HW |
| 11 | Series 10 nF → 100 nF QVAR caps | Mk II BOM change (Stage 8 § 5.3) | HW |
| 12 | LIS3MDL Z-axis hard-iron cal (currently uncalibrated) | Mk II vertical-plane sweep in run_mag_cal | firmware |
| 13 | Port CTRL7/CTRL1 bit-layout fixes back into driver headers | cleanup, do at Mk II | firmware |
| 14 | 11_test_bcg Arduino sketch | not yet written; bench iteration would help | firmware |
| 15 | Ground-truth BCG BPM vs a chest-strap during rest | Stage 10 validation | procedural |
| 16 | BCG SD recording — was silently failing (dir missing) | **fixed 2026-07-18** — `ensure_sd()` now mkdirs `bcg` + `qvar` | firmware |
| 17 | BCG first-beat BPM=1 artifact | **fixed 2026-07-18** — skip interval on first beat | firmware |
| 18 | MAX30101 green LED in TEMP mode | **fixed 2026-07-18** — wake with LED_PA=0 | firmware |
| 19 | Compass tilt-comp (Stage 7 attempt) | **reverted 2026-07-18** — see #2 above | firmware |
| 20 | Session duration timers (30 s max on BCG/QVAR/MIC/ENV/MOTION/SKIN) | **removed 2026-07-18** — sessions now run until click | firmware |
| 21 | 15-minute uptime cap for damage control | **added 2026-07-18** — priority-6 watcher fires ship-mode unconditionally at 15 min | firmware |
| 22 | Provenance header on every SD file | **added 2026-07-18** — `rtc/hw/fw/boot/seq/mode` in every CSV + WAV `.meta.txt` sidecar | firmware |
| 23 | Per-driver `<NAME>_DRIVER_VERSION` macros | **added 2026-07-18** — 26 headers seeded at `"0.2.0"` | firmware |
| 24 | Aggregate `KOMPIC_FW_VERSION` in components/field_capture/firmware_version.h | **added 2026-07-18** — bump when any driver version bumps | process |

---

## 8. Bench cheat sheet — filter coefficients & thresholds

Everything a bench operator might want to tune from the code, one place:

### QVAR
```c
#define QVAR_HPF_ALPHA          0.952f    // HPF @ ~2 Hz, fs=250 Hz
#define QVAR_ONSET_THRESH_RAW   800.0f    // |y| to latch sign
#define QVAR_TOUCH_THRESH_RAW   400.0f    // envelope to declare touching
#define QVAR_ENV_DECAY          0.985f    // ~66 ms tau
#define QVAR_RAIL_ABS_MIN       15000.0f  // |raw_mean| ≥ this = near amp rail
#define QVAR_QUIET_ENV_MAX      150.0f    // envelope this low = flat
#define QVAR_RAW_MEAN_ALPHA     0.02f     // ~50-sample raw EMA
#define QVAR_PLOTTER_MODE       0         // 1 = Serial Plotter output
#define QVAR_SESSION_DURATION_MS 30000
```

### BCG
```c
#define BCG_HPF_ALPHA           0.9745f   // HPF @ 1 Hz, fs=240 Hz
#define BCG_LPF_ALPHA           0.2822f   // LPF @ 15 Hz
#define BCG_REFRACTORY_MS       400       // 150 BPM cap
#define BCG_THRESH_FLOOR        0.003f    // 3 mg (empirical, on-wrist rest)
#define BCG_BPM_MEDIAN_N        5         // rolling median window
#define BCG_PLOTTER_MODE        0
#define BCG_SESSION_DURATION_MS 30000
```

### Compass
```c
#define MAG_CAL_MAX_ABS_UT      500.0f    // outlier reject during figure-8
// heading = atan2f(+ny, nx). Flip sign on ny if CW/CCW reversed on bench.
```

### Shutdown watcher
```c
#define SHDN_WARN_MS         2000
#define SHDN_FIRE_MS         4000
#define SHDN_BUZZ_MS          500
#define SHDN_MAX_UPTIME_MS  (15u * 60u * 1000u)  // damage-control cap
#define WS_MAX                 26                // WS2812 brightness cap (eyes-safe)
```

### Recording sessions
```c
#define VOICE_ANNOT_MS   5000       // fixed 5 s WAV prelude for ENV/MOT/SKIN
// No RECORDING_MS / *_SESSION_DURATION_MS constants remain. All sessions
// exit on button click or on the 15-min uptime cap.
```

### Versioning
```c
// components/field_capture/firmware_version.h
#define KOMPIC_HW_VERSION   "iv7.1"
#define KOMPIC_FW_VERSION   "0.2.0"

// Every component's primary header:
#define <NAME>_DRIVER_VERSION  "0.2.0"
```

---

## 9. Handoff to Stage 10

Suggested next-session priorities in order:

1. **QVAR classifier fix** — sign hysteresis + longer sign-hold window
   so state stops flapping between Qvar1/Qvar2 during a single-electrode
   touch. Blocking any UI plan that uses QVAR as a button.

2. **11_test_bcg sketch** — Arduino bench version of the BCG chain, so
   thresholds can be iterated in seconds instead of the full ESP-IDF
   rebuild loop. Once bench is happy, port constants to the ESP-IDF
   BCG defines.

3. **BCG validation** — chest-strap side-by-side during 5 min rest to
   quantify BPM error. Publish a plot in Stage 10.

4. **Wrist telemetry campaign** — with SD recording working for BCG+QVAR,
   collect several hours of on-wrist data during normal activity for
   future ML / classifier training material.

5. **Compass tilt-comp** — Mk II hardware, after chip footprint rotations
   are captured from KiCad symbols. Attempt again from scratch, not
   from the reverted code.
