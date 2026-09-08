# Kompic Mk1 -- Basic User Manual

**Hardware:** iv7.1 (Mk1 bench prototype). Mk1b PCB uses the same firmware.
**Firmware target:** `KOMPIC_FW_VERSION` in `firmware_version.h`.
**Status:** Pre-display baseline. The screen bring-up is deferred. All
interaction runs through the button, the rotary encoder, the RGB LED, and
the USB serial console.

---

## 1. What Kompic Mk1 does

Kompic Mk1 is a wrist-worn sensor node. The device records timestamped
data from onboard sensors to a microSD card. The user can also inspect
the state of the device on the USB serial console.

A single button, a rotary encoder, and a WS2812 RGB LED control the
device. The device runs as a "field capture" state machine with one
active mode at a time.

---

## 2. Controls and connectors

- **Button.** GPIO16. Doubles as the BQ25619 `/QON` line.
- **Rotary encoder.** ALPS EC05E with detent. A/B channels on GPIO21 /
  GPIO43. The encoder also holds an integrated push switch (not read
  by firmware today).
- **RGB LED.** WS2812 on GPIO42. One pixel.
- **Haptic actuator.** LRA driven by DRV2605L on I2C bus 1.
- **microSD holder.** SDMMC. The card holds a FAT32 filesystem.
- **USB-C port.** USB CDC serial console at 115200 baud. USB also
  powers the device and charges the battery through the BQ25619.

---

## 3. Start-up

Perform this sequence:

1. Connect USB or press the button for one second on a charged battery.
2. The BQ25619 pulls `/QON` low and powers the ESP32-S3.
3. The firmware boots. The RGB LED shows no light during the first
   ~500 ms.
4. The haptic actuator plays two short "click" pulses at the end of
   boot. The two clicks confirm every driver reached its init callback.
5. The device enters STANDBY in the last-used mode. The RGB LED begins
   the idle ladder (Section 6).

Expected cold-boot time: ~4 seconds from BQ25619 power-on to STANDBY.
The delay comes from the BQ25619 `/QON` timing, the ESP32-S3 boot ROM,
and the DRV2605 auto-calibration (2 s bench-verified settling).

---

## 4. Button gestures

The button uses a debounced state machine (`button_poll()` in
`fc_common.c`). The state machine emits three events:

| Gesture                             | Event         | Effect                             |
|:------------------------------------|:--------------|:-----------------------------------|
| Single click (< 1 second, no double)| `event = 1`   | Confirm / enter mode / mark        |
| Double click (< 350 ms gap)         | `event = 2`   | Mode-specific (see Section 7)      |
| Long press (>= 4 seconds)           | Ship mode     | Drop the BATFET; power off         |

**Timing rules:**

- `BTN_DEBOUNCE_MS = 30`. Sub-30 ms bounces do not count.
- `BTN_DOUBLE_GAP_MS = 350`. A second press within this window emits a
  double-click. A single-click event fires only after this window
  closes.
- `BTN_LONG_PRESS_MS = 1000`. A release past this window emits no
  click. The shutdown watcher owns the press from here.
- Press-edge haptic. Every confirmed press fires a `DRV_STRONG_CLICK`
  the moment the pin goes low. The haptic confirms the device received
  the press.

**Shutdown ladder** (see `SHDN_*` constants in `fc_shutdown.c`):

| Held for      | Feedback                                              |
|:--------------|:------------------------------------------------------|
| 0..200 ms     | Nothing visible. Definitely a click.                  |
| 200..1000 ms  | RGB LED ramps dim red. Release still fires a click.   |
| 1000..2000 ms | RGB LED continues to ramp. Release no longer clicks.  |
| 2000..3700 ms | Haptic warning click every 500 ms.                    |
| 3700..3950 ms | Sustained warm-up buzz (three clicks 80 ms apart).    |
| >= 4000 ms    | Long buzz. Fire ship mode. Drop BATFET if on battery. |

**Ship mode on USB.** The BATFET drop does not kill VDD when USB is
plugged. The firmware aborts any live recording and logs the event.
Charging continues. Unplug USB to complete the shutdown.

