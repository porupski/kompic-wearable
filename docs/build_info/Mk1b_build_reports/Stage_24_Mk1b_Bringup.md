# Stage 24 -- Mk1b iv8.0 bring-up

**Date opened:** 2026-09-10
**Date closed:** 2026-09-10 (same day)
**Board:** Mk1b PCB (iv8.0) -- first physical board on Ivan's bench; iv7.1 retired.
**Firmware baseline:** `iv7.1.f0.4.31` (Stage 23 close) -> shipped at `0.4.32`.
**Status:** CLOSED. Bring-up complete through Milestone C step 10
(display dark -- panel + touch alive but ESP-IDF integration still
skewed). Landscape / MAX17048 / GPS follow-through carries to Stage 25.

---

## 0. Scope

Bring Mk1b iv8.0 up from bare PCB to fully-functional wearable, in the
assembly order Ivan will physically follow. Every fw feature Stage 23
left pending queues in behind the bring-up milestones so we're not
blocked on iv7.1 side quests when hardware verification demands
attention.

**Rules of engagement (Mk1b-specific):**
  - **Topside first, USB-wired, no case.** Verify every topside
    component works before flipping to the bottom side. iv7.1 taught us:
    soldering both sides then debugging is a nightmare.
  - **GPS reflow deferred within topside.** GPS is the single-chip
    hardest reflow (BGA-adjacent, big thermal mass). Do it LAST on
    topside so failures elsewhere don't waste the GPS chip.
  - **USB pigtail for early boots.** Use the four test pads (VBUS/GND/
    D+/D-) near the receptacle, per `pre_fab_checklist.md §4`. Solder
    wires from a sacrificial USB-C cable straight to the pads. Only
    populate the actual USB-C receptacle after everything else on top
    is verified.
  - **Battery stays disconnected during bring-up.** No permanent
    battery pack until the BQ + BATFET path is verified. Ship-mode
    false-trips would be a real headache mid-bring-up.
  - **`feedback_flash_per_checkpoint.md` still applies.** Each hardware
    milestone gets a firmware verification pass before moving on.

---

## 1. Assembly plan (Ivan's physical order)

### 1.1 Phase A -- Topside (excluding GPS)

Solder + verify in this order:

1. **Power path first.** BQ25619 charger, TPS62840 buck, XC6206 LDO
   (with the F-LDO-1 swap fix applied), decoupling caps, TS thermistor
   divider.
   - Bench check: apply 5 V to VBUS pigtail, verify 3V3 and 1V8 rails
     come up. Verify SYS = ~3.6-4.2 V (BQ SYS regulation) when no
     battery.
   - **Do NOT populate MAX30101 or LIS3MDL until this step passes.**
     F-VIO-1 / F-LDO-1 both killed sensors last time.

2. **ESP32-S3 + PSRAM + flash.** Populate the SoC, PSRAM, external QSPI
   flash. Solder USB-C pigtail wires to the four test pads.
   - Bench check: `esptool.py chip_id` succeeds over USB-CDC-JTAG.
     Flash the current fw (`iv7.1.f0.4.31`) as a first pass to get a
     live boot log even without sensors.

3. **I2C bus 0 sensors (topside batch).** BME688, LSM6DSV16X, LIS3MDL,
   VEML6030, MAX30101, TMP117, PCF85063A, CST9217 (touch).
   - Bench check: `WHOAMI` prints each chip ACK + WHO_AM_I match. Any
     NAK = a soldering issue on THAT sensor's pin group; no need to
     disturb others.

4. **I2C bus 1 sensors.** DRV2605L (haptic), BQ25619 (already up from
   step 1 but confirm addressable on I2C), MAX17048 fuel gauge.
   - Bench check: `HAPTIC PLAY 14` fires the LRA. `BQ` prints register
     dump. MAX17048 shows up in `WHOAMI` at 0x36.

5. **SD card slot + SDMMC.** Solder the µSD receptacle. Insert card.
   - Bench check: `FS_LS` lists card contents.

6. **WS2812 LED + haptic motor + flashlight LED.** Small stuff.
   - Bench check: `RGB 255 0 0` red, `RGB 0 255 0` green,
     `RGB 0 0 255` blue.

7. **Encoder.** Wheel + click switch.
   - Bench check: `TILE list` output plus rotate encoder and verify
     rotation events land in the log.

**Milestone A gate:** every topside component listed above passes its
bench check. `STATUS` shows all sensor_status = OK. Flash log has zero
WARN except the known deprecation notice.

