---
Date opened: 2026-10-04
Date closed:
Status: PROMPT (handoff for next session)
---

# Stage 38 handoff -- flight recording + port plan + CSV standard

Board: Mk1b iv8.0. Firmware baseline on-chip after Stage 37 revert:
0.4.97 (portrait, last known good). Stage 37 abandoned both SW rotate
(Flash A) and HW MADCTL (Flash B); portrait is the shipping baseline
until landscape is re-attempted from empirical MADCTL data. See
`Stage_37_Mk1b_Display_Orientation.md` §4 for the post-mortem.

Three concerns in this prompt, in the order Ivan picked (C, B, A):

  §1  Port skeleton outline  -- the clean-rebuild plan at high level.
                                Not something we start THIS session;
                                mental model for the direction.
  §2  CSV standard           -- codify the schema NOW so the flight
                                recording in §3 is already consistent
                                with the eventual port.
  §3  Flight recording mode  -- THIS SESSION'S WORK. LSM + LIS + BME
                                CSV logging mode for taxi/takeoff/
                                flight/landing. Double-tap sync spikes
                                for later video alignment.

---

## 1. Port skeleton outline (strategic -- not this session)

Context: Stage 37 failed in both directions (SW rotate, HW MADCTL),
and multiple earlier stages have accreted ad-hoc fixes on top of
decisions that were already fragile. Current codebase works but is
"far from day-to-day usable" (Ivan 2026-10-04). The direction is a
clean ESP-IDF project with minimal, robust scaffolding, porting one
subsystem at a time -- NOT a big-bang rewrite.

### 1.1 Tier layout (the Legos)

```
Tier 0 -- Platform                 ESP-IDF 5.5.2 + FreeRTOS; sdkconfig
                                   (PSRAM mode, 16MB flash, CPU/SPI,
                                   LVGL Kconfig); partition table.

Tier 1 -- OS core (no I/O)         app_nvs, app_cli, data broker,
                                   app_boot state machine, shutdown
                                   watcher.

Tier 2 -- Core peripherals         I2C bus, SPI buses (display vs SD
                                   separate), PCF85063A RTC, encoder
                                   (polled state machine per
                                   [[feedback_encoder_polling]]),
                                   button + BQ25619, MAX17048 fuel,
                                   RGB LED, DRV2605 haptic, flashlight.

Tier 3 -- Display + UI             CO5300 panel driver (QSPI), CST9217
                                   touch, esp_lvgl_port glue, LVGL 9
                                   tiles + navigation + theme, fonts
                                   (Space Grotesk + FA icons).
                                   Landscape resolved via Arduino
                                   characterization sketch FIRST --
                                   see §1.3.

Tier 4 -- Storage pipeline         SDMMC driver, CSV standard (§2),
                                   PSRAM ring buffer (see 1.4),
                                   datetime filenames per
                                   [[feedback_datetime_filenames]],
                                   USB CDC+MSC lazy init per
                                   [[feedback_usb_msc_lazy]].

Tier 5 -- Expedition sensors       LSM6DSV16X IMU, LIS3MDL mag, BME688
                                   env, MAX-M10S GPS, MAX30101 PPG
                                   (medical, defer), mic (IC TBD -- see
                                   §1.5).

Tier 6 -- Recording modes          Mode registry per
                                   [[project_mode_restructure]];
                                   flight mode, blackbox, batt_test,
                                   etc.
```

### 1.2 Timeline estimate

Rough per-tier, assuming one bench cycle per session + flash-per-
checkpoint cadence per [[feedback_flash_per_checkpoint]]:

| Tier                                | Sessions |
|-------------------------------------|---------:|
| 0-1 (platform + OS core)            | 2-3      |
| 2 (core peripherals, ~5 drivers)    | 4-5      |
| 3 (display + LVGL + landscape done) | 3-5      |
| 4 (SD + CSV + USB + PSRAM buffer)   | 2-3      |
| 5 (expedition sensors, 4-5 ICs)     | 5-8      |
| 6 (recording modes + CLI)           | 2-3      |
| **Total**                           | **18-27**|

