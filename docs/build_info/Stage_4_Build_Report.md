# Stage 4 Build Report — Kompic Mk I (iv7.1)

**Date:** 2026-07-05
**Board:** First Kompic Mk I prototype, PCB iv7.1 (same physical board as Stages 1–3)
**Builder:** Ivan
**Stage:** 4 of 5 (SD subsystem verification + diagnostic corrections)
**Prerequisite:** Stage 3 closed 2026-06-20 with 4/5 critical bugs resolved on the bench; 2026-06-29 desk session added the DRV chip damage conclusion. Stage 4 was originally scoped as "SD rescue + parallel rework"; the actual outcome was radically different — see § 1.

---

## 1. Executive summary

**The SD subsystem on iv7.1 is fully functional as designed.**

Testing with a 32 GB SDHC msdos card and a 128 MB SDSC msdos card at 400 kHz, 1 MHz, 4 MHz, and 20 MHz (SDMMC 1-bit) produced **65 / 65 PASSes** with the original iv7.1 wiring — no 1 µF decoupling cap, no GND bodge wire, no series-R on CLK. The bus is robust enough to tolerate 3 cm stub wires on CLK / CMD / DAT0 without regression.

The failures documented in `Stage_3_Build_Report.md § 4.1` and `SDMMC_Rescue_Plan_iv7.1.md` were dominated by **a dying 64 GB Kodak AliExpress SDXC card** (also formatted exFAT, unsupported by arduino-esp32 SD_MMC by default). No signal-integrity intervention was needed. The rescue plan is retracted — the whole EXP-0 through EXP-5 experiment ladder was unnecessary. See § 4 for the full diagnostic autopsy.

Concretely, Stage 4 today accomplished:

- ✓ SDMMC 1-bit verified at 20 MHz (full high-speed) with original iv7.1 wiring
- ✓ Filesystem rule confirmed: FAT16 / FAT32 only, no exFAT (unless ESP-IDF rebuilt with exFAT support)
- ✓ **New discovery: USB-C plug orientation matters on iv7.1** — only one D+/D- pair is wired; one plug orientation enumerates, the other fails with `error -71` in dmesg
- ✓ Second Kodak 64 GB card recovered by reformatting FAT32 — the previous "dead" 64 GB card really was dead, but the failure mode was card-side, not board-side
- ✓ Bench decoupling / GND / pull-up experiments (1 µF cap at socket, temporary GND bypass wire) applied and then reverted — the bus works either way; caps and bodges kept off the board

Also delivered this session:

- ✓ `6_sd_logger_mk1` — single-sensor (LSM6DSV16X IMU) 100 Hz CSV logger with boot-time file review. Proved the write-then-read round-trip.
- ✓ `7_demo_mk1` — multi-sensor bring-up rig with in-line ammeter markers. Ran all five connected sensors (LIS, LSM, BME, MIC, VEML) solo for 10 s each with live serial heartbeat + SD logging. Data landed correctly under `/data/<sensor>/`. Mic WAV playable and audible at gain x8 (+18 dB). Observed value ranges captured in § 7.

Carried forward to Stage 5:

- DRV2605L chip replacement (silicon damaged per 2026-06-29 finding)
- MAX30101 replacement (killed by Bug 3 + Bug 4 in Stage 3)
- TMP117 exercise (on the daughter board, alongside MAX30101)
- R26 restock (lost during Stage 2/3 probing)
- Flashlight LED leakage mitigation (cosmetic Bug 7)
- Set RTC via a small serial command (currently reads `oscstop` — see § 7.3)

---

## 2. Session accomplishments

### 2.1 Diagnostic sketches added

| Sketch | Purpose | Result |
|---|---|---|
| `firmware/test/3c_sd_pin_probe_mk1/` | Replicates ESP-IDF "PIN recovery time" test in Arduino. Drives each SD pin as open-drain, measures cycles to return HIGH via external pull-up. Distinguishes external-pull-up-missing from pin-dead. | All three SD pins (CLK / CMD / DAT0) showed healthy recovery times: ~166 cycles CLK (10 kΩ ext), ~112 cycles CMD / DAT0 (~3 kΩ ext). Pull-up chains intact. |
| `firmware/test/3b_sd_pump_test_mk1/` | Existing Stage 3 retry loop, run today across four clock speeds. | 65/65 PASS with 32 GB card at 400 kHz / 1 MHz / 4 MHz / 20 MHz. |

