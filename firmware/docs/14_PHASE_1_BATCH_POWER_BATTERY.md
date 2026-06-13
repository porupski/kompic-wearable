# PHASE 1 BATCH — POWER + BATTERY
**Assigned:** 10 June 2026
**Firmware version:** iv7.1.f0.0
**Master instructions:** docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md (required reading)

---

## MODULES TO PORT (in order)

### Module 1: BQ25619 (Charger + Fuel Gauge)

**Hardware spec from v7.2:**
- Bus: I2C bus 2 (GPIO4 SDA, GPIO5 SCL, 400 kHz)
- Address: 0x6A
- INT pin: polled via I2C (no GPIO INT; read status registers)
- QON pin: dual-wired to GPIO16 (button input, ship-mode exit trigger)
- BATSNS: voltage/SoC read via I2C (no external ADC)
- Boost output: 5V PMID (FW-gated for MAX30101 LEDs, WS2812, flashlight)

**Code audit:**
Old code: ETA6098 + ADC battery (dead). BQ25619 is completely new — I2C charger with built-in fuel gauge, ship-mode support, boost control. No code to reuse.

**Implementation scope:**
- Register map from BQ25619 datasheet (voltage, current, SoC, charging state, faults)
- I2C polling task (1 Hz) reads BATSNS + status, writes broker
- Boost control: FW enables/disables PMID via I2C register (for LED/flashlight power gating)
- Ship-mode entry: write to BQ25619 REG_RST.bit3 (hardware power-off)
- Profiling: boot init, per-read I2C cost, broker write overhead

**Test harness:**
- Can we initialize I2C + BQ25619?
- Can we read BATSNS (voltage in mV, SoC in %)?
- Can we detect charging state (is_charging flag)?
- Can we read fault register?
- Can we enable/disable boost (PMID)?
- Can we trigger ship-mode entry (write to REG_RST)?
- Measure: I2C transaction times, boot-to-ready

**Deliverables:**
- `/docs/porting/BQ25619_2026-06-10_iv7.1.f0.0.md`
- `/components/bq25619/bq25619.{c,h}` (register map, I2C wrappers, boost control, ship-mode)
- `/components/bq25619/battery_tile.{c,h}` (display voltage/SoC/charging state)
- `/components/bq25619/CMakeLists.txt`
- `/test/test_bq25619.c` (standalone, I2C init + BATSNS read + boost toggle + ship-mode verify)

---

### Module 2: Boot Power Button + Ship Mode Logic

**Hardware spec from v7.2:**
- GPIO16 (BQ_BUTTON): button input, RTC wake source, dual-wired to BQ25619 QON
- GPIO0 (DRV_EN): haptic enable, boot strap, FW drives it low (see below)
- GPIO41 (GPIO_FLASHLIGHT): flashlight LED control (LEDC PWM, Phase 4)
- No power latch GPIO (old design artifact; dead)

**Code audit:**
Old code: power latch on GPIO41, button polling on GPIO40. Dead. New design: GPIO16 button (ISR-driven), BQ25619 ship-mode (via I2C), no latch.

**Implementation scope:**
- GPIO16 ISR: button press edge detection
- Task: measure hold duration (use `esp_timer_get_time()` for sub-ms precision)
- Short press (~200ms): toggle display / wake from STANDBY (Phase 1.5)
- Long press (~3s): trigger ship-mode via `bq25619_enter_ship_mode()` + shutdown overlay
- Haptic feedback on press (via DRV2605, Phase 4)
- GPIO0 (DRV_EN): drive LOW at boot (strap-only pull-up, must be driven by FW to hold haptic enabled)
- Profiling: ISR latency, hold-detection accuracy, broker update time

**Test harness:**
- Can we initialize GPIO16 ISR?
- Does a button press fire the ISR and log the edge?
- Can we measure hold duration correctly (short vs long)?
- Can we call `bq25619_enter_ship_mode()` and verify the BQ register write?
- Does GPIO0 drive LOW correctly?
- Measure: ISR latency, timer precision, broker write cost

**Deliverables:**
- `/docs/porting/BootPower_2026-06-10_iv7.1.f0.0.md`
- `/components/boot_logic/boot_power.{c,h}` (rewritten: GPIO16 ISR, hold-duration task, ship-mode call)
- `/components/boot_logic/boot_tasks.c` (add `task_power_btn` to task table, prio 5)
- Modify `/main/main.c` to call GPIO0 setup at boot (DRV_EN low)
- `/test/test_boot_power.c` (standalone, GPIO16 button emulation + hold detection + BQ ship-mode trigger)

---

## OUTPUT

You will produce **2 dated .md files** in `/docs/porting/`:
- BQ25619_2026-06-10_iv7.1.f0.0.md
- BootPower_2026-06-10_iv7.1.f0.0.md

Plus driver skeletons in `/components/` and test harnesses in `/test/` for each.

**Profiling template must be filled:**
- Boot-time cost (I2C init + BQ init, GPIO setup)
- Per-operation cost (voltage read, SoC read, boost toggle, ship-mode trigger)
- Memory (task stack, driver state)
- Current draw estimate (TBD; measure on real hardware)

**Defects logged** as `[DEFECT-NN]` in each .md.

**Commit strategy:** one commit per module (2 total), or one commit at the end. Your choice.

---

## DEPENDENCY NOTES

- BQ25619 must come before BootPower (BootPower calls into BQ25619 API for ship-mode)
- BootPower is a rewrite of old boot_power.c; the old button/latch logic is completely replaced
- GPIO0 (DRV_EN) setup in main.c happens at app_main() before any tasks start

---

## REMEMBER

- Reference v7.2 §GPIO ASSIGNMENT (GPIO0, GPIO16, GPIO41), §POWER ARCHITECTURE (BQ25619 boost/charge), §I2C BUS ASSIGNMENT (bus 2)
- BQ25619 datasheet is your source of truth for register map
- Profiling is real: `esp_timer_get_time()`, ISR latency measurement, don't guess
- Test harnesses are standalone and reproducible
- Live in the .md. All work is visible, all decisions documented

Go.