~20-30 sessions to reach current parity on clean foundations. Weeks
to a couple of months at Ivan's casual pace. Not a weekend, not a
quarter.

### 1.3 Landscape, before Tier 3 starts

Burn one session on an **Arduino characterization sketch** per
[[feedback_arduino_sketch_pattern]] that pokes the CO5300 at every
plausible MADCTL combination with a known test pattern (diagonal
colour ramps + corner markers). Capture by phone the output for each
MADCTL byte; derive empirically:
  - Which axis CASET addresses under MV=1.
  - Where COL_OFFSET=22 actually belongs under each combo.
  - Which MX/MY produce "portrait top on RIGHT of landscape".

Only then wire the result into the ESP-IDF port's panel driver. Two
Stage 30+ attempts failed because we deduced MADCTL semantics from
analogy to ST7789. CO5300 is not ST7789.

### 1.4 PSRAM policy

8 MB external, ~1/3 bandwidth of internal SRAM, DMA-capable through
cache. Current firmware doesn't use it (Stage 30 moved LVGL draw
buffers to internal SRAM). The port's planned use:

  - **CSV ring buffer** -- Tier 4 writes rows into a PSRAM ring, a
    flush task copies to SD in one write every N rows OR T seconds
    (whichever first). Policy knob: durability window = T. Default
    1 s for flight (acceptable sample loss on power cut) OR 100 ms
    for medical.
  - **Audio sync clip buffer** -- 5-10 s PDM mic burst into PSRAM
    before SD flush, avoids per-sample SD write pressure.
  - **NOT for LVGL frame buffer** -- internal SRAM is faster for the
    partial-render strips we use. Confirmed Stage 30.

### 1.5 Known gaps in Ivan's device list

  - **SD card** -- not a sensor, lives in Tier 4. Essential for CSV.
  - **USB** -- CLI (CDC) + data pull-off (MSC lazy). Tier 4.
  - **Mic IC** -- Ivan said "mic" without naming the part. Decide
    before Tier 5: I2S PDM (e.g. MP34DT05) or analog? Flags budget
    + PCB rev. Mk2 question.
  - **Shutdown watcher / auto-shdn** -- Tier 1.
  - **PPG (MAX30101)** -- Ivan treats as medical-research tier; put
    in Tier 5 but mark "defer until the flight-recording surface
    settles".

### 1.6 When to start the port

Not this session. Flight recording in §3 is the pressing goal and
rides on the current firmware. The port starts after:
  - Flight recording ships.
  - Arduino MADCTL sketch resolves landscape semantics.
  - CSV standard (§2) is codified in-tree so the port imports it
    without re-litigation.

---

## 2. CSV standard

The current writers (`batt_`, `blackbox_`, `env_`, `mot_`, `qvar_`,
`ppgbcg_`, `bcg_`, `steps_`, `mlc_train_`) have ad-hoc formats. The
port needs one convention; stage 38 should start honouring it so
flight-mode CSVs are already compliant.

### 2.1 Filename

Per [[feedback_datetime_filenames]]:
```
<device>_YYYY-MM-DD_HH-MM-SS_<mode>.csv
```
Example: `kompic_2026-10-10_14-32-05_flight.csv`.

If RTC unknown at start, prefix `boot_<seq>_` and rename post-fix
when RTC syncs. Keep the sidecar sync log so later rename maps.

### 2.2 Header block

Every CSV opens with a `#` -prefixed header block (one key=value
per line) BEFORE the column header row:

```
# device_hw = iv8.0
# device_fw = 0.4.97
# device_id = <chip_mac_short>  (deterministic across reflashes)
# record_mode = flight
# record_start_utc = 2026-10-10T14:32:05Z
# record_start_tz_offset_h = +2
# rtc_valid_at_start = 1
# sample_rate_hz = 100
# notes =
# columns = ts_ms, ax, ay, az, gx, gy, gz, mx, my, mz, temp_c, p_hpa, h_pct, gas_ohm
# units   = ms, mg, mg, mg, mdps, mdps, mdps, mG, mG, mG, degC, hPa, pct, ohm
```