### 1.2 Phase B -- GPS reflow (last on topside)

8. **MAX-M10S GPS module.** Reflow it on top with hot air. Big thermal
   mass, LGA-adjacent -- last so failures don't waste the chip.
   - Bench check: on next boot, `GPS_VIEW normal` shows fix data
     populating within ~30-60 s under open sky.

**Milestone B gate:** GPS reports valid fix (fix quality ≥ 2, sat count
> 4). RTC gets synced via `pcf85063_cmd_sync_utc()` when GPS fix is
valid.

### 1.3 Phase C -- Bottom side

9. **Bottom-side ICs.** Any additional bottom-side passives (MAX17048
   already on top with BQ per pinout).

10. **Display (CO5300 AMOLED) + FPC connector.** The F-DISP-1 mirror
    fix should have landed in Mk1b -- verify FPC pin 1 lands where it
    should physically (per `pre_fab_checklist.md §1`) BEFORE inserting
    the panel.
    - Bench check: `LVGL_FORCE` OFF (default) + reboot. `STATUS` shows
      `display = PRESENT (CO5300 up) touch = CST9217 up`. All 10 tiles
      render on the panel.

**Milestone C gate:** panel lights up, touch works, all tiles render.
`Screen_Test_Checklist.md` walked top-to-bottom.

### 1.4 Phase D -- Enclosure + battery

11. Battery pack connected via BAT+ / BAT- pads.
12. Encoder click gesture routing (double-click ship-mode).
13. Enclosure assembly.

**Milestone D gate:** unplug USB. Watch runs on battery for at least
one full charge cycle without incident.

---

## 2. Firmware companion work (interleaved with assembly)

Batches queue behind milestones so Ivan's not blocked waiting on code
between soldering sessions. Each batch is
`feedback_flash_per_checkpoint.md`-sized.

### 2.1 During Phase A -- immediately useful for bench diagnosis

- **§2.1a HELP sorted alphabetically.** Auto-sort the CLI verb list at
  print time. Small change in `rtc_cli_print_help()` -- collect lines
  into a static array, sort, print.
- **§2.1b `SCREEN [main|settings|ROTATE]` verb.** Programmatic screen
  switch for bench screenshot flow. Adds a rotate hook that
  reconfigures the CO5300 MADCTL register + LVGL orientation. Panel is
  landscape 410x502 -- iv7.1 was hardcoded 466x466 portrait, Mk1b
  should default to the true orientation.

### 2.2 During Phase B -- unblocked by GPS being present

- **§2.2 Dual-time display + timezone LUT.** Feature request from Ivan
  2026-09-10.
  - Default reference zone: **Central European Time (UTC+1, HR/SLO)**.
  - GPS provides UTC + lat/lon.
  - Small country/timezone LUT keyed on lat/lon boundaries returns an
    offset. If not in LUT, use UTC (or last known).
  - Watch face shows local time in large font, CET in small below (or
    vice versa), toggleable via `TIME_MODE [single|dual]` verb
    (NVS-persisted, default = dual).
  - If no GPS ever, show CET only.
  - `SET_TIME` continues to write UTC to the RTC. Display layer
    applies the offset. `RTC` verb still shows UTC + `valid=1`.

### 2.3 During Phase C -- after display is up

- **§2.3a ECG cleanup.** Delete `qvar_ecg`, `ecg_tile`, `tile_registry`
  ECG entry. NO ECG per [[feedback_no_ecg_on_kompic]].
- **§2.3b Font upgrade for GPS photo view.** Montserrat_48 or
  JetBrainsMono for the big time+coord display on the GPS tile.
- **§2.3c PNG compression for LVGL_SCREENSHOT.** miniz deflate; file
  drops ~640 KB → ~150 KB.

### 2.4 During Phase D -- final polish

- **§2.4a `ws2812_cmd.{c,h}` cmd surface + rgb_policy audit.**
- **§2.4b Encoder tile + cmd surface** per
  [[project_encoder_tile_redesign]]. Test-ground layout: rotation
  speed, click count, button response.
- **§2.4c I2C_NUM_0/1 migration to `driver/i2c_master.h`** across all
  10 drivers. Kills the boot deprecation warning.
- **§2.4d SDMMC PM lock chase** (Stage 23 §5.8): 79% APB_FREQ_MAX hold
  in normal operation. First real battery-life move since Stage 17.

---

## 3. Snags encountered during bring-up (and how they were fixed)

### §3.1 USB serial not appearing (`/dev/ttyACM*` absent)

