# Kompic Mk I firmware — build, flash, serial

**Target chip:** ESP32-S3 (WROOM-1U-N16R8: 16 MB flash / 8 MB octal PSRAM).
**Repo path:** `firmware/esp-idf/`
**Hardware baseline:** iv7.1.
**Latest fw:** `KOMPIC_FW_VERSION` in `components/field_capture/firmware_version.h`.

---

## 1. One-time environment setup

```
# Install ESP-IDF v5.5.2 under ~/.espressif/ once (see esp-idf docs).
# Then set up a shell alias:
echo 'alias get_idf=". $HOME/.espressif/v5.5.2/esp-idf/export.sh"' >> ~/.bashrc
```

## 2. Per-terminal setup

```
get_idf
cd firmware/esp-idf
```

## 3. Common commands

```
idf.py menuconfig                        # Kconfig UI (same as the VS Code cog icon)
idf.py build                             # compile
idf.py -p /dev/ttyACM0 flash monitor     # flash + serial monitor (Ctrl-] to exit)
idf.py fullclean                         # wipe build/ and managed_components/
idf.py set-target esp32s3                # only if switching chip family
```

**VS Code ESP-IDF toolbar missing?** Open **`firmware/esp-idf/`** as the workspace root. The extension only detects the project when `CMakeLists.txt` + `sdkconfig` sit at the workspace root.

---

## 4. Required menuconfig settings

**Set once after `idf.py set-target esp32s3`, then verify with `grep -E "^CONFIG_(FATFS_LFN|PM|FREERTOS|TINYUSB)" sdkconfig`.**

### 4.1 Board-level

| Path | Setting |
|---|---|
| Serial flasher config → Flash size | **16 MB, QIO, 80 MHz** |
| Partition Table → Custom | **`partitions.csv`** |
| Component config → ESP System Settings → CPU frequency | **240 MHz** |
| Component config → ESP PSRAM → SPI RAM support | **ON, mode Octal, 80 MHz** |
| Component config → FreeRTOS → Kernel → `configTICK_RATE_HZ` | **1000** |
| Component config → FAT Filesystem support → Long filename support | **Long filename buffer in heap** (mandatory — default 8.3 rejects `s0058_r0001_annot.wav`-style names) |

### 4.2 LVGL fonts (for display path; safe even without display)

| Path | Setting |
|---|---|
| Component config → LVGL configuration → Font Usage → Enable built-in fonts | **Monaco 8 through 48** (all sizes on) |

### 4.3 Power management (fw ≥ 0.4.2)

| Path | Setting | Kconfig symbol |
|---|---|---|
| Component config → Power Management → Support for power management | **ON** | `CONFIG_PM_ENABLE=y` |
| Power Management → Enable DFS at startup | **ON** | `CONFIG_PM_DFS_INIT_AUTO=y` |
| Power Management → Put lightsleep-related functions in IRAM | **ON** | `CONFIG_PM_SLP_IRAM_OPT=y` |
| Power Management → Enable profiling counters for PM locks | **ON** | `CONFIG_PM_PROFILING=y` |
| FreeRTOS → Kernel → configUSE_TICKLESS_IDLE | **ON** | `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` |
| FreeRTOS → Kernel → Idle tick count threshold before entering sleep | **3** | `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3` |
| FreeRTOS → Kernel → Enable FreeRTOS to collect run time stats | **ON** | `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` |
| FreeRTOS → Kernel → configRUN_TIME_COUNTER_TYPE | **uint32_t** | `CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U32=y` |

### 4.4 USB Mass Storage (Stage 11 Item A, ≥ 0.3.0)

Applied via `sdkconfig.defaults` on first build after the managed component pulls in.

| Path | Setting | Kconfig symbol |
|---|---|---|
| Component config → TinyUSB Stack → TinyUSB task priority | **5** | `CONFIG_TINYUSB_TASK_PRIORITY=5` |
| TinyUSB Stack → TinyUSB Classes → USB MSC | **ON** | `CONFIG_TINYUSB_MSC_ENABLED=y` |
| TinyUSB Stack → TinyUSB Classes → MSC → USB MSC FIFO size | **4096** | `CONFIG_TINYUSB_MSC_BUFSIZE=4096` |
| TinyUSB Stack → TinyUSB Classes → USB Serial (CDC-ACM) | **OFF** | `# CONFIG_TINYUSB_CDC_ENABLED is not set` |

### 4.5 What NOT to touch

- **Component config → ESP System Settings → Channel for console output** must stay **UART0**.
- **… → Channel for console secondary output** must stay **USB Serial/JTAG**.
- These two keep `esptool` + `idf.py monitor` working on every boot except while USB MSC mode is active.

---

## 5. Partition table

`partitions.csv` in this directory. 16 MB layout:

```
nvs      16 KB   sensor cal, UI, NVS-command-state
otadata   8 KB   (reserved for future OTA)
factory   3 MB   main app
storage  ~12 MB  spiffs/data reserved
```

Not usually touched. If you change it, run `idf.py fullclean` first.

---

## 6. SD card durability rule (mandatory)

Any code that writes to SD **must** guarantee per-row durability. `fflush()` alone leaves 0-byte files on power loss.

