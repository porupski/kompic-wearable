# Stage 11 Item D — Power management implementation plan

Baseline to beat: **106 mA** at USB with `batt_test=1` (sensors parked, LEDs blipping every 10 s, RTC + BQ25619 + ESP core running). Battery-life run in progress from `/sd/data/battery/batt_0025.csv`.

This plan is written to survive a re-flash cycle: keep `batt_test=1` as the measurement mode. Each numbered block below is meant to land as a separate small commit + retest so we can attribute the delta.

---

## 1. Current sinks (what's drawing at baseline)

At 106 mA @ ~4.15 V ≈ 440 mW. Order-of-magnitude breakdown (rough — needs profiler validation, but useful to prioritise):

| Sink | Rough share | Notes |
|---|---|---|
| ESP32-S3 CPU @ 240 MHz active (never idle) | ~40-60 mA | Field-capture task's 5 ms poll = no light-sleep opportunity |
| WS2812 quiescent when off | ~1-2 mA | 50 ms cyan blip every 10 s adds ~0.1 mA average |
| BQ25619 running + I2C 1 Hz | ~1 mA | Small |
| PCF85063 + coin cell trickle | ~µA | Negligible |
| USB peripheral (Serial-JTAG active) | ~10-15 mA | Non-trivial — JTAG is polled by the RTC-CLI task |
| SDMMC host (mounted, idle) | ~5-10 mA | Currently kept mounted for the battery-test CSV writes |
| Regulator quiescent + LDOs | ~5-10 mA | Board-level, can't do much in firmware |

**The big pot: CPU active time. Everything else is small.**

---

## 2. The four blocks — ordered by effort × value

### Block A — Enable PM + tickless + DFS  (small; sets the stage)

**sdkconfig deltas (to be clicked, mirrored to `sdkconfig.defaults`):**
```
CONFIG_PM_ENABLE=y
CONFIG_PM_DFS_INIT_AUTO=y                    # DFS on from boot
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y          # required for auto-light-sleep
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3     # tick threshold (3 = 30 ms @ 100 Hz)
CONFIG_PM_SLP_IRAM_OPT=y                     # place sleep hooks in IRAM
CONFIG_PM_RTOS_IDLE_OPT=y
```

`esp_pm_config_t` set at boot:
```c
esp_pm_config_t pm = {
    .max_freq_mhz  = 240,     // CPU when active
    .min_freq_mhz  = 40,      // CPU when DFS decides to drop
    .light_sleep_enable = true,
};
esp_pm_configure(&pm);
```

**Expected delta on its own:** small (~5-15 mA) because the field-capture task's 5 ms tick prevents light-sleep from ever engaging. Real wins require Block B first.

**Risks:** PM locks are needed by any driver that requires a fixed frequency (RMT for WS2812, SDMMC clock, PDM mic, I2S). Every existing "just polls" driver is safe. The dodgy paths are RMT (LED) and PDM (mic). Both need `ESP_PM_APB_FREQ_MAX` locks around active use.

### Block B — Field-capture task: event-driven wake  (medium; the big win)

**Current problem:** `task_field_capture_fn` spins `POLL_TICK_MS = 5 ms` regardless of activity. In STANDBY that's 200 wake-ups/s that block light-sleep.

**Approach:**
- Keep the 5 ms poll only when we've seen encoder or button activity in the last ~2 seconds ("active window").
- After 2 s of no input, drop to 50 ms poll AND set up ESP_SLEEP_EXT1 wake sources for GPIO16 (button), GPIO21 (enc A), GPIO43 (enc B). Any edge wakes us.
- LSM tap-Z can also be a wake source (INT1 on GPIO8) — see Stage 10 §9 line about wake-on-motion.

**Concrete code changes:**
1. Add `s_input_active_until_ms` — bumped to `now + 2000` on any encoder/button event.
2. In the task loop, choose `vTaskDelay(pdMS_TO_TICKS(now < s_input_active_until_ms ? 5 : 50))`.
3. In `field_capture_init()`, `esp_sleep_enable_ext1_wakeup(1 << PIN_BUTTON | 1 << PIN_ENC_A | 1 << PIN_ENC_B, ESP_EXT1_WAKEUP_ANY_LOW)`. Note ESP32-S3 EXT1 needs RTC-capable GPIOs — verify all three qualify.
4. Encoder polling remains software (feedback_encoder_polling.md prohibits ISR), but the wake trigger is fine — first tick after wake resumes the state machine cleanly.

