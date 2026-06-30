# 02 — Audit Report (v5 → Kompic̄ Mk I)

**Date:** 2026-06-10
**Source:** `11_SmartWatch_v5_project_only/` (~99 source files in `components/` + `main/`, plus root configs)
**Ground truth:** `Kompic_Mk1_System_Instructions_v7.2.md` (iv7.1, 2 June 2026) — wins on every conflict.

---

## Questions for Ivan

1. **Power latch / shutdown model.** Old design latched on via GPIO41 + power button on GPIO40 + a discrete charger with no I2C. Mk I uses BQ25619 with the button dual-wired to GPIO16 (`BQ_BUTTON`) and *ship-mode exit* through BQ. There is no "latch" GPIO. Provisional call: rewrite `boot_power.c` to drive the new model — button GPIO16, hold-3s sends BQ to ship mode (via I2C bus 2), no latch line.
2. **Backlight / brightness.** AMOLED has no PWM backlight; brightness is controlled by display register (0x51 BRIGHT) — not LEDC. Provisional call: kill `backlight_init()` (LEDC), expose `display_set_brightness_pct()` via the new CO5300 driver, keep `g_saved_brightness` semantics intact.
3. **Single I2C mutex vs per-bus.** The v7 doc has **two buses**; both have sensors. Provisional call: replace `g_i2c_mutex` with `g_i2c1_mutex` + `g_i2c2_mutex`, two new-API (`driver/i2c_master.h`) bus handles, each driver declares which bus it lives on. No single global lock — bus 2 (charger + haptic) must not be starved by bus 1 sensor traffic.
4. **Time zone storage.** `g_tz_offset_hours` lives in `ui_broker.c`. RTC stores UTC (Phase 15). Mk I should keep that contract. Provisional call: keep TZ as a UI-owned setting; RTC tile renders local time, alarm matches local time, broker stays UTC.
5. **Compass-tile + LIS3MDLTR re-wiring.** Old code wires `compass_tile_set_calibrate_callback(on_mag_calibrate_requested)` from `boot_hw_init.c`. LIS3MDLTR runs at a different scale (±4 gauss vs ±30 gauss QMC) and gets a known small DC offset from the LRA magnet. Provisional call: PORT the calibration UI + NVS storage as-is, REWRITE the chip driver, **add LRA-offset measurement to first-boot calibration**.
6. **Boot-time RTC seeding.** Code on `boot_hw_init.c:293` uses `mktime()` then sets `TZ=UTC0`; a commented `timegm()` line right below it says that is the fix. The fix is not actually applied at boot — only `pcf85063_sync_utc()` (Phase 15) does it right. Provisional call: in the new project apply the `timegm()` path everywhere (boot seed + GPS atomic sync + any future BLE sync). Flag the boot path as still-broken in the old tree.
7. **`%f` in LVGL.** No actual violations exist — only warning comments. Confirmed clean across all tiles. Carry the rule forward via `lv_conf.h` `LV_USE_FLOAT_PRINTF=1` plus the existing "snprintf-first" comments in each tile header.

---

## Section A — Classification Table