- **Pattern A** (< 10 Hz writers): `fopen("a")` + write + `fclose` per row.
- **Pattern B** (≥ 10 Hz writers): keep open, `fflush()` + `fsync(fileno(f))` per row.

Rule and both patterns documented at the top of `components/field_capture/field_capture.c`. See also memory `[[feedback-sd-write-durable]]`.

---

## 7. Serial CLI (USB-Serial-JTAG @ 115200)

Reachable via `idf.py monitor` or any serial terminal. Case-insensitive. Line-terminated (`\n` or `\r`). Type `HELP` any time to reprint this list.

### 7.1 Time

```
GET_TIME [-v]                    read RTC now (-v also dumps NVS + RAM_byte)
SET_TIME YYYY-MM-DDTHH:MM:SS     write UTC + persist to NVS + PCF RAM_byte (0x03)
RTC_DUMP                         hex dump of all 18 PCF85063A registers
```

### 7.2 NVS config

```
NVS_PRINT [ON|OFF]               toggle boot-time NVS printout (no arg = dump)
BATT_TEST [ON|OFF]               enter battery-test mode on next boot (no arg = state)
BLACKBOX [ON|OFF]                background telemetry logger (reboot to start/stop)
BLACKBOX_CADENCE <s>             blackbox sample cadence (default 10 s, range 1..3600)
```

### 7.3 Diagnostics

```
HELP                             re-print this command list
STATUS                           one-shot state dump (uptime, sensors, batt, heap, NVS)
WHOAMI                           I2C sensor identification + hw_alive status per chip
PM_DUMP                          dump PM lock inventory now (identical to boot dump)
```

### 7.4 Filesystem (SD)

```
FS_LS [/sd/path]                 list SD directory contents (default /sd)
FS_CAT </sd/path>                dump a file to console (capped at 32 KB)
```

### 7.5 Control

```
RGB <r> <g> <b> | RGB AUTO       bench-poke WS2812 (0..255) / release manual override
SHIPMODE                         drop BATFET (button-stuck escape). USB unplug needed to finish.
REBOOT                           esp_restart() -- clean SW reset
```

---

## 8. Modes (top-level encoder cycle)

Scroll encoder to select, click to enter. Colours are the standby-LED indicator.

| Mode | Colour | Purpose |
|---|---|---|
| MIC | red | 30 s mono WAV @ 16 kHz |
| ENV | green | BME688 + VEML6030 CSV |
| SKIN | pink | MAX30101 PPG + TMP117 CSV |
| FLASHLIGHT | white | encoder = brightness while ON |
| ALARM | purple | DRV wake-alarm test |
| COMPASS | magenta alt | LIS3MDL heading (10 s figure-8 cal on first press) |
| ECG | yellow/purple alt | LSM6DSV16X QVAR touch |
| TEMP | fire strobe | all onboard temp sensors printed 1 Hz |
| LSM | yellow | gateway to LSM submenu (click to enter) |
| USB_MSC | cyan | click → SD becomes USB drive to host. Click again OR unplug USB → esp_restart(). See `components/usb_msc/` |

**LSM submenu:** MOTION → BCG → STEPS → MLC_COLLECT → TAP_DBG. Click enters, tap-Z-double OR button-double exits.

---

## 9. NVS-flagged battery-run modes

Both toggled via serial and applied on next reboot.

- **BATT_TEST** — replaces FCM with a fixed-cadence power-baseline logger. Every 10 s: `/sd/data/battery/batt_<boot_seq>.csv`, one row: `t_ms, iso_utc, soc_temp_c, batt_mv, batt_pct, charging, vbat_adc_mv, heap_free_kb, cpu_mhz, sensors_on, idle_pct`. 15-min uptime cap disabled. Sensors parked. Escape: `SHIPMODE` or `REBOOT` via serial.
- **BLACKBOX** — background telemetry that runs alongside normal FCM. Every `bb_cadence_s` seconds (default 10): `/sd/data/blackbox/bb_<boot_seq>.csv` with ~35 columns covering every broker + system state. Self-exits if `batt_test` is on. See report §BLACKBOX for column schema.

---

## 10. Bench routine (per-change)

1. Read the roadmap item / handoff.
2. Change code, `idf.py build`.
3. Flash + monitor: `idf.py -p /dev/ttyACM0 flash monitor`.
4. Verify boot log (PM banner, NVS dump, hw_alive lines).
5. Run the relevant mode; verify against bench expectations.
6. Snapshot the source: `python3 hardware/Reflow_info/reference_files/make_archive.py <label>`.
7. Bump the touched driver's `_DRIVER_VERSION` (PATCH) + `KOMPIC_FW_VERSION` in `firmware_version.h`.
8. Commit is Ivan's ([[feedback-commits]]).

---

## 11. Vbat ADC hack (temporary, iv7.1)

Between screen data pads on GPIO9 (ADC1_CH8) — 5k1-5k1 divider from Vbat to GND, optional 100 nF to GND. Read via `vbat_adc_read_mv()` in `field_capture.c`. TEMPORARY: remove when the screen returns to the FPC OR when mk2's BQ25896 lands with its integrated VBAT ADC.

```
Vbat ── 5k1 ─┬── GPIO9 ─── (opt 100 nF) ── GND
             │
            5k1
             │
            GND
```
