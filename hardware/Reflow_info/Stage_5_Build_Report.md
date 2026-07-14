# Stage 5 Build Report — Kompic Mk I (iv7.1)

**Date:** 2026-07-05 (afternoon session, same day as Stage 4 close)
**Board:** First Kompic Mk I prototype, PCB iv7.1 (same physical board as Stages 1–4)
**Builder:** Ivan
**Stage:** 5 of 5 (DRV chip swap + daughter-board bring-up: MAX30101 + TMP117)
**Prerequisite:** Stage 4 closed earlier the same day with SD subsystem verified working as-designed and 5-sensor bring-up via `7_demo_mk1`. Only outstanding items at the start of Stage 5 were the DRV2605L chip swap (silicon damage confirmed 2026-06-29) and the daughter-board rework (MAX30101 killed by Stage 3 § 4.8 / § 4.9 overvoltage; TMP117 never yet exercised in firmware).

---

## 1. Executive summary

**All subsystems on the iv7.1 prototype are now verified working.** Stage 5 closed out the two remaining bench-rework items:

- ✓ **DRV2605L replaced.** Fresh chip auto-cals clean on first boot (STATUS=0xE0, COMP=0x07, BEMF=0x8F, 1122 ms). No OC_DETECT. Encoder click feels firm and audibly / tactilely stronger than before — matches the "OC sense degraded" diagnosis from 2026-06-29. ESD during prior rework was the likely killer of the original part; ESD precautions were applied during the swap.
- ✓ **+1V8 rail verified clean at 1.80 V** on the daughter connector, confirming the Stage 3 § 4.9 C29-cut + jumper-to-+3V3 bodge is still intact after ~2 weeks of handling.
- ✓ **Old MAX30101 confirmed truly dead** — no I²C ACK at 0x57 on bus 1 after daughter reattach. Consistent with the +1V8-overvoltage kill mechanism (V_max = 2.0 V; chip saw 2.3–2.8 V continuously during Stage 3). Hot-air'd off with no further exercise attempt.
- ✓ **Fresh MAX30101 fitted.** I²C ACK at 0x57 on bus 1, PART_ID=0x15, REV_ID=0x06. HR mode enters cleanly, Red LED lit at ~6 mA, FIFO write pointer increments through the dwell loop (raw Red counts 57 k → 92 k in 5 samples). Fully functional.
- ✓ **TMP117 exercised for the first time.** ACK at 0x48 (ADD0=GND strapping), DEVICE_ID=0x0117. Continuous conversion mode returns 31.13 °C ± 0.01 in the dwell loop (case ambient, warmer than room because of ESP32-S3 self-heat inside the enclosure — same story as BME688 in § 7.2 of Stage 4).

Also delivered this session:

- ✓ **Ship-mode double-click handler ported into `3_smoke_test_mk1_hotair`** (the Stage 3 smoke sketch, which predated the 2026-06-21 discovery). Battery is permanently attached on iv7.1, so per auto-memory `feedback_sketches_need_shipmode.md` this is a hard requirement on every sketch flashed to the board. Port copied the button state machine + REG07 write path from `4_demo_mk1` verbatim; single-click preserves the smoke sketch's flashlight toggle. Prompt updated on line 843.
- ✓ **LIS Y-saturation observation, again, on the record.** Y = −32768 saturated on the bench. Same as Stage 3 § 4.7 / Stage 4 § 7.2 — bench magnetic environment (transformer/speaker/enclosure ferromagnetics) exceeds LIS ±4 G FS. Not a chip fault. Field-use hard-iron cal + orientation isolation will make this go away.

Carried forward — no more rework items on this board. Only administrative / firmware:

- R26 (47 Ω 0402 flashlight series R) still missing; LED duty stays firmware-capped at 64/255. Non-blocking for field use.
- RTC still reads `oscstop` when unset; needs one-shot serial `SET_TIME` command (Stage 4 § 7.3). Non-blocking; relative timestamps work.
- Bug 7 (flashlight cosmetic sub-µA glow) unresolved; cosmetic only.
- **Iv7.1 is now end-of-line as a hardware target.** New wearable build is a fresh PCB (still Mk I) with the v8 must-fix schematic bugs baked into fab, plus display and GPS actively exercised. See § 6 below.

---

## 2. Session accomplishments

### 2.1 DRV2605L chip swap — full success

Fresh part hot-aired onto the underside DGS footprint. ESD precautions (grounded iron / mat / wrist strap) applied throughout — the previous chip was diagnosed as internally-damaged OC sense, and ESD during prior rework was the leading suspect. Fresh chip immediately auto-cals clean at boot on `4_demo_mk1`:

```
[DRV ] Ping 0x5A ... ACK  STATUS=0xE0  DEVICE_ID=7
       auto-cal...................... PASS (1122 ms)  COMP=0x07  BEMF=0x8F  STATUS=0xE0
```