| # | Module / file | Verdict | New name (folder) | One-line reason |
|---|---|---|---|---|
| 1 | `main/main.c` | **PORT** | `main/main.c` | Orchestrator pattern is correct; reorder steps for two-bus + new display + new power path; drop ST7789/CST816S calls. |
| 2 | `main/lv_conf.h` | **PORT** | `main/lv_conf.h` | Flip to RGB888 (CO5300 mandates COLMOD 0x77); reduce/clear `LV_COLOR_16_SWAP`; reconsider widget set; bump default refresh period. |
| 3 | `main/CMakeLists.txt`, `idf_component.yml` | **COPY** | unchanged | Trivial — re-curate `idf_component.yml` for the new touch/display drivers later. |
| 4 | `partitions.csv` | **COPY** | unchanged | 16 MB layout with OTA stays valid for the same flash size. |
| 5 | `sdkconfig*` | **DROP** | regen via menuconfig | Old defaults reference deprecated I2C, ST7789, SPI host 2 dedicated to display. Start from an IDF 5.5+ default and re-add: PSRAM octal 80 MHz, FREERTOS_HZ=1000, LVGL v9 RGB888, MMU/IDF features. |
| 6 | `components/data_broker/data_broker.{c,h}` | **COPY** | `components/data_broker/` | Macro-based broker with single mutex held only for memcpy. Pattern is correct. Expand for new modules (touch, charger, mic, encoder, …). |
| 7 | `components/data_broker/power_flags.h` | **PORT** | same | Strip stale comments (`g_screen_locked`); rework consumers around BQ-button + AMOLED SLPIN sleep state. |
| 8 | `components/data_broker/ui_event.h` | **COPY** | same | Core 0 → Core 1 event queue is the canonical command path. Extend the enum. |
| 9 | `components/cross_driver/*` | **COPY** | same | Pub/sub framework is small, correct, and Core 0–only. Use as-is. |
| 10 | `components/fusion/*` | **PORT** | same | Skeleton + altitude arbitration logic is correct, but the ENV branch is commented-out; finish it for BME688 and add HR/IMU activity classification. |
| 11 | `components/boot_logic/boot_hw_init.{c,h}` | **REWRITE** | `components/boot_logic/` | Replaces legacy `driver/i2c.h` with new-API `i2c_master`, two buses, new device table, new WHO_AM_I values, and removes inline `boot_cst816d_configure()`. |
| 12 | `components/boot_logic/boot_display.{c,h}` | **REWRITE** | same | ST7789 SPI is gone. Build new CO5300 QSPI panel driver, RGB888 framebuffer in PSRAM with DMA-complete `lv_disp_flush_ready`, partial invalidation, no LEDC backlight. |
| 13 | `components/boot_logic/boot_power.{c,h}` | **PORT** | same | Keep button task / overlay / haptic-on-press pattern; rebuild around BQ_BUTTON GPIO16, BQ25619 ship-mode, no GPIO41 latch. |
| 14 | `components/boot_logic/boot_tasks.{c,h}` | **PORT** | same | Task table pattern is correct. Replace rows (`task_mag` → `task_lis`, `task_imu` → `task_lsm`, …) and add new ones (touch ISR task, charger, mic, encoder, WS2812, flashlight, ECG, SD card). |
| 15 | `components/app_logic/app_battery.{c,h}` | **DROP** | replaced by `components/bq25619/` | ADC + ETA6098 inference path is dead. Battery voltage/percentage now read from BQ25619 BATSNS via I2C bus 2. |
| 16 | `components/app_logic/app_nvs.{c,h}` | **COPY** | same | Pure NVS layer; calibration struct grows to hold LIS3MDLTR hard-iron values, BQ25619 SoC fuel-gauge state, encoder zero, etc. |
| 17 | `components/alarm/*` (4 files) | **COPY** | same | Software-only alarm + haptic-pattern + UI-event-dispatch module. Zero hardware coupling. |
| 18 | `components/pcf85063/*` (4 files) | **COPY + extend** | `components/pcf85063/` | Same chip on Mk I, INT line moves to GPIO15. Keep Phase-15 UTC sync. Add 1 PPS GPS-driven trim later (forward idea, not Phase 1). |
| 19 | `components/drv2605/*` (6 files) | **COPY** | `components/drv2605/` | Same chip, same I2C address (0x5A) on **bus 2**. Plug straight in; reroute through bus-2 mutex. DRV_EN moves from old pin to GPIO0 (boot strap — drive as FW output, see v7.2 §GPIO0). |
| 20 | `components/bh1750/*` (4 files) | **PORT** | `components/veml6030/` | Chip changes (BH1750→VEML6030, 0x23→0x10, different command set, INT line available). Tile + EMA filter + auto-brightness logic reusable; rewrite low-level driver. |
| 21 | `components/bme280/*` (7 files, inc. bme280_drv) | **PORT** | `components/bme688/` | Bosch sensor family. Replace `bme280` vendor with Bosch BME68x library, add gas-sensor (heater) state machine. Tile, NVS height-reference, fusion altitude branch reusable. |
| 22 | `components/max30102/*` (4 files) | **PORT** | `components/max30101/` | Register-compatible cousin. Same FIFO / IRQ pattern. New device adds 3 LEDs + tighter SpO2 path; INT moves to GPIO7 (wake-source). |
| 23 | `components/qmc5883p/*` (4 files) | **PORT** | `components/lis3mdl/` | QMC5883P dies. Reuse calibration tile + NVS + heading math. Rewrite chip-level. Note LRA-magnet DC offset (v7.2 §LRA-vs-mag). |
| 24 | `components/qmi8658/*` (4 files) | **PORT** | `components/lsm6dsv16x/` | New IMU is ST LSM6DSV16X (INT1=GPIO8, raise-to-wake source). Keep imu_tile + complementary filter + read-before-write; rewrite chip layer; add Machine Learning Core hook later. |
| 25 | `components/tu10f/*` (4 files) | **PORT** | `components/max_m10s/` | u-blox NMEA core (GGA/RMC parsing, timegm-based RTC seed, debug-sentence capture) is reusable as-is. Replace UART pins (17/18) + add 1 PPS edge ISR on GPIO46 for RTC discipline. |
| 26 | `components/lvgl_ui/lvgl_ui.{c,h}` | **PORT** | same | Init scaffolding + UI-event-queue drain pattern reused. **Refresh task at 200 ms is incompatible with the ≤5 ms LVGL handler mandate** — rebuild around the new pipeline (see Defect List D-04). |
| 27 | `components/lvgl_ui/tile_registry.{c,h}` | **COPY** | same | Plug-in registry — add/remove a tile by editing one struct, exactly the Lego pattern in spec. |
| 28 | `components/lvgl_ui/ui_broker.{c,h}` | **COPY** | same | NVS async save queue + Core-1 globals. |
| 29 | `components/lvgl_ui/ui_main_screen.*`, `ui_status_bar.*`, `ui_navigation.*`, `ui_settings_screen.*`, `ui_lock_screen.*`, `ui_notif_overlay.*`, `ui_shutdown_overlay.*`, `ui_theme_colors.h`, `system_tile.*`, `fa5_select_14.c` | **PORT** | same | Native LVGL widgets — most port directly. Resize coordinates for 410×502 vs 240×280; pick a denser font set; recompute tile slot dimensions. Keep theme + navigation patterns. |
| 30 | (No file yet — Mk I net-new modules) | **WRITE** | `components/co5300/` (QSPI display) ; `components/cst9217/` (touch) ; `components/bq25619/` (charger) ; `components/tmp117/` (skin temp) ; `components/encoder/` (crown) ; `components/ws2812/` ; `components/flashlight/` ; `components/sdcard/` ; `components/mic_pdm/` ; `components/qvar_ecg/` | **WRITE** | All net-new. Each gets the 4-file pattern (`driver.{c,h}` + `xxx_tile.{c,h}` where it has a UI presence) and a broker slot. |