---

## 5. Rotary encoder

The encoder uses a polled detent-rest state machine
(`encoder_delta()`). The state machine emits one event per confirmed
detent transition, regardless of scroll speed.

- **Rotate one detent clockwise** -- move to the next mode.
- **Rotate one detent counter-clockwise** -- move to the previous mode.
- **Long, fast scroll** -- one event per detent. The state machine
  does not skip or double-count.

The integrated push switch inside the encoder is not read by firmware
today.

---

## 6. RGB LED behaviour

The `rgb_policy` component drives the WS2812. The policy uses a 50 ms
tick. The idle ladder runs when no mode owns the LED.

**Idle ladder (STANDBY, no charging, no low-battery):**

| Phase      | Duration | Colour              | Purpose                       |
|:-----------|:---------|:--------------------|:------------------------------|
| Preview    | 5 s      | Mode preview colour | Show the current mode.        |
| Normal     | 10 s     | Dim white           | Show the device is alive.     |
| Fade       | 10 s     | Dim white -> black  | Prepare for the off phase.    |
| Off        | Until    | Black               | Zero draw between wakes.      |
|            | input    |                     |                               |

**Priority overrides** (highest first):

1. **Battery alert.** Red pulse when the state of charge is below 15%.
2. **Charging.** Slow green pulse while `BQ_STATUS_PG = 1` and the
   charger is not `DONE`.
3. **Shutdown ramp.** Solid red from 200 ms of button hold. Intensity
   increases with held time. The ramp releases on button release
   below 4 seconds; the ramp goes to full red on fire.
4. **Mode preview.** The colour matches the current mode.

**Mode preview colours** (see `rgb_policy.c` mode table):

| Mode           | Colour |
|:---------------|:-------|
| STANDBY        | White  |
| FCM_MIC        | Blue   |
| FCM_ENV        | Green  |
| FCM_LSM        | Yellow |
| FCM_PPG_BCG    | Purple |
| FCM_COMPASS    | Cyan   |
| FCM_ECG        | Orange |
| FCM_TEMP       | Fire strobe |
| FCM_FLASHLIGHT | White (torch on) |
| FCM_ALARM      | Amber  |
| FCM_USB_MSC    | Cyan   |

---

## 7. Modes

A "mode" is one recording target or one utility. The encoder selects
the mode. The button opens the mode for use.

| Mode              | Purpose                                                 |
|:------------------|:--------------------------------------------------------|
| FCM_MIC           | PDM microphone capture (16 kHz mono).                   |
| FCM_ENV           | BME688 air (temp / RH / pressure) + VEML6030 lux.       |
| FCM_LSM           | Gateway to the LSM6DSV16X submenu.                      |
| FCM_PPG_BCG       | MAX30101 raw green + LSM6DSV16X accel at 200 Hz.        |
| FCM_FLASHLIGHT    | White torch on GPIO41.                                  |
| FCM_ALARM         | Set an RTC alarm through the CLI.                       |
| FCM_COMPASS       | LIS3MDL magnetometer heading.                           |
| FCM_ECG           | LSM6DSV16X Qvar electrostatic ECG at 480 Hz.            |
| FCM_TEMP          | Aggregate every onboard temperature source.             |
| FCM_USB_MSC       | Unmount SD then expose it to the host via USB MSC.      |

LSM submenu entries (open from FCM_LSM):

| Submenu           | Purpose                                                 |
|:------------------|:--------------------------------------------------------|
| FCM_MOTION        | Accel + gyro + magnetometer CSV.                        |
| FCM_BCG           | Ballistocardiogram from the accel Z-axis.               |
| FCM_STEPS         | Embedded pedometer step counter.                        |
| FCM_MLC_COLLECT   | Raw accel + gyro CSV for MLC training.                  |
| FCM_TAP_DBG       | Host-side tap detector debug view.                      |

---

## 8. Recording

Each recording mode uses the same pattern:

1. Rotate the encoder to the mode. The RGB LED shows the preview
   colour for 5 seconds.