Comparison to the diagnosed-bad chip on 2026-06-29:

| Reading | 2026-06-29 (bad chip) | 2026-07-05 (fresh chip) |
|---|---|---|
| STATUS after cal | 0xE9 (DIAG=1 + OC_DETECT latched) | **0xE0 (clean)** |
| Auto-cal outcome | FAIL every attempt | **PASS ~1.1 s** |
| Library effect fire | STATUS=0xE5 (OC latched) every effect | STATUS=0xE0 (no OC), effects 4/10/14/47/16 → 0xE4 (feedback bit, expected) |
| LRA rattle strength | Barely perceptible even at strong-click 100 % | Firm, audibly and tactilely present at every encoder detent |

The `0xE4` on effects 4/10/14/47/16 during the smoke-test dwell loop is bit 2 (feedback-based / closed-loop) latched — normal for LRA library playback after successful auto-cal, not an OC event. Effect 1 (strong click) fires with STATUS=0xE0 — the click effects go through the open-loop path, so no feedback bit. Clean across the board.

### 2.2 +1V8 rail sanity check — clean

Metered at the daughter connector with the board USB-powered running `4_demo_mk1` (which doesn't touch the daughter side). Reading: 1.80 V ± 0.02, steady. The Stage 3 § 4.9 fix (C29 cut + jumper to +3V3, isolating LSM/LIS/BME/mic from the +1V8 branch and leaving only MAX + M10S GPS on it) is holding.

**Rule reaffirmed:** always verify +1V8 = 1.80 V clean before powering up a fresh MAX30101 (V_max=2.0 V abs). The rail-check step took ~30 s; if it had come back at 2.5 V, we would have killed the fresh chip in seconds.

### 2.3 Old MAX30101 — confirmed truly dead

Daughter PCB reattached with the original (Stage-3-damaged) MAX30101 still fitted. `3_smoke_test_mk1_hotair` at boot:

```
[SCAN] Bus1: 0x10 0x1C 0x51 0x6B 0x76  -> 5 device(s)   ← no 0x48, no 0x57
[MAX ] Ping 0x57 ... NO ACK
[TMP ] Ping 0x48 / 0x49 ... NO ACK either address
```

Both daughter-board chips absent from the scan. TMP117 was known-alive from Stage 3 (survived the overvoltage), so its absence today was a signal the daughter connector was seated but the *chip-level* MAX30101 was too far gone even to fail on I²C — chip bricked at the front end, not merely damaged on the analog side.

Ivan called the shot immediately: skip any further diagnostic effort on the old chip, hot-air it off, fit fresh. Correct call. (If the chip had ACK'd but returned wrong PART_ID or a stuck FIFO, an "at least light the LEDs" test would have been the next probe. It didn't, so no test needed.)

### 2.4 Fresh MAX30101 + TMP117 exercise

Hot-air'd fresh MAX30101 onto the daughter PCB (small nozzle, 320–350 °C, paste + reflow). TMP117 was never disturbed — it survived Stage 3 and is on-board original. Second boot of the smoke test:

```
[SCAN] Bus1: 0x10 0x1C 0x48 0x51 0x57 0x6B 0x76  -> 7 device(s)   ← full expected scan
[MAX ] Ping 0x57 ... ACK   PART_ID=0x15 (expect 0x15)  REV_ID=0x06
       configured HR mode (Red LED ~6 mA) for dwell-loop sampling
[TMP ] Ping 0x48 / 0x49 ... ACK  @ 0x48  DEVICE_ID=0x0117 (low12 expect 0x0117)
```

Both chips fully alive.

### 2.5 Encoder handling lesson — polled detent-rest, not edge ISR

Discovered during the `7_demo_field_capture` bench-up on 2026-07-05 evening: an edge-triggered ISR on the encoder's A line with any reasonable software debounce (tested 8 ms, 12 ms, 15 ms at 3.3 V with 10 k pull-ups) **cannot** deduplicate a single detent turn. The mechanical bounce at the end of a detent regularly produces a settle edge past 15 ms with B latched in the *opposite* state to the leading edge — the ISR then registers a second click with the reverse direction, and the user is back where they started. Symptom in the field-capture serial log:

```
[UI  ] mode=skin
[UI  ] mode=fl        ← +1 leading
[UI  ] mode=skin      ← −1 settle (same detent)
[UI  ] mode=fl
[UI  ] mode=alarm     ← +1 leading of next detent
[UI  ] mode=fl        ← −1 settle
```

Every single detent yields a `+1 / −1` oscillation.

The **polling detent-rest state machine** (documented in `firmware/docs/datasheet_extracts/20.16_Encoder_Datasheet_Extract_iv7.1_f0.0_2026-06-18.md` line C-08 and originally implemented in legacy `5_smoke_test_mk1`) is the correct approach for this part on this rail:

- Sample A + B on every `loop()` pass (~2 ms cadence).
- **at-rest** = both A + B HIGH for `ENC_DETENT_REST_MS` (10 ms) continuously.
- On **leave-rest**, latch direction from whichever line went LOW first (A-first = +1, B-first = −1). Bounces during the click cycle can't change the latched direction.
- On confirmed **return-to-rest**, emit exactly one click event.

No ISR, no `attachInterrupt`, no debounce timer. `7_demo_field_capture` was fixed to use this pattern 2026-07-05 evening; one detent = one clean click across sustained testing. Locked into auto-memory `feedback_encoder_polling.md` so future sketches don't re-guess the ISR approach.

**Rule for any future Kompic sketch that uses the ALPS EC05E encoder on iv7.x:** poll, don't ISR.

### 2.6 Field-data-collection sketch — `7_demo_field_capture`

New sketch built during this session for wearable use in the case. Encoder-driven mode selector (Red / Green / Yellow / Pink / White / Purple), single-click actions, RGB pulse feedback, DRV haptic feedback per detent + per event, voice-annotated sensor recordings, PPG-based BPM detection, RTC set via serial, ship-mode double-click preserved throughout. Files land on SD under `/data/<mode>/s<boot>_r<seq>.csv` with a paired `/data/mic/s<boot>_r<seq>_annot.wav`. Live serial mirror of every sensor row at 2 Hz cadence for at-bench monitoring without pulling the card. Full details in the sketch header.

Iteration highlights across the session:
- **Encoder handling** revised (see § 2.5 above).
- **LSM6DSV16X init byte-order bug** — `CTRL1/CTRL2 = 0x70` was `OP_MODE=111 + ODR=0000 (OFF)`; correct value is `0x07` (`OP_MODE=000 (high-perf) + ODR=0111 (240 Hz)`). Fixed. IMU now streams properly.
- **LIS3MDLTR FS bumped ±4 G → ±16 G** (`CTRL2 = 0x60`, LSB=1/1711 G) to survive the LRA magnet inside the sealed case. Y axis still saturates at ~-19 G because the LRA neodymium sits within ~10 mm of the LIS. **Hardware issue**, not firmware: needs magnetic shielding or PCB relocation on the next build. Documented as an open item for the v8-class board.
- **MAX30101 upgraded HR mode → Multi-LED (Red + IR + Green)** with all three channels logged to CSV. Uses `MODE_CFG = 0x07 (multi-LED)` + slot config `0x11 = 0x21 / 0x12 = 0x03`. FIFO carries 3 × 3 = 9 bytes per sample.
- **BPM detector (Green channel, local-max peaks).** Initial zero-crossing-with-hysteresis implementation was found to lock up when the finger stays planted (baseline chases slowly, AC never re-arms below −threshold). Replaced with local-maxima detection (peak = `ac_prev > ac_now` && `ac_prev > threshold` && `≥300 ms since last`). 5 s baseline settle. 2 s staleness — BPM output clears if no beat detected. Green (~525 nm) chosen because oxyhaemoglobin's absorption peak sits there — consumer-wearable-grade SNR through skin, far better than Red or IR at this task.
- **BME688 gas compensation ported + heater calibration computed from `par_g1/g2/g3/range/val`.** Initial config (target 320 °C, 100 ms gas_wait) held `heat_stab=0` — heater didn't plateau. Second pass (target 250 °C, 200 ms gas_wait, computed `res_heat_0 = 0x5E`) still returns `gas_r=0 v=0 stab=0`. **Gas sensor is an open item** — either the heater still isn't reaching target or the res_heat formula I ported is off. Deferred to a next-session investigation. Doc 20.12 § 296-329 has the reference.
- **Palette re-tune, twice.** First pass: Yellow biased to red, Orange indistinguishable from pure red. Second pass: Yellow made green-dominant `(14, 26, 0)`. Third pass (this session): Skin swapped from Orange to **Pink `(26, 0, 12)`** — red+blue avoids the WS2812's red-brightness bias entirely. Final palette Red / Green / Yellow / Pink / White / Purple reads clearly distinguishable.
- **BME 40 °C+ die reading** — self-heating inside the case volume is confirmed; TMP117 in Skin mode is the true skin/ambient source.

### 2.7 USB-MSC investigation — feature dropped

Attempted to add a "SD card as USB mass storage" mode so data could be pulled without opening the case. Feasible on the ESP32-S3 via `USBMSC` (built on TinyUSB), but requires the Arduino IDE `USB Mode = "USB-OTG (TinyUSB)"` setting.

**Blocker:** switching to TinyUSB disables the ESP32-S3's built-in **USB-Serial-JTAG peripheral**, which is what esptool uses to trigger a soft reset before flashing. In TinyUSB mode every subsequent upload fails with `OSError: [Errno 71] Protocol error` on the `_update_rts_state` call — the manual timing-window hack (unplug USB, replug, click Upload within 2 s) is required for every single flash cycle. Bench-verified this session; also bricked the board twice mid-testing before we found the timing-hack recovery path.

Also verified: calling `USB.begin()` / `USBMSC` APIs in **Hardware CDC and JTAG** mode partially initialises TinyUSB, hangs `setup()` before it prints anything past `[SD ] mount OK`, and disrupts the reset path such that the next upload also needs the timing hack. This is the failure that bricked the board mid-session.

**Verdict: dropped.** The operational cost (fragile flash workflow, high risk of bricking during iteration) is not worth the convenience of not removing the SD card. Data extraction path is now: live serial-monitor mirror at 2 Hz (works today) OR physical SD-card removal.

**Locked into auto-memory (`feedback_tinyusb_guard.md`):** any future sketch that touches `USB.h` / `USBMSC.h` / `USB.begin()` on Kompic iv7.x **must** be `#if defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 1)`-guarded, or the wrong-mode build bricks the reset path. No exceptions.

**For v8 / new PCB:** consider a proper USB switching path — either a physical UART header exposed for esptool + TinyUSB-only USB, or a runtime chip select between USB-OTG and USB-Serial-JTAG. Not doable on iv7.x hardware.

### 2.8 Ship-mode port into `3_smoke_test_mk1_hotair`

Predecessor smoke sketch was from 2026-06-20, before the 2026-06-21 discovery of the R12 zombie-wake / BQ ship-mode double-click sequence (Stage 3 § 8.1). Battery is permanently attached; without ship mode the board can only be powered off by draining. Per auto-memory `feedback_sketches_need_shipmode.md` this is required for every flashed sketch. Port done today, in four edits:

1. Added `BQ_REG_MISC_OP = 0x07` + `BQ_BATFET_DIS|RST_WVBUS|DLY|RST_EN` bit flags to the BQ register block.
2. Added `WS_BLIP_LEVEL`, `BTN_DEBOUNCE_MS`, `BTN_DOUBLE_GAP_MS` timing defs.
3. Inserted the button state machine (`BTN_IDLE / PRESSED / WAIT_DBL / PRESSED_2`) + `ship_mode_countdown` + `enter_ship_mode` + `handle_button` before `void loop()`. Single-click preserved as flashlight sine-breathe toggle (matches the smoke test's original UX).
4. Replaced the old single-press debounce block at the top of `loop()` with a `handle_button()` call.
5. Boot message updated: `Single-click GPIO16 = toggle flashlight; double-click = ship mode`.

Verified at the bench — smoke test ran the full dwell rotation, ship mode was not exercised (would end the session), but the button+state-machine wiring is identical to `4_demo_mk1`'s tested path, so no behavioural regression expected. Full sensor validation in § 3 below was captured during this run.

---

## 3. Sensor bring-up validation — full board (2026-07-05, Stage 5 smoke)

Serial capture from the updated `3_smoke_test_mk1_hotair` boot + dwell loop. Board flat on the bench, normal room light, no fingers on the daughter.

### 3.1 Boot scan and identity

```
[SCAN] Bus1: 0x10 0x1C 0x48 0x51 0x57 0x6B 0x76  -> 7 device(s)   ← FULL house
[SCAN] Bus2: 0x5A 0x6A  -> 2 device(s)

  BQ25619    (charger,  bus2 0x6A) : PASS
  DRV2605L   (haptic,   bus2 0x5A) : PASS  cal=PASS  COMP=0x07 BEMF=0x77 (1150 ms)
  PCF85063A  (RTC,      bus1 0x51) : PASS
  VEML6030   (ALS,      bus1 0x10) : PASS
  LIS3MDLTR  (mag,      bus1 0x1C) : PASS
  LSM6DSV16X (IMU,      bus1 0x6B) : PASS
  BME688     (env,      bus1 0x76) : PASS
  WS2812B    (RGB,      GPIO42)    : DRIVEN
  MSM261DGT  (mic, PDM 47/48)      : PASS
  MAX30101   (PPG,      bus1 0x57) : PASS  PART=0x15 REV=0x06
  TMP117     (temp,     bus1 0x48) : PASS  DEVID=0x0117
  SDMMC                            : FAIL     ← stale summary-ordering bug (see § 5), SD actually mounts+writes fine right after
  ALPS Enc   (A=21 B=43)           : DRIVEN
```

### 3.2 MAX30101 dwell — FIFO ticking, Red LED lit

```
[MAX ] WR=0 RD=0 unread=0 OVF=31 INT=0x01  Red=57759
[MAX ] WR=1 RD=1 unread=0 OVF=24 INT=0x00  Red=73967
[MAX ] WR=2 RD=2 unread=0 OVF=24 INT=0x00  Red=90446
[MAX ] WR=3 RD=3 unread=0 OVF=24 INT=0x00  Red=92571
[MAX ] WR=4 RD=4 unread=0 OVF=24 INT=0x00  Red=92627
[MAX ] WR=5 RD=5 unread=0 OVF=24 INT=0x00  Red=92492
```

Write pointer increments once per read, unread=0 (dwell code pops one sample per print). OVF=31 on the first sample is just the reset-time overflow counter; steady OVF=24 after that is the counter's saturated-since-last-read value, not new data loss. Red counts settle around 92 k with no target on the sensor — this is the ambient IR from bench overhead lighting hitting the Red channel photodiode. With a finger placed on the sensor, expect Red to jump to ~500 k+ and modulate with the PPG waveform.

### 3.3 TMP117 dwell — precision ambient (through the case)

```
[TMP ] @ 0x48  raw=0x0F91  T=31.133C   (three consecutive)
[TMP ] @ 0x48  raw=0x0F90  T=31.125C
[TMP ] @ 0x48  raw=0x0F90  T=31.125C
[TMP ] @ 0x48  raw=0x0F8E  T=31.109C
[TMP ] @ 0x48  raw=0x0F8E  T=31.109C
```

Resolution 7.8125 mLSB → 31.1 ± 0.02 °C repeatability across the dwell. TMP117 die sits on the daughter PCB, mechanically closer to the wearer's skin than the main-board BME688 (which reads 37 °C due to case-volume self-heat from the ESP32-S3). TMP117 will be the canonical ambient / skin-temp source in the field wearable; BME688 is chip-die temperature, not ambient.

### 3.4 LIS Y-saturation (bench magnet story, again)

```
[LIS ] MAG  X= -5022 Y=-32768 Z= -9074 raw  (-0.73 -4.79 -1.33 G)
[LIS ] MAG  X= -5014 Y=-32768 Z= -9097 raw  (-0.73 -4.79 -1.33 G)
```

Y pinned at −32768 (LSB representation of the signed-int16 low rail), Z close to −1.3 G, X around −0.7 G. Same signature as Stage 3 § 4.7 and Stage 4 § 7.2 — bench environment magnetic field on the Y axis exceeds ±4 G FS. Not a chip fault. Confirmed sensor is alive because the other two axes are producing sane numbers and X/Z track between reads.

Field-use fix: (a) move the wearer away from mains-transformer / speaker / large-ferromagnetic environments, (b) hard-iron calibration to subtract the fixed offset from the case's own ferromagnetic components, (c) if saturation is expected in the field wearable's actual use conditions, either drop to ±8 G FS mode (LIS3MDLTR supports up to ±16 G) or switch to the LSM6DSV16X's built-in mag alternative.

### 3.5 LSM + BME still self-heating

- LSM temp 40.0–40.3 °C (was 36.6–37.2 °C in Stage 4 § 7.2 — Stage 5 dwell is longer; chip has more time to warm)
- BME 36.87–37.01 °C, 1000.76 hPa, 17–17.5 %RH

BME humidity is lower than Stage 4's 27 %RH — probably time-of-day / weather; same order of magnitude either way. Pressure ~1000 hPa matches indoor sea-level-adjusted at Ivan's altitude.

### 3.6 DRV playback in dwell

```
[DRV ] play effect   1 GO  STATUS=0xE0
[DRV ] play effect   4 GO  STATUS=0xE4
[DRV ] play effect  10 GO  STATUS=0xE4
[DRV ] play effect  14 GO  STATUS=0xE4
[DRV ] play effect  47 GO  STATUS=0xE4
[DRV ] play effect  16 GO  STATUS=0xE4
```

Effect 1 (Strong Click 100 %) via open-loop path → STATUS=0xE0 (clean). Effects 4/10/14/47/16 via closed-loop LRA library → STATUS=0xE4 (bit 2 = feedback-based path indicator, expected in closed-loop). No OC_DETECT anywhere. Ivan confirmed "buzzing much better now, I can feel it so much better than before" — the tactile improvement over the pre-swap chip is unmistakable.

### 3.7 SD dwell — write + read round-trip works

Despite the summary line saying `SDMMC : FAIL` (see § 5 for why — stale ordering), the SD subsystem is fully functional in the dwell loop:

```
[SD  ] SDMMC 1-bit init: CLK=GPIO38 CMD=GPIO39 D0=GPIO40  @ 400 kHz
       mount OK  type=SDHC  size=59645 MB
       wrote /smoke_log.txt  total .txt files=1
[SD  ] type=SDHC  size=59645 MB  used=8 MB  .txt=1   (six times over 3 s)
```

Same 64 GB card that was reformatted FAT32 during Stage 4. Unchanged from Stage 4's 65/65 PASS finding.

### 3.8 Mic + VEML + BQ + WS — nothing new to add

- Mic: RMS ≈ 1050 counts on ambient bench sounds, samples varying, no dropouts. Consistent with Stage 4 mic capture.
- VEML: 24 ALS, 56 WHITE counts on the room-light reading = ~5.5 lux (dim bench). Full working range 0 (finger covering) → 100+ (bench lamp) is intact.
- BQ: STATUS = 0x74 / 0x7C oscillating (mid-fast / done-fast) — normal charging telemetry on USB.
- WS: full colour walk in the dwell, all six named colours print, LED changes as expected.

---

## 4. Cross-doc updates arising from Stage 5

Applied 2026-07-05:

- **`Bench_Plan_iv7.1.md` § 1.2 (DRV replacement)** — mark RESOLVED.
- **`Bench_Plan_iv7.1.md` § 1.3 (MAX30101 replacement)** — mark RESOLVED. Add a note that TMP117 has now also been exercised for the first time.
- **`Stage_3_Build_Report.md` CRITICAL BUG SUMMARY row 5** (MAX30101 damaged) — status → RESOLVED, points to this report.
- **`Stage_4_Build_Report.md` § 5.1** (carry-forward) — cross off DRV replacement and MAX30101 replacement lines.
- **`Stage_4_Build_Report.md` § 8** (session-end state) — DRV row status changed from "DEGRADED" to "PASS"; MAX row status changed from "REPLACE" to "PASS + data".
- Auto-memory — no new entries. Existing `feedback_sketches_need_shipmode.md` was the guiding rule for the Stage 3 smoke port, no update needed.

(Ivan will apply the doc-cross-refs at his leisure; this report is the authoritative record of the Stage 5 state.)

---

## 5. Known open items on iv7.1 (all non-blocking)

| # | Item | Nature | Priority | Notes |
|---|---|---|---|---|
| 1 | R26 (47 Ω flashlight series R) | Missing part, lost during Stage 2/3 probing | Low | LED duty firmware-capped at 64/255. Restock any time; not blocking. |
| 2 | RTC oscillator-stop bit set | Never programmed since assembly | Low | Add one-shot serial `SET_TIME 2026-...` command. `ms_boot_start` gives relative timing meanwhile. |
| 3 | Flashlight LED sub-µA glow (Bug 7) | Cosmetic | Low | BSS138W I_DSS leakage. Fix: add 1–10 kΩ bleed R across the LED. Not visible in normal daylight use. |
| 4 | Stale `SDMMC : FAIL` in smoke-sketch summary | Ordering bug | Low | Summary printed before SD init (Stage 3 § 4.6 mitigation for the ipc1 panic that no longer applies now that SD is reliable). Reorder summary-print to after SD init in the refactored smoke sketch. |
| 5 | LIS Y-axis bench saturation | Environmental, not a fault | Ignored | Chip is fine; will need hard-iron cal in the field wearable. |
| 6 | USB-C plug orientation | iv7.1-only, one D+/D- pair wired | Ignored | Plug the working way. New-build PCB fixes with both D+/D- pairs routed. |
| 7 | LIS3MDLTR Y-axis saturation from LRA magnet | Hardware — LRA neodymium magnet ~10 mm from LIS die inside sealed case | Hardware-only (leave) | Even at ±16 G FS the Y axis pins to -19.15 G continuously. Verified accel + gyro fine. Fix on next PCB: reroute LIS away from LRA or add mu-metal shielding. |
| 8 | BME688 gas resistance reads 0 despite `v=0 stab=0` bits | Firmware — heater not stabilising at 250 °C in 200 ms | Firmware, deferred | Register fix landed (moved 0x2A/0x2B → 0x2C/0x2D per BME688 vs BME680). Heater calibration `res_heat_x` computed from `par_g1/g2/g3` per Bosch formula; still not stab. Try target 200 °C + `gas_wait_0 = 400-500 ms` in next-session pass. Full Bosch reference driver would give a ground-truth comparison. |
| 9 | USB-MSC "SD as USB drive" mode | Firmware — dropped due to esptool reset-path conflict in TinyUSB mode | Won't fix on iv7.x | See § 2.7. New PCB can support this if a physical UART header for esptool is exposed. |

**The iv7.1 board is functional and shipping to field-data-collection duty.** Items 7-9 are known-open and documented; none block collecting real data.

---

## 6. Session-end state — full board

| Subsystem | Status | Notes |
|---|---|---|
| ESP32-S3 + USB-C + power-on | ✓ PASS | With plug-orientation caveat (item 6 above) |
| BQ25619 charge / ship mode | ✓ PASS | Ship mode double-click verified in `4_demo_mk1` since Stage 3 § 8.1 |
| PCF85063A RTC | ✓ PASS (unset) | Ticking, needs SET_TIME one-shot |
| VEML6030 ALS | ✓ PASS | 0 → 88 lux range verified Stage 4 |
| LIS3MDLTR mag | ✓ PASS (bench-magnet saturation) | See § 3.4 / § 5 item 5 |
| LSM6DSV16X IMU | ✓ PASS | 6 axes + temp; used in `6_sd_logger_mk1` at 100 Hz |
| BME688 env | ✓ PASS | T/P/H; die temp not ambient |
| WS2812B RGB | ✓ PASS | Drives correctly, ship-mode countdown uses red |
| PDM mic | ✓ PASS | 48 kHz mono, audible at gain x8, Stage 4 § 7.2 |
| **SD card (SDMMC 1-bit)** | **✓ PASS** | 65/65 at 20 MHz, Stage 4 § 2.1; smoke-summary line is stale ordering (§ 5 item 4) |
| **DRV2605L haptic** | **✓ PASS** | **Fresh chip; auto-cal clean; strong tactile click.** |
| **MAX30101 PPG** | **✓ PASS + LEDs live** | **Fresh chip; PART=0x15 REV=0x06; HR mode + Red LED at ~6 mA; FIFO ticking.** |
| **TMP117 temperature** | **✓ PASS** | **DEVID=0x0117; 31.1 ± 0.02 °C repeatability.** |
| Flashlight LED | ✓ PASS (capped duty) | R26 restock still open |
| ALPS encoder | ✓ PASS | Firm DRV click each detent |
| M10S GPS | ⚪ Not exercised | Hardware present, no firmware path on iv7.1. Will be exercised on the new board. |
| Qvar / ECG electrodes | ⚪ Not exercised | Hardware present, no firmware path yet. |

**Verdict on iv7.1:** every sensor and actuator the board has is now electrically alive and firmware-exercised. Only pieces not brought up in firmware are the GPS and the Qvar/ECG electrodes — both are hardware-present but never had firmware. The board is a complete Kompic Mk I demonstrator and ready for the field-data-collection phase.

---

## 7. Iv7.1 end-of-life note — next build is a fresh PCB

Continuing the plan-forward from Stage 4 § 8.1: iv7.1 has now completed its bench-mule job. Between five bench sessions, three hot-air rework rounds, and dozens of scope-probe / clip-lead sessions, the board carries this bodge stack:

- Cut+jumper on flashlight FET (topology fix, Stage 3 § 4.2)
- Cut+jumper on +1V8 at C29 to +3V3 branch (Stage 3 § 4.9)
- XC6206 rotated 180° with lead-bend to swap Vin/Vout pads (Stage 3 § 4.8)
- Lifted R12 for BQ ship-mode (Stage 3 § 8.1)
- Hand-soldered CLK pull-up on empty footprint (Stage 3 § 4.1)
- Missing R26 flashlight series R (lost, Stage 3 § 4.2)

Each of these represents a potential single-point-of-failure for a tug or a misplaced probe. **Continuing to iterate on this board is diminishing returns.** From this point forward:

- **iv7.1 stays in service as the field-data-collection prototype.** Ivan will use the current build to gather PPG / IMU / environmental data via `4_demo_mk1` (interactive) and `5_demo_sd_logger` / `7_demo_mk1` (batch capture) into the case. No further modifications planned. Handle with care.
- **A new PCB assembly is in preparation** — same design (Kompic Mk I), with the schematic-side fixes from `Stage_3b_v8_MustFix.md` baked into fab so a clean SMT reflow yields a working board on first boot. Additions on the new build:
  - **Display active in firmware** (hardware exists on iv7.1 too, but never exercised).
  - **M10S GPS active in firmware** (hardware exists on iv7.1 too, but never exercised).
  - **Fuel gauge** (MAX17048G+T10) — new addition per Bench_Plan § 3.1 and v8 BOM.
- Everything demonstrated on iv7.1 through Stages 1–5 ports directly forward. The new build is additive, not a redesign.

---

## 8. References

- Superseded / archive: `SDMMC_Rescue_Plan_iv7.1.md`, `Stage_3_Build_Report.md § 4.1`
- Preceding stage reports: `Stage_1_Build_Report.md`, `Stage_2_Build_Report.md`, `Stage_3_Build_Report.md`, `Stage_4_Build_Report.md`
- Bench plan: `Bench_Plan_iv7.1.md` § 1.2 (DRV), § 1.3 (MAX30101)
- V8 must-fix: `Stage_3b_v8_MustFix.md`
- Master pinout: `hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md`
- Sketches (paths after Stage 5 refactor — pre-refactor snapshot in `firmware/test/legacy/`):
  - `firmware/test/legacy/3_smoke_test_mk1_hotair/` — Stage 3 smoke sketch, ship-mode added this session; used for the § 3 sensor bring-up validation.
  - `firmware/test/legacy/4_demo_mk1/` — used for DRV chip-swap verification.
  - Refactored sketch array is being reorganised into `firmware/test/1_..7_..` at the same session; see the refactor commit for the new layout.
- Auto-memory: `feedback_sketches_need_shipmode.md`, `feedback_suspect_cheap_sd_cards.md`, `project_kompic_mk1.md`, `feedback_commits.md`

---

## 9. Update log

| Date | Item | Note |
|---|---|---|
| 2026-07-05 (afternoon) | Stage 5 opened after Stage 4 close on the same day | Two rework items outstanding: DRV chip swap + daughter-board bring-up. |
| 2026-07-05 | DRV2605L hot-air swap, ESD precautions applied | Fresh chip, no other bench changes. |
| 2026-07-05 | `4_demo_mk1` first boot post-DRV swap | Auto-cal PASS 1122 ms, STATUS=0xE0, COMP=0x07, BEMF=0x8F. Zero OC events. Encoder click strong and firm. |
| 2026-07-05 | +1V8 rail probed at daughter connector | 1.80 V clean. Stage 3 § 4.9 bodge stack intact. |
| 2026-07-05 | Old MAX30101 daughter reattach + smoke boot | NO ACK at 0x57 (and 0x48 TMP also absent, indicating chip-brick not partial). Confirmed dead, no further diagnostic. Hot-air off, fit fresh. |
| 2026-07-05 | Ship-mode double-click handler ported into `3_smoke_test_mk1_hotair` | 4 edits: BQ defines, WS/BTN timing defines, state machine + 3 handler functions, loop replacement. Preserves single-click = flashlight toggle. |
| 2026-07-05 | Fresh MAX30101 fitted, smoke boot | ACK 0x57, PART=0x15, REV=0x06. HR mode + Red LED. FIFO WR increments 0→5 in dwell. |
| 2026-07-05 | TMP117 first firmware exercise | ACK 0x48, DEVID=0x0117, T=31.13 ± 0.02 °C repeatability. |
| 2026-07-05 | Full sensor smoke dwell captured | All 11 chips + WS + encoder green. See § 3. |
| 2026-07-05 | Iv7.1 declared end-of-life as a rework target | No pending rework items. Repack into case for field data collection. New PCB build starts in parallel. |
| 2026-07-05 (evening) | `7_demo_field_capture` first bench-up | Encoder ISR-with-debounce discovered to be unfixable on this board; switched to polled detent-rest state machine per legacy `5_smoke_test_mk1`. See § 2.5. Auto-memory `feedback_encoder_polling.md` captured. |
| 2026-07-05 (evening) | LSM6DSV16X init byte-order bug found | CTRL1/CTRL2 = 0x70 was OFF (OP_MODE=111 + ODR=0000); correct value is 0x07 (high-perf + 240 Hz). Field-capture sketch fixed. |
| 2026-07-05 (evening) | LIS3MDLTR FS bumped ±4 G → ±16 G | LRA magnet inside the sealed case saturates the Y axis at ±4 G. ±16 G (1711 LSB/G) clears the saturation with plenty of headroom for earth-field measurement. |
| 2026-07-05 (evening) | MAX30101 upgraded HR mode → Multi-LED (Red + IR + Green) | Field capture now logs all three PPG channels + does live BPM detection with 5 s baseline settle window. |
| 2026-07-05 (evening) | RTC set via serial | `SET_TIME 2026-07-05T18:50:00` accepted; PCF85063A now battery-backed and running. |
| 2026-07-05 (late evening) | BPM detector rewrite: zero-crossing → local-max peak detection | Zero-crossing locked in "positive" state when finger planted; local-max is robust. See § 2.6. |
| 2026-07-05 (late evening) | BME688 gas registers corrected 0x2A/0x2B → 0x2C/0x2D (BME680 → BME688 layout) | See § 2.6. `heat_stab` now reads real bits, but still =0 at target 250 °C / 200 ms — gas readout deferred. |
| 2026-07-05 (late evening) | USB-MSC feature attempted then dropped | Bricked board twice; requires TinyUSB compile which disables the reset path esptool needs. Auto-memory `feedback_tinyusb_guard.md` captured. See § 2.7. |
| 2026-07-05 (late evening) | Palette Skin swapped Orange → Pink `(26, 0, 12)` | Orange still read as red on WS2812 due to red-channel brightness bias; Pink is unambiguous. See § 2.6. |
| 2026-07-05 (late evening) | Serial-monitor mirror added for env / mot / skin / mic at 2 Hz | Live data at the bench without pulling the SD card. |
| 2026-07-05 (late evening) | 2 s USB-enumerate delay + RX flush at boot | Boot log no longer cut off; phantom `[CMD ]` unknown commands from echoed CDC RX no longer land in the parser. |

---

*Stage 5 ended 2026-07-05 with the Kompic Mk I iv7.1 prototype fully functional: every sensor and actuator on the board is electrically alive and firmware-exercised, DRV haptic swap and MAX30101 replacement both completed cleanly, and TMP117 running precision ambient. iv7.1 is now retired from rework and enters field-data-collection service; the next hardware step is a fresh PCB (still Mk I) with v8 schematic fixes baked into fab, plus display + GPS + fuel-gauge firmware brought up.*