### 2.2 Meter measurements (EXP-0)

Board unpowered, three continuity checks:

| Check | Reading | Verdict |
|---|---|---|
| SD socket pin 7 (DAT0) ↔ GPIO40 pad | ~2 Ω (meter probe offset) | OK |
| SD socket pin 3 (CMD)  ↔ GPIO39 pad | ~2 Ω | OK |
| SD socket pin 5 (CLK)  ↔ GPIO38 pad | ~2 Ω | OK |
| SD socket pin 6 (Vdd)  ↔ +3V2 rail | ~2 Ω | OK |
| **SD socket pin 4 (Vss) ↔ nearest GND** | **2 – 12 Ω, fluctuating** | Anomalous. Fixed live with a temporary bypass wire; later removed and confirmed bus still works. Likely a probing-artifact false-positive that we mis-read as a real fault (probe pressure was inconsistent on the socket-shell tab). |

### 2.3 Scope observations (2 µs/div, single-channel)

At 400 kHz clock:
- **CLK:** sharp fall (open-drain to GND), fast rise via pull-up, then ringing/tapering on the baseline. Shape consistent with a 10 kΩ pull-up on CLK vs 3 kΩ on CMD/DAT0.
- **CMD:** clean square edges (initial "noise" observation turned out to be wind interference on the probe lead — cleared on re-look).
- **DAT0:** clean square edges when the card is driving.

The "sine-wavy CLK" observation from a previous session was an artifact of scoping at 4 MHz with only 2 µs/div resolution — 8 clock periods per div is too coarse to see edge shape. At 400 kHz the CLK edge quality is clearly fine.

### 2.4 Failure-mode timeline (autopsy)

| Session | Symptom | Root cause (now known) |
|---|---|---|
| Stage 3 2026-06-20 | 3 cards "fail identically" — send_scr / send_op_cond / mount_to_vfs timeouts across pull-up + clock sweep | 64 GB Kodak dying + insufficient runtime on 32 GB / 128 MB cards. The "identically failing" observation was based on early terminating attempts, not full retry runs. |
| 2026-06-29 desk | Deterministic `send_scr (1) returned 0x107` at exactly 643 ms, 0/41 PASSes, all clocks identical | Same Kodak card, now further degraded. Deterministic timing came from the card's internal firmware timing out at the same step every attempt — not from a hardware time-constant on the bus. |
| 2026-07-05 EXP-0 | GND pin 4 fluctuating 2–12 Ω | Meter probe pressure artifact, not a real fault (bus works fine with bypass wire removed). |
| 2026-07-05 after GND bypass added | Failure moved from send_scr 0x107 to `mount_to_vfs 0xffffffff` | Card swap during rework, not the bypass wire. The mount failure was really an exFAT rejection from the 64 GB Kodak. |
| 2026-07-05 with 32 GB / 128 MB msdos cards | 65/65 PASS at all clocks, no bodges | Bus was always fine. |

### 2.5 USB-C orientation bug discovered

Symptom: after removing the 1 µF cap and GND bypass wire to test whether they were necessary, USB failed to enumerate. `dmesg` showed `device descriptor read/64, error -71` — device present at electrical level but descriptor read failing.

Root cause: iv7.1's USB-C connector has only **one** D+/D- pair wired to the ESP32-S3's D+/D- (GPIO19/20). USB-C is a reversible connector *only if* both A-side and B-side D+/D- pairs are wired (either by direct tie or through a CC-controlled mux). Ivan's cable had been in the "working" orientation for the whole session; unplug/replug landed the plug in the other orientation, connecting the ESP to floating pads.

Not a firmware issue, not the ESP32-S3 USB PHY, not the DRV chip damage — just an untested reversibility case.

**iv7.1 workaround:** always plug USB-C the working way (label the cable / connector).
**v8 fix:** wire both D+/D- pairs (tie A6↔B6, A7↔B7) or add a CC-based mux. This is a new CRITICAL item for `Stage_3b_v8_MustFix.md`.

---