2. Click the button. The mode enters its RECORDING state. The RGB LED
   goes solid on the mode colour.
3. The firmware opens a CSV file on the SD card. The path is
   `/sd/data/<mode>/<mode>_NNNN.csv` where `NNNN` is a monotonic index.
4. Every CSV row carries an ISO-8601 UTC timestamp from the PCF85063A
   RTC and a `t_ms` millisecond-since-boot value.
5. Click the button again. The mode returns to STANDBY. The file
   closes cleanly.

**Durability rule.** Every row calls `fclose()` or `fsync()` before
returning. A power loss during a row loses at most one row. See
`feedback_sd_write_durable.md` for the incident that led to this rule.

---

## 9. Shutdown and the 15-minute uptime cap

Kompic has two paths to shutdown:

**Manual shutdown.** The user holds the button for 4 seconds. The
shutdown ladder in Section 4 fires. `watcher_ship_mode()` writes the
BQ25619 REG_MISC to disable the BATFET. The device dies within ~1
second on battery.

**Autonomous shutdown (uptime cap).** The firmware runs a 15-minute
damage-control timer (`SHDN_MAX_UPTIME_MS`). The timer fires the same
ship-mode path.

The timer resets on any of the following:

- A recording is active (`g_recording_active`).
- The battery test mode is active (`g_batt_test_active`).
- A button press begins (`button_poll` fresh-press edge).
- The encoder emits a confirmed detent.
- Any code calls `shutdown_watcher_kick()` (2026-09-08 addition).

The timer does not reset on serial console input alone. Send a `REBOOT`
or any input that reaches the button or encoder handlers to postpone
the cap.

**USB behaviour on uptime-cap fire.** The uptime cap fires while USB
is plugged. Before Ivan's 2026-09-08 fix, this dropped BATFET and
stopped charging. The current firmware detects the USB state through
the BQ25619 `power_good` flag. If USB is present, the firmware aborts
any live recording, logs the event, re-arms the deadline, and leaves
BATFET on. Charging continues until the operator unplugs the cable.

---

## 10. Charging behaviour

The BQ25619 handles charging. The firmware never writes to the
charge-enable path. The chip charges the battery whenever VBUS is
present and no fault is active.

The RGB LED shows a slow green pulse while charging (see Section 6).

**Ship mode and charging.** Ship mode writes `BATFET_DIS = 1`. The
BATFET disables. The battery cannot receive current from VBUS through
BATFET while `BATFET_DIS = 1`.

`BATFET_RST_WVBUS = 1` re-enables the BATFET on the next VBUS unplug.
This is the "unplug USB after ship + re-plug" recovery path.
`BATFET_RST_EN = 0` prevents a fresh VBUS insert from re-enabling the
BATFET on its own.

---

## 11. Serial console (USB CDC)

Connect USB. Open a serial terminal at 115200 baud. The device echoes
its command prompt. Type `HELP` for the current list.

Common verbs:

| Verb               | Effect                                                |
|:-------------------|:------------------------------------------------------|
| `HELP`             | List available commands.                              |
| `STATUS`           | One-shot state dump.                                  |
| `GET_TIME [-v]`    | Read the RTC.                                         |
| `SET_TIME <ISO>`   | Write UTC to the RTC + NVS + PCF RAM_byte.            |
| `RTC_DUMP`         | Hex-dump of the PCF85063A registers.                  |
| `NVS_PRINT [ON|OFF]` | Toggle boot-time NVS printout.                      |
| `LOGLEVEL [level]` | Runtime log-level override. See Section 12.           |
| `GESTURE`          | Dump the wrist-gesture state.                         |
| `BATT_TEST [ON|OFF]` | Enter battery-test mode on next boot.               |
| `BLACKBOX [ON|OFF]` | Background telemetry logger.                         |
| `REC_AUDIO [ON|OFF]` | 5-second voice annotation on selected modes.        |
| `WHOAMI`           | I2C sensor identification + broker `hw_alive` status. |
| `TEMP_DUMP`        | Read every onboard temperature source.                |
| `PM_DUMP`          | Dump the PM lock inventory.                           |
| `RGB <r> <g> <b>`  | Bench-poke the WS2812. `RGB AUTO` releases.           |
| `FS_LS [/sd/path]` | List an SD directory.                                 |
| `FS_CAT </sd/path>` | Dump an SD file to the console.                      |
| `SHIPMODE`         | Drop the BATFET now.                                  |
| `REBOOT`           | Clean software reset.                                 |

