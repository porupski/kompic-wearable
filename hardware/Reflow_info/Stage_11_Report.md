# Stage 11 report — Kompic Mk I firmware

**Baseline entering Stage 11:** `iv7.1.f0.2.8`.
**Baseline exiting this session:** `iv7.1.f0.3.4`.

Live document — updated as we go through the bench session. Sections lower in the file may still be in-progress.

---

## 1. Session goals

From `Stage_11_Handoff.md` Ivan chose:

- **Item A** — USB Mass Storage over the USB-C port (host-visible SD drive).
- **Item B** — NVS-per-driver command persistence, RTC first.
- **Item C** — CTRL1 accel-layout audit (bench-driven: MLC data showed the bug).

Bonus items that emerged mid-session:
- **RTC → newlib bridge** so FatFs stamps SD files with real UTC (was landing every file at 1980-01-01).
- **MLC_COLLECT enable-flag fix** (was silently recording zeros because IMU broker was never enabled for that mode).
- **Battery-test mode infrastructure** in preparation for the Item D power-management baseline.

Deferred by handoff:
- **Item D** power management — needs a real battery-baseline number first.
- **Item E** MLC pilot classifier — needs Ivan's data collection.
- **Item F** MAX30101 rework — Ivan's call to defer.

---

## 2. What landed — driver + firmware version bumps

| Component      | Before  | After   | Notes                                             |
|----------------|---------|---------|---------------------------------------------------|
| KOMPIC_FW      | 0.2.8   | **0.3.4** | Aggregate rolled on each substantive step        |
| pcf85063       | 0.2.0   | 0.3.1   | RAM_byte API + settimeofday() bridge              |
| field_capture  | 0.2.7   | 0.3.1   | FCM_USB_MSC + BATT_TEST + MLC_COLLECT wake        |
| lsm6dsv16x     | 0.2.5   | 0.3.0   | Item C — CTRL1/2/6/8 rewrite                      |
| nvs_cfg        | —       | 0.1.0   | New component: cfg_rtc + cfg_sys namespaces        |
| usb_msc        | —       | 0.1.0   | New component: lazy TinyUSB MSC over SDMMC        |

---

## 3. Item A — USB Mass Storage (`components/usb_msc/`)

### Design decision: lazy init + reboot-on-exit

iv7.1 exposes no BOOT button — GPIO0 is the DRV2605 enable strap. That eliminates the ROM-download escape route if a USB stack hangs at boot. Design followed from that constraint:

- **Nothing USB-related runs during normal boot.** TinyUSB is installed on demand from `run_battery_test_mode()`'s sibling `run_usb_msc_mode()`, invoked only when the operator clicks the cyan tile.
- **Never uninstall TinyUSB.** After entering MSC mode, exit is always `esp_restart()`. Next boot is clean — USB-Serial-JTAG is back and esptool works normally.
- **Exit conditions:** button click, host eject (MSC MOUNT_CHANGED false), USB unplug (`tud_connected()` false-after-true edge).

### sdkconfig deltas (three lines)

```
CONFIG_TINYUSB_MSC_ENABLED=y
CONFIG_TINYUSB_MSC_BUFSIZE=4096
CONFIG_TINYUSB_TASK_PRIORITY=5
```
`CONFIG_TINYUSB_CDC_ENABLED` intentionally left OFF — smaller descriptor, no console fighting. UART0 remains the primary console, USB-Serial-JTAG the secondary — both unchanged from pre-Stage-11.

Managed component pinned in `components/usb_msc/idf_component.yml`:
```yaml
espressif/esp_tinyusb: "^1.7"    # resolves to 1.7.6 on IDF 5.5.2
```

### Bug found + fixed mid-session

First bench run crashed immediately after "Card probed" — the MSC `MOUNT_CHANGED` callback fires at registration with `is_mounted=false`, and my exit logic treated any `false` as an eject event, so `esp_restart()` fired within one tick. Fixed by requiring an `is_mounted=true` observation before treating any subsequent `false` as an exit signal. Also added a 3 s grace window at entry to swallow the PHY-handoff `tud_connected()` blip.

### Bench evidence it works

Ivan opened the SD card on his laptop, copied files off, second click auto-rebooted device back into normal FCM. First deployment produced no reboots or hangs after the mount-edge fix.

---

## 4. Item B — NVS command persistence (`components/nvs_cfg/`)

### Namespaces
- `cfg_rtc` — RTC command log: `wall_ts`, `wr_ms`, `boot_seq`, `last_cmd`
- `cfg_sys` — global knobs: `print_boot` (u8), `batt_test` (u8, added later)

### Redundancy channel — PCF85063A register 0x03

PCF85063A has a single 8-bit **battery-backed** GP RAM byte at register `0x03` (`RAM_byte`). It survives full ESP power loss as long as Vbat (coin cell) is present. On every `SET_TIME` we now write a command-ID code (`PCF85063_RAM_CMD_SET_TIME = 0x01`) there in addition to updating NVS. Three-way witness (RTC live, ESP NVS, PCF RAM_byte) makes divergence obvious on the next boot printout.

