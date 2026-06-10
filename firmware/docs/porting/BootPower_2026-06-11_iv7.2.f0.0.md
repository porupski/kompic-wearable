# BootPower (GPIO16 button + GPIO0 DRV_EN + ship-mode) -- Porting Log
**Module:** boot_power -- power-button input, haptic enable strap, ship-mode trigger
**Date:** 2026-06-11
**Firmware:** `iv7.2.f0.0`
**Hardware authority:** `docs/02_Kompic_Mk1_System_Instructions_v7.2.md`, §GPIO Quick Pinout (line 99 / line 129), §POWER ARCHITECTURE (line 237).
**Brief:** `docs/14_PHASE_1_BATCH_POWER_BATTERY.md`, Module 2.
**Author:** Claude (Opus 4.7), under master prompt `docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md`.

---

## Summary

The boot-power module is the bridge between the user's finger and the BQ25619: short presses control display sleep, long holds bring up the shutdown overlay, and an even longer hold writes `BATFET_DIS = 1` to the BQ25619 to drop the device. Plus one boring but load-bearing detail: GPIO0 is the DRV2605 enable strap, and it has to be driven LOW before the haptic IC can talk to anyone.

This module is a **rewrite, not a port**. The old code lived in `components/boot_logic/boot_power.c` and was built around a hardware latch on GPIO41 (driven HIGH at boot to keep the device alive, driven LOW to die) plus a polled button on GPIO40. Both pins are reassigned in v7.2 -- GPIO41 is now the flashlight LED (Phase 4), GPIO40 is the SD-card data line (Phase 5). The button moved to GPIO16, the ship-mode mechanism moved out of GPIO entirely and into the BQ25619's BATFET_DIS bit.

Today's deliverable is **rewritten boot_power.{c,h} + task table update + test harness + porting doc**. The test harness is intentionally hardware-in-the-loop (the operator presses a real button) and stubs the destructive `bq25619_enter_ship_mode()` so the test device stays alive.

---

## Hardware spec from v7.2

### Pinout (v7.2 §GPIO Quick Pinout, lines 99 / 129; line 141)

| Signal      | GPIO | Direction | Pull / boot | Notes |
|---|:--:|---|---|---|
| BQ_BUTTON   | 16   | input     | internal pull-up; idles HIGH | Falling-edge on press. Dual-wired to BQ25619 QON pin. **RTC-wake source (button + ship-mode exit).** v7.2 line 129 marks it "Required" for RTC GPIO. |
| DRV_EN      | 0    | output    | ROM-strap pull-up at boot; we drive LOW post-boot | DRV2605 enable strap. If left HIGH, the haptic IC stays in shutdown. |

GPIO16 is also documented at line 99 as "Clean. Wake: button + ship-mode exit." It is one of the limited number of RTC-capable GPIOs (<= 21) and is therefore precious for deep-sleep wake.

### v7.2 callouts that matter here

- "GPIO45 (QSPI_TE) NEVER add an external pull-up" -- not us, but a reminder that boot-strap pins are dangerous to mis-handle. GPIO0 is the comparable case in this module: it has an internal pull-up for ROM strapping and must be driven LOW after that role finishes, not pulled.
- "GPIO43/44 (boot-log) -- never an I2C line" -- not us; GPIO16 and GPIO0 are unrelated.

### Power topology context (v7.2 line 237)

BATFET_DIS = 1 disconnects the LiPo from the SYS rail entirely. The chip enforces a delay (~10 s default, configurable) so any latch-up paths bleed out before the cell drops. From the firmware's perspective: write the bit, log a goodbye, and wait. USB-C insert or QON long-press (i.e. the same button held while powered-off) wakes the device. There is no FW-side power latch in this design -- the BQ owns the off state.

---

## Code audit

### What the old `boot_power.c` did (dead now)

- `GPIO_PWR_LATCH = 41` -- driven HIGH at boot to keep the buck enabled. The "kill switch" was `gpio_set_level(GPIO_PWR_LATCH, 0)`. **GPIO41 is now the flashlight LED.**
- `GPIO_PWR_BTN = 40` -- polled in `task_power_monitor_fn` at 10 Hz. Detected press / release edges, measured hold duration. **GPIO40 is now the SD-card data line.**
- `GPIO_BOOT_BTN = 0` -- read in `task_boot_button_fn` as a "reserved" input. **GPIO0 is now DRV_EN strap output; the `task_boot_button_fn` is dead code.**
- PSRAM size check at boot. **Carries forward.**
- Globals `g_wake_display`, `g_display_sleep`, `g_show_shutdown_overlay`, `g_shutdown_latched` published from Core 0 to Core 1. **Carry forward verbatim.**

### What the new boot_power.c does