## 3. Cross-doc updates arising from Stage 4

Applied 2026-07-05:

- **`SDMMC_Rescue_Plan_iv7.1.md`** — SUPERSEDED banner at top. Body preserved for archaeology. Points to this report.
- **`Stage_3_Build_Report.md` § 4.1** — postscript retracting the "SDMMC unfixable" conclusion. Section body preserved.
- **`Stage_3_Build_Report.md` CRITICAL BUG SUMMARY row 2** — status changed to RESOLVED, points to this report.
- **`Bench_Plan_iv7.1.md` § 1.1** — SUPERSEDED note pointing to this report. Update log § 6 gets a new row.
- **`Stage_3b_v8_MustFix.md` § 3** — SDMMC items 3.1 / 3.2 / 3.4 / 3.5 / 3.6 / 3.7 demoted from CRITICAL to IMPROVEMENT. Section 3.3 (CLK pull-up populated by BOM) stands as CRITICAL. New CRITICAL item added: USB-C both-side D+/D- routing.
- **Auto-memory** — `feedback_suspect_cheap_sd_cards.md` added to encode the lesson.

---

## 4. Lessons

1. **When failures are deterministic across every bus variable, suspect the *component under test* before the bus.** We swept clock (0.4 → 4 MHz), pull-up (10 k → 3 k), drive strength (CAP_2 → CAP_3), card (3 cards), and drove ourselves into a rescue-plan spiral, without adequately rotating the test article. Two known-good cards from a different supplier would have short-circuited the entire arc.

2. **Deterministic timing ≠ hardware time-constant.** The 643 ms failure signature at every clock was interpreted as evidence of a physical fault (only a physical time-constant would be clock-invariant). It was actually the *card's* internal firmware timeout — a completely different mechanism that just happens to have deterministic timing.

3. **arduino-esp32 SD_MMC does not support exFAT by default.** SDXC (>32 GB) ships exFAT. The `mount_to_vfs 0xffffffff` error is filesystem-level, not electrical. Test with FAT-formatted cards first; a `stat` or `hexdump` check on the card's boot sector can confirm filesystem type before any board interaction.

4. **USB-C is not automatically reversible.** It requires either both D+/D- pairs wired or a CC-based mux. Iv7.1 has neither. Test USB with the plug in both orientations early in bring-up; a working single-orientation is a schematic bug for any future revision.

5. **Meter probing on unpowered PCB spring-tab contacts can produce false anomalies.** The 2–12 Ω "GND fluctuation" reading was probe pressure, not a real intermittent — the bypass wire "fix" was a coincidence. Repeat suspect meter readings with fresh probe placement before declaring a fault.

6. **Bench discipline: revert experimental interventions before declaring a fix.** The 1 µF cap + GND bypass appeared to help, but only because we didn't remove them and re-test *without* the good FAT-formatted card also in place. Isolate variables one at a time. (Ivan caught this today and reverted properly.)

---

## 5. Carry-forward for Stage 5

### 5.1 Bench rework still outstanding (unchanged from Stage 3 → 2026-06-29)

- **DRV2605L replacement.** Silicon damaged per Bench_Plan § 1.2 / 2026-06-29 conclusion. LRA is healthy (23.6 Ω), chip trips OC at ~28 mA vs 250 mA spec. Also see USB-C observation: GPIO0 (DRV_EN) reads 2.9 V idle → DRV isn't leaking on the strap, boot path is unaffected by the chip damage.
- **MAX30101 replacement.** Daughter PCB removed for the rework. +1V8 rail confirmed clean at 1.80 V after Stage 3 § 4.9 fix.
- **R26 restock** (47 Ω 0402 flashlight series resistor lost during Stage 2/3 probing).
- **Bug 7** — flashlight LED sub-µA glow when GPIO is LOW (cosmetic).

### 5.2 New Stage 5 items from today

- **SD data-logging sketch (`firmware/test/6_sd_logger_mk1/`)** — implement CSV or JSONL logging of one sensor as the initial bring-up. Read files on boot (`ls` + first-lines of most recent log) to prove the round-trip. Details in § 6.
- **Daughter PCB reattach** — after MAX30101 is replaced.

### 5.3 New v8 items from today