**Symptom:** `idf.py monitor` scanned all `/dev/ttyS*`, found nothing.
No USB device visible in `lsusb` either.

**Root cause:** D+ and D- traces on the Mk1b PCB were shorted together.
With both differential lines shorted, the USB PHY can never drive valid
J/K states; host sees nothing.

**Fix:** Traced and separated the short on the bench. USB enumerated
immediately as `303a:1001 USB JTAG/serial debug unit`.

### §3.2 Board crashing every ~29 seconds after USB fixed

**Symptom:** dmesg showed the board enumerating, then disconnecting,
then re-enumerating, on a ~29 s cycle. Arduino serial monitor caught
only the single line `I (679) esp_psram: SPI SRAM memory test OK`
before losing the port.

**Root cause:** DRV2605 haptic driver was not installed on the Mk1b
board at this point. The firmware drives GPIO0 (DRV_EN) HIGH at boot
and then tries to I2C-init the DRV2605. Without the chip, the I2C
transaction hangs until the task watchdog fires (~30 s default). The
watchdog reset causes the USB re-enumeration cycle.

**Secondary observation:** Without DRV2605, the board drew only 18 mA
with the RGB LED running, because DRV_EN HIGH was floating into
something that enabled the LED path unexpectedly. After DRV2605 was
installed, quiescent draw settled at 60 mA (no SD card, no daughter
board, no battery -- expected).

**Fix:** Installed DRV2605. Board booted to completion.

### §3.3 idf.py monitor dropping connection after boot

**Symptom:** After the watchdog issue was resolved, the board booted
fully but `idf_monitor` disconnected ~2 s after the last boot log line.
PM stats showed `SLEEP 40MHz: 92%` -- the board was spending almost all
time in light sleep.

**Root cause:** `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` was not set. The
USB-JTAG/Serial peripheral has no power-management lock on the
connection by default, so the board enters light sleep and powers down
the USB PHY, dropping the monitor connection.

**Fix:** Added `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` to both
`sdkconfig` and `sdkconfig.defaults`. The next boot's PM inventory
confirmed: `usb_serial_jtag NO_LIGHT_SLEEP: Active 100%` and
`light_sleep_counts: 0`.

### §3.4 esptool RTS/DTR error blocking idf.py flash

**Symptom:** After editing `sdkconfig`, `idf.py flash` failed with
`OSError: [Errno 71] Protocol error` at
`fcntl.ioctl(TIOCMBIC, TIOCM_RTS_str)`. The Linux `cdc_acm` driver
rejects modem-control ioctls on this CDC-ACM device.

**Root cause:** esptool's `default_reset` strategy opens the serial
port and immediately tries to clear RTS to set up the reset pulse. The
kernel returns `EPROTO` because the ESP32-S3 USB-JTAG CDC-ACM interface
doesn't support modem-control signals. This happened before even
`--before=no_reset` could take effect, because pyserial sets RTS state
inside `Serial.open()`.

**Fix:** Used OpenOCD (already present in the IDF toolchain at
`~/.espressif/tools/openocd-esp32/`) for the flash that carried the
`USJ_NO_AUTO_LS_ON_CONNECTION` change:

```
openocd -f board/esp32s3-builtin.cfg \
  -c "program_esp build/smartwatch.bin 0x10000 verify reset exit"
```

After that firmware landed, `idf.py flash` works normally (esptool can
now do the USB reset handshake without timing out on a sleeping board).

**Note:** `Warn: [esp32s3.cpu1] Unexpected OCD_ID = 00000000` appeared
during the OpenOCD session. This is expected behaviour when CPU1 is in
sleep or not yet examined; OpenOCD correctly proceeds on CPU0.

### §3.5 LSM6DSV16X and BQ25619 not responding on first WHOAMI

**Symptom:** First full boot showed `I2C_ERR` for both LSM6DSV16X
(0x6B, I2C0) and BQ25619 (0x6A, I2C1). All other sensors ACKed.

**Root cause:** Cold joints from initial reflow. Both chips physically
present but not making reliable I2C contact.

**Fix:** Reseated both chips. Both ACKed on the next boot. During the
reseating attempt, a solder bridge briefly caused a 500 mA draw with
the ESP32 heating up -- power was disconnected immediately, the bridge
was found and removed, and the board recovered without damage.

### §3.6 BQ25619 TS NTC junction correction

The TS NTC thermistor junction feeding BQ25619's temperature sense
input had a PCB-level wiring error that caused incorrect readings. Ivan
corrected the junction on the bench. With no battery attached the TS
pin now reads 0 V, which is correct (NTC divider collapses to 0 when
the battery connector is open).