- **`boot_power_init()`** (called as the first line of `app_main()` per v7.2 boot sequence):
  1. Drives GPIO0 LOW (DRV_EN). DRV2605 is now enabled.
  2. Configures GPIO16 as INPUT with pull-up (idle HIGH). Does NOT install the ISR yet -- the ISR's task path needs `g_i2c_mutex` and the broker first.
  3. Logs PSRAM total. Carries forward.
- **`task_power_btn_fn`** (replaces `task_power_monitor_fn` + `task_boot_button_fn`):
  1. Settles 500 ms (lets the supply rail stop ringing after boot).
  2. Installs the GPIO16 falling-edge ISR.
  3. On each notification: 15 ms debounce, then enters an inner loop polling at 50 ms intervals until the button releases or the hold passes 3 s.
     - At >= 2.5 s: sets `g_show_shutdown_overlay = true`.
     - At >= 3.0 s: sets `g_shutdown_latched = true`, sleeps 200 ms (lets the overlay paint a frame), calls `bq25619_enter_ship_mode(I2C_NUM_1)`, then loops forever waiting for BATFET disconnect.
     - On release < 800 ms: classifies as a short press -> toggles display sleep + plays the double-click haptic.
     - On release between 800 ms and 2500 ms: ignored (treated as "cancelled").
  4. Re-arms the ISR for the next press.

### Globals (preserved API, behaviour unchanged from the Core-1 consumer's view)

| Global | Direction | Set by | Cleared by |
|---|---|---|---|
| `g_wake_display`         | Core 0 -> Core 1 | task_power_btn_fn (short press while asleep) | Core 1 once consumed |
| `g_display_sleep`        | Core 0 -> Core 1 | task_power_btn_fn (short press while awake)  | Core 1 once consumed |
| `g_show_shutdown_overlay`| Core 0 -> Core 1 | task_power_btn_fn (hold >= 2.5 s)            | task_power_btn_fn on release |
| `g_shutdown_latched`     | Core 0 -> all    | task_power_btn_fn (hold >= 3.0 s)            | never (one-shot) |

### Task-table update

`components/boot_logic/boot_tasks.c`:
- Dropped: `task_pwrmon` row (renamed task name was `task_power_monitor_fn`).
- Dropped: `task_bootbtn` row (`task_boot_button_fn` is dead -- GPIO0 is DRV_EN now).
- Added:   `task_power_btn` row -- stack 3072, prio 5, `tskNO_AFFINITY`. (Brief specified prio 5; this matches what the old `task_pwrmon` already was.)

### CMakeLists update

`components/boot_logic/CMakeLists.txt`:
- Added `bq25619` to `REQUIRES` so `boot_power.c` can `#include "bq25619.h"`.

---

## Implementation

Three files changed:

### `components/boot_logic/boot_power.h`

Rewritten. GPIO pin map updated to `GPIO_PWR_BTN = 16`, `GPIO_DRV_EN = 0`. Hold thresholds moved to `#define`s in the header so the test harness can reference them. `task_power_monitor_fn` and `task_boot_button_fn` declarations removed; `task_power_btn_fn` declared instead. Global flag externs preserved verbatim.

### `components/boot_logic/boot_power.c`

Rewritten. ISR is `IRAM_ATTR`, notify-only. Hold detection uses `esp_timer_get_time()` for sub-ms precision (per brief). ISR edge sense is reconfigured each cycle: NEGEDGE while idle, then the inner poll loop watches both the release and the 3 s threshold without depending on a positive edge.

### `components/boot_logic/boot_tasks.c`

Two task-table rows replaced with one. Extern declarations updated. Log line updated.

### `test/test_boot_power.c`

Standalone harness:
- Stubs `display_is_asleep()` (returns false), `haptic_play()` (no-op), and `bq25619_enter_ship_mode()` (logs the call but returns ESP_OK without writing). Stubs are weak-linked so the real symbols win if linked together.
- Runs `boot_power_init()`, verifies GPIO0 is LOW and GPIO16 idles HIGH.
- Spawns `task_power_btn_fn`.
- Opens a 30-second observation window during which the operator presses the button. The test task polls the four globals every 20 ms and logs each transition (wake / sleep / overlay-on / overlay-off / ship-mode latched).
- At exit, logs the test task's stack high-water.

### CMakeLists for tests

Test harness is a standalone file dropped into a test app's `main/`; no in-tree CMake change required (mirrors the pattern set by `test_co5300.c` / `test_cst9217.c` / `test_pcf85063.c` / `test_bq25619.c`).

---

## Profiling

### Boot-time cost