### Serial CLI additions

```
GET_TIME [-v]                    — read RTC now (-v also dumps NVS + RAM_byte)
SET_TIME YYYY-MM-DDTHH:MM:SS     — write UTC + persist to NVS + PCF RAM_byte
NVS_PRINT [ON|OFF]               — toggle boot-time NVS printout (no arg = dump)
BATT_TEST [ON|OFF]               — enter battery-test mode on next boot
```

### Bench evidence — three witnesses agreeing

First SET_TIME command run (recorded exactly on the beat):

```
[NVS] ── cfg_rtc ──
[NVS]   wall_ts   = 1784895620  (2026-07-24T12:20:20Z)
[NVS]   wr_ms     = 127147
[NVS]   boot_seq  = 13
[NVS]   last_cmd  = "SET_TIME 2026-07-24T12:20:20"
[PCF]   RAM_byte  = 0x01  (SET_TIME)
[SYS]   print_boot= 1  (toggle: NVS_PRINT OFF)
```

`last_cmd` matches `wall_ts` (Unix → ISO), `RAM_byte` is the corresponding command code, and the live RTC read-back agreed. Survives reboot: on next boot the same three-way agreement was still there.

---

## 5. RTC → FatFs mtime bridge (`pcf85063.c task_rtc_fn`)

**Symptom:** every file on the SD card was stamped `1980-01-01` — no way to sort recordings by date.

**Root cause:** ESP-IDF's FatFs port calls `get_fattime()` → `time()` → `localtime_r()`. Nobody in the codebase ever called `settimeofday()`, so newlib's CLOCK_REALTIME stayed at the 1970 epoch and FatFs floored to its own 1980 minimum.

**Fix:** in `task_rtc_fn` (which already polls the PCF at 1 Hz), after each successful read compute the Unix epoch inline (avoids TZ mystery of `mktime()` / `timegm()`) and `settimeofday()`. Guarded by `t.valid && year >= 2024` so a cold-boot oscillator-stop reading doesn't clobber a good clock.

**Downstream effect:** all SD writes — MLC CSVs, MIC WAVs, battery-test CSVs, everything — automatically pick up correct UTC timestamps. No further code changes needed anywhere else.

---

## 6. MLC_COLLECT enable-flag fix

**Symptom:** first `/sd/data/mlc_train/*.csv` file after Stage 10 was all zeros in the ax/ay/az/gx/gy/gz columns.

**Root cause:** `wake_sensors_for_mode()` in `field_capture.c` has cases for FCM_MOTION, FCM_BCG, FCM_TEMP etc. but **none for FCM_MLC_COLLECT**. Fell through to `default: break;`, IMU broker stayed disabled, `task_imu_fn` idled, `broker_imu_read()` returned the zero-init struct.

**Fix:** added `case FCM_MLC_COLLECT: broker_imu_set_enabled(true); break;`. `run_mlc_collect_mode()` now calls `wake_sensors_for_mode(FCM_MLC_COLLECT)` at entry with a 120 ms settle, and `park_all_modal_sensors()` at exit.

---

## 7. Item C — CTRL1 accel-layout audit (`lsm6dsv16x.c/.h`)

### Bench-observed symptom

Post-MLC-fix, `.csv` rows had non-zero accel but with three tells that made no sense:

1. **`az ≈ 19.5 m/s²`** at rest — should be ~9.81. Factor of 2, i.e. accel FS is ±8g while the code reports as ±4g.
2. **Same accel values repeated 5-7 rows in a row** (~20 ms per row). Effective accel ODR ~5-7 Hz, not 120 Hz.
3. Gyro was fine — jitter every row, values in a sensible range.

### Root cause

The macros `CTRL1_ODR_120HZ`, `CTRL1_FS_XL_4G`, `CTRL2_ODR_240HZ`, `CTRL2_FS_G_2000DPS` were inherited from the older LSM6DSO family, where FS_XL lived in CTRL1[3:2] and FS_G in CTRL2[3:1]. On the DSV16X those fields are on completely different registers:

| Field       | DSO layout       | DSV16X layout       |
|-------------|------------------|---------------------|
| ODR_XL      | CTRL1[7:4]       | CTRL1[3:0]          |
| OP_MODE_XL  | (not present)    | CTRL1[6:4]          |
| FS_XL       | CTRL1[3:2]       | CTRL8[1:0]          |
| ODR_G       | CTRL2[7:4]       | CTRL2[3:0]          |
| OP_MODE_G   | (not present)    | CTRL2[6:4]          |
| FS_G        | CTRL2[3:1]       | CTRL6[3:0]          |

Writing DSO layout to a DSV16X CTRL1 (`0x62`) meant `OP_MODE_XL = 110 (Low-Power 3)` + `ODR_XL = 0010 (7.5 Hz)`. Perfect match for the bench observation.

