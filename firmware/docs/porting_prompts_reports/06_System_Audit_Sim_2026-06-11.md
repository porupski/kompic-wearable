# 06 — SYSTEM AUDIT & INTERACTION SIMULATION
**Kompic̄ Mk I · 11 June 2026 · fw-iv1**
**Sources consulted:** `Kompic_Mk1_System_Instructions_v7.2.md` (hardware law) · `01_Blueprint_Architecture.md` (§4 pipeline, §5 I2C, §8 task table, §9 sleep) · `02_Audit_Report.md` (defect history) · `10_Master_Prompt` (module queue)
**Method note:** all simulation results are model-based (transaction timing from 400 kHz arithmetic + driver overhead; scheduling from FreeRTOS mutex/priority semantics). They bound and rank risks; they do not replace bench measurement. Every current-draw figure is a datasheet-derived estimate marked as such.

---

## 1. SYSTEM IN WHOLE (concise)

ESP32-S3 (dual LX7 @240 MHz, 8 MB octal PSRAM, 16 MB flash), two 4-layer boards, ~380 mAh LiPo. Core 0 = drivers + broker writes; Core 1 = LVGL + broker reads; touch fast path bypasses the broker (lock-free queue). Cross-Driver (XD) events for Core0→Core0 reactions; fusion task for derived values; UI event queue Core0→Core1; service-request queue Core1→Core0 (new, §8).

| Subsystem | Part | Bus / pins | INT (wake) | ACTIVE cadence | Role |
|---|---|---|---|---|---|
| Display | CO5300 AMOLED 410×502 | QSPI SPI2 (9–14), TE=45, RST=3 | — | event-driven | RGB888 UI, brightness via reg 0x51, sleep via SLPIN |
| Touch | CST9217 | I2C1 @0x5A | GPIO6 (tap) | INT, ~100 Hz touched | input, tap-to-wake |
| IMU | LSM6DSV16X | I2C1 @0x6B | GPIO8 (raise) | 50 Hz | motion, gestures, MLC (v2), Qvar ECG host |
| Mag | LIS3MDLTR | I2C1 @0x1C | — | 10 Hz | heading (with IMU tilt-comp) |
| HR/PPG | MAX30101 | I2C1 @0x57 | GPIO7 (sleep-HR) | FIFO drain ~5.9 Hz | HR, SpO₂ (deferred algo) |
| Skin temp | TMP117 | I2C1 @0x48 | — | 0.2 Hz | skin temperature |
| Env | BME688 | I2C1 @0x76 | — | 0.5 Hz forced | T/P/H, gas (BSEC TBD), baro altitude |
| Light | VEML6030 | I2C1 @0x10 | — | 2 Hz | lux, auto-brightness |
| RTC | PCF85063 | I2C1 @0x51 | GPIO15 (alarm) | 1 Hz | timekeeping, alarms, 1PPS-disciplined |
| GPS | MAX-M10S | UART1 (17/18), 1PPS=46 | — | 200 ms drain | position, UTC-before-fix, RTC trim |
| Charger | BQ25619 | I2C2 @0x6A | GPIO16 = button | 1 Hz poll | charge, SoC (BATSNS), ship-mode, 5 V boost |
| Haptic | DRV2605 + LRA | I2C2 @0x5A, EN=GPIO0 | — | on demand | feedback; LRA contains a magnet (§5) |
| Status LED | WS2812 | RMT GPIO42 | — | on demand | 5 V consumer |
| Flashlight | LED | LEDC GPIO41 | — | on demand | 5 V consumer |
| Mic | PDM MEMS | I2S0 (47/48) | — | DMA on demand | capture (v1), VAD/sleep audio (v2) |
| SD | socket | SDMMC (38/39/40) | — | mount on demand | logs, tracks, ML models |
| Crown | encoder + button | A=21 (wake) / B=43 | GPIO21 (bonus) | PCNT, no ISR | navigation |

---

## 2. SIM 1 — I2C CONTENTION (discrete-event, 120 simulated s, ACTIVE workload, finger on screen)

**Model:** txn = bytes × 22.5 µs + 50 µs overhead; non-preemptive bus; priority-ordered mutex queue (FreeRTOS priority inheritance).

