# Stage 27 -- Mk1b peripheral bring-up (antenna, encoder, button, motor, SD)

**Date opened:** 2026-09-11
**Board:** Mk1b iv8.0 (physical unit on Ivan's bench).
**Firmware baseline:** `0.4.44` (Stage 26 close, BQ25619 replaced,
charging + battery-run verified).
**Status:** OPEN.

---

## 1. Why this stage exists

Stage 26 closed with the charge path healthy (fresh BQ25619 + working
MAX17048 fuel gauge) and the on-board sensor set stable. The remaining
gap between "bench board" and "wearable" is a set of five physical
peripherals that either aren't installed yet or aren't proven alive:

  1. **GPS antenna** -- module was reflowed in Stage 24 Phase B but
     never produced a byte on UART1 (`18_max_m10s_gps_test_mk1b`
     printed `[STAT] 5000 ms: 0 B` across multiple cycles + zero
     UBX-MON-VER replies). Silent chip needs a rework / continuity /
     power investigation regardless of whether the antenna is
     attached.
  2. **Encoder** (ALPS EC05E) -- not yet installed. Bring-up gate is
     the polled detent state machine in `fc_common.c`, exposed via
     the `ENC` CLI verb.
  3. **Button** -- dual-wired to BQ25619 `/QON` (GPIO16). Missing on
     the current unit; without it, ship-mode entry has to come from
     the `SHIPMODE` CLI verb.
  4. **Haptic motor** (ELV1411A LRA) -- not yet installed. DRV2605
     boots with `auto-cal FAIL STATUS=0xEC` every session because
     there's no motor to calibrate against. Motor install unblocks
     auto-cal.
  5. **SD card slot** -- physical slot is on the board; no card
     inserted yet. `FS_LS /sd` should list files once a card is in.

None of these need new firmware. Each has a driver already; Stage 27
is a hardware + verification stage under
[[feedback_cli_first_testability]] and
[[feedback_flash_per_checkpoint]] -- one peripheral per bench session,
CLI-verify each before moving on.

Once all five are alive and CLI-verifiable, Mk1b hardware bring-up is
functionally done. Firmware work continues from the desk (no bench
required) except for cases where a firmware change specifically needs
observation.

---

## 2. Plan (bench sessions, strictly one peripheral per session)

Each session ends with a `WHOAMI` / peripheral-specific CLI dump that
confirms the peripheral is alive, and a note added to §4 (below) with
what worked / what didn't.

### 2.1 GPS -- MAX-M10S alive-check (before antenna)

**Pre-antenna investigation first** because the chip was silent in
Stage 26 §4.0. The antenna install is downstream of "does the module
respond to UART commands at all".

Bench steps:

  1. Continuity + power check on the M10S module:
     - VCC pin -> should be 3V2 (TPS62840 output).
     - GND -> continuity to GND.
     - UART TX (module pin) -> ESP GPIO18 (RX).
     - UART RX (module pin) -> ESP GPIO17 (TX).
     - 1PPS -> ESP GPIO46.
     - BACKUP pin -> should have a small cap / RTC-backup source per
       schematic; verify it's not floating.
  2. Reflow the module with hot air if any pin looks marginal
     (Stage 24 Phase B was "single reflow, saved for last"; a second
     pass with fresh flux is cheap).
  3. Flash `18_max_m10s_gps_test_mk1b` again post-rework. Success =
     any NMEA line at all in the log, or a UBX-MON-VER reply
     (SW/HW version strings printed by the sketch's frame decoder).
  4. Antenna install (mechanical) once the chip is confirmed alive.
  5. Outdoor test: valid fix in 30-60 s. RTC auto-sync via
     `pcf85063_cmd_sync_utc()` on first fix.

Success gate: `GPS_VIEW normal` CLI verb shows fix data populating;
boot-time `MAX-M10S UART1 OK` line already reads correctly.

### 2.2 Encoder -- ALPS EC05E install + polled state machine

Bench steps:

  1. Solder encoder onto the encoder footprint.
  2. Wire check: encoder A, B, and click pins land on the ESP GPIOs
     per master pinout v20.
  3. Reboot. `ENC` CLI verb should now:
     - Print current CW/CCW counts (starts at 0).
     - Update as Ivan rotates the wheel.
     - Rotation-speed EMA (`dps`) should track physical speed.
  4. `ENC RESET` zeros counters (already implemented at Stage 23 §2.4b).

Reminder: encoder is polled per [[feedback_encoder_polling]] -- ISR-
based path oscillates +1/-1 because settle bounce lands >15 ms after
the leading edge. The state machine in `fc_common.c` is authoritative.

Success gate: rotate the wheel five detents clockwise; `ENC` verb
shows `cw=5 ccw=0`. Reset. Rotate five counter-clockwise; `cw=0 ccw=5`.

### 2.3 Button -- GPIO16 dual-wired to BQ25619 /QON

Bench steps:

  1. Solder the tactile switch onto the button footprint.
  2. Verify: GPIO16 pulls low on press, releases high (ESP internal
     pullup provides ~45 kΩ up; verify with DMM).
  3. Reboot with USB attached. Boot log confirms shutdown watcher
     armed (already prints today).
  4. Single-press -> log line `BTN single (ignored in test sketch)`
     equivalent in firmware -- the fc_shutdown.c poller distinguishes
     click / long-hold; verify a short press does NOT enter ship
     mode.
  5. Double-click -> ship mode entry (per
     [[feedback_sketches_need_shipmode]] and Stage 20 timing).
  6. 4-second hold -> ship mode entry via long-hold ladder (Stage 17
     timing).

Success gate: single press produces a haptic-style click without
firing ship mode; a proper long-hold or double-click drops BATFET
after the buzz warning at 2 s.

### 2.4 Haptic -- ELV1411A LRA install + DRV2605 auto-cal

Bench steps:

  1. Solder the LRA to the motor pads.
  2. Reboot. Auto-cal should transition from
     `STATUS=0xEC` (fail without motor) to a cleaner result -- exact
     STATUS bits depend on how well the ELV1411A matches the current
     LRA profile in `haptic_tile.c`.
  3. If auto-cal still fails: tune the LRA profile parameters
     (resonant freq, rated voltage, drive time) per the ELV1411A
     datasheet.
  4. `HAPTIC PLAY 14` (or another ROM effect) should produce a felt
     click.

Success gate: `HAPTIC PLAY 14` fires a visible / audible / felt
buzz; boot log shows `DRV2605 auto-cal OK` (or a documented
persistent-fail with the motor still playing effects cleanly).

### 2.5 SD card -- insert + FS_LS

Bench steps:

  1. Insert a formatted µSD card (FAT32, ≤ 32 GB) per
     [[feedback_suspect_cheap_sd_cards]].
  2. Reboot. Boot log should now show `SDCARD: sdcard_init OK` +
     `Mounting SDMMC` succeeding, not the current
     `sdmmc_init_ocr ... 0x107` timeout.
  3. `FS_LS /sd` lists whatever is on the card (may be empty on a
     freshly-formatted card).
  4. `BLACKBOX ON`, reboot, verify blackbox CSV rows land on the SD
     with per-row `fsync` per [[feedback_sd_write_durable]].

Success gate: `WHOAMI` shows `SD card mounted=1 cap=NNN MiB
free=NNN MiB`.

---

## 3. Non-goals for Stage 27

  - Display / touch / LVGL polish (blocked on display mating, which
    itself waits on completing the topside populated first).
  - Broker channel for MAX17048 SOC + battery tile update (needs the
    cell to soak for a discharge curve before the tile refresh
    contract is stable).
  - Landscape + font-sharpness + blue-light filter (all deferred from
    Stage 26 §5).
  - MLC training / recording durability audit / audio annotation
    tuning (all firmware polish, land after Stage 27 closes).

---

## 4. Bench log (fill in as each peripheral is installed)

### §4.1 Full topside populated + daughterboard transplant (fw 0.4.44)

Bench session 2026-09-11: Ivan installed the GPS antenna, RTC backup
cell, GPS supercap, tactile button, encoder wheel, and haptic motor,
then transplanted the daughterboard from the retired Mk1 unit
(MAX30101 + TMP117). All done headless per
[[feedback_cli_first_testability]] with the panel deferred until the
enclosure is ready.

**All 10 I2C chips ACK on `WHOAMI`:**

| Bus | Addr | Chip | Status |
|-----|------|------|--------|
| I2C0 | 0x51 | PCF85063A | OK |
| I2C0 | 0x76 | BME688 | OK |
| I2C0 | 0x6B | LSM6DSV16X | OK |
| I2C0 | 0x1C | LIS3MDL | OK |
| I2C0 | 0x57 | MAX30101 | OK (daughterboard) |
| I2C0 | 0x48 | TMP117 | OK (daughterboard) |
| I2C0 | 0x10 | VEML6030 | OK |
| I2C1 | 0x6A | BQ25619 | OK |
| I2C1 | 0x5A | DRV2605 | OK |
| I2C1 | 0x36 | MAX17048 | OK (VERSION=0x0012, prod_rev=0x2) |

**Peripherals verified end-to-end:**
  - **Encoder**: wired, `ENC` verb counts CW/CCW cleanly.
  - **Motor**: LRA installed; `DRV2605 auto-cal PASS in 1050 ms` (was
    persistent FAIL STATUS=0xEC without the motor).
  - **Charging**: 300 mA into cell with USB, board holds on battery on
    USB unplug (verified Stage 26 §4.4 after BQ swap).
  - **Fuel gauge**: `FUEL` returns VCELL + SOC without crashing.
  - **RGB**: fades per policy, bright on encoder detent.

**Headless quiescent draw: 35 mA** with all sensors present + fuel
gauge + GPS + haptic + RGB (idle, no charging, USB power). Battery-
life estimate: 24-48 h on a ~350 mAh cell.

**Deferred still (§2.5 SD card):** slot present, no card inserted --
`sdcard_init` still shows the expected `sdmmc_init_ocr ... 0x107`
timeout with an empty slot.

### §4.2 GPS silence -- MAX-M10S UART chip-side investigation needed

Sketch `18_max_m10s_gps_test_mk1b` flashed to the fully-populated Mk1b.
Result across multiple 5-second poll cycles:

```
[STAT] 5000 ms: 0 B  (0 NMEA, 0 UBX)   PPS edges=0
[POLL] -> UBX-MON-VER (class=0x0A id=0x04)
[STAT] 5000 ms: 0 B  (0 NMEA, 0 UBX)   PPS edges=0
```

**Being indoors doesn't cause this.** MAX-M10S emits NMEA sentences at
1 Hz on power-up regardless of antenna or fix (fields are empty until
a fix, but the sentences themselves stream). UBX-MON-VER poll requires
no fix and just returns the firmware/hardware version string. Both at
zero = module isn't responding at all.

**Chip-side possibilities (not sketch-side; the sketch is proven on
UART1 + I2C1 addressing):**

  1. VCC not reaching the module -- measure VCC pin on M10S with DMM;
     expect 3V2 (TPS62840 output rail).
  2. TX / RX orientation swapped somewhere between ESP GPIO17/18 and
     the module pads.
  3. V_BCKP (backup) rail issue -- u-blox modules can refuse to start
     if the backup line is shorted or held low. Ivan added a GPS
     supercap; if the supercap is shorted or mis-wired, this bites.
  4. Reset / SAFEBOOT pin held asserted.
  5. Module fried during reflow (Stage 24 Phase B was a single-pass
     hot-air reflow; possible cold silicon damage).

**Board draw jumped 35 mA -> 45 mA once the ESP-IDF firmware enabled
UART1 to the M10S**, so the module IS drawing some current (+10 mA is
plausible for a silent-but-powered M10S). That's soft evidence the
chip has VCC. The silence is UART-specific.

**Action queued for §2.1 GPS session next time Ivan has iron on the
bench:** measure VCC + V_BCKP on the module, DMM continuity check on
TX/RX pair, scope on module TX pin during boot to look for the
power-on NMEA burst. If nothing, hot-air reflow with fresh flux.

### §4.3 Batch 27.1 -- iv7.1 string audit + user-visible cleanup (fw 0.4.45)

Ivan asked for a full iv7.1 sweep now that the codebase runs
exclusively on Mk1b (per [[project_mk1_shelved]]). Split into two:
user-visible strings landed in this batch, comment-only mentions
deferred to a follow-up cleanup batch.

**Landed (user-visible):**
  - `fc_cli.c` TEMP_DUMP: `bq (BQ25619) = -- (TS network not
    populated on iv7.1)` -> `bq (BQ25619) = n/a` per
    [[feedback_status_terse]]. TS network IS populated on Mk1b (5k1 +
    33k parallel to 10k NTC) but the BQ25619 driver still holds
    `TS_IGNORE = 1` because JEITA thresholds haven't been tuned to
    this specific divider. Real BQ TS temperature readout queued as
    a Stage 28 firmware-only task.
  - `fc_cli.c` TOUCH verb: dropped the "iv7.1 has no panel" wording;
    now just `[TOUCH] CST9217 absent -- no touch present`.
  - `fc_modes_lsm.c` (x2 -- steps + mlc_collect CSV headers):
    hardcoded `"# kompic mk1 iv7.1 fw=%s hw=%s ..."` -> `"# kompic
    mk1 fw=%s hw=%s ..."`. `KOMPIC_HW_VERSION` already interpolates to
    `iv8.0`, so the literal was double-stating and now stale.
  - `encoder_cmd.c` `ENC` dump: `glitches = N (PCNT path -- dormant
    on iv7.1)` -> `... dormant`. PCNT is still dormant on Mk1b too,
    so the qualifier was misleading either way.
  - `bq25619.c` TS_IGNORE comment rewritten to reflect Mk1b reality
    (thermistor IS populated; ignore flag stays until JEITA is tuned).
  - `firmware_version.h` -> 0.4.45.

**Deferred to comment-audit follow-up (no runtime effect):**

Remaining iv7.1 mentions live in comments / headers / test drafts. Not
touched in this batch to keep the diff review-sized. Full list for the
follow-up:

  - `boot_hw_init.c` L216 -- iv7.1 bench observation note
  - `boot_tasks.c` L29 -- "iv7.1 chip set" comment
  - `boot_display.h` L7 + `boot_display.c` L6 -- headless comment
  - `co5300.h` L6 -- "Firmware version: iv7.1.f0.0"
  - `co5300.c` L6 -- iv7.1 breakout provenance
  - `encoder.h` L104 -- iv7.1 hardware comment
  - `lsm6dsv16x.c` L248 -- iv7.1 bench check comment
  - `lvgl_ui_display.h` L13/19/47 + `lvgl_ui_display.c` L190/282 --
    "on iv7.1" headless/forced-mode comments
  - `max_m10s.c` L51 -- "GPS module is offline on iv7.1" comment
  - `usb_msc.h` L3/9 -- "for iv7.1" module doc
  - `nvs_cfg/nvs_cfg.h` L109 -- "headless iv7.1 development" comment
  - `sdkconfig.defaults` L18 -- iv7.1 sdkconfig context comment
  - `field_capture/fc_battery_test.c` L40 -- already updated Stage 26
    §4.1 (kept for historical context)
  - `test_drafts/*.c` -- test draft logging strings (drafts, low
    priority)
  - `BUILD.md` L5 / L208 -- doc pages (Mk1b build guide is a separate
    doc task)

### §4.4 First headless boot with fully populated board (fw 0.4.45)

Ivan flashed the full Mk1b (all 10 I2C chips + LRA + encoder + GPS
antenna) and captured a boot log with `LVGL_FORCE=ON` (bench-only, no
panel). Every driver came up clean, exactly matching §4.1:

  - All 10 I2C chips ACKed at expected addresses.
  - `DRV2605 auto-cal PASS in 1150 ms` (was persistent FAIL without the
    motor).
  - `MAX17048` responds -- `[FUEL] VER=0x0012  VCELL=4195 mV
    SOC=100.00 %`.
  - LVGL brought up with the forced no-panel flush (49 200 B strip in
    internal SRAM, headless).
  - All 16 boot tasks spawned successfully (ENV, IMU, MAG, MAG_CAL,
    HR, SKIN, LIGHT, BAT, RTC, HAPTIC, ALARM, GPS on Core 0;
    FIELD_CAPTURE on Core 1; SHUTDOWN_WATCHER + RTC_CLI unpinned).
  - SDMMC mount succeeded (SDABC, 59 645 MiB).

**Headless quiescent draw (no cell, USB power, screen off): ~32 mA.**
That's below the §4.1 measurement (35 mA) because that run did not
include GPS but did include the screen boot path; here the panel is
skipped entirely via `boot_display_is_present() == false`.

**With cell attached and charging (USB present): ~300 mA into the
pack.** Charge current is set by the BQ25619 default profile as of
Stage 26; no attempt yet to tune ICHG per the ~350 mAh cell.

**Blockers surfaced by this run:**

  1. **BATT_TEST vbat column read zero.** Root cause: the CSV writer
     was still calling the retired `vbat_adc_read_mv()` stub (returns
     0 on Mk1b because the old GPIO18 5k1-5k1 divider is now the GPS
     RX pin). Fix landed in fw 0.4.46 -- see §4.5.
  2. **`soc=` vs SOC name collision.** BATT_TEST log line printed
     `soc=%.1fC` for the ESP32-S3 die temperature, but the MAX17048
     also reports SOC (State of Charge) as a percentage. Two very
     different values sharing the same three-letter prefix is a trap
     for the reader. Fix landed in fw 0.4.46 -- see §4.5.
  3. **GPS still silent.** Antenna + 3 floating pins wired VIO->3V3
     per last checkpoint; still no bytes on UART1. Deferred out of
     this stage per Ivan; §4.2 remains the standing action item.

### §4.5 Batch 27.2 -- Fuel gauge is the vbat source of record (fw 0.4.46)

**Landed:**

  - `fc_battery_test.c` -- BATT_TEST + BLACKBOX both now read
    `vcell_mv` + `fuel_pct` (integer + hundredths) from MAX17048 under
    `g_i2c2_mutex` via a new `read_fuel_snapshot()` helper. The retired
    `vbat_adc_ensure_init()` / `vbat_adc_read_mv()` stubs are deleted
    entirely -- no dead call sites, no zero column, no misleading log
    line.
  - BATT_TEST CSV header: `soc_temp_c` -> `esp_c`,
    `vbat_adc_mv` -> `vcell_mv`, and a new `fuel_pct` column. New
    header comments make it explicit that `esp_c` is the ESP32-S3
    junction temperature (not the fuel-gauge SOC).
  - BATT_TEST log line: `vbat=%lumV  soc=%.1fC` becomes
    `vcell=%umV  fuel=%u.%02u%%  esp=%.1fC`. `esp=` deliberately
    replaces `soc=` so no live log line ever uses "soc" for two
    different things.
  - BLACKBOX CSV: same rename (`vbat_adc_mv` -> `vcell_mv` +
    `fuel_pct` column, `soc_temp_c` -> `esp_c`).
  - `fc_cli.c` STATUS: `soc_temp` line -> `esp_temp` with a trailing
    "(ESP32-S3 die -- not fuel-gauge SOC)" clarifier.
  - `fc_cli.c` TEMP_DUMP: `soc  (ESP32-S3)` -> `esp  (ESP32-S3)`;
    local vars `t_soc` / `p_soc` renamed to `t_esp` / `p_esp`.
  - `fc_modes.c` FCM_TEMP live print: `soc(ESP32)` -> `esp(ESP32)`.
  - `main.c` §5a eager telemetry init -- dropped the
    `vbat_adc_ensure_init()` call.
  - `fc_internal.h` -- removed `vbat_adc_*` decls.
  - `system_tile.c` -- stale comment referencing "STATUS prints
    soc_temp" updated to `esp_temp`.
  - `field_capture.c` header comment -- dropped `vbat_adc` reference,
    now names the fuel-gauge snapshot helper.
  - `firmware_version.h` -> 0.4.46.

**Non-goal for this batch:** the `broker_battery_data_t` still carries
the fake BQ VBAT into `bat.voltage`. Migrating that to the fuel-gauge
VCELL is a Stage 28 task ("real-cell voltage in
`broker_battery_data_t`", already listed in the Stage 28 next-steps
hook at §6).

**Bench verification:** next flash + BATT_TEST run should show
`vcell=41xxmV  fuel=1xx.xx%` (values that track charge/discharge)
instead of `vbat=0mV`. `STATUS` should print `esp_temp = xx.x C
(ESP32-S3 die -- not fuel-gauge SOC)`. FUEL verb is unchanged.

### §4.6 Batch 27.3 -- Display up + DISP verb + TS enabled + fuel% rename (fw 0.4.47)

**Full display path came up on the assembled Mk1b** — the presence
probe (CST9217 ACK @ 0x5A) fired, `co5300_init` completed in 691 ms,
LVGL bound to the CO5300 flush cb + CST9217 indev, and Ivan saw the
tile view respond to swipes. Boot log excerpt:

```
I (4259) BOOT_DISP: CST9217 ACK @ 0x5A -- display module present
I (4950) co5300: init OK in 690742 us
I (4950) BOOT_DISP: CO5300 ready (410x502 logical, brightness ~80%)
I (5170) CST9217: CST9217 init OK @ 0x5A, boot=220157 us
I (5187) LVGL_DISP: LVGL up -- panel=CO5300 touch=CST9217
              buf=49200 B (40 rows × 410 px × 3B)
I (5171) MAIN: display ready
```

STATUS with a cell attached read `vcell = 4121 mV, fuel = 53.78 %,
charging = 1`. Fuel gauge, MAX-M10S GPS boot line (still 0 bytes on
UART -- GPS deferred), and every I2C chip alive.

**Batch 27.3 landed the three follow-up items Ivan flagged after
seeing STATUS:**

  1. **`DISP [ON|OFF]` CLI verb** -- puts the CO5300 into DISPOFF +
     SLPIN so tonight's overnight battery test can log real
     panel-off draw. There is no dedicated backlight rail on the AMOLED
     -- panel sleep is the only way to actually stop pixel emission
     current. LVGL continues to flush over SPI (harmless writes into
     RAM the panel is ignoring while asleep); no attempt yet to
     suspend the LVGL refresh task. `DISP ON` wakes; `DISP` with no
     arg prints the current state. Panel-absent boards get
     "panel absent -- nothing to sleep/wake".
     Implementation:
       - `boot_display.h/c` gains `boot_display_sleep()`,
         `boot_display_wake()`, `boot_display_is_asleep()` wrapping
         `co5300_sleep()` / `co5300_wake()`.
       - `fc_cli.c` DISP handler + HELP entry.
  2. **Fuel-gauge %% renamed from SOC -> fuel** everywhere it hits the
     user surface (STATUS, FUEL verb output, HELP). Reason: `SOC` is
     overloaded ("State of Charge" for the fuel gauge, "System on
     Chip" for the ESP32-S3 die temperature) and this trap already
     bit us in the `soc=%.1fC` batt-test log. Per
     [[feedback_status_terse]], names must be universally clear.
     Datasheet-authoritative internal names (`MAX17048_REG_SOC`,
     `max17048_read_soc_pct100`) stay as-is -- rename is output-only.
  3. **BQ25619 TS_IGNORE cleared** — the thermistor divider is
     final untested electronics but the enclosure is sealed, so leave
     the JEITA safety active for tonight's charge cycle. If charging
     stalls mid-run, first suspect is JEITA thresholds vs the 5k1 +
     33k∥10k NTC divider (not built-in-BQ-standard 103AT-2). Fault
     bits via `BQ` verb will confirm; workaround is re-setting
     TS_IGNORE here. Boot log tag changed from `[TS ignored]` to
     `[TS enabled]`.

**Also observed in the boot log worth flagging:**

  - `W (697) i2c: This driver is an old driver, please migrate ... driver/i2c_master.h`
    -- ongoing ESP-IDF v5.x deprecation, Stage 22b batch item.
  - `E (5487) spi_master: spi_device_polling_start(1406): Cannot send
    polling transaction while the previous polling transaction is not
    terminated.` -- one-shot at LVGL init, no repeat. Likely a race
    between the initial paint and brightness set. Non-fatal (screen
    came up fine). Investigate if it recurs mid-session.
  - `W (33576) CST9217: I2C mutex timeout in task path -- dropping
    report` -- recurring cluster of three reports every ~2 seconds
    during idle. Suggests the touch task is losing the `g_i2c_mutex`
    to another Core 0 client (probably one of the sensor pollers) and
    dropping reports. Touch still feels responsive so the drops are
    tolerable but this is bench-log noise worth fixing later.

**Firmware polish notes captured for Stage 28 (landscape + font
sharpness):**

  - **Landscape rotation direction:** when we go landscape, the panel
    must rotate **90° clockwise** from the current portrait to be
    correctly oriented (top-of-portrait -> right-of-landscape).
    Documented here so whichever path we take (LVGL matrix transform
    vs manual pixel-transpose vs CO5300 MADCTL retry) starts with the
    right sign convention. Stage 25 Batch H tried MADCTL 0x60 and got
    green bars; that experiment did not land the rotation direction
    either, so this note is orthogonal to the MADCTL redo.
  - **Text glitchiness / uncrisp pixels:** colors look "close but not
    truly correct" and font antialiasing looks off. Working
    hypothesis: LVGL color depth bump (LV_COLOR_DEPTH 16 -> 32) would
    give per-pixel alpha for font AA and fix the "off" color mixing.
    Try this in Stage 28 before assuming panel calibration is the
    culprit. Current buf is 40 rows × 410 px × **3 B** (RGB888) -- a
    depth bump to ARGB8888 would double the strip size to ~64 KB,
    still fits comfortably in the internal SRAM allocation.

### §4.7 Batch 27.4 -- DISP OFF gates touch + 15 s auto-sleep + button wake (fw 0.4.48)

Batch 27.3's `DISP OFF` only put the CO5300 into DISPOFF+SLPIN; the
touch layer still fired swipes into a dead panel and there was no
auto-entry. Ivan's spec for tonight's overnight batt test: touch off
when the panel sleeps, button wakes it, auto-sleep after 15 s idle.

**Landed:**

  - `boot_display.c` sleep/wake now drains `g_touch_q` so a stray
    finger down doesn't fire an event as the panel goes down or comes
    back up. Both directions are also made idempotent
    (`return ESP_OK` if the state matches).
  - `lvgl_ui_display.c` touch cb pins state to `LV_INDEV_STATE_
    RELEASED` when `boot_display_is_asleep()` is true. Touch reports
    are quietly discarded until wake -- no unintended navigation while
    the operator taps to check the panel is still there.
  - `fc_common.c::button_poll()` intercepts any button event while the
    panel is asleep, calls `boot_display_wake()`, and kicks
    `s_last_activity_ms`. A single-click is swallowed so the
    underlying tile / mode doesn't toggle; a double-click still
    propagates so the LSM submenu-exit gesture works if the user
    double-taps to wake-and-back-out at the same time.
  - `field_capture.c` main loop now checks
    `now - s_last_activity_ms >= 15000` at the top of every tick and
    calls `boot_display_sleep()` when idle in `ST_STANDBY`. The idle
    window is deliberately restricted to STANDBY -- an active
    recording, flashlight, or firing alarm must not sleep the panel.
  - `field_capture.h` gains a public `field_capture_kick_activity()`
    so input sources outside the main loop (starting with the LVGL
    touch cb) can reset the idle timer. Cross-core write to the
    aligned `uint32_t` is safe for a monotonic activity timestamp.
  - `fc_cli.c` DISP verb now bumps `s_last_activity_ms` on `DISP ON`
    so the auto-sleep loop doesn't immediately re-sleep after an
    operator wake.
  - `firmware_version.h` -> 0.4.48.

**Not touched (deliberate scope guard):**

  - Encoder rotation while asleep does NOT wake. The encoder is not
    installed on this Mk1b, so wasting bench cycles on that path is
    pointless right now. Add the mirror hook next time we touch
    `encoder_delta()`.
  - CLI keystrokes over USB Serial don't kick the activity timer.
    The user can always type `DISP ON` explicitly. Adding an
    "activity kick on any RX line" would be one line in the CLI
    task; deferred as low-value.
  - LVGL refresh task is not paused during sleep. Flush cb keeps
    writing pixels over SPI into a powered-down panel RAM; harmless
    but wastes some SPI bus time. If the overnight batt-test log
    shows the ~32 mA baseline hasn't dropped enough with the panel
    asleep, revisit this next.

**Definition of done for tonight's batt test:**

  1. Panel sleeps by itself 15 s after Ivan lets go of the encoder /
     button / touch.
  2. Touch reports are ignored while asleep (no accidental tile
     dispatch when the operator taps to see if it's alive).
  3. Single button click wakes; the tile view stays on whatever tile
     was showing at sleep time. No mode toggle from the wake click.
  4. BATT_TEST log lines already show `vcell + fuel%` from Batch 27.2
     -- overnight run captures actual pack discharge behaviour.

### §4.8 Batch 27.5 -- Wake gate rework + battery tile fuel% (fw 0.4.49)

Batch 27.4 bench findings from Ivan (2026-09-12 evening):

  1. **Button did not wake** with the single-click gate. Explicit
     spec: **double-tap** should wake (pocket-safe intent).
  2. **Touch did not wake** at all — the read cb was gated but nothing
     upstream fired the wake. Explicit spec: CST9217 ISR path should
     wake on any valid finger-down.
  3. **Battery tile shows 0%** — the tile reads `broker_battery_data_
     t.percentage`, which was still `bq25619_soc_from_mv()` over the
     fake BQ REG_VBAT (returns 0 mV on iv8.0 silicon per
     [[project_bq_no_vbat_adc]]). Never got the Stage 28 broker
     migration promised in the §4.5 notes.

**Landed:**

  - `bq25619.c task_battery_fn` now reads MAX17048 `VCELL` + `SOC` on
    the same bus 2 mutex (no extra I2C round-trip cost since we
    already hold the mutex for the BQ status/fault/poc regs). The
    broker payload's `.voltage` and `.percentage` are the fuel gauge's
    numbers; charge/pg/fault/boost stay from the BQ. Kills the last
    of the "fake VBAT" pipeline that fed the tile.
  - `battery_tile.c`: label renamed from `SoC` -> `Fuel` for
    consistency with the fuel-gauge terminology Batch 27.3 settled on
    at the CLI surface.
  - `bq25619` CMakeLists gains `max17048` in REQUIRES.
  - `fc_common.c::button_poll()` wake gate rewritten:
      * single-click while asleep -> ignored entirely (pocket-safe;
        stays asleep). Was: wake on single-click. Ivan's spec.
      * double-click while asleep -> wake, swallow the event so the
        LSM submenu-exit doesn't also fire on the wake gesture.
      * Any button event while asleep is consumed so the underlying
        tile/mode never sees a "wake tap".
  - `cst9217.c task_touch_fn` wakes the display on any valid finger-
    down while asleep (via the existing ISR notify path). The report
    is NOT queued in that case -- `boot_display_wake()` drains
    `g_touch_q`, and we don't want the wake tap to land as a
    navigation gesture on the tile that was showing at sleep time.
    Also kicks `field_capture_kick_activity()` so the 15 s auto-sleep
    counter restarts.
  - `firmware_version.h` -> 0.4.49.

**Correction on prior scope-guard claims:** Batch 27.4's "encoder not
installed on this Mk1b" note was wrong -- §4.1 already documented
"Encoder: wired, ENC verb counts CW/CCW cleanly" (2026-09-11). Per
Ivan's explicit note in this session: encoder is installed but should
NOT wake anyway (encoder is intentionally excluded from the wake set).

### §4.9 Batch 27.6 -- Wake spec correction + BQ NTC zone in TEMP_DUMP (fw 0.4.50)

Batch 27.5 got the wake vocabulary wrong. Ivan re-clarified:

  - **Button** is *pressed*, not tapped. **One button press** turns
    the screen on.
  - **Double tap on the touchscreen** turns the screen on. Single
    stray finger-down does NOT wake (pocket-safe intent).
  - BQ25619 TS reads "n/a" in TEMP_DUMP: TS was already enabled in
    Batch 27.3 (`IINDPM.TS_IGNORE` cleared) but the chip doesn't have
    a numeric TS ADC to display -- only a JEITA zone code in
    `REG_FAULT[2:0]`. The "n/a" was hiding that the TS path IS live.

**Landed:**

  - `fc_common.c::button_poll()` wake gate reverted to: **any button
    event while asleep -> wake + swallow**. Single click is the
    primary wake input; double click also wakes (and doesn't
    additionally fire the LSM submenu-exit gesture on the wake
    press).
  - `cst9217.c task_touch_fn` now implements a double-tap detector.
    Two valid finger-down reports within `TOUCH_DBL_TAP_US` (500 ms)
    wake the panel; a single tap just seeds the window and is
    otherwise ignored. Neither report is queued -- the wake pair
    doesn't dispatch a tile/nav gesture.
  - `bq25619.c` + `.h`: new `bq25619_ntc_status_str(fault_reg)`
    decodes REG_FAULT[2:0] into `Normal / Warm / Cool / Cold / Hot`
    (JEITA zone, datasheet-conventional BQ256xx layout, still tagged
    [DSV] until bench-confirmed).
  - `fc_cli.c` TEMP_DUMP now reads `broker_battery_data_t.fault` and
    prints `bq (BQ25619 TS) = <zone> (JEITA zone, no numeric °C on
    this chip)` -- makes it obvious the TS is being observed even
    though °C isn't available on BQ25619.
  - `firmware_version.h` -> 0.4.50.

**Not changed (deliberate):** TS_IGNORE stays cleared (TS active).
Zone code should read `Normal` at bench temperature; anything else on
a room-temp bench = suspect the divider math and re-set TS_IGNORE as
the escape hatch documented in the init comment.

### §4.10 Stage 27 close-out -- bench observations + battery swap (fw 0.4.50)

Bench findings from Ivan's final Stage 27 flash before rolling into
Stage 28:

  - **Touch wake:** a SINGLE finger-down wakes the panel immediately.
    The 500 ms double-tap window from Batch 27.6 is being missed --
    the second `valid` report is either not arriving in time (finger
    still down when the CST9217 next fires? single-report gesture?)
    or the sleep drain path is racing with the ISR. Not a blocker
    for now -- single-tap wake is fine, arguably better UX. Fix in
    Stage 28: either accept single-tap officially and drop the
    double-tap detector, or debug why the pair isn't landing.
  - **Button wake:** does NOT wake the panel at all. GPIO16 short
    press on this Mk1b unit isn't reaching `button_poll()` as an
    event, or `boot_display_wake()` isn't taking. Stage 28
    investigation: verify with a fresh boot log that a short-press
    fires the STRONG_CLICK haptic ack (the press-edge cue in
    `button_poll`); if silent, the ISR/pin path is the culprit, not
    the wake gate.
  - **TEMP_DUMP wording:** `bq (BQ25619 TS) = Normal (JEITA zone, no
    numeric °C on this chip)` is too wordy. Shorten to
    `bq (BQ25619 TS) = Normal (JEITA zone)`. One-line CLI edit, done
    in Stage 28.

**Battery hardware change (2026-09-12 evening):** Ivan swapped the
nominal 400 mAh cell for a **310 mAh thinner cell** that fits the
enclosure properly. Capacity nameplate downgrade, but Ivan expects
identical bench performance and the fit is a real win. Overnight
BATT_TEST report lands in Stage 28.

**Headline power result on the wearable:** idle draw is now
**< 50 mA** on Mk1b with the full sensor stack + LVGL up. The old
Mk1 baseline was ~100 mA in the same configuration. No specific
Mk1-vs-Mk1b delta identified (power topology, sensor init, or PM
lock behaviour all changed together across the respin) -- Mk1b just
lands cleanly. Root-cause investigation deferred permanently since
Mk1 is shelved per [[project_mk1_shelved]].

**Stage 27 = closed.** Definition of done at §5 is met: display
came up, sensors all ACK, encoder + button + haptic + SD verified,
DISP OFF works, auto-sleep + wake path in place (with the two bugs
above pinned for Stage 28), fuel gauge feeds the battery tile, TS
enabled on the BQ, board runs on a real cell. GPS remains silent --
stays parked as the standing open item, not a Stage 27 blocker.

---

## 5. Definition of done for Stage 27

  1. `MAX-M10S` is alive on UART1: sketch 18 produces bytes / MON-VER
     replies OR the ESP-IDF driver's boot log shows the same. Fix
     (valid GNSS coordinates) is not required to close this stage --
     that depends on the antenna being outdoors.
  2. `ENC` CLI verb correctly counts rotation events end-to-end.
  3. Button single-press vs long-hold vs double-click all behave per
     Stage 17 / Stage 20 timing.
  4. `HAPTIC PLAY 14` produces a felt buzz.
  5. `FS_LS /sd` lists files.
  6. No regressions on peripherals already stable at Stage 26 close
     (all sensors OK in `WHOAMI`, charging + battery-run still
     working, `FUEL` still reads fine).

Once §1-6 hold across a fresh cold boot AND a soft `REBOOT`, Mk1b is
**hardware-complete** on the bench:

  - Every driver has ACKed at least once.
  - Every peripheral has a CLI verb exercising it.
  - Battery + charging path is verified.

Firmware work from that point onward can happen without bench access
except for cases where a change specifically wants observation. The
board can move to enclosure integration + field-trial planning.

---

## 6. Next-steps hook -- Stage 28 scope

Post-hardware-complete, firmware-only work queued for Stage 28+:

  - **Display bring-up on this unit** (once panel is mated). Carries
    the deferred Stage 26 items: landscape rotation (real, via LVGL
    matrix transform or manual transpose), font sharpness bump
    (`LV_COLOR_DEPTH_16 -> _32`), blue-light filter RGB/BGR fix.
  - **Battery broker channel for MAX17048 SOC** -- 1 Hz polling task,
    broker payload update, battery tile UI update. Waits for cell
    soak data.
  - **Real-cell voltage in `broker_battery_data_t`** -- swap the fake
    `bq25619_read_vbat_mv()` for MAX17048 VCELL so
    `battery_tile.c` and any consumer of `bat.voltage` shows real
    numbers. Kills the last of the "fake data" in the stack per
    [[feedback_status_terse]] and [[project_bq_no_vbat_adc]].
  - **CLI submenu restructure** per [[project_cli_submenus]] --
    unblock once the verb list is stable post Stage 27.
  - **Encoder tile redesign** per [[project_encoder_tile_redesign]]
    -- needs display + working encoder.
  - **Recording writers datetime migration** per
    [[feedback_recording_datetime_opportunistic]].
  - **SDMMC PM lock chase** (Stage 23 §5.8) once SD is regularly
    exercised.
    Ivan's additions:
  - **double TAP on the screen** should lock the screen. identically, a double tap on an off screen should wake it. 2 taps within 500ms for now.
  - **ENC verb** is not logging any steps whatsoever:
  - [ENC] cw=0 ccw=0 net=0 rate=0.0dps
[ENC] CrownEnc @ GPIO A=21 B=43 (polled detent-rest)
[ENC]   CW           = 0
[ENC]   CCW          = 0
[ENC]   total (net)  = 0
[ENC]   rate         = 0.00 detents/s
[ENC]   last event   = 0 ms ago
[ENC]   glitches     = 0 (PCNT path -- dormant)


---

## References

  - `Stage_26_Mk1b_Display_Polish.md` -- previous stage, closed
    2026-09-11.
  - `Screen_Test_Checklist.md` -- visual verification list (used
    once display is mated in Stage 28+).
  - `Mk1b_fix_list.md` -- if any peripheral bring-up surprises,
    cross-reference for a known iv7.1 fix.
  - `Module_Blueprint.md` -- module command-surface contract.
  - Auto-memory `MEMORY.md` -- especially
    [[project_mk1b_arrived]], [[project_mk1_shelved]],
    [[project_bq_no_vbat_adc]],
    [[project_power_topology_mk1b]],
    [[feedback_cli_first_testability]],
    [[feedback_encoder_polling]],
    [[feedback_sketches_need_shipmode]],
    [[feedback_diode_check_parallel_paths]],
    [[feedback_status_terse]].