---

## 12. Log-level policy

The firmware carries a runtime log-level policy (see `main.c`
Stage 17 §3.2 block).

Precedence:

1. An NVS-stored level (0..5) always wins.
2. Sentinel `0xFF` selects the auto policy.

Auto policy:

- USB CDC connected -- level `INFO`.
- USB CDC absent (battery-only) -- level `WARN`.

The `LOGLEVEL` verb changes the level at runtime. Values: `off`,
`error`, `warn`, `info`, `debug`, `verbose`, `auto`. `LOGLEVEL AUTO`
restores the sentinel.

The `WARN` default keeps the battery-only log quiet during a
protocol run. Mode-entry, mode-exit, session-open, and session-close
milestones stay at `INFO`. Per-tick status prints stay at `DEBUG`
after the Stage 18 audit. See `LOG_LEVEL_POLICY.md`.

---

## 13. Files on the SD card

Layout:

```
/sd/
  data/
    battery/    -- BATT_TEST CSVs and BLACKBOX telemetry.
    env/        -- FCM_ENV recordings.
    lsm/        -- LSM submenu recordings.
    mic/        -- FCM_MIC recordings (WAV).
    ppg_bcg/    -- FCM_PPG_BCG recordings.
    ...
```

Every CSV carries a header comment. The header includes the firmware
version (`fw=`), the hardware version (`hw=`), a boot sequence number
(`boot_seq=`), and the mode name. Downstream analysis groups by
these fields. The `battery_tag` NVS field is reserved for future
cell-provenance tagging (Stage 19 §7).

---

## 14. Boot log provenance

Every boot logs one banner from `main.c` and one line per driver
`_init()`. The pattern:

```
MAIN: KOMPIC hw=iv7.1 fw=0.4.18
BOOT_POWER: boot_logic v0.3.0
BROKER: driver v0.2.1
BME688: driver v0.2.0
BQ25619: driver v0.2.0
...
```

Any log capture identifies the source tree that produced it. The
per-driver version comes from `<NAME>_DRIVER_VERSION` at the top of
each driver's header. The umbrella `KOMPIC_FW_VERSION` bumps every
time any driver bumps.

---

## 15. Known limitations (pre-Mk1b)

- **Display.** The AMOLED receptacle footprint is mirrored on iv7.1.
  The panel is offline. Kompic runs headless until Mk1b arrives.
- **Touch.** The CST9217 shares the panel FPC. Touch is offline on
  iv7.1 for the same reason.
- **GPS.** The MAX-M10S is not populated on iv7.1. The GPS task is
  disabled at compile-time.
- **Cold-boot delay.** The device takes ~4 seconds from BQ25619 wake
  to STANDBY. The delay is BQ25619 QON timing + ESP boot ROM + DRV
  auto-cal settling. No firmware-only shortening is possible.
- **Battery runtime.** ~2.5 hours between charges at the Stage 18
  baseline. Stage 19 addresses the drain.

---

## 16. Where to look next

- `docs/build_info/Mk1_build_reports/` -- stage-by-stage engineering
  log. Every recent decision is there.
- `docs/build_info/reference_files/ASD-STE100_reference.md` -- writing
  style used in this manual.
- `docs/build_info/reference_files/LOG_LEVEL_POLICY.md` -- log-level
  classification.
- `docs/build_info/reference_files/IV71_TO_MK1B_FIRMWARE_DELTA.md` --
  firmware changes for Mk1b bring-up.
- `firmware/esp-idf/components/field_capture/firmware_version.h` --
  umbrella firmware version.