| Scenario | Bus-1 util | Touch I2C wait mean / p99 / max | Worst low-prio wait | Max queue depth |
|---|---|---|---|---|
| A — correct BME688 (cfg → release → read) | 6.8 % | 0.02 / 0.71 / **2.33 ms** | 0.25 ms | 2 |
| B — BME688 holds mutex through heater | 14.3 % | 5.9 / **131.6 / 150.6 ms** | 94 ms | 27 |
| C — all tasks phase-aligned at t=0 | 6.8 % | ~0 (touch wins arbitration) | 4.0 ms (TMP117) | 8 |
| Bus 2 (BQ 1 Hz + DRV on demand) | 0.03 % | — | — | no contention case exists |

**Findings:**
- Bus 1 has ~14× throughput headroom; the threat is **latency tails only**, and there are exactly two sources: the MAX30101 FIFO drain (2.35 ms — it *is* the worst-case touch wait) and the BME688 heater if mishandled.
- Scenario B reproduces the old project's touch-lag disease numerically. The "release the mutex while the heater cooks" rule is worth more than every other bus optimization combined.
- Moving DRV2605 to bus 2 was correct: haptic sweeps can hold bus-2 across delays with zero victim.

**Mandates (bake-in §9):** transaction-length cap asserted in the bus manager; MAX30101 FIFO threshold ≤17 samples (a 32-sample full drain = 4.4 ms — legal but doubles the touch tail for nothing); mutexes (not binary semaphores) so priority inheritance exists.

---

## 3. SIM 2 — INTERRUPT CONCURRENCY

**Model:** chain = ISR 5 µs + notify/schedule 15 µs + bus wait + own txn + 10 µs post. Monte Carlo (200 k samples of steady-state bus) + deterministic response-time bounds + simultaneous storm.

| Measure | Result |
|---|---|
| Touch INT → coords-in-queue, mean / p99 / max | 0.35 / 1.02 / **2.67 ms** (budget ≤20 ms tap-to-pixel: 7× margin) |
| Worst-case bound, HR / IMU / RTC chains | ≤ 4.3 ms each (vs 170 / 20 / 1000 ms periods — no overrun possible) |
| Button (GPIO16) | CPU-only, ~0.03 ms; no bus dependency |
| Storm (touch+HR+IMU+RTC+button same µs) | touch done 0.32 ms, HR 2.66, IMU 3.07, RTC 3.35 ms — graceful priority cascade |
| Cross-validation | MC max 2.343 ms vs discrete-event 2.334 ms — independent methods agree |

**Harmony rules derived:**
1. **Crown encoder goes on PCNT**, never per-edge GPIO ISR. Contact bounce can burst at kHz; PCNT makes it zero ISR load and immune to the GPIO43 boot-log edges (additionally ignore GPIO43/44 for the first ~50 ms after boot).
2. **MAX30101 INT is level-low until the FIFO is drained.** After every drain, re-read the status register before re-arming the edge wait — the classic lost-interrupt deadlock on this family. Same defensive pattern for CST9217.
3. **`lv_disp_flush_ready()` lives only in the DMA-complete callback, `IRAM_ATTR`.** GPIO ISR storms add only µs to it — no constraint violated.
4. **Deep-sleep wake:** one `ext1` OR-mask over GPIO {6, 7, 8, 15, 16, 21}; on wake, disambiguate via `esp_sleep_get_ext1_wakeup_status()` in fixed priority: button > alarm > tap > raise > PPG > crown.
5. No two interrupt sources share a handler task; every ISR is one `xTaskNotifyFromISR`. Already law — sim confirms no chain interferes with another's deadline.

---

## 4. DISPLAY PIPELINE BUDGET (arithmetic)

| Quantity | Value |
|---|---|
| Full frame 410×502×3 | 603 KiB → **30.9 ms @ QSPI 40 MHz → 32 fps hard ceiling** |
| 80-line slice | 96 KiB → 4.92 ms per slice |
| Tear-free single-TE-window dirty budget (~12 ms usable of 16.7 ms) | ~234 KiB ≈ **40 % of screen per frame** |
| PSRAM bandwidth consumed by flush | 20 of ~80 MB/s (25 %) — render + flush coexist with headroom |