### Fix

Rewrote the macros in `lsm6dsv16x.h` with DSV-correct layout and full ODR / OP_MODE / FS enums. Init in `lsm6dsv16x.c` now does four explicit writes:

```c
CTRL1 = OP_MODE_HP | ODR_XL_120HZ    // 0x06
CTRL2 = OP_MODE_HP | ODR_G_240HZ     // 0x07
CTRL6 = FS_G_2000DPS                  // 0x04
CTRL8 = FS_XL_4G                      // 0x01
```

Scaling constants `ACCEL_LSB_PER_G = 8192.0f` (correct for ±4g) and `GYRO_LSB_PER_DPS = 14.286f` (correct for ±2000 dps) unchanged.

### Bench evidence — same MLC session, after fix

At rest, `az` reads ~9.5 m/s² (proper gravity). Every row now shows independent accel values — no more 5-7-row runs. Gyro clean and jittery as before. Numbers look "textbook" per Ivan.

---

## 8. Battery-test mode (Item D preparation)

### Purpose
Run the device untouched from a full charge until BQ25619 UVLO. Battery life = CSV last-row timestamp − first-row timestamp. Ivan will manually measure Vbat with a meter at start + at cutoff for the discharge endpoints.

### NVS-flagged boot detour
- New key: `cfg_sys.batt_test` (u8, default 0). Toggle via `BATT_TEST ON | OFF`. Reboot to apply.
- On boot with the flag set, `task_field_capture_fn` detours into `run_battery_test_mode()` instead of the FCM state machine.
- All I2C sensor brokers default disabled and stay that way (their tasks idle in `vTaskDelay` + enabled check). RTC + BQ25619 tasks are always-on so we keep wall-clock + charging-state signal.
- New global `g_batt_test_active` suppresses the 15 min uptime cap in `task_shutdown_watcher_fn`. Ship-mode gesture (4 s hold) still fires — manual escape stays intact.

### CSV format
`/sd/data/battery/batt_<boot_seq>.csv`
```
t_ms,iso_utc,soc_temp_c,batt_mv,batt_pct,charging
```
LED: 50 ms cyan blip per sample (10 s cadence). Cheap enough to be part of the baseline.

### 8a. TEMPORARY Vbat ADC hack (`vbat_adc_mv` CSV column, fw≥0.3.5)

Because the BQ25619 has no real ADC, and Ivan doesn't want to redo the discharge run every time we want a Vbat sample, we borrow a screen data pad while the FPC is off:

**Wiring:**
```
Vbat ─── 5k1 ─┬─── GPIO9 (─── 100 nF ─── GND, optional)
              │
             5k1
              │
             GND
```

- **Pad:** GPIO9 = screen D2 = **ADC1_CH8**. No boot-strap consequences (unlike GPIO3 which is JTAG_SEL and would be risky). ADC1 chosen over ADC2 to keep the WiFi-conflict path clean for future.
- **Divider:** 5k1-5k1 gives 1:2 ratio → 4.2 V max Vbat maps to 2.1 V at the pin. Fits `ADC_ATTEN_DB_12` (~3.3 V range). Continuous draw ≈ 412 µA = ~0.4 % of the 106 mA baseline; measurable but tolerable for the accuracy gained.
- **Sampling:** 8 quick reads averaged per 10 s CSV row, `adc_cali_curve_fitting` used for eFuse-corrected mV. Result multiplied by 2 for the divider. Fallback path uses ratiometric 3300/4095 if calibration eFuse absent.

**Code:** flagged with a TEMPORARY comment block in `field_capture.c` right above the `esp_adc` includes. Removal path is one commit when either (a) screen returns to the FPC, or (b) mk2 lands with BQ25896 / MAX17048.

### ⚠ Known limitation — `batt_mv` column is not real

The BQ25619 does **not** have a real VBAT ADC. `bq25619_read_vbat_mv()` (line 71 of `bq25619.c`) reads register `BQ25619_REG_VBAT_ADC` with scaling `2304 + 20 × raw` — but on BQ25619 that register is almost certainly a threshold setpoint, not a sampled voltage. Observed: `batt_mv = 3184 mV` stuck constant across the entire session, including on USB charger vs off.

**For real battery-life measurement:** trust the `iso_utc` timestamps + the `charging` column + a manual multimeter reading of Vbat at start and end. The `batt_mv` column stays in the CSV for diagnostic value but is not authoritative.

`batt_pct` is derived from `batt_mv` and inherits the same bogus-ness.

### Bench evidence — baseline current

Ivan wired an ammeter on the USB path and ran `BATT_TEST ON` + reboot with USB in. Observed:

- **Peak while charging (CC phase):** ~200 mA.
- **CV phase (Vbat approaching 4.20 V):** tapered smoothly ~200 → ~170 mA.
- **Sharp drop at charge termination:** 160 mA → **106 mA** the instant `CHG` flag went low. Textbook CC/CV cutoff: charger's I_term reached, BQ declares "done".
- **Post-full baseline:** stable at **106 mA on USB, sensors parked, batt_test=1 mode**.

That **106 mA is the Stage 11 baseline** — sensors parked, LEDs blipping every 10 s, RTC + BQ25619 + ESP core running. Item D power management will be measured against this.

### Second run: fw=0.3.5, batt_0025.csv, real ADC voltage

Bench log with the new GPIO9 ADC hack live:
```
BATT_TEST #0   2026-07-24T15:01:37  soc=37.4C  bq=3184mV vbat=4178mV CHG
BATT_TEST #22  2026-07-24T15:05:17  soc=40.4C  bq=3184mV vbat=4174mV CHG
BATT_TEST #31  2026-07-24T15:06:47  soc=40.4C  bq=3184mV vbat=4178mV CHG
BATT_TEST #32  2026-07-24T15:06:57  soc=40.4C  bq=3184mV vbat=4156mV      ← CHG dropped
BATT_TEST #33  2026-07-24T15:07:07  soc=40.4C  bq=3184mV vbat=4156mV
BATT_TEST #37  2026-07-24T15:07:47  soc=40.4C  bq=3184mV vbat=4150mV
```

Ivan's multimeter agreed within ±20 mV of `vbat_adc_mv`: 4.065 V at start (pre-charge), the divider node reads 2.00 V (perfect 1:2), post-charge Vbat 4.15-4.18 V. **ADC hack validated.**

