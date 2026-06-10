# CLAUDE CODE TASK — KOMPIC̄ Mk I FIRMWARE AUDIT & MIGRATION BLUEPRINT
**10 June 2026 · run from: `/home/ivan/esp/ESP32-S3/WS-Touch-LCD-1.69/11_SmartWatch_v5_project_only/`**

## Role

You are an expert embedded systems engineer (ESP-IDF, FreeRTOS, LVGL). You are a
peer, not an assistant. This is an **audit and planning task — you do not write
new firmware code in this task.** Your outputs are: an audit report, a migration
blueprint, and a curated copy of reusable source into the new-project folder.

## Required reading — in this order, before anything else

1. `NEW_PROJECT_FROM_SCRATCH/Kompic_Mk1_System_Instructions_v7.2.md` — the
   **hardware ground truth** for the new device (Kompic̄ Mk I). GPIOs, I2C
   addresses, rails, and the display interface in that file are non-negotiable.
2. The entire old project tree in this folder (~113 files under `components/`,
   `main/`, `partitions.csv`, `sdkconfig`).

## Context

The old project is a **semi-stable, working dual-core build** for a previous
prototype (ST7789 SPI display, CST816S touch, TU10F GPS, QMI8658 IMU, BME280,
MAX30102, BH1750, QMC5883P). It works, but it is old, inefficient, buggy in
places, and had **noticeably laggy touch**. It is being retired.

The new device (Kompic̄ Mk I) keeps almost nothing of the old hardware. The
architecture, however, is largely sound and should carry forward.

### Hardware delta (old → new)

| Subsystem | Old | New (Mk I) | Verdict |
|---|---|---|---|
| Display | ST7789, SPI2 @20MHz, 240×280, RGB565 | **CO5300 QSPI** (GPIO9–14), 410×502, **RGB888 mandatory (COLMOD 0x77)**, TE=GPIO45, RST=GPIO3 | Full rewrite |
| Touch | CST816S @0x15 | **CST9217 @0x5A** (bus 1), INT=GPIO6, RST=GPIO44 | Full rewrite |
| GPS | TU10F | **MAX-M10S**, UART1 GPIO17/18, 1PPS→GPIO46 | Port (NMEA core reusable) |
| IMU | QMI8658 | **LSM6DSV16X** @0x6B, INT1=GPIO8 | New driver, tile pattern reusable |
| Mag | QMC5883P (dead) | **LIS3MDLTR** @0x1C | New driver |
| Env | BME280 | **BME688** @0x76 | Port-ish (Bosch API family) |
| HR/PPG | MAX30102 | **MAX30101** @0x57, INT=GPIO7 | Near-port (register-compatible family) |
| Light | BH1750 | **VEML6030** @0x10 | New driver, tile reusable |
| Skin temp | — | **TMP117** @0x48 | New |
| Haptic | DRV2605 + LRA | **Identical** | Copy |
| RTC | PCF85063 | **Identical** (INT=GPIO15, 1PPS disciplining new) | Copy + extend |
| Charger | ADC battery sense | **BQ25619** @0x6A (bus 2), I2C BATSNS, QON/GPIO16 | New |
| New peripherals | — | SD (GPIO38–40), PDM mic (47/48), WS2812 (42), flashlight (41), crown encoder (21/43), Qvar ECG | New |

Note the **two I2C buses** on the Mk I (bus 1 = GPIO1/2, bus 2 = GPIO4/5) — the
old project's single-bus assumption is dead. Bus topology per the v7.2 doc.

### Architectural rules that carry forward (treat as law)

- Core 0 = hardware drivers + broker writes. Core 1 = LVGL UI + broker reads.
  They never cross.
- Data Broker (mutex, held for memcpy only), read-before-write pattern.
- Cross-Driver (XD) event framework for Core0→Core0 reactions.
- Fusion task for multi-sensor derived values.
- UI Event Queue for Core0→Core1 commands.
- 4-file module pattern: `driver.c/.h` (Core 0) + `driver_tile.c/.h` (Core 1).
- main.c is **orchestrator only**. Adding/removing a sensor module must be
  Lego-grade: register in boot init + tile registry, nothing else changes.
- No `%f` in LVGL. NVS manifest discipline. Source-of-truth hierarchy:
  blueprints > logs > suggestions.

### Mandatory performance directions (from the touch-latency post-mortem)

These are requirements for the new architecture, not suggestions:

1. **Touch is interrupt-driven**, never polled: GPIO6 ISR →
   `xTaskNotifyFromISR` → I2C read in task context.
2. **Touch coordinates bypass the broker mutex**: single-producer
   single-consumer `xQueueOverwrite` (lock-free for this path).