- **CRITICAL: USB-C both-side D+/D- routing** (added to `Stage_3b_v8_MustFix.md` — see cross-doc update above).
- **SDMMC v8 items demoted** — the layout was fine all along. § 3.3 (CLK pull-up populated by BOM) is the only remaining CRITICAL SD item. Everything else is IMPROVEMENT (nice to have for margin, not required for function).

---

## 6. SD data-logging design brief (for Stage 5 sketch)

### 6.1 Theoretical throughput ceiling

SDMMC 1-bit peak line rate = clock rate. Real throughput is ≤60 % of line rate for small random writes (FatFS + block allocation overhead):

| Clock | Line rate | Realistic small-write | Realistic sequential |
|---|---|---|---|
| 400 kHz | 400 kbps = 50 KB/s | ~15 KB/s | ~30 KB/s |
| 1 MHz | 1 Mbps = 125 KB/s | ~40 KB/s | ~80 KB/s |
| 4 MHz | 4 Mbps = 500 KB/s | ~150 KB/s | ~300 KB/s |
| 20 MHz | 20 Mbps = 2.5 MB/s | ~600 KB/s | ~1.5 MB/s |
| 40 MHz (spec max SDR) | 40 Mbps = 5 MB/s | ~1 MB/s | ~3 MB/s |

Sensor bandwidth budget (all-sensors upper bound):

- LSM6DSV16X IMU @ 1 kHz × 6 axes × 2 bytes = 12 KB/s (binary) / ~40 KB/s (CSV)
- LIS3MDL mag @ 1 kHz × 3 axes × 2 bytes = 6 KB/s / ~15 KB/s (CSV)
- BME688 env @ 1 Hz = <100 B/s (negligible)
- VEML6030 ALS @ 10 Hz = <100 B/s (negligible)
- MAX30101 PPG @ 400 Hz × 3 channels × 3 bytes = 3.6 KB/s / ~15 KB/s (CSV)
- TMP117 @ 1 Hz = negligible
- PDM mic @ 16 kHz × 2 bytes = 32 KB/s (raw PCM)

**All-sensors combined ≈ 100–150 KB/s in CSV.** Comfortably inside 4 MHz SDMMC (300 KB/s sequential). No reason to run higher than 4 MHz for logging use.

### 6.2 File format choice

Options considered:

- **CSV per sensor.** Small overhead, one file per sensor type, easy to import to pandas/Excel. Clunky if sensor list evolves.
- **JSONL, one file, tagged records.** `{"t":ms,"src":"imu","ax":...}` — self-describing, mixed sensors coexist, ~2× overhead vs CSV, slower parse.
- **Custom binary.** Smallest, fastest — but requires a parser and breaks the "cat the file on the PC" convenience.

**Recommended for Stage 5 bring-up:** **one CSV per sensor**, header row on each file, filename `SENSOR_<boot_seq>.csv` (rotated per boot). Rationale: simplest to write, simplest to read, gives us throughput data. When we know the actual data-per-sensor demands, we can decide whether to consolidate.

### 6.3 Boot-time round-trip check

On boot, sketch should:

1. Mount SD at 4 MHz (verified working).
2. `ls /sdcard/` — count files, sum file sizes, print summary.
3. Open the most recent CSV of the sensor being logged, print header + last 3 data rows. (Confirms the previous session actually wrote data, not just created a file.)
4. Open a new file `IMU_<seq>.csv` where `<seq>` is boot number persisted in NVS.
5. Write header row.
6. Enter logging loop.

Flush every N samples (say 100) so power loss loses at most 1 s of data. Full close on ship-mode entry.

### 6.4 Starting sensor

**LSM6DSV16X IMU** (bus 1, I²C address 0x6A). Reasons: highest interesting sample rate of the healthy sensors (up to 1 kHz), 6-axis makes for interesting log content, and it's the sensor that will drive the wearable's motion detection later — worth exercising the write path at its rate.

Fallback if IMU is uncooperative: **BME688** (bus 1, 0x76), 1 Hz T/P/H — trivially slow, human-readable per line, easy to spot check.

Actual implementation shipped this session: § 6.3 boot-time round-trip is done in `6_sd_logger_mk1`. Multi-sensor bring-up is done in `7_demo_mk1`. Validation results in § 7.