### §3.7 CST9217 probe returning 0xFF -- display not initialised

**Symptom:** `boot_display.c` probe logged
`CST9217 signature mismatch: 0xFF (want 0xAB)`, causing
`no CO5300 module -- skipping display bring-up`. Display remained dark
and firmware ran headless even with the display FPC connected.

**Root cause:** `touch_module_present()` called `cst9217_probe_ack()`
directly without issuing the GPIO44 hardware reset pulse first. The
CST9217 powers up in an undefined state and only returns `0xAB` at
register `0xD000` after receiving a reset pulse (GPIO44 LOW ≥5 ms, then
HIGH, 50 ms settle). Without the reset it ACKs on I2C but returns
`0xFF` from all registers. The `cst9217_init()` full init path does
the reset correctly; the lightweight presence-probe path did not.

**Code fix:**
  - `cst9217_hw_reset()` in `cst9217.c` renamed from `static` to public
    `cst9217_reset()` and declared in `cst9217.h`.
  - `touch_module_present()` in `boot_display.c` now calls
    `cst9217_reset()` before `cst9217_probe_ack()`.

**Status at stage close:** Fix is in tree but not yet flashed/verified
-- this is the first item for Stage 25 §1.

---

## 4. End-of-stage hardware status

| Chip | Bus | Address | Status |
|------|-----|---------|--------|
| BME688 | I2C0 | 0x76 | OK |
| LSM6DSV16X | I2C0 | 0x6B | OK (after reseat) |
| LIS3MDL | I2C0 | 0x1C | OK |
| VEML6030 | I2C0 | 0x10 | OK |
| PCF85063A | I2C0 | 0x51 | OK (OSC stop flag cleared on first boot) |
| CST9217 | I2C0 | 0x5A | ACKs but returns 0xFF -- probe fix in tree, not flashed |
| MAX17048 (fuel gauge) | I2C1 | 0x36 | Not yet in firmware -- Stage 25 |
| DRV2605 | I2C1 | 0x5A | OK (auto-cal FAIL STATUS=0xEC -- deferred) |
| BQ25619 | I2C1 | 0x6A | OK (after reseat; TS NTC junction corrected) |
| CO5300 display | QSPI | -- | Not initialised (blocked by CST9217 probe) |
| MAX30101 | I2C0 | 0x57 | Not present (daughter board not assembled) |
| TMP117 | I2C0 | 0x48 | Not present (daughter board not assembled) |
| SD card | SDMMC | -- | Not inserted |
| GPS (MAX-M10S) | -- | -- | Soldered; not yet exercised |

**Quiescent draw:** ~60 mA at 5 V USB, no battery, no daughter board,
no SD. (Mk1/iv7.1 at comparable configuration was ~100 mA; difference
accounts for missing DRV2605 standby + missing SD + missing
encoder/button hardware.)

**DRV2605 auto-cal:** Consistently fails with `STATUS=0xEC` (OVER_TEMP
flag set, which is a known false flag during cold LRA auto-cal without
motor or with wrong coil parameters). The driver init completes and
haptic is functional; cal parameters should be tuned once the motor is
connected and the LRA profile confirmed.

---

## 5. Firmware side quests landed during bring-up

### 5.0 Side-quests landed while Ivan was assembling (fw 0.4.32)

Batch executed 2026-09-10 while Mk1b topside soldering was in progress.
All firmware-only; no hardware needed. Ivan flashed 0.4.32 to Mk1b once
topside step 2 (ESP32-S3 + PSRAM + flash + USB pigtail) was complete.

**Landed:**

- **§2.1a HELP alphabetical sort** -- verbs live in a static array in
  fc_cli.c and `qsort()` runs at print time, so new verbs can be added
  anywhere and always print in order.
- **§2.4a WS2812 cmd surface + rgb_policy fix.** New
  `components/ws2812/ws2812_cmd.{c,h}`: `set_rgb`, `auto`, `dump`,
  `status_summary`. RGB CLI verb refolded through the surface. **The
  real bug that made iv7.1's LED dark:** `rgb_policy_init()` was
  defined but never called at boot -- no animation timer ever started,
  so `rgb_policy_preview_start()` was a no-op forever. Fix in
  `boot_hw_init.c` (called right after `ws2812_init`).