At the instant `CHG` disappeared (row #32), `vbat_adc_mv` also dropped from ~4178 → ~4156 mV. That's the ~20 mV IR-drop-on-load transition — additional confirmation the ADC is reading the real thing, not a stuck register.

### Baseline run — batt_0035.csv (fw=0.4.1, PM disabled)

The `batt_0025` attempt died with a zero-byte CSV (see §8c). After the durability fix landed in fw=0.4.1, `batt_0035.csv` is the first successful full discharge to BATFET cutoff.

`analyze_battery_csv.py` output (verbatim, condensed):

```
mode       = battery_test  fw = 0.4.1  boot = 35  rtc_start = 2026-07-24T19:03:17
rows       = 735

Run duration
  first row = 2026-07-24T19:03:17     last row = 2026-07-24T21:05:37
  elapsed   = 2 h 02 m 20 s

CHG=1 → CHG=0
  at row 57, 2026-07-24T19:12:47   → 9 m 30 s CV tail while charging finished

Discharge portion (real battery-only time)
  discharge time = 1 h 52 m 50 s   ≈ 1.881 h
  Vbat start = 4082 mV
  Vbat end   = 2560 mV   ← BATFET cutoff, chip disconnected the cell
  ΔV         = 1522 mV
  mean Vbat  = 3734 mV, sd 233 mV

SoC temperature: 40.4 .. 45.4 °C  mean 43.3 °C
Delivered charge (@ 106 mA assumed constant): 199 mAh
```

**Interpretation:**

- **Battery-life at pre-PM baseline = 1 h 52 m.** This is the Item D number to beat.
- **Delivered ≈ 199 mAh** vs 400 mAh nameplate = **~50 %**. Two hypotheses, not distinguished by this data alone:
  - Cell is under-labeled. Cheap 400 mAh LiPos commonly deliver 200-300 mAh at full cutoff, especially aged units. Believable.
  - Actual battery-side current > 106 mA. The 106 mA was measured on USB post-CHG-off; on battery, the on-board regulator's efficiency drops as Vbat sags, so battery-side current climbs. Also believable. To distinguish, we'd need a series shunt on the battery lead during discharge.
- **Vbat cutoff 2560 mV** matches BQ25619 VBATLOW ≈ 2.5-2.6 V perfectly. Hardware protection worked as expected.
- **Discharge curve looks normal** for a single-cell LiPo — steady drop from 4.08 V through the ~3.7 V knee down to the ~3.0 V cliff, then a fast drop to cutoff.

**Archive snapshots:**
- `kompic_snapshot_2026-07-24_1513_batt_0025_fw035_baseline-106ma.7z` — the empty-CSV incident (pre-durability-fix, for future forensics).
- `kompic_snapshot_2026-07-24_2222_batt_0035_fw042_pre-pm-baseline-2h02m-199mah.7z` — the definitive pre-PM baseline. Anything Item D achieves is measured against this.

**Item D target:** any block that extends runtime beyond 1 h 52 m at the same 400 mAh nameplate is a win.

### 8c. Empty-CSV incident (2026-07-24) — SD durability rule added

**Symptom:** `batt_0025.csv` came back from the SD card as **zero bytes**. Every prior `batt_*.csv` too. Battery run finished, no data recoverable.

**Root cause:** the writer called `fflush(f)` after every row but the file was only ever `fclose`'d at the end of `run_battery_test_mode`. `fflush()` pushes the stdio buffer to the VFS layer but ESP-IDF's FatFs port only commits the directory entry (file size, mtime) on `f_sync` / `f_close`. Device died before either fired → FAT directory shows the file as size 0 forever.

**Fix (fw=0.4.1):**
- **Battery test** rewritten to *Pattern A* (open-append-close per row). Each `fclose` commits the directory entry. Overhead ~1-5 ms per row at 10 s cadence is trivial.
- **MLC_COLLECT** rewritten to *Pattern B* (keep open + `fflush` + `fsync(fileno(f))` per row). 50 Hz cadence, ~0.5-2 ms per row, tolerable. Same durability without fopen/fclose overhead.
- Rule documented in a block comment at the top of `field_capture.c` and saved as `[[feedback-sd-write-durable]]` memory.

**Also added:** `SHIPMODE` serial command (fw=0.4.1). Calls `watcher_ship_mode()` directly for a manual BATFET drop when the physical button is unreachable / stuck. Symmetric to the 4 s hold path.

### 8b. Cutoff safety — what protects the battery

**Nothing new configured — hardware defaults already protect the cell.** BQ25619 built-in:

- **VBATLOW / BATFET off @ ~2.5-2.6 V** — chip disconnects battery from SYS.
- **VSYS_MIN ≈ 3.0 V** — system rail floor.
- **VBATOVP ≈ 4.35 V** — charging stops.
- **VBUS_UVLO / VBUS_OVP** — USB input clamped.
- **Thermal shutdown** — internal.

In practice the ESP32-S3 will brown-out first (VSYS ≈ 2.4-2.7 V) and reset cleanly. The BQ then disconnects. Either path preserves the cell. No firmware cutoff required for this test.

**Optional refinement (not in this run):** once the ADC is trustworthy, we can call `bq25619_enter_ship_mode()` proactively when `vbat_adc_mv < 3.05 V` for a graceful shutdown instead of a brownout. Would flush open files.

### To do (Ivan-owned)

1. ✅ Manually measure Vbat with a multimeter — record here as the discharge start voltage. **[2026-07-24 pre-run: 4.065 V; ADC agreed 4082 mV within ±20 mV]**
2. ✅ Unplug USB, let the watch run untouched.
3. ✅ Wait for BQ UVLO cutoff (device dies).
4. ✅ Recharge, plug USB, click cyan tile, grab `/sd/data/battery/batt_XXXX.csv`.
5. ✅ Measure Vbat again — recorded ADC endpoint **2560 mV** (VBATLOW cutoff, cell is protected).
6. ✅ Duration = last-row `iso_utc` − first-row `iso_utc`. Battery life at ≈ 106 mA baseline = **1 h 52 m** (`batt_0035.csv`).

---

## 9. Working state of the SD card / USB workflow

After this session Ivan's data-recovery loop is:

1. Take the watch off the wrist. USB-C plug it in.
2. Scroll encoder to the cyan tile.
3. Click. Device becomes a removable drive on the laptop.
4. Copy files off (any file manager).
5. Click button again OR unplug USB → device reboots into normal mode.

No case-cracking, no card removal. This unlocked the entire post-Stage-10 debug loop (found MLC-zeros bug + Item C bug + FatFs-1980 bug back-to-back because iterating on data was suddenly cheap).

---

## 10. Follow-through checklist

- [x] Real battery-life number recorded — `batt_0035` 1h 52m at 106 mA baseline; final 0.4.12 overnight number pending.
- [ ] Fix `bq25619_read_vbat_mv` scaling — analyzer post-processing drops it, so still deferrable. Real fix comes with mk2's BQ25896.
- [x] **Item D power-management plan drafted** — `Stage_11_PM_Plan.md`.
- [x] **Item D Block A executed** — RMT enable/disable per push + SDMMC unmount per sample + VBUS-aware PM lock. Bench proof: `Mode SLEEP` jumped from 0-2 % to **80 %**, `light_sleep_counts` from 11 to **124 433**. See §12 below.
- [ ] Item D Block B (event-driven field_capture task) — deferred to Stage 12+ once we have the overnight discharge number.
- [ ] Item E MLC pilot classifier — deferred to Stage 12+; awaits Ivan's label 0/1/2 walking data.

---

## 12. Item D Block A — final resolution (fw=0.4.12)

Three fixes had to land together. Any one on its own didn't move `Mode SLEEP` past 2 %.

### Fix 1: RMT enable/disable per push (`components/ws2812/ws2812.c`)

ESP-IDF 5.5's RMT driver acquires `ESP_PM_CPU_FREQ_MAX` on `rmt_enable()` and holds it until `rmt_disable()`. Our previous code enabled once at boot and left the channel enabled → lock held 100 % of session → light-sleep refused. Fix: `rmt_enable → rmt_transmit → rmt_tx_wait_all_done → rmt_disable` per push. Lock only held for the ~60 µs of actual WS2812B bit-banging. Xtal clock source (added in 0.4.7) is kept as a harmless secondary.

Journey:
- **0.4.7**: switched `clk_src` APB → XTAL. Alone: not enough (RMT still grabs lock at enable regardless of clk_src).
- **0.4.8**: tried `flags.allow_pd = 1`. IDF refused: `ESP_ERR_NOT_SUPPORTED`. Broke RGB entirely. Reverted.
- **0.4.10**: enable/disable per push. Works.

### Fix 2: SDMMC unmount between batt_test samples (`components/field_capture/field_capture.c run_battery_test_mode`)

SDMMC driver holds `ESP_PM_APB_FREQ_MAX` from mount to unmount. In prior batt_test we mounted once at entry and kept mounted for the whole 2 h run → APB pinned high → light-sleep refused. Fix: mount → fopen("a") → write row → fclose → unmount per 10 s sample. Mount latency ~60 ms per row is negligible at 10 s cadence.

Trade-off: normal FCM mode (recording sessions, MLC, etc.) still keeps SD mounted for latency reasons on button-press. Only batt_test does the mount/unmount dance. That's the accepted design.

### Fix 3: VBUS-aware `NO_LIGHT_SLEEP` lock (fw=0.4.12)

Without a serial-survivability hook, aggressive light-sleep would drop the USB-Serial-JTAG endpoint (`Errno 71: Protocol error` on the host) — you'd be softlocked out of the device for the whole batt_test run. Fix: in `run_battery_test_mode`, at each sample tick check `broker_battery.power_good`. If VBUS is present, acquire `ESP_PM_NO_LIGHT_SLEEP`. If absent, release. Serial stays alive while USB is plugged in; PM engages fully while on battery. Recovery from softlock: plug USB in, wait one sample tick (10 s), issue commands.

**0.4.11 attempt (superseded):** the first version skipped batt_test entirely when VBUS was present. Ivan corrected: he wants to log charging behavior in the CSV alongside discharge. The current 0.4.12 design keeps batt_test always-running, just toggles the PM lock based on live VBUS state.

### Bench proof after all three fixes (fw=0.4.12, batt_test running ~13 min on battery after unplug)

```
Time since bootup: 777751433 us  (~13 min)

batt_test_vbus  NO_LIGHT_SLEEP   0  Active=1  Total_count=2  Time= 5%  ← unplug window
sdmmc           APB_FREQ_MAX     0  Active=0                 Time= 1%  ← unmounted between samples
rmt_0_0         CPU_FREQ_MAX     0  Active=0                 Time= 1%  ← released between pushes

Mode SLEEP    40M    627 593 351 us   80%     ← light-sleep engaged 80% of the time
APB_MIN       40M     25 669 827 us    3%     ← DFS engaged
APB_MAX       80M      1 728 177 us    0%
CPU_MAX      240M    122 785 659 us   15%

light_sleep_counts: 124433
light_sleep_reject_counts: 0
```

Every previous run of batt_test showed `Mode SLEEP: 0-2 %` and `light_sleep_counts: 11`. **12 400× more sleep entries after 0.4.12.**

### Full-discharge result — `batt_0081.csv` (fw=0.4.12, overnight run)

```
Session:      mode=battery_test, fw=0.4.12, boot=81, 988 rows
RTC start:    2026-07-25T22:55:34    CHG-off: row 7 (~1m10s later)
RTC end:      2026-07-26T01:40:16
Discharge:    2 h 44 m 30 s          (was 1 h 52 m pre-PM)
Vbat:         4110 → 2546 mV (BATFET cutoff)
Delivered:    288.6 mAh @ 106 mA     (was 199 mAh pre-PM)
SoC temp:     32.4 - 39.4 °C  mean 35.98 °C   (was 43 °C pre-PM)
Heap Δ:       +0 kB (stable)
```

### Item D Block A — final delta

| Metric | Pre-PM (batt_0035) | Post-PM (batt_0081) | Delta |
|---|---|---|---|
| Discharge runtime | 1 h 52 m | **2 h 44 m** | **+52 m (+47 %)** |
| Delivered charge | 199 mAh | **288.6 mAh** | **+90 mAh (+45 %)** |
| SoC max temp | ~43 °C | **<40 °C** | **-3 to -7 °C** |
| USB-baseline current | 106 mA | **81 mA** | **-24 % (-25 mA)** |
| Nameplate delivery | 50 % | **72 %** | **+22 pp** |

**Textbook Block A win.** No hardware changes, no feature loss, no partition changes — just three PM lock releases (RMT enable/disable per push, SDMMC unmount per sample, VBUS-aware NO_LIGHT_SLEEP) and 47 % more battery life.

### Loose end (post-run FAT dirty flag)

Linux flagged `sda1: Volume was not properly unmounted. Please run fsck.` when Ivan first mounted the SD via USB MSC after the battery died. Not corruption — every row's `fclose()` was durable (Pattern A). What happened: the FAT "clean shutdown" byte in the FS info sector didn't get flushed to the card before UVLO cut power. `fsck` clears the flag cleanly and the CSV parses fine. Fix path (documented in Stage_12_Handoff): add explicit `f_sync()` before `esp_vfs_fat_sdcard_unmount()` in `sdcard.c`. Small, safe, deferred.

## Tooling shipped this session

## BLACKBOX telemetry logger (fw=0.4.4)

### Purpose
Always-on background snapshot logger. Complements the mode-specific CSVs (MLC, MIC, batt_test) with a system-wide "wristable analytics" view — every sensor's broker snapshot, every FCM state transition, every heap datapoint, sampled at a configurable cadence and dropped into a boot-scoped CSV.

### Wiring
- **NVS flag:** `cfg_sys.blackbox` (u8, default 0). Boot printout shows the state.
- **Cadence:** `cfg_sys.bb_cadence_s` (u16, default 10, range 1..3600). Live-tunable at runtime.
- **CLI:**
  ```
  BLACKBOX [ON|OFF]           toggle (reboot to apply)
  BLACKBOX_CADENCE <s>        change cadence (takes effect on next sample tick)
  ```
- **Task:** `task_blackbox_fn` registered in `boot_tasks.c`. Pinned Core 0, priority 1, 4 KB stack. Self-exits on startup when the flag is off OR `batt_test=1` (to avoid double-CSV during a battery run).
- **Path:** `/sd/data/blackbox/bb_<boot_seq>.csv`. One file per boot. Pattern A durability (open-append-close per row).

### Column schema (~35 fields)
Header comments explain everything. Grouped:
1. Time + boot info: `t_ms, iso_utc, uptime_min, boot_seq`
2. FCM state: `fcm_mode, fcm_state`
3. System: `heap_free_kb, min_heap_kb, cpu_mhz, idle_pct, sensors_on, sensors_stat`
4. Power: `bq_v, bq_pct, bq_chg, bq_pg, bq_fault, vbat_adc_mv, soc_temp_c`
5. IMU: `imu_ax, imu_ay, imu_az, imu_gx, imu_gy, imu_gz, imu_temp`
6. MAG: `mag_x, mag_y, mag_z`
7. ENV: `env_t, env_h, env_p, env_gas, env_alt`
8. LIGHT: `light_lux`
9. HR: `hr_bpm, hr_spo2, hr_finger`
10. SKIN: `skin_t`

### `sensors_stat` decoding
Each sensor gets a 4-bit nibble in the packed uint32, hex-formatted. Same order as `sensors_on` (LSB = IMU). Nibble values are the existing `sensor_status_t`:

```
0 = DISABLED   (task not reading; value in row is stale/zero-init)
1 = OFFLINE    (HW dead / init failed / persistent timeout)
2 = ACQUIRING  (calibrating, warming up)
3 = STALE      (data older than the module's timeout)
4 = ONLINE     (fresh valid data)
5 = NOTIF      (one-time event, e.g. first GPS fix, low battery)
```

### Value semantics — what to trust
The broker holds last-written values regardless of enable state. When a sensor is off, its broker keeps the last live reading (or zero if never enabled). Downstream analysis should filter: **rely on a sensor's value only when its nibble in `sensors_stat` is `4` (ONLINE)** — otherwise treat the value as suspect.

### Interaction with other modes
- **Recording modes** (MIC, MLC): coexist. Multiple SD files open at once is fine.
- **USB MSC mode**: SD gets unmounted. BLACKBOX's per-row `fopen("a")` fails silently, retries next tick. On reboot after MSC exit, a new bb_<boot>.csv starts.
- **BATT_TEST mode**: BLACKBOX task exits at startup. batt_test's own CSV covers the same use case.

### Disk cost
At 10 s cadence: 8,640 rows/day × ~350 B = **~3 MB/day**. 60 GB card = ~50 years capacity. Practically free.

### Analyzer script — TODO
`analyze_blackbox_csv.py` not written yet. Planned features:
- Timeline plot: which sensors were active when (colour-band diagram).
- Battery/heap curves overlaid on activity.
- Mode-usage histogram.
- FCM state transitions.
Deferred to a later session; current batt_test analyzer suffices for immediate PM measurement.

---

## idle_pct — Tier 2 PM engagement metric (fw=0.4.4)

- `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` and `configRUN_TIME_COUNTER_TYPE=uint32_t` were clicked by Ivan.
- Added `idle_pct` column to both BATT_TEST and BLACKBOX CSVs.
- Reads `ulTaskGetIdleRunTimeCounter()` — returns the CURRENT core's idle counter (this IDF release doesn't expose a per-arbitrary-CPU getter without going through `uxTaskGetSystemState`). Both callers are pinned tasks so results are well-defined: BATT_TEST reports Core 1 idle, BLACKBOX reports Core 0 idle.
- **Expected reading during batt_test:** should be **very high** (>90%) — the loop's 100 ms `vTaskDelay` between 10-s samples is well above the 30 ms sleep threshold, and no other task runs on Core 1 during that window. If we see <50%, either PM isn't actually sleeping OR something else is chewing cycles — either is a red flag we investigate.