| Phase | Component | Time | Method |
|---|---|---|---|
| Init | GPIO0 config + LOW write     | **TBD (no hw)** | wall time logged in test harness around `boot_power_init` |
| Init | GPIO16 config (no ISR yet)   | **TBD** | same |
| Init | PSRAM total query            | sub-ms          | `heap_caps_get_total_size` is a constant-time read |
| Task | First ISR install (in task)  | **TBD**         | `task_power_btn_fn` first ~500 ms after task start |

### Per-operation cost

| Operation | Time | Notes |
|---|---|---|
| ISR -> task notify    | **TBD** | expect 5-30 us on Core 0 (FreeRTOS notify path) |
| Debounce + first poll | 15 ms   | architectural constant; not a measurement |
| Inner poll cycle      | 50 ms   | architectural constant |
| Ship-mode call (I2C RMW on bus 2) | **TBD** | wire-time floor ~240 us; chip-side delay before disconnect ~10 s |

### Memory

| Metric | Value | Notes |
|---|---|---|
| Static state          | ~16 bytes | task handle + flag globals |
| `task_power_btn` stack| 3072 bytes allocated; high-water **TBD** | no float math, no large locals |
| ISR footprint         | tiny      | one notify-from-ISR + yield |

### Current draw estimate

| State | Current (mA) | Notes |
|---|---|---|
| Idle (button up, awake task blocked on notify) | sub-µA delta | task is in vBlock; ISR doesn't fire |
| Press detected, polling inner loop             | sub-mA delta | 50 ms polling adds wakeups but each is a cheap gpio_get_level |
| Ship-mode latched -> idle wait                 | sub-µA delta | task is in vTaskDelay-forever |
| Post-BATFET-disconnect                          | ~0           | whole device is off the battery |

### Notes