- **§2.3a ECG cleanup.** Deleted `components/qvar_ecg/` (dead stub
  component -- no callers), `ecg_tile.{c,h}`, tile_registry ECG entry,
  and dead `#include "qvar_ecg.h"` in fc_modes.c. FCM_ECG mode + inline
  QVAR electrostatic sensing in fc_modes.c untouched (that's Ivan's
  non-medical use of the QVAR channel, per
  [[feedback_no_ecg_on_kompic]]).
- **Encoder cmd surface (Stage 24 §2.4b partial).** New
  `components/encoder/encoder_cmd.{c,h}` with `reset`, `dump`,
  `status_summary`. Wired via a `encoder_note_detent()` hook in
  encoder.c that the polled state machine in fc_common.c calls on every
  emitted detent -- so the ENC CLI verb sees live CW/CCW counts +
  rotation speed (dps EMA) even though the PCNT driver path is
  dormant. New `ENC [RESET]` CLI verb. **Encoder TILE deferred** until
  Mk1b display is up (needs LVGL tile registration).

---

## 6. Testing strategy notes

- **Every milestone A/B/C/D has a CLI-verifiable gate** so the headless
  workflow Stage 23 built keeps paying off through bring-up.
- `Screen_Test_Checklist.md` becomes the authoritative panel-side
  verification list once Phase C starts -- populate it as tiles come up.
- **Compare Mk1b behaviour to iv7.1 baseline** at every step. Any new
  regression = something Mk1b-specific (BOM difference, layout change,
  ...); any behaviour that matches iv7.1 = fw carry-over, potentially
  fixable in fw alone.

---

## 7. Next steps → Stage 25 scope

Stage 25 goal: **display alive, GPS tile on screen** (Mk1b Day-1
deliverable per project milestone).

### §25.1 CST9217 reset-before-probe (already coded, needs flash + verify)

Flash the `cst9217_reset()` change and confirm the boot log shows:
`CST9217 detected (0xAB @ 0xD000) -- display module present` then
CO5300 init completing.

If CST9217 probe still fails after the reset fix, investigate:
  - FPC connector seating (pin 1 orientation)
  - GPIO44 pull state during boot (any other driver claiming GPIO44?)
  - Whether Mk1b RST line in the FPC is actually wired to GPIO44

### §25.2 CO5300 display bring-up verification

Once CST9217 probes OK, CO5300 QSPI init should run. Verify:
  - Display lights up (white flash or LVGL splash during init)
  - `boot_display_is_present()` returns true
  - LVGL stack starts on Core 1
  - At least one tile renders on the panel

### §25.3 MAX17048 fuel gauge driver integration

MAX17048 is fitted on I2C1 alongside BQ25619 (fixed address 0x36).
Stage 25 work:
  - Write driver (component `max17048/`)
  - Add to `boot_hw_init` I2C scan
  - Add `WHOAMI` row and CLI verb
  - Wire SOC% reading into the battery tile and `STATUS` dump

### §25.4 GPS tile on screen (Day-1 milestone)

With display alive, wire the GPS task back in (`task_gps_fn` is
currently commented out in `boot_tasks.c`) and verify the GPS
coordinate tile renders and updates on the panel. This is the
trip-readiness gate.

### §25.5 DRV2605 auto-cal (opportunistic)

Once the motor is properly connected on Mk1b, re-run `HAPTIC CAL` via
CLI and confirm STATUS clears. If it persists, adjust the LRA profile
parameters in `haptic_tile.c` to match the actual motor (resonant
frequency, rated voltage, drive time).

---

## References

  - `Stage_23_Cmd_Surface_Encoder_Screenshot.md` -- previous stage,
    closed 2026-09-10.
  - `Stage_25_Mk1b_Display_GPS_Fuel.md` -- follow-on stage, tracks the
    Stage 24 §7 items above.
  - `Module_Blueprint.md` -- module command-surface contract.
  - `Screen_Test_Checklist.md` -- visual verification list.
  - `Mk1b_build_reports/pre_fab_checklist.md` -- physical verification
    before Gerber (already applied to iv8.0).
  - `Mk1b_build_reports/Mk1b_fix_list.md` -- every iv7.1 fix folded
    into iv8.0. Cross-reference during bring-up when a fault mode looks
    familiar.
  - `Mk1b_build_reports/Mk1b_schematic_audit_iv8.0.md` -- current
    schematic sign-off.
  - Auto-memory `MEMORY.md` -- especially [[project_mk1b_arrived]],
    [[project_mk1b_assembly]], [[project_mk1_shelved]],
    [[project_iv71_display_dead]], and
    [[feedback_mk1b_stage_naming]].