**Rules:** dirty region per frame ≤ ~40 % of screen, or the frame spills into a second TE period (drops that frame to ~30 fps — acceptable occasionally, forbidden as steady state). Animations and scrolling must be designed under this cap.

**Decision flag (inconsistency in Blueprint):** §4.1 specifies two *full-frame* PSRAM buffers (1.18 MiB) while §4.3 specifies 80-line *double slice* buffers (192 KiB). These are different strategies. **Recommendation: slice double-buffering** — render slice N+1 while DMA flushes N; full-frame canvases add nothing under partial invalidation and cost 1 MiB of PSRAM better spent on ML later. Blueprint to be amended once confirmed.

---

## 5. SENSOR FUNCTION & INTERACTION MATRIX

| Device | Functions | Positive interactions | Negative interactions | Mitigation |
|---|---|---|---|---|
| LSM6DSV16X | motion, steps, raise-to-wake, MLC, Qvar host | aids PPG artifact-cancel, heading, activity, sleep staging | **LRA vibration injects accel noise** (false steps/raise) | XD `HAPTIC_ACTIVE` → discard/flag IMU samples during playback |
| LIS3MDLTR | heading | tilt-comp with IMU | **LRA magnet: DC hard-iron offset + AC corruption while vibrating** | hard-iron cal at first boot (measure LRA offset per audit Q5); XD gate during haptics; verify no range saturation on prototype |
| MAX30101 | HR, SpO₂, sleep HR | IMU-assisted artifact cancel; VEML flags sunlight saturation | LRA vibration = motion artifact; ambient light ingress | XD gate/flag; optical window design (hardware); 5 V LED rail arbitration |
| TMP117 | skin temp | with BME688 T → core-temp estimate, fever context | board self-heating (S3 @240 MHz, charger nearby) biases reading | offset calibration; sample strategy TBD on bench |
| BME688 | T/P/H, gas/VOC, baro altitude | GPS fusion altitude; weather trend; circadian context | **heater can block bus 1** (Sim 1-B); heater warms own die + board | burst transactions (law); Bosch lib compensates die temp; ambient-T offset cal |
| VEML6030 | lux, auto-brightness | RTC → circadian blue-light schedule | **flashlight / WS2812 / AMOLED self-illumination → feedback loop** | XD `LIGHT_SOURCE_ON` freezes auto-brightness; log-domain EMA (per audit) |
| MAX-M10S | position, speed, UTC-before-fix | + baro = robust altitude; 1PPS disciplines RTC; pace for fitness | biggest power consumer (~25–30 mA est.) | profile-gated; hot-start supercap helps duty-cycling |
| PCF85063 | time, alarms | 1PPS trim; timestamps anchor all fusion | — | — |
| BQ25619 | charge, SoC, ship-mode, **5 V boost** | SoC-aware profile degradation | **boost and charge mutually exclusive**; INT not on GPIO (poll) | 5 V rail arbiter service (§9); verify PMID-follows-VBUS-while-charging on bench |
| DRV2605 + LRA | haptic feedback | UX | **perturbs mag, IMU, PPG, mic simultaneously** | single XD `HAPTIC_ACTIVE/IDLE` event consumed by all four |
| WS2812 / flashlight | status, torch | — | 5 V consumers; light pollution → VEML | arbiter + XD light event |
| PDM mic | capture (v1), VAD/snore (v2) | + IMU/HR = sleep staging | LRA vibration = acoustic noise; mic capture is a privacy-sensitive function | XD gate; capture only via explicit service request |
| SD | logs, tracks, models | logging service backend | write bursts on CPU — keep off latency-critical paths | logging service batches, low-prio task |
| Crown encoder | nav, second ECG electrode | wake source (bonus) | GPIO43 boot-log edges | PCNT + boot-window ignore |
| CST9217 | input, tap-wake | — | shares bus 1 with everything | highest bus prio (sim-verified sufficient) |

Address collision 0x5A (CST9217 / DRV2605) is resolved by the two-bus split — **standing rule: these buses are never merged, no device migrates between them.**

---

## 6. FUSION DEPENDENCY GRAPH