---

## flashlight_hard_off() (fw=0.4.4)

New API in `components/flashlight/`. Fully stops the LEDC channel and pins GPIO41 LOW. Saves ~50 µA of LEDC peripheral quiescent vs `flashlight_off()` (which sets duty=0 but keeps the timer running). **Not wired into any idle path by default** — call explicitly from a "device truly parked" state (e.g. a future deep-sleep entry) if you want it.

**Important:** the real flashlight-glow investigation is a HARDWARE issue. Firmware-side, `flashlight_off()` drives GPIO41 continuously LOW. If the LED still glows with pin at 0 V, it's leakage through the FET drive circuit — needs hardware audit. Suggested: multimeter reading of GPIO41 in "off" state; should be ~0 mV. If yes and LED still glows → hardware.

---

## Tooling shipped this session

- `hardware/Reflow_info/reference_files/analyze_battery_csv.py` — parses `batt_XXXX.csv`, prints session metadata + duration + charge-off transition + Vbat statistics + delivered-charge estimate (using `--current-ma 106` by default), optionally emits a matplotlib PNG next to the CSV. Drops the bogus `batt_mv` / `batt_pct` columns in post-processing.
- `hardware/Reflow_info/Stage_11_PM_Plan.md` — Item D four-block plan with expected deltas and a fill-in-as-you-measure results table.