Rationale:
  - `#` prefix = standard CSV-with-metadata; Pandas / numpy
    `np.loadtxt(skiprows=auto)` and spreadsheet tools skip comment
    rows cleanly.
  - `device_fw` tracks `KOMPIC_FW_VERSION`; post-fact filtering of
    "all logs on fw X" trivial.
  - `columns` + `units` as explicit lines; the row below is the raw
    CSV column header (no `#`).

### 2.3 Column conventions

  - **Timestamp**: first column, ALWAYS `ts_ms` = ms since RTC
    epoch (or since boot if RTC was invalid at start). Monotonic
    within a file.
  - **Integer when sensor is**: raw ADC / LSB values go int; use
    `_raw` suffix if ambiguous.
  - **Float decimals**: 3 for engineering units (m/s^2, degC, hPa),
    6 for GPS lat/lon.
  - **Enum / state**: lowercase short strings, `|`-delimited bitmap
    if multi-valued.
  - **Row delimiter**: `\n` (LF), NOT CRLF. Reduce bytes.
  - **Field delimiter**: `,`. Never space-padded.

### 2.4 Durability + flush

Per [[feedback_sd_write_durable]]: `fflush` alone leaves 0-byte
files on power cut. Flight mode must `fsync` every N seconds (or N
rows) once the PSRAM ring is up; for now in Stage 38, `fflush` +
`fsync` every row is acceptable (flight is the first durable use
case worth the per-row cost).

### 2.5 Rotate when

New file every 1 h of continuous recording, OR when `record_mode`
changes, OR when RTC jumps (clock changed mid-record). Each new
file re-emits the §2.2 header block.

### 2.6 Landing in current tree