**Expected delta:** with tickless idle actually engaging in STANDBY, ~40-60% baseline reduction is plausible. Rerun `batt_test=1` → target **≤50 mA baseline**.

**Risks:**
- Encoder state machine (feedback_encoder_polling.md) needs 15 ms detent-rest confirmation. 50 ms poll means edge-of-edge cases could miss a detent — the wake-on-any-edge should catch the leaving-rest transition and re-enter 5 ms poll immediately, but this needs bench verification.
- Any recording session needs the fast poll — extend `s_input_active_until_ms` for the whole recording, not just per-input.

### Block C — Shutdown watcher: interrupt-driven button  (small; nice bonus)

**Current:** `task_shutdown_watcher_fn` polls GPIO16 at ~5 ms cadence for the 4 s hold detection. That's another 200 wake-ups/s blocking light-sleep.

**Approach:** attach a rising-edge OR falling-edge GPIO ISR to GPIO16 that sends a task notification. Task blocks on `xTaskNotifyWait(portMAX_DELAY)` between events. When the notification fires, poll fast during the hold to draw the LED ramp; return to blocking after release. Uptime cap check becomes a `vTaskDelayUntil` for 60 s max intervals.

Combines with Block B: with both tasks quiet in STANDBY, the CPU actually gets to idle.

**Risks:** ISR must not conflict with the field-capture task's polled button — one owner per GPIO. Simplest is: watcher owns the ISR, and it publishes a `g_watcher_button_state` global that field-capture reads. Refactor for the mutex.

### Block D — Per-driver PM locks + sensor-hub audit  (medium; long tail)

For each driver, verify that active use grabs `ESP_PM_APB_FREQ_MAX` around the critical section:
- **WS2812 (RMT)** — must grab lock before `rmt_transmit`, release after. Otherwise LED colour changes glitch when DFS drops APB.
- **SDMMC** — periph clock derived from APB; write path needs `ESP_PM_APB_FREQ_MAX`. Losing this is a corruption risk on FatFs metadata.
- **PDM mic** — `i2s_std_channel_read` needs APB stable during read.
- **I2C** — the driver already handles APB changes gracefully on S3.
- **UART0 console** — same.

Audit + wrap each. Small individual changes, adds up.

Bonus: enable **sensor-hub mode 2** on LSM6DSV16X to have LSM master the LIS3MDL reads. Saves an ESP-side I2C task. Marked "Med / Low" in Stage 10 §9 — worth reconsidering with PM in mind because idling a CORE 0 task removes one wake-source-per-100ms.

---

## 3. Deep-sleep window (Item D+, once wearable pattern is clear)

Once wake-on-motion is wired, the "watch on desk, not moving for 5+ minutes" case can go into esp_deep_sleep_start() with LSM6DSV16X INT1 (GPIO8) as wake source. Target sleep current ≈ 100 µA (chip + BQ + PCF quiescent + LSM motion detect). Wake latency ≈ 500 ms tolerable if user glances / picks up device.

Ordering: **do Blocks A → B → C → D first, measure each**, then add deep-sleep as a separate patch. Deep-sleep involves state reconstruction on wake and is where bugs love to hide.

---

## 4. Measurement protocol

For each block:

1. Flash. `BATT_TEST ON`. Full charge, unplug, walk away.
2. Note baseline current from ammeter (USB path, right after charge termination).
3. Note runtime from CSV last-row timestamp − charge-off timestamp.
4. Add row to the table below.