---

## 7. Sensor bring-up validation — `7_demo_mk1` (2026-07-05, boot 7)

After the SD subsystem was verified, wrote a single-sensor-at-a-time rolling logger with a phase table that walks the ammeter through:

```
P00 idle_baseline  10 s   -- all sensors parked, SD idle
P01 lis            10 s   -- LIS3MDLTR mag @ 40 Hz
P02 lsm            10 s   -- LSM6DSV16X IMU @ 100 Hz
P03 bme            10 s   -- BME688 T/P/H @ 2 Hz
P04 mic            10 s   -- MSM261DGT003 PDM @ 48 kHz mono WAV, gain x8
P05 veml           10 s   -- VEML6030 ALS @ 5 Hz
P06 idle_after      5 s   -- return-to-baseline check
```

The first draft included all 10 pair combinations (C(5,2)), but the ammeter showed no meaningful current delta between solo and paired phases — pairs were dropped as noise. Each solo phase extended to 10 s to give the ammeter a clean settled reading.

Files land under `/data/<sensor>/s<boot_seq>_<phase>.csv|wav` with a header-comment block (sketch, sensor, phase, rtc_start, ms_boot_start, rate_hz) then the CSV body. Mic WAV files have a `.txt` sidecar carrying the same provenance (WAV headers don't fit custom keys).

### 7.1 Power draw per sensor (in-line USB ammeter)

| Phase | Current | Delta vs idle | Notes |
|---|---|---|---|
| Idle baseline (P00) | ~100 mA | — | CPU 240 MHz default, all sensors parked, SD mounted idle |
| LIS solo (P01)   | ~130 mA | +30 mA | 40 Hz continuous mag reads + CSV writes |
| LSM solo (P02)   | ~130 mA | +30 mA | 100 Hz IMU reads + CSV writes |
| BME solo (P03)   | ~130 mA | +30 mA | 2 Hz forced-mode T/P/H |
| MIC solo (P04)   | ~130 mA | +30 mA | 48 kHz PDM continuous + WAV writes (~96 KB/s) |
| VEML solo (P05)  | ~130 mA | +30 mA | 5 Hz ALS |
| Idle after (P06) | ~100 mA | — | Returns to baseline; park-on-phase-exit is clean |

**Conclusion:** at 240 MHz + SD active, the per-sensor incremental cost is small vs the ESP32-S3's own idle consumption — all five sensors sit within a couple of mA of each other. Real low-power gains will come from CPU clock throttling, brief wake windows, and Wi-Fi/BT off — not from choosing "the low-power sensor." Also confirms sensor `park()` calls work: idle_after returns to the same ~100 mA as idle_baseline.

Handy to know when v8 budgets look at power: assume ~130 mA continuous at 240 MHz + SD + 1 sensor active. Cut CPU to 80 MHz per Stage 3 § 8.4 and this drops closer to ~70-80 mA. Deep-sleep between wake windows drops it to sub-mA (BQ25619 quiescent + RTC crystal).

### 7.2 Observed sensor values (typical ranges from boot 7)

Board flat on the bench, normal room lighting. Values are direct-from-file / direct-from-serial captures.

**LIS3MDL magnetometer (40 Hz, ±4 Gauss FS, 6842 LSB/Gauss)**

| Reading | Raw | Gauss |
|---|---|---|
| X | -8258 to -4521 | -1.21 to -0.66 |
| Y | +355 to +3669 | +0.05 to +0.54 |
| Z | -19464 to -13512 | -2.84 to -1.97 |

Earth's ambient field is ~0.5 G total. Readings > 1 G on some axes indicate local magnetic interference from bench equipment (transformers, speakers, ferromagnetic case) — not a sensor fault. No saturation at ±4 G FS today. If Y/Z ever pin to ±32767 in field use, the bench-magnet story from Stage 3 § 4.7 is back and hard-iron cal is needed.

**LSM6DSV16X IMU (100 Hz sample @ 240 Hz ODR, XL ±2 g, G ±250 dps, temp = /256 + 25 °C)**

| Channel | Quiet reading (board flat) | During handling | Sensitivity |
|---|---|---|---|
| Temp | 36.6 – 37.2 °C | (self-heat, warms during test) | 256 LSB/°C + 25 °C offset |
| A_X | ~0 g | brief spikes to ±2 g on flick | 2 g / 32768 LSB |
| A_Y | ~0 g | brief spikes to ±2 g | 2 g / 32768 LSB |
| A_Z | ~-1.0 g (gravity) | flips to +1.0 g when inverted | 2 g / 32768 LSB |
| G_x/y/z | ~0 dps | ±250 dps saturation on brisk rotation | 250 dps / 32768 LSB |

Chip self-heats a couple of °C during the phase — normal for continuous 240 Hz ODR. The FS_G = 250 dps is aggressive for wrist motion; a real wearable firmware will bump to 500 or 1000 dps to avoid gyro saturation on quick gestures.

**BME688 environmental (2 Hz forced-mode, T oversample ×2, P oversample ×16, H oversample ×1)**

| Reading | Range (10 s, bench, no fan) |
|---|---|
| T | 33.4 – 33.8 °C |
| P | 1005.8 – 1005.9 hPa |
| H | 26.3 – 27.2 %RH |

Caveat: the T here is the **BME688 die** temperature, not ambient — the sensor self-heats a few °C above room during forced conversions. Consistent with the LSM's 37 °C reading (both chips are inside the case volume). For accurate ambient, the daughter-board TMP117 (on a separate die further from active ICs) will be the better source once fitted. The pressure reading (~1006 hPa) matches a typical mid-elevation indoor sea-level-adjusted reading; humidity ~27 %RH is a dry room in July.

**MSM261DGT003 PDM mic (48 kHz mono, 16-bit, digital gain x8 = +18 dB)**

| Condition | Chunk spread (min-max after gain) |
|---|---|
| Silent room | ~800 – 3600 counts (mostly gain-amplified self-noise) |
| Voice ~30 cm away | 8 000 – 22 000 counts (comfortable listening level in WAV playback) |
| Occasional peaks | up to ±32k (saturation cap engaged, imperceptible in playback) |

Sweet spot found by Ivan: **gain x8 (+18 dB)** — silent room sounds nearly noise-free, voice sounds decent. Gain x16 (+24 dB) makes silence audibly hissy; gain x4 (+12 dB) is too quiet on voice. Digital-only fix: no analog-side change possible on the MSM261DGT003 (its analog gain is fixed by the part). Gain is stored per-recording in the WAV sidecar `.txt` so post-processing can recover the un-gained level if needed.

**VEML6030 ambient-light (5 Hz, ALS gain 1/8, IT 100 ms → 0.2304 lx/count)**

| Condition | ALS count | ~Lux |
|---|---|---|
| Finger covering | 0 | 0 |
| Normal room light | 40 | 9 |
| Bright bench lamp | 382 | 88 |

ALS and WHITE track together (both channels present), which rules out a stuck-channel fault. VEML6030 has 16 levels of gain/IT combinations if room-light readings ever pin to 0 or FS in field use — see datasheet Table 4.

### 7.3 Known gap — RTC unset

Every file this session reports `rtc_start=oscstop`. The PCF85063A oscillator-stop bit is set (either from a super-cap discharge or the RTC was never programmed). Data files still have valid `ms_boot_start` for relative timing, but wall-clock time is unavailable.

**Stage 5 fix (low priority):** add a serial command like `SET_TIME 2026-07-05T15:30:00` to a small sketch — parse, BCD-encode into RTC regs 0x04–0x0A, clear OS bit. Once set, PCF85063A survives USB replug + short battery removals via the backing capacitor on iv7.1. Not blocking any bench experiments.

---

## 8. Session-end state

| Subsystem | Status | Notes |
|---|---|---|
| ESP32-S3 + USB-C + power-on | ✓ PASS (with orientation caveat) | Iv7.1 wires only one D+/D- pair; plug the cable the correct way |
| BQ25619 charge / ship mode | ✓ PASS | Unchanged from Stage 3 |
| PCF85063A RTC | ⚠ PASS (unset) | Chip alive, but oscillator-stop bit set. Wall-clock time unavailable. See § 7.3. |
| VEML6030 ALS | ✓ PASS + data logged | § 7.2: 0 → 88 lux across room-light range. |
| LIS3MDLTR mag | ✓ PASS + data logged | § 7.2: producing plausible earth-field-plus-bench-interference reads. |
| LSM6DSV16X IMU | ✓ PASS + data logged | § 7.2: A_Z ≈ -1.0 g at rest, gyro clean; used as the reference sensor for `6_sd_logger_mk1`. |
| BME688 env | ✓ PASS + data logged | § 7.2: 33.4 °C / 1006 hPa / 27 %RH indoor (chip self-heats vs ambient). |
| WS2812B RGB | ✓ PASS | Unchanged |
| PDM mic | ✓ PASS + audible WAV | § 7.2: 48 kHz mono, digital gain x8 (+18 dB) is the sweet spot. WAV files playable and voice-audible. |
| **SD card (SDMMC 1-bit)** | **✓ PASS** | **65/65 at 400 k / 1 M / 4 M / 20 M with a 32 GB SDHC msdos card, no bodges. FAT16/FAT32 only. Real-workload logging exercised through `7_demo_mk1`.** |
| DRV2605L haptic | ⚠ DEGRADED | Chip silicon damage confirmed 2026-06-29; queued for replacement. |
| MAX30101 PPG | ✗ REPLACE | Killed in Stage 3; safe to replace now that +1V8 is verified clean. |
| Flashlight LED | ✓ PASS (post-bodge, capped duty) | R26 still to restock. |
| ALPS encoder | ✓ PASS | Unchanged. |
| M10S GPS | ⚪ Not exercised | No firmware path yet. |
| Qvar / ECG | ⚪ Not exercised | No firmware path yet. |

### Bench-rework debt (unchanged status)

- R26 flashlight resistor: restock.
- LED topology bodge: cut+jumper still needed as schematic redraw for v8.
- CLK pull-up bodge: still hand-soldered on empty footprint (v8 must populate).
- +1V8 rail bodge (C29 cut + jumper to +3V3): still active on iv7.1; v8 schematic rework tracked in `v8_MustFix.md § 1.2 / 1.3`.
- VDDIO routing on LSM/BME/mic: still bodged to +3V3; v8 tracked in same.
- SD bus pull-ups: today's session proved 3–10 kΩ works at all speeds up to 20 MHz. V8 recommendation of 4.7 kΩ stands as good practice; no longer critical.

### 8.1 Iv7.1 is nearing end-of-life as a bench mule

Recording this frankly: the iv7.1 board has taken a lot of physical abuse across five bench sessions (three hot-air rework rounds, plus dozens of scope-probe / meter contacts / clip-lead sessions). Bodges are stacking: cut+jumper on the flashlight FET, cut on +1V8 at C29 + jumper to +3V3, lifted R12, hand-soldered CLK pull-up on an empty footprint, and everything above under "bench-rework debt". A single wrong tug on any of these bodges could kill the board.

**Plan going forward:** finish Stage 5 (DRV chip swap + daughter board bring-up for MAX30101 / TMP117), then close out active development on this specific board. Next physical build is **a fresh iv7.x-family PCB, still Mk I** — same design as the current board, ideally with the `Stage_3b_v8_MustFix.md` schematic-side bugs baked into the fab (XC6206 pin-name swap, LSM/BME VDDIO on +3V3, flashlight FET topology, R12 DNP, empty CLK pull-up footprint populated, USB-C both-side D+/D-) so a clean SMT reflow produces a working board on the first power-up, without the compounding bodge stack. **Two new additions on the new assembly: a display and the M10S GPS actively exercised** (GPS is on the current board too but was never brought up in firmware).

Not a Mk II, not a new architecture — just a cleaner instance of the same wearable, with less thermal history and two more subsystems fitted. Everything demonstrated on iv7.1 through Stage 4 (SD @ 20 MHz, 5-sensor bring-up, ~100 mA idle / ~130 mA active baseline, mic-audible WAV recording, ship-mode, encoder/haptic/RGB/flashlight, all bus-1 sensors on +3V3) ports directly — the new build is additive.

---

## 9. References

- **Superseded docs (preserved with SUPERSEDED banners):** `SDMMC_Rescue_Plan_iv7.1.md`, `Stage_3_Build_Report.md § 4.1`
- Bench plan: `Bench_Plan_iv7.1.md`
- V8 must-fix: `Stage_3b_v8_MustFix.md`
- Sketches:
  - `firmware/test/3b_sd_pump_test_mk1/` — retry loop, used for the 65/65 verification
  - `firmware/test/3c_sd_pin_probe_mk1/` — pin-recovery diagnostic (Stage 4 addition)
  - `firmware/test/4_demo_mk1/` — production-ish demo baseline
  - `firmware/test/5_smoke_test_mk1/` — diagnostic build with encoder rewrite + DRV open-loop
  - `firmware/test/6_sd_logger_mk1/` — first SD writer, one-sensor (LSM) at 100 Hz CSV. Boot-time file review (head-3 of latest LSM_*.csv). Written and verified this session.
  - `firmware/test/7_demo_mk1/` — multi-sensor rolling logger, 10 s solo per sensor, ammeter-friendly stage markers, WAV mic. Written and verified this session — see § 7.
- Master pinout: `hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md`
- Auto-memory: `feedback_suspect_cheap_sd_cards.md`

---

## 10. Update log

| Date | Item | Note |
|---|---|---|
| 2026-07-05 | Stage 4 opened at the bench | Original scope: SD rescue + parallel rework. |
| 2026-07-05 | EXP-0 meter checks | All continuity readings clean (probe offset ~2 Ω). GND pin 4 fluctuation turned out to be probing artifact. |
| 2026-07-05 | `3c_sd_pin_probe_mk1` written and run | All three SD pins healthy: CLK ~166 cycles, CMD/DAT0 ~112 cycles. External pull-up chains intact. |
| 2026-07-05 | GND bypass wire added, pump test re-run | Failure mode changed from send_scr 0x107 to mount_to_vfs 0xffffffff (this was a red herring — see below). |
| 2026-07-05 | 1 µF cap added at socket VDD | No improvement. |
| 2026-07-05 | 128 MB SDSC (msdos) tested | **PASSes reliably.** First real evidence the bus is fine. |
| 2026-07-05 | 32 GB SDHC (msdos) tested | **65 / 65 PASSes at 400 kHz / 1 MHz / 4 MHz / 20 MHz.** Bus verified. |
| 2026-07-05 | Cap and GND bypass removed | 32 GB card continues to PASS reliably. Confirms neither intervention was necessary. |
| 2026-07-05 | Second Kodak 64 GB card, reformatted FAT32 | PASSes. Confirms the first Kodak 64 GB card was dead / dying, not the bus. |
| 2026-07-05 | USB-C orientation bug discovered | Iv7.1 wires only one D+/D- pair; only one plug orientation enumerates. New CRITICAL v8 item. |
| 2026-07-05 | Cross-doc SUPERSEDED banners applied | See § 3. Prior arc preserved for archaeology. |
| 2026-07-05 | `6_sd_logger_mk1` written and run | LSM6DSV IMU @ 100 Hz to `/LSM_<seq>.csv`, boot-time head-3 review. Confirmed round-trip write-then-read on the card. |
| 2026-07-05 | `7_demo_mk1` v1 written (pair-phase edition) | 18 phases (idle + 5 solo + 10 pair + all + idle_after). Ran once. |
| 2026-07-05 | `7_demo_mk1` v2 (this session's final) | Pair phases dropped (ammeter showed no delta), phases extended to 10 s each. Mic gain settled at x8 (+18 dB) as Ivan's sweet spot. All five connected sensors produced sensible data (§ 7.2). |

---

*Stage 4 ended 2026-07-05. SD subsystem verified working as-designed. Full sensor bring-up completed via `7_demo_mk1`: all five connected sensors (LIS, LSM, BME, MIC, VEML) log valid data to SD, WAV mic is audible at gain x8, ammeter shows a clean ~100 mA idle / ~130 mA single-sensor+SD baseline. Stage 5 opens with DRV chip swap + daughter board reattach (MAX30101 + TMP117); after that, the iv7.1 board's job is done and design attention shifts to the Mk II v8 clean-sheet — carrying everything proven here forward, adding display and GPS integration (see `Stage_3b_v8_MustFix.md`).*