| Fusion output | Inputs | Rate | Phase | Notes |
|---|---|---|---|---|
| Altitude | GPS (primary) + BME688 baro (relative) + last-known | 1 Hz | v1 (Ph 3) | baro re-zeroed on every good GPS fix; carries through GPS-off profiles |
| Heading | LSM6DSV16X (tilt) + LIS3MDL (field) | 10 Hz | v1 (Ph 2) | hard-iron from NVS cal |
| HR artifact cancel | MAX30101 + IMU accel | 100/50 Hz NN-aligned | v1.5 | timestamp alignment, not synchronized reads (§7) |
| Activity class | IMU + HR | 1 Hz | v1 (Ph 3) basic; MLC v2 | |
| Sleep staging | IMU + HR + mic (v2) | epoch 30 s | v2 | mic adds snore/respiration |
| Core-temp estimate | TMP117 + BME688 T | 0.2 Hz | v2 | needs bench characterization |
| Circadian / blue-light | RTC + VEML6030 | 1 min | v1 (Ph 4–6) | closes audit bug #3 |
| RTC discipline | 1PPS + PCF85063 | 1 Hz | v1.5 | count-only in Ph 3, trim later |
| Weather trend | BME688 baro history | 10 min | v2 | needs SD or NVS ring |
| SoC-aware degradation | BQ25619 → mode manager | 1 Hz | v1 (Ph 4) | low battery auto-switches profile |

---

## 7. SENSOR READ ORDERING — VERDICT

**No enforced runtime read order is needed.** The broker decouples producers from consumers; ordering reads would buy nothing and create coupling. What *is* needed:

1. **Every broker slot carries a sample timestamp** (`esp_timer_get_time()` at acquisition). Fusion performs staleness checks and nearest-neighbor alignment (e.g. PPG 100 Hz ↔ IMU 50 Hz for artifact cancellation) instead of assuming freshness. This is the single highest-leverage retrofit-avoider in this document — adding it later touches every module.
2. Order matters in exactly two places: **boot init** (v7.2 / Blueprint §7 order stands) and **wake-from-STANDBY re-init** (display SLPOUT sequence last, after sensors resume, mirroring boot).

---

## 8. MODE ARCHITECTURE — PROFILES / SERVICES / PROGRAMS (confirmed 2026-06-11)

**Profiles describe. Services execute. Programs orchestrate.** Two-step rule accepted: a new capability means first a service verb (Core 0), then the program that uses it. No program ever touches hardware.

### Layer 1 — Profiles (pure data, Core 0)
One row per module: `{power_state ∈ ACTIVE/STANDBY/OFF, cadence, wake_mask}`. A minimal mode-manager FSM applies a profile by walking rows and calling each module's power hooks. Adding a profile = one table entry, zero new code.

| Profile | Display | GPS | IMU | HR | Mic | Mag | Env | Light | LEDs/torch | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| ACTIVE | on | off | 50 Hz | on-demand | off | 10 Hz | 0.5 Hz | 2 Hz | on-demand | default |
| DOZE | dim | off | 50 Hz | off | off | off | 0.5 Hz | 2 Hz | off | brightness step |
| STANDBY | SLPIN | off | wake-on-tilt | sleep-HR opt. | off | off | off | off | off | INT-driven only |
| FITNESS | on | **on** | 50 Hz | continuous | off | 10 Hz | 0.5 Hz | 2 Hz | off | + track logging service |
| SLEEP (night) | SLPIN | off | low-ODR | sleep-HR | epoch capture (v2) | off | off | off | off | sleep-staging program |
| SHIP | off | off | off | off | off | off | off | off | off | BQ ship-mode, QON exit |

### Layer 2 — Services (code, Core 0, the baked-in verbs)
Exposed through **one service-request queue (Core 1 → Core 0)** — the mirror of the UI event queue, generalizing the existing settings-saver pattern. v1 verb set: `profile_switch(id)` · `log_start/stop(source_mask, file)` · `haptic_play(effect)` · `mic_capture(duration)` (v2 impl, API reserved) · `rail5v_request/release(consumer_id)` · `time_sync_request`. Verb enum is append-only, like XD ordinals.

### Layer 3 — Programs (code, Core 1)
A program = super-tile: owns screens, reads broker, acts only via service requests. Lifecycle: `register → start (requests its profile) → tick → stop (releases)`. The program registry generalizes the tile registry — one row to add a program. Raw per-sensor tiles are degenerate programs with a debug flag unlocking direct register access (the "peek under the hood" path; debug builds only). Headless programs (no screens) cover background logic like sleep staging.