| Block           | fw       | Baseline mA @ USB | Battery runtime            | Delivered mAh | Notes |
|-----------------|----------|-------------------|----------------------------|----------------|-------|
| pre-Item-D      | 0.4.1    | **106 mA**        | **1 h 52 m** (`batt_0035`) | 199 mAh @106 mA | Vbat 4082 → 2560 mV cutoff. SoC temp ~43 °C during discharge. |
| A. PM+DFS       | 0.4.3    | (not remeasured)  | **1 h 51 m** (`batt_0042`) | 195 mAh @106 mA | No runtime change. SoC ~42 °C. idle_pct not yet in CSV. |
| A. PM confirmed | 0.4.5    | (not remeasured)  | **1 h 52 m** (`batt_0050`) | 197 mAh @106 mA | **SoC temp dropped to ~36 °C** (thermal signature of PM working) but **runtime unchanged**. `idle_pct=99%` throughout — PM engaging as designed, savings masked by non-CPU draw. |
| A. RMT+SDMMC unlock | 0.4.10 | **~81 mA**       | (pending discharge)         | (pending)       | **First real Block-A win.** SDMMC unmount between samples + RMT enable/disable per push released both blocking PM locks. Ammeter dropped from 106 → 81 mA immediately on plug-in with `batt_test=1`. **BUT**: light-sleep also gates USB-Serial-JTAG → serial soft-lock. Recovery required GPIO0 force-download OR the 0.4.11 VBUS-safety-override below. |
| A. VBUS safety   | 0.4.11 | 81 mA (unchanged) | (pending)                   | (pending)       | First VBUS-detection attempt: if USB present at boot, SKIP batt_test entirely. Ivan corrected: he wants batt_test to still run so charging behavior is logged. Superseded by 0.4.12. |
| **A. VBUS-aware PM lock** | **0.4.12** | **81 mA + 24-mA re-boost when VBUS-in** | **2h 44m** (`batt_0081`) | **288.6 mAh @106 mA** | **Definitive Block A finish.** batt_test always runs; a `NO_LIGHT_SLEEP` PM lock is held only while `power_good` is true. Plug USB → lock acquired within one 10 s tick → serial responsive. Unplug → lock released → light-sleep engages. Bench PM_DUMP over ~13 min after unplug: `Mode SLEEP 80 %`, `light_sleep_counts=124433` (up from 11 pre-fix). Full discharge: **+52 min runtime (+47%), +90 mAh delivered (+45%) vs pre-PM baseline**. SoC thermal peaked <40 °C (was 43 °C pre-PM). RMT-enable/disable per push + SDMMC unmount per sample also released. |
| B. Event wake   |          |                   |                            |                |       |
| C. IRQ button   |          |                   |                            |                |       |
| D. PM locks     |          |                   |                            |                |       |
| + deep-sleep    |          |                   |                            |                |       |

### PM-diagnosis (post-batt_0042, 2026-07-24)

Block A shipped with all correct config (`CONFIG_PM_ENABLE=y`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, `CONFIG_PM_DFS_INIT_AUTO=y`, `IDLE_TIME_BEFORE_SLEEP=3`) plus PM locks on WS2812/SDMMC. Boot log shows the kernel armed correctly (`Light sleep: ENABLED`). Runtime is unchanged from pre-PM.

The 0.4.2/0.4.3 batt_test CSV recorded `cpu_mhz = 240` on every sample row, but that reading is taken while the sampling code is executing on the CPU — inconclusive on whether DFS drops between samples.

**Prime suspect: a driver is holding `ESP_PM_NO_LIGHT_SLEEP`.**

Highest-probability culprit: **`usb_serial_jtag_driver_install()`** in `task_rtc_cli_fn`. The ESP-IDF USB-Serial-JTAG driver on IDF 5.5 acquires a `NO_LIGHT_SLEEP` lock so it can service host RX at any moment. We install it unconditionally at boot, on every boot. That would fully explain a 0 % delta from PM.

Other candidates to rule in/out with the diagnostic below:
- I2C drivers holding APB locks (unlikely to be `NO_LIGHT_SLEEP`).
- RMT (WS2812) — we already added a per-transmit lock; not held between transmits.
- SDMMC — holds `APB_FREQ_MAX` while mounted, not `NO_LIGHT_SLEEP`. Should not block CPU sleep on ESP32-S3.

### Diagnostic path (fw=0.4.5)

Two orthogonal instruments now available:

1. **`idle_pct` column** in BATT_TEST CSV (fw=0.4.4 onwards). Direct signal: high (>80 %) = PM is sleeping between samples; low (<10 %) = PM blocked.
2. **`boot_pm_dump_locks()`** printed at boot (fw=0.4.5). Requires `CONFIG_PM_PROFILING=y` to output actual holders; without it prints a stub line.