### Verdict roll-up (file count, approximate)

| Verdict | Files (old → new project) |
|---|---|
| COPY (verbatim or trivial edits) | ~32 |
| PORT (skeleton kept, internals rewritten) | ~46 |
| REWRITE (full new code, keep pattern only) | ~6 (boot_display, boot_hw_init, main.c) |
| DROP (delete entirely) | ~11 (`app_battery`, all of `bh1750/` body, sdkconfig, ST7789 paths …) |

---

## Section B — Defect List (cited)

> Defects below are confirmed by source inspection. "Inherited" = the defect would re-appear in the Mk I architecture if naively copied; "Hardware-dies" = the underlying code is being dropped anyway.

| ID | File:Line | Defect | Disposition |
|---|---|---|---|
| D-01 | `components/boot_logic/boot_hw_init.c:60-62` | I2C bus configured on **GPIO11 (SDA) / GPIO10 (SCL) @ 100 kHz, single bus**. GPIO10/11 are **QSPI_CS / QSPI-I0** on Mk I — total collision. | Inherited → REWRITE: two new-API buses on 1/2 and 4/5; per-bus mutex. |
| D-02 | `components/boot_logic/boot_hw_init.c:45,144-156` | Uses **deprecated** `driver/i2c.h` / `i2c_cmd_link_create` / `i2c_master_cmd_begin` API (IDF ≤4.x). | Inherited → switch every driver to `driver/i2c_master.h` (`i2c_master_bus_handle_t`, `i2c_master_transmit_receive`). |
| D-03 | `components/boot_logic/boot_hw_init.c:289-295` | RTC boot seed uses `mktime()` after `setenv("TZ","UTC0",1)`; a commented `timegm()` line below it is the actual fix. Current code only works because of the env-var dance — fragile. | Inherited (mostly) → in new project use `timegm()` directly; never depend on TZ env var for UTC math. |
| D-04 | `components/lvgl_ui/lvgl_ui.c:195-237` | `task_ui_refresh_fn` runs **every 200 ms**, takes `lvgl_port_lock` for the whole window (broker reads + main-screen update + screen dispatch + UI events). Hard-wired 5 Hz UI cadence — incompatible with ≤5 ms LVGL handler mandate and the partial-invalidation requirement. | Hardware-dies (UI gets rebuilt anyway). Carry forward only the event-queue drain pattern. |
| D-05 | `components/boot_logic/boot_display.c:104-122` | Pull-based ST7789 over SPI2; `lvgl_port_add_disp` with `buff_dma=true buff_spiram=true swap_bytes=true` — RGB565 + byte-swap is ST7789-only. | Hardware-dies. New panel: QSPI, RGB888, no swap_bytes. |
| D-06 | `components/boot_logic/boot_display.c:170-200` | Touch driver is **`esp_lcd_touch_cst816s`**, configured with `int_gpio_num = TOUCH_GPIO_INT` but with `levels.interrupt = 0` — and there is no ISR-driven path; the LVGL port polls. | Hardware-dies. CST9217 requires ISR + `xTaskNotifyFromISR` + `xQueueOverwrite` (mandated). |
| D-07 | `components/boot_logic/boot_hw_init.c:206-215` | `boot_cst816d_configure()` writes raw chip registers from `boot_hw_init` (out of the touch driver's module). Layering violation. | Hardware-dies + fix: the new CST9217 driver owns all post-reset register writes. |
| D-08 | `components/app_logic/app_battery.{c,h}` (entire file) | Charging inferred from EMA-filtered ADC voltage delta; multiple magic numbers (`EMA_ALPHA=0.1`, `CHARGE_DELTA_THRESHOLD=0.002`); ADC channel hard-coded to **GPIO1 (ADC1_CH0)**, but **GPIO1 = SDA bus1** on Mk I. | Hardware-dies. Replace with BQ25619 driver; BQ owns SoC + status flags. |
| D-09 | `components/boot_logic/boot_power.c:30-67` | Power latch on **GPIO41**, button on **GPIO40**. GPIO41 in Mk I is `GPIO_FLASHLIGHT`; GPIO40 is `SD_DAT0`. Total reassignment. | Hardware-dies. Rewrite around GPIO16 (BQ_BUTTON, RTC-wake), no latch. |
| D-10 | `components/boot_logic/boot_power.c:77-122` | Power monitor task polls the button at 100 ms (`PWR_POLL_MS = 100`). Polling drift makes it hard to distinguish a tap from a hold; also burns Core-0 cycles when idle. | Inherited (concept). New design: GPIO16 ISR + task notification + sub-tick `esp_timer_get_time()` for duration measurement. |
| D-11 | `components/boot_logic/boot_hw_init.c:87-96` | Hard-coded WHO_AM_I table contains `QMC5883P 0x2C`, `QMI8658C 0x6B`, `BME688 0x76`, `MAX30102 0x57`, `DRV2605 0x5A`, `BH1750 0x23`, `CST816D 0x15`. Three of those addresses survive on Mk I (BME688 0x76, MAX30101 0x57, DRV2605 0x5A); the rest die. | Hardware-dies. Rebuild table from v7.2 §I2C BUS ASSIGNMENT (two-bus). |
| D-12 | `components/qmc5883p/qmc5883p.h:37` | `QMC5883P_ADDR 0x2C` (note: the sub-audit reported `0x0C` — actual code is `0x2C`). | Hardware-dies. LIS3MDLTR @ 0x1C (bus 1). |
| D-13 | `components/boot_logic/boot_hw_init.c:421-433` | Cross-driver init + fusion init are called from `init_cross_driver()`; the function name lies (it also calls `fusion_init`). | Cosmetic; clean up while porting. |
| D-14 | `components/boot_logic/boot_hw_init.c:343-355` | BME280 ENV init writes the broker slot with `bd = {0}` then `home_ref_altitude_m` then `enabled = true` — **no read-before-write**. If anything else has already written the slot (it hasn't, at this boot stage, but the pattern is fragile), state is lost. | Inherited; fix during port. |
| D-15 | `components/boot_logic/boot_hw_init.c:386-393` | Battery init is unconditional ("ADC — always present, no I2C") and `broker_battery_set_hw_status(ret == ESP_OK)` ignores that the divider is on a colliding GPIO. | Hardware-dies. |
| D-16 | `components/lvgl_ui/ui_broker.c:15` (per sub-audit) | `g_tz_offset_hours` hardcoded (likely UTC+1) in source instead of being NVS-loaded by `app_nvs_load_ui_settings`. | Inherited; promote TZ to a UI setting in the port. |
| D-17 | `components/lvgl_ui/lvgl_ui.c:200` | `lvgl_port_lock(pdMS_TO_TICKS(10))` — a 10 ms timeout while every other Core-0 sensor task can be blocking on I2C is a real risk of dropping a UI frame silently. | Inherited; new design needs to either (a) keep Core-0 work outside the LVGL lock entirely (already mostly true), or (b) raise the timeout and assert on failure. |
| D-18 | `components/data_broker/data_broker.h:81-93` | Broker header `#include`s every driver header. Convenient for compile, but it means changing any driver's `broker_xxx_data_t` recompiles every translation unit that pulls in the broker — slow build, viral coupling. | Inherited. Mitigation: split into `broker_types.h` (struct typedefs only) + `data_broker.h` (API). Worth doing during the port. |
| D-19 | `components/boot_logic/boot_tasks.c:78-100` | All sensor stacks are 3072–8192 bytes — guesses, not measurements. The file comment admits this. | Inherited; use `uxTaskGetStackHighWaterMark` after first build, trim. |
| D-20 | `components/data_broker/data_broker.c:117,124,141,165` | Broker mutex timeouts are 5–10 ms. If a Core-0 driver holds the mutex during something pathological (it shouldn't, by rule), Core-1 reads silently fall through to "OFFLINE" — no logging on the read path other than `ESP_LOGW`. | Inherited (low risk). New project: add a periodic mutex-contention counter and surface it. |

---

## Section C — Mutex / Lock-Order Audit Highlights

- `g_i2c_mutex` is owned by `boot_hw_init.c` and taken correctly inside every Core-0 sensor task (`bh1750`, `bme280_drv`, `qmc5883p`, `qmi8658`, `max30102`, `drv2605`, `pcf85063`). All releases happen immediately after the transaction. **No driver holds the I2C mutex across `vTaskDelay` or LVGL calls.** Good baseline.
- Broker mutex is held only for `memcpy()` (confirmed via the `BROKER_MODULE_IMPL` macro pattern in `data_broker.c`). **No violations.**
- Cross-driver fires (`cross_driver_fire`) are synchronous Core-0 calls; callbacks register before tasks start (`cross_driver.c:50-72`). Re-entrancy is bounded by the four-listener cap. Safe.
- `lvgl_port_lock` is taken only by `task_ui_refresh_fn` and by `boot_hw_init_poll_sync` (the latter from Core 0, briefly, to call a tile function for GPS sync UI feedback). Acceptable for now but the second site is a smell — it crosses Core 0 → Core 1 by directly touching LVGL state. Replace with a `UI_EVENT_GPS_SYNC_RESULT` enqueue in the new project.

---

## Section D — Known Open Bug Disposition

| Bug (verbal report) | Diagnosis in source | Mk I disposition |
|---|---|---|
| **GPS time gated on full position fix.** | `tu10f.c` only writes `time_valid` after RMC, which carries date + position together; the seed path in `boot_hw_init_poll_sync` further requires `gps.time_valid`. Sub-audit confirmed Phase 15 `timegm()` fix is in place. | **Carries forward.** In Mk I, the MAX-M10S exposes UTC before fix via UBX-NAV-TIMEUTC; the new driver should expose `time_valid` without `position_valid`. Add UBX-only path. |
| **Auto-brightness EMA undershoot.** | `bh1750.c` + `light_tile.c` use a 0.1 EMA on lux; under low light the filter never reaches max-allowed darkness because of integer-truncation in the percentage map. | **Hardware-dies + redesign.** VEML6030 has wider dynamic range and per-gain auto-ranging; redo the percentage curve from VEML lux values, and keep the EMA but compute on the log of lux (perceptual). |
| **Blue-light bedtime mode missing.** | `g_blue_light_on` exists in `data_broker`; nothing schedules it from time-of-day. | **Carries forward as a feature gap.** Add to fusion / app_logic in Phase 4. Not a Phase-0 blocker. |

---

## Section E — Conflicts vs `Kompic_Mk1_System_Instructions_v7.2.md`

Every conflict below is resolved by the v7.2 doc, by rule.

| Conflict | Resolution |
|---|---|
| Single I2C bus (boot_hw_init) vs **two I2C buses** in v7.2 §I2C BUS ASSIGNMENT | Two buses in new project. |
| GPIO11/10 as SDA/SCL (boot_hw_init) vs **GPIO1/2** (bus 1) and **GPIO4/5** (bus 2) in v7.2 | Use v7.2 pinout. |
| ST7789 SPI (boot_display) vs **CO5300 QSPI** in v7.2 §DISPLAY | Use CO5300; mandatory RGB888 (COLMOD 0x77). |
| RGB565 + `LV_COLOR_16_SWAP=1` (lv_conf.h, sdkconfig) vs **RGB888 mandatory** (v7.2) | LVGL RGB888 / 32-bit draw path; no swap. |
| CST816S touch @0x15 (boot_display, boot_hw_init) vs **CST9217 @0x5A bus 1** (v7.2) | Use CST9217; ISR-driven. |
| TU10F GPS (UART, no pin lock) vs **MAX-M10S, UART1 GPIO17/18, 1PPS→GPIO46** (v7.2) | Use MAX-M10S; add 1PPS edge ISR. |
| QMI8658 @0x6B (qmi8658.h) vs **LSM6DSV16X @0x6B INT1=GPIO8** (v7.2) | Address matches by coincidence; chip differs — rewrite. |
| QMC5883P @0x2C (qmc5883p.h) vs **LIS3MDLTR @0x1C** (v7.2) | Replace driver. |
| BH1750 @0x23 vs **VEML6030 @0x10** | Replace driver. |
| BME280 @0x76 vs **BME688 @0x76** | Same address, different chip family; port. |
| MAX30102 @0x57 vs **MAX30101 @0x57 INT=GPIO7 (wake-source)** | Port + wire interrupt. |
| ETA6098 + ADC battery vs **BQ25619 @0x6A bus 2, BATSNS via I2C** | Drop old; write new charger driver. |
| Power latch GPIO41 vs **GPIO41 = flashlight on Mk I**; button on **GPIO16 (BQ_BUTTON)** | Drop latch; new button model. |
| DRV2605 on the same single bus vs **DRV2605 @0x5A on bus 2** | Move to bus 2; address unchanged. |
| Backlight on LEDC GPIO15 vs **GPIO15 = RTC_INT** on Mk I; AMOLED brightness via reg 0x51 | Drop LEDC; brightness through panel command. |

---

## Section F — Things the audit explicitly did **not** find (and that's fine)

- No `%f` in `lv_label_set_text_fmt` calls. All grep hits are *warning comments* against the practice. Carry the discipline forward but no clean-up needed.
- No driver holds the broker mutex across I/O.
- No driver holds the I2C mutex across `vTaskDelay`.
- Cross-driver framework is correctly Core 0-only.
- Tile registry truly is plug-in: adding a tile is a one-line registry edit plus the 2 tile files.
- `main.c` is genuinely orchestrator-only — zero hardware logic in it.

Everything above is worth preserving as architecture in the new project.

---
*End of audit report.*