3. **`lv_disp_flush_ready()` is called from the DMA-complete callback**, never
   inline after starting the transfer.
4. LVGL handler loop runs at **≤5ms** period on Core 1.
5. **Partial invalidation is mandatory.** 410×502 RGB888 over QSPI@40MHz is
   ~620KB/frame ≈ 31ms full-frame floor (~30fps ceiling). Full-screen redraws
   on touch are forbidden by design. Double-buffered framebuffers in PSRAM, DMA.
6. Broker mutex held for memcpy only — audit the old code for violations and
   list every one you find.

## Your tasks

### Task 1 — Audit the old project

Read every component. For each module and each piece of infrastructure
(`data_broker`, `cross_driver`, `fusion`, `boot_logic`, `lvgl_ui`, `app_logic`,
each sensor component), classify:

- **COPY** — reusable as-is or with trivial edits (expect: drv2605, pcf85063,
  data_broker, cross_driver, fusion, most of lvgl_ui scaffolding, app_nvs).
- **PORT** — pattern/skeleton reusable, internals rewritten (tu10f→max_m10s,
  max30102→max30101, bme280→bme688, tile files for replaced sensors).
- **REWRITE** — new from scratch (display driver, touch driver, all
  net-new peripherals).
- **DROP** — dead weight (qmc5883p dead-hardware handling, ST7789 anything,
  battery-via-ADC, anything contradicting the v7.2 pinout).

While auditing, also produce a **defect list**: mutex-hold violations, polling
where interrupts should be, blocking calls inside callbacks, stack-size guesses,
magic numbers that belong in Kconfig/headers, single-bus I2C assumptions,
`%f`-in-LVGL violations, anything that smells. Cite file + function for each.

Verify the old project's known open bugs (GPS time gated on fix; auto-brightness
EMA undershoot; blue-light bedtime mode missing) — confirm whether each is
inherited by the new architecture or dies with the old hardware.

### Task 2 — Write the migration blueprint

Author `NEW_PROJECT_FROM_SCRATCH/01_Blueprint_Architecture.md`:

- Target stack: **ESP-IDF (latest stable 5.x), LVGL 9 (latest 9.x), FreeRTOS**,
  dual-core split as above, 240MHz (justify if you propose otherwise).
- Full module inventory for the Mk I sensor suite, each mapped to the 4-file
  pattern, with its bus, address, GPIO, interrupt, and wake-source role pulled
  from the v7.2 doc.
- Display + touch pipeline design honoring the six performance directions.
- Two-bus I2C manager design (bus ownership, who initializes, contention rules
  — note BME688 heater sequences can block tens of ms; design so they can't
  starve anyone).
- Boot/init order, task table (core, priority, stack, period), power-state
  hooks (ACTIVE/STANDBY/OFF per module — scoped, not implemented).
- Sleep/wake design around the RTC GPIO wake sources (GPIO 6/7/8/15/16/21).
- Phased implementation plan: Phase 0 = scaffold + display + touch bring-up,
  then sensors in dependency order. Each phase ends in a bootable build.
- Explicit non-goals for Mk I firmware v1 (WiFi/OTA deferred, telemetry
  reserved, etc.).

### Task 3 — Populate the new folder

Copy everything classified COPY or PORT into
`NEW_PROJECT_FROM_SCRATCH/`, **preserving the old folder structure**
(`components/<module>/…`, `main/`, etc.). For PORT items, copy the old file as
the starting skeleton — do not rewrite it in this task. Rename component
folders to match new hardware where the part changed (e.g. `max30102/` →
`max30101/`), and prefix nothing inside `components/` — numbered prefixes are
for docs only.

Also create:
- `NEW_PROJECT_FROM_SCRATCH/02_Audit_Report.md` — Task 1 output (classification
  table + defect list + bug disposition).
- `NEW_PROJECT_FROM_SCRATCH/03_Migration_Map.md` — old path → new path → verdict
  → one-line reason, for every file in the old tree (all ~113).

### Rules

- **Do not modify anything outside `NEW_PROJECT_FROM_SCRATCH/`.** The old
  project stays untouched.
- Do not write or refactor firmware logic — copy, classify, plan.
- Where the old code conflicts with `Kompic_Mk1_System_Instructions_v7.2.md`,
  the v7.2 doc wins, always. Flag the conflict in the audit report.
- If you hit a genuine ambiguity that blocks classification, list it in a
  "Questions for Ivan" section at the top of the audit report and make your
  best provisional call — do not stall.
- Be terse and technical in the reports. Tables over prose. No fluff.

### Definition of done

`NEW_PROJECT_FROM_SCRATCH/` contains: the blueprint (01), audit report (02),
migration map (03), and the curated source tree — and I can read 01+02 in
under 15 minutes and know exactly what we're building and why.