---

## 9. BAKE IN NOW (each item is expensive to retrofit, cheap today)

| # | Item | Phase | Why now |
|---|---|---|---|
| 1 | **Service-request queue** Core1→Core0 + append-only verb enum | 0 | the pipeline everything in §8 rides on; retrofit = re-plumb every program |
| 2 | **Broker sample timestamps** on every slot | 0 | retrofit touches every module (§7) |
| 3 | Per-module **power hooks** ACTIVE/STANDBY/OFF | 0 (stubs) | profiles are inert without them |
| 4 | **Mode manager + profile table format** | 1 | one FSM, applied via hooks; programs depend on `profile_switch` |
| 5 | **Bus-manager txn-length cap (~2.5 ms, debug assert) + contention counters** | 0 | makes Sim 1-B structurally impossible; counters close audit D-20 |
| 6 | **XD ordinals reserved:** `HAPTIC_ACTIVE/IDLE`, `LIGHT_SOURCE_ON/OFF`, `CHARGER_EVENT`, `PROFILE_CHANGED` | 0 | enum is append-only forever — claim them before drift |
| 7 | **5 V rail arbiter** (refcounted, owns BQ boost vs charge) | 1 | three consumers + a mutual-exclusion constraint = arbiter or chaos |
| 8 | **Generic SD logging service API** (impl Phase 5) | 0 (API) | programs must code against it from day one |
| 9 | **Program registry** (tile-registry generalization) | 0 | shape must exist before the first tile is written |
| 10 | **Crown on PCNT** | 4 | per-edge ISR is a dead end (§3) |
| 11 | **MAX30101 FIFO threshold ≤17 samples** | 3 | caps the only meaningful bus-1 latency tail |
| 12 | **Dirty-region ≤40 % rule** in lvgl_ui | 0 | display budget law (§4) |

---

## 10. POWER REALITY CHECK (estimates — every figure TBD on bench)

| Profile | Est. draw | 380 mAh runtime | Basis |
|---|---|---|---|
| ACTIVE | ~70–100 mA | ~4–5 h screen-on | S3 @240 MHz ~45–65 mA + AMOLED 15–40 mA + sensors ~5 mA |
| FITNESS | ~100–130 mA | ~3 h | + GPS ~25–30 mA |
| STANDBY (v1: no light sleep, CPU idling at 240 MHz) | **~15–25 mA** | **~15–25 h** | S3 idle floor dominates |
| STANDBY (hypothetical, with light sleep between INTs) | ~1–2 mA | weeks-class | sensor LP modes + sleep current |

**Risk flag:** v1's "no light sleep / no DFS" stance makes STANDBY ≈ one day. Overnight sleep-tracking survives, but the watch arrives at morning near-empty. Recommendation (peer pushback on Blueprint §11): pull *automatic light-sleep entry in STANDBY only* forward from "deferred" into Phase 6 scope — it is the single largest battery lever in the system and the wake-source matrix (§3.4) is already designed for it. The careful-approach warning on power management stands: blueprint first, then implementation.

---

## 11. OPEN QUESTIONS / DECISIONS NEEDED

1. **Framebuffer strategy:** confirm slice double-buffering over full-frame pair (§4) → amend Blueprint §4.1.
2. **Light sleep in STANDBY:** accept the §10 recommendation into Phase 6, or accept ~1-day standby for v1?
3. **BME688 gas:** Bosch BSEC (closed-source blob, real VOC/IAQ) vs raw resistance only — decision gates the env tile's feature set.
4. **5 V while charging:** datasheet implies PMID follows VBUS during charge (boost not needed when plugged in) — verify on bench before the arbiter hard-codes it.
5. **Sleep-HR duty cycle:** MAX30101 LED duty in STANDBY drives the sleep-tracking battery cost — needs bench numbers before the SLEEP profile row is final.

---

**Companion artifacts:** `sim_i2c.py` (Sim 1), `sim_interrupts.py` (Sim 2) — both standalone Python, no dependencies, seeds fixed for reproducibility. Suggested repo home: `docs/sim/`.

*End of 06.*