Codify §2.1-2.5 as `docs/build_info/Mk2_intel/CSV_Standard.md`
(reuse the Mk2_intel folder per [[reference_mk2_intel_folder]];
it's the forward-looking convention bank). Flight mode in §3 writes
compliant CSVs on first flash so we're not retrofitting later.

---

## 3. Flight recording mode (THIS SESSION'S WORK)

Immediate goal. Ivan is flying soon, wants LSM + LIS + BME time-
stamped CSV capture during taxi, takeoff, flight, landing. Sync
with video via double-tap-on-watch producing clear spikes in LSM
accel + mag channels (and later in mic once that lands).

### 3.1 UX contract

  - **Mode slot**: reuse an existing FCM_* slot. Candidates:
      - FCM_BATT_TEST  -- already SD-logging, closest shape
      - FCM_BLACKBOX   -- also SD-logging, broader sensor coverage
    Prefer BLACKBOX repurpose (closer semantics) OR add a new
    FCM_FLIGHT slot if BLACKBOX is still needed as-is. Ivan calls.
  - **Mode colour**: pick an unused RGB rainbow slot per
    [[project_mode_restructure]]. Suggested: WHITE (clean sky).
  - **Start/stop**: encoder click starts recording when the mode
    tile is active. Second click stops + closes file durably.
  - **Visual feedback**: tile shows ELAPSED time + sample count +
    SD write OK/fail flag + estimated remaining SD space.
  - **Haptic**: short buzz on start, double buzz on stop, long
    buzz if SD write fails mid-flight (don't block flight on the
    buzz).
  - **Sync protocol**: Ivan double-taps the watch body at known
    wall-clock moments (not touch screen -- the device shell).
    Spikes land in LSM accel. The CSV captures them like any other
    sample; alignment is a post-process concern.
  - **GPS**: enable if a fix is available at start, but DO NOT
    block recording start on GPS fix. GPS antenna is a known
    ceiling per [[project_gps_antenna_mount_learning]].

### 3.2 Sensor configuration

LSM6DSV16X:
  - ODR: 100 Hz accel + gyro (plenty for aircraft dynamics; mic
    adds Hz if we need the sync clip later).
  - Full scale: ±16 g accel, ±2000 dps gyro (takeoff / turbulence
    headroom).

LIS3MDL:
  - 80 Hz mag (max).
  - FS: ±4 gauss (geomagnetic).

BME688:
  - 1 Hz forced-mode cycle (temp + pressure + humidity + gas is
    plenty for altitude / cabin conditions trend; no need faster).

### 3.3 Data flow

```
LSM ISR  -> queue -> flight_task -> csv row builder -> fflush+fsync
LIS poll -> queue
BME poll -> queue
```

flight_task runs on Core 0 (sensors), writes CSV on the LVGL-OFF
core so SD stalls don't jank the UI. Core 1 keeps LVGL + tile
updates.

Sample rate asymmetry: LSM at 100 Hz is "primary"; LIS at 80 Hz
and BME at 1 Hz are "sparse". Row strategy: emit one row per LSM
sample; LIS / BME fields carry last-known value (step function)
with a `_fresh` boolean column OR with the sparse channels left
blank in rows where no new sample arrived. Pick one and stick with
it; §2.3 says blank is fine (empty between commas).

Suggested columns for flight CSV:
```
ts_ms, ax_mg, ay_mg, az_mg, gx_mdps, gy_mdps, gz_mdps,
mx_mG, my_mG, mz_mG, temp_c, p_hpa, h_pct, gas_ohm,
mag_fresh, env_fresh,
gps_lat_deg, gps_lon_deg, gps_alt_m, gps_fix
```

### 3.4 Non-goals for Stage 38

  - CSV ring buffer in PSRAM. Per-row fsync is the correct Stage
    38 move. Ring buffer is Tier 4 of the port -- future.
  - Mic recording. IC TBD; punt.
  - Landscape UI. Portrait is the Stage 38 baseline. Mode tile
    layout is a portrait layout, keep it legible.
  - Any sensor outside LSM / LIS / BME / GPS.
  - Audio sync clip. Double-tap on body gives us accel spike;
    audio sync is a nice-to-have once mic IC lands.

### 3.5 Flash gate

Per [[feedback_concise_flash_gate]]: one terse "what am I testing"
block. Expected success:
  1. Boot. Encoder cycles to the FLIGHT (or repurposed BLACKBOX)
     mode. Tile shows "ready".
  2. Encoder click starts. Haptic buzz. Tile shows elapsed time
     counting up, sample count climbing at ~100 Hz, SD OK flag
     green.
  3. Double-tap the watch body twice, 2 s apart. Clock passes.
  4. Encoder click stops. Haptic double buzz. File closes.
  5. Pull SD card, open the CSV. Header block present per §2.2.
     Column names match §3.3. ts_ms monotonic. Two obvious
     accel spikes at the double-tap moments.

### 3.6 Open questions for Ivan at session start

  1. Repurpose FCM_BLACKBOX or add new FCM_FLIGHT slot?
  2. Confirm colour choice (WHITE suggested).
  3. SD card durability: fsync every row (safe + slow) or every
     100 ms (10 rows at 100 Hz, 100 ms worst-case data loss)?
  4. GPS: include columns even if no fix (NaN / blank), or omit
     until fix?

Answer these in opening, log into Stage 38 §4, start coding.

---

## 4. Carry-forward from Stage 37

  - fw 0.4.97 baseline on-chip (portrait, working).
  - [[feedback_lvgl_auto_rotates_indev]] memory in place -- next
    time a custom indev callback is wired on a rotated display,
    hand raw coords.
  - Stage 37 doc kept as post-mortem; §5 "Lessons for next
    landscape attempt" seeds the Arduino sketch work.
  - All Stage 37 code changes reverted in-source. Stage 32-36 WIP
    intact.

## 5. Memory updates to consider after Stage 38 lands

  - If BLACKBOX got repurposed into FLIGHT: update
    [[project_mode_restructure]] to reflect the new slot.
  - If PSRAM ring buffer discussion drifts into "do now", save a
    feedback memory on durability window policy before coding.
  - If a CSV field convention surprises us on bench, update
    `CSV_Standard.md` in-place (not memory -- it's a doc).