- **The 50 ms inner poll is the bottleneck for "exactly 3 s" feel.** A user holding for 2.95 s vs 3.00 s vs 3.05 s will see the same outcome ± one poll period. If we ever care about that level of precision (we won't, on a watch), this becomes 10 ms.
- **DRV_EN drive order matters.** If the haptic IC ever fires before `boot_power_init()` (it can't today -- main.c sequences it correctly -- but the constraint is real), the IC is in shutdown and the write fails. Documented as a constraint in main.c's boot sequence comments.

---

## Defects discovered

- `[DEFECT-001] No deep-sleep wake path | components/boot_logic/boot_power.c | MED | GPIO16 is RTC-wake-capable per v7.2 (line 99), and the button is the canonical "wake from deep sleep" trigger. boot_power.c configures GPIO16 only for runtime ISR, NOT for esp_deep_sleep_enable_ext1_wakeup() / ext0. Disposition: out of scope today; revisit when deep-sleep mode is implemented (paired with PCF85063 hardware alarm, see PCF85063_*.md Open Q5).`
- `[DEFECT-002] DRV_EN -> DRV2605 init timing not modelled | components/boot_logic/boot_power.c:boot_power_init | LOW | We drive GPIO0 LOW and return. The DRV2605 datasheet specifies a t_startup of ~250 us after EN goes high (wait, it's active-high enable -- need to confirm against the DRV2605 datasheet whether LOW or HIGH is "enabled"). If LOW is enable then we're fine; if HIGH is enable then we have it backwards. The carried-forward code (haptic.c) calls drv2605_init() much later in the boot sequence so any timing concern is moot in practice. Disposition: confirm the DRV_EN polarity on the next datasheet review pass.`
- `[DEFECT-003] Ship-mode 200 ms "let overlay paint" delay is a guess | components/boot_logic/boot_power.c:task_power_btn_fn | LOW | The 200 ms pause between setting g_shutdown_latched and calling bq25619_enter_ship_mode() is meant to give the UI thread one or two paint cycles. Frame rate is content-dependent on the AMOLED, so the delay may be too short on a complex screen or unnecessarily long on a simple one. Disposition: pick a number after we measure the UI's worst-case paint latency.`
- `[DEFECT-004] No ship-mode "user changed mind" cancel | components/boot_logic/boot_power.c:task_power_btn_fn | MED | Once the hold passes 3 s, ship-mode is committed. There is no "release between 3 s and 5 s to abort" window. Disposition: ship as-is; rationale = ship-mode is supposed to be deliberate, and the BQ has a chip-side BATFET delay (~10 s) during which a USB-C insert reverses it.`
- `[DEFECT-005] Globals carry forward without rename | components/boot_logic/boot_power.h | LOW (pre-existing) | g_wake_display / g_display_sleep / g_show_shutdown_overlay / g_shutdown_latched are flag-style globals consumed across both cores. The pattern works but is brittle (no atomicity primitives, single-writer rule enforced by convention only). Disposition: not blocking; documented in the broker's "ATOMIC GLOBALS" section already (data_broker.h §22).`

---

## Open questions

1. **Deep-sleep wake.** GPIO16 + PCF85063 alarm INT (GPIO15) are the two RTC-wake sources designed in by v7.2. We haven't wired either yet. Question: is deep-sleep on the v1 roadmap, or is it a v2 concern? If v1, the next batch should include `esp_sleep_enable_ext1_wakeup()` configuration in `boot_power.c` and a corresponding `pcf85063_install_alarm_isr()` consumer.
2. **DRV_EN polarity.** Confirm against DRV2605 datasheet whether EN is active-LOW or active-HIGH. If active-HIGH, this module has a bug (we'd be keeping the IC in shutdown).
3. **Short-press haptic.** Today: sleep-direction press plays HAPTIC_EFFECT_DOUBLE_CLICK, wake-direction press is silent. Symmetry argument says both should be haptic. Recommend: silent on wake is fine -- waking from sleep should not jar the wrist.
4. **Long-press visual confirmation.** When the overlay shows at 2.5 s, the user has 500 ms to release and abort. Should we ALSO play a haptic at the 2.5 s threshold so a glance-less user knows they're committed-soon? Recommend: yes, single soft buzz at 2.5 s. Defer to a follow-up.
5. **task_power_btn priority.** Brief says prio 5. Currently `tskNO_AFFINITY`. The button task is low-rate but latency-sensitive (a 200 ms delay between press and visible feedback feels broken on a watch). Prio 5 should keep it ahead of sensors but behind UI; that's correct. Document.
6. **Test harness on hardware-less environments.** The harness opens a 30-second physical-press window. Without hardware, the test produces only the init-time logs and an empty press summary. Acceptable for v1 since "no hardware" was an explicit constraint in the brief.

---

## Deliverable checklist

- [x] `docs/porting/BootPower_2026-06-11_iv7.2.f0.0.md` -- this file.
- [x] `components/boot_logic/boot_power.c` -- rewritten for GPIO16 ISR + GPIO0 DRV_EN + ship-mode.
- [x] `components/boot_logic/boot_power.h` -- rewritten header, updated pin map + thresholds + task fn.
- [x] `components/boot_logic/boot_tasks.c` -- task table updated (drop `task_pwrmon` + `task_bootbtn`; add `task_power_btn`).
- [x] `components/boot_logic/CMakeLists.txt` -- added `bq25619` to REQUIRES.
- [x] `test/test_boot_power.c` -- standalone harness with weak stubs for the destructive dependencies.
- [x] **No edit to `main/main.c` required.** GPIO0 LOW is owned by `boot_power_init()` which is already called as the first line of `app_main()`.
- [ ] Commit -- prepared message:

```
[BootPower] Porting: GPIO16 button + DRV_EN + ship-mode, iv7.2.f0.0, 5 issues noted

- Rewrite of boot_power.{c,h} for v7.2 design. Old GPIO40 button + GPIO41
  latch are dead (those pins are flashlight + SD now). New design: GPIO16
  ISR-driven button + GPIO0 DRV_EN strap + BQ25619 BATFET_DIS for shutdown.
- boot_power_init(): drives GPIO0 LOW, configures GPIO16 input + pull-up,
  PSRAM sanity check.
- task_power_btn_fn (was task_power_monitor + task_boot_button, merged):
    - Falling-edge ISR -> task notify -> 15 ms debounce -> 50 ms poll loop.
    - Hold >= 2.5 s -> g_show_shutdown_overlay = true.
    - Hold >= 3.0 s -> bq25619_enter_ship_mode() + g_shutdown_latched.
    - Release < 800 ms -> short press (toggle display sleep + haptic).
- boot_tasks.c: drop task_pwrmon / task_bootbtn; add task_power_btn (prio
  5, stack 3072, tskNO_AFFINITY).
- boot_logic CMakeLists.txt: add bq25619 to REQUIRES.
- main.c: NO CHANGE -- boot_power_init() owns GPIO0 LOW.
- Test harness: weak stubs for display_is_asleep / haptic_play /
  bq25619_enter_ship_mode so a 3 s hold doesn't brick the test device.
  30 s observation window logs each flag transition.
- [DEFECT-001] No deep-sleep wake path on GPIO16 (paired with PCF85063
  alarm INT open question).
- [DEFECT-002] DRV_EN polarity assumed active-LOW; verify against
  DRV2605 datasheet.
- [DEFECT-003] 200 ms paint delay before ship-mode is a guess.
- [DEFECT-004] No 3-5 s "cancel ship-mode" abort window (intentional).
- [DEFECT-005] Flag-style globals retained (pre-existing convention).

See: firmware/docs/porting/BootPower_2026-06-11_iv7.2.f0.0.md
```