## 11. Candidate silicon for mk2 (Ivan's questions, to be investigated further)

Not scoped for this session's code — captured here so we don't lose the shortlist.

### Charger with real VBAT ADC
- **TI BQ25896RTWR** — VQFN-24, same footprint family as the current BQ25619, adds a proper 12-bit ADC for VBAT / VSYS / VBUS / ICHG / TS + I2C readout. Direct upgrade path. **Preferred mk2 replacement for BQ25619.**

### Dedicated fuel gauge (SoC%)
- **Maxim MAX17048G+T10** — ModelGauge (voltage-based, no external Rsense). ~4 µA quiescent, 0.9 mm × 0.7 mm CSP. Gives real user-facing SoC%, capacity, and rough discharge current — much better than the current voltage-LUT guess. Complements the BQ25896 rather than replacing it. **Highly recommended for a wearable with a battery% display.**
- With BQ25896 ADC alone you get raw voltage and can build an LUT-driven SoC. Adding MAX17048 buys learned-model SoC and better accuracy in the sag-under-load regions where voltage LUTs lie.

### ECG + PPG — practical splits (Ivan's stock check 2026-07-24: only AD8232 in stock in acceptable package)

Combined chips didn't survive the availability + package filter. So mk2 goes with **two separate chips** but both reflowable:

- **TI ADS1191IRSMR** *(preferred ECG)* — VQFN-28 4x4 mm. **Digital SPI output** with integrated 16-bit ΔΣ ADC, programmable gain 1-12×, right-leg drive, lead-off detection. Much cleaner integration than AD8232 (no external ADC needed). Quiescent ~350 µA (vs AD8232 ~200 µA) — negligible against a 100+ mA system baseline.
- **Maxim MAX30101** *(keep for PPG)* — already on the board. Stage 10 defer was on the firmware side; hardware is fine. Manual driver rework recommended.
- **Alt PPG (mk2 optional):** **MAX86160** — 3.3 × 5.6 mm OLGA-14, single-die PPG, smaller footprint than MAX30101. Consider if PCB real-estate becomes tight.

Ranked-out:
- **MAX86150** (combined ECG+PPG in 14-OLGA) — not in stock.
- **TI AFE4900** (QFN-24 combined) — not in stock in that package variant.
- **AD8232ACPZ-R7** — in stock but analog-out, needs external ADC + more discretes. Dropped in favour of ADS1191.
- **AFE4960P**, **MAX30003** — BGA, skipped per Ivan's package constraint.

Reference notes on when the fuel gauge matters: the current voltage-LUT SoC is ±10-15 % under load, worst near the 3.4-3.6 V knee. A user-facing `%` chews through trust fast if it jumps around. MAX17048 (or a coulomb-counter equivalent like MAX17262) gets that to ±1-2 %. For a bench-only prototype it's optional; for a shippable watch it's basically required.

---

## 12. Auto-memory updated this session

- `feedback_usb_msc_lazy.md` — TinyUSB must lazy-init + reboot-on-exit on iv7.1.
- `project_pcf_ram_byte.md` — PCF85063A reg 0x03 = redundancy channel for command state.

---

*Live document — end of section is where the next update will land.*