**Next click for the operator:** `Component config → Power Management → [*] Enable profiling counters for PM locks` → `CONFIG_PM_PROFILING=y`. Then reflash 0.4.5 and read the `PM lock inventory` block in the boot log. If USB-Serial-JTAG appears there — that's confirmed. Fix path: guard the driver install with an NVS toggle, or add a `USJ ON/OFF` runtime switch that uninstalls the driver on demand (RTC CLI would become BATT_TEST-only casualty during power runs).

### Block A resolution plan (once diagnosis lands)

- **If USJ is the blocker:** add an NVS flag `sys.pm_no_console` that, when set, skips the `usb_serial_jtag_driver_install()` in `task_rtc_cli_fn`. Documented consequence: no serial input until reboot with flag off. Retest batt_test. Expect several-hour gain.
- **If SDMMC is the blocker:** switch the SDMMC lock from `APB_FREQ_MAX` to conditional (release on unmount, don't hold across the whole batt_test loop). More complex.
- **If nothing obvious holds NO_LIGHT_SLEEP:** the kernel might be blocked by ISR service depth or GPIO wake config. Deeper investigation with tracing.

### batt_0050 findings — PM works, savings hidden by peripheral load (2026-07-25)

The 0.4.5 run definitively confirms via `idle_pct=99%` + PM lock inventory dump that PM is engaging as designed. Two orthogonal signals prove it:

1. **`idle_pct=99%` in every CSV row** — CPU is genuinely asleep 99% of wall time between samples.
2. **SoC die temperature dropped from ~42-43 °C (batt_0035/batt_0042) to ~36 °C (batt_0050)** — the CPU is not warming itself. Real thermal evidence of light-sleep.

Yet the ammeter reading and battery runtime are unchanged. Interpretation: **the CPU's contribution to the 106 mA baseline is less than we assumed**, and the bulk of the draw is fixed-peripheral overhead that PM can't touch. FreeRTOS was already parking the S3 into WAITI before PM_ENABLE; going from WAITI → light-sleep saves ~5-8 mA on the CPU domain, which is below the ammeter's resolution against a background of 25-45 mA of "always-on" peripheral load.

### Suspected non-CPU drains (Ivan's bench observation 2026-07-25)

Ranked by likelihood of being the "missing 5-30 mA":

| Suspect | Est. mA | Attribution |
|---|---:|---|
| **Flashlight LED glowing at duty=0** | 5-10 | Ivan's visual confirmation. Hardware leakage in GPIO41 drive; `flashlight_hard_off()` (added 0.4.4, not yet wired into batt_test) would fully halt LEDC. |
| **Sensor chips still in HP mode when brokers "parked"** | 3-8 | `broker_xxx_set_enabled(false)` only stops the TASK from reading. LSM6DSV16X stays at HP+120/240 Hz (~500 µA), BME688 in whatever last mode, etc. Needs per-driver "chip park" API. |
| **SDMMC host mounted continuously** | 5-10 | Held for entire 2 h batt_test to log 10 s samples. Could unmount between samples. |
| **USB-Serial-JTAG active** | 5-10 | Would sacrifice serial access if uninstalled. |
| **PSRAM refresh** | ~5 | Mandatory; cannot disable at compile-time without removing PSRAM support. |
| **BQ25619 + LDO + board LDOs** | 2-8 | Fixed; hardware-level. |

Sum: **25-45 mA of "PM can't touch" load**. Reconciles with ammeter 106 mA baseline and PM confirmed engaging.

### Battery capacity caveat

Nameplate 360-400 mAh; batt_0050 delivered 197 mAh (~55%). Even accounting for cheap-LiPo 60-80% deliverable, this is low. Two possible root causes: aged/mislabeled cell OR discharge current higher on battery than the 106 mA measured on USB. Definitive test: **multimeter directly at battery terminal during discharge**. Also worth a fresh-cell swap.

### 0.4.6 target — actual power moves

1. **Wire `flashlight_hard_off()` into `run_battery_test_mode()` entry.** One line. Immediately tests the flashlight-glow theory.
2. **Add per-driver "chip park" on `set_enabled(false)`**. Start with LSM6DSV16X (biggest single-chip suspect). Write `CTRL1=0x00 | CTRL2=0x00` on disable; restore HP config on enable.
3. **Bench-verify with multimeter at battery.** Compare batt_test drain before/after each fix.

### 0.4.7 findings — real root causes located, fixed for RMT (2026-07-25)

`PM_DUMP` on running device revealed the actual PM lock holders:

```
sdmmc     APB_FREQ_MAX  Active=1  Time=100%  ← held whenever SD mounted
rmt_0_0   CPU_FREQ_MAX  Active=1  Time=100%  ← held for entire session
Mode SLEEP    40M   0 %          ← real light-sleep only during boot
light_sleep_counts: 9            ← nothing added after tasks started
```

**Two blockers, in order of impact:**

1. **`rmt_0_0` holding `CPU_FREQ_MAX` permanently** — the ESP-IDF RMT driver acquires this on `rmt_new_tx_channel()` when the clock source is APB, so bit timing stays deterministic through DFS. **Fixed in 0.4.7:** switched `clk_src` to `RMT_CLK_SRC_XTAL` (40 MHz crystal, XTAL/4 = 10 MHz still hits WS2812B's 400/800 ns symbols). Crystal is DFS-independent → driver does not acquire the lock. Verify via `PM_DUMP`: `rmt_0_0` line should show `Active=0` or disappear entirely.
2. **`sdmmc` holding `APB_FREQ_MAX` permanently** — held from `sdcard_mount()` to `sdcard_unmount()`. Blocks APB from dropping and (on ESP-IDF 5.5) blocks auto-light-sleep from engaging. Not fixed in 0.4.7 yet — the fix path is per-writer: batt_test could unmount/mount around each 10 s sample (200 ms remount latency is tolerable). BLACKBOX at 10 s cadence same story. Higher-rate writers (MLC, WAV) keep the mount because remount overhead exceeds the sample interval.

**Predicted Block A after 0.4.7:** rmt_0_0 released → light-sleep should engage more often. But SDMMC-APB is still held during any batt_test / BLACKBOX run. Real runtime gain depends on how strict ESP-IDF's sleep-refusal is with APB_FREQ_MAX alone (some IDF versions do sleep with APB pinned, some don't). The next PM_DUMP after 0.4.7 will show the truth.

### Key insight — idle_pct vs actual light-sleep

`idle_pct=99%` (from `ulTaskGetIdleRunTimeCounter`) means the CPU is executing the FreeRTOS Idle task 99% of the time. That's NOT the same as actual light-sleep engagement. The Idle task runs `WAITI` (wait-for-interrupt) when PM can't sleep — CPU clock stays on. Real light-sleep is what shows up in the PM `Mode SLEEP` percent + `light_sleep_counts`. Both need to move for battery gains.

**batt_0050 lesson:** idle_pct=99% is a necessary but insufficient signal. From now on the definitive Block-A verification is `Mode SLEEP > 50%` in `PM_DUMP` output during a batt_test.

---

## 5. Non-goals for Item D

- **WiFi power** — not in scope (WiFi off in this project). BT off too. Zero draw from radios.
- **BQ25619 quiescent** — hardware-level, we can't touch without swapping the chip. BQ25896 for mk2 will help here.
- **WS2812 quiescent when off** — the LED IC pulls ~1 mA even when at (0,0,0). To eliminate: mk2 could gate its supply through a MOSFET on a GPIO. Firmware-only: no fix.
- **USB-Serial-JTAG "off"** — the peripheral can be uninstalled but esptool will complain on the next reflash. Not worth it in dev; consider for shippable firmware.

---

## 6. Order of implementation (target sequence)

1. **Block A** — enable PM. Verify LED / SDMMC don't glitch. Measure. Small delta expected.
2. **Block B** — event-driven field-capture. Verify STANDBY light-sleep engages (log wake reason). Measure. Big delta expected.
3. **Block D partial** — WS2812 + SDMMC PM locks. Fixes any glitches from Block A. No delta expected on baseline (only during active LED/SD).
4. **Block C** — ISR button. Measure. Smaller delta.
5. **Deep-sleep on inactivity** — Item D+ once we know the wearable's actual usage pattern.

Each block is one commit + one battery-test run. Data goes in §4.

---

## 7. What blocks the plan today

- Need the current `batt_0025.csv` full-discharge number first — that's Item D's baseline. In progress.
- Ivan's next-session choice: after the baseline lands, decide whether to do Item D or Item E (MLC classifier) first. Item E depends on Ivan's data collection (labelled walking / still / running sessions), which is now unblocked by Item C + FatFs mtime fix.
