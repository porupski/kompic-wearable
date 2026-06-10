# 01 — Kompic̄ Mk I Firmware Architecture Blueprint

**Date:** 2026-06-10
**Hardware authority:** `Kompic_Mk1_System_Instructions_v7.2.md` (iv7.1). Where this blueprint and v7.2 disagree, **v7.2 wins**.
**Companion docs:** `02_Audit_Report.md` (defect inventory), `03_Migration_Map.md` (per-file move list).

---

## 1. Target stack

| Layer | Choice | Reason |
|---|---|---|
| MCU | **ESP32-S3-WROOM-1U-N16R8** @ **240 MHz** | v7.2 fixed; full 240 MHz for LVGL throughput + ML inference headroom. PSRAM-octal @ 80 MHz (sdkconfig was 40 MHz — bump it). |
| Toolchain | **ESP-IDF latest stable 5.x** (currently 5.5.x branch) | New `i2c_master` driver, new `esp_lcd` QSPI helpers, IDF-stable LVGL port, ULP and sleep-state APIs. |
| RTOS | **FreeRTOS** (IDF-bundled) at **CONFIG_FREERTOS_HZ = 1000** | Old project ran HZ=100 (10 ms tick). LVGL ≤5 ms handler period requires a higher tick rate; HZ=1000 also gives 1 ms `vTaskDelay` granularity needed for button-hold timing and PDM mic gating. |
| UI | **LVGL 9 (latest 9.x)**, **`esp_lvgl_port`** v2.x, **RGB888 (24-bit) draw** | CO5300 mandates COLMOD 0x77 (3 bytes per pixel). LVGL v9 with `LV_COLOR_DEPTH=24` and `LV_DRAW_SW_SUPPORT_RGB888=y`. No `LV_COLOR_16_SWAP`. |
| Filesystem | NVS for settings/calibration; **FATFS on SD** (mounted on demand) for logs and ML model files. | Mk I has SD socket (GPIO38–40). Keep NVS partition for settings only. |
| Build system | CMake (IDF default), `idf_component.yml` per component for managed deps (`espressif/esp_lvgl_port`, Bosch BME68x, etc.) | Same as the old project. |
| Optional | Espressif ESP-DSP (FIR/IIR/FFT) for PPG and Qvar ECG; ST `iks01a3`-derived helpers for LSM6DSV16X MLC config blobs | Phase 5+. |

> **Why 240 MHz and not 80 MHz dynamic.** v7.2 §POWER ARCHITECTURE shows a single battery budget with no aggressive power-state requirement on Mk I v1; the LVGL pipeline + PPG signal processing + (future) ML inference benefit from full clock. Dynamic frequency scaling and light-sleep are deferred to v2.

---

## 2. Architectural rules (carry forward, verbatim)

These are non-negotiable and identical to the old project's working rules:

1. **Core 0 = drivers + broker writes. Core 1 = LVGL UI + broker reads.** They never cross. No exception.
2. **Data Broker** owns all cross-core data; mutex held for `memcpy` only.
3. **Cross-Driver (XD)** framework handles Core0→Core0 reactions; Core-0 only; registered before tasks start.
4. **Fusion** task computes multi-sensor derived values; reads broker, writes broker.
5. **UI Event Queue** is the only legal Core 0 → Core 1 command path (`g_ui_event_q`).
6. **4-file module pattern:** `xxx.{c,h}` (Core 0, driver + task) + `xxx_tile.{c,h}` (Core 1, LVGL widgets).
7. **`main.c` is orchestrator-only.** Adding or removing a module touches: a row in the device table (boot_hw_init), a row in the task table (boot_tasks), a row in the tile registry (lvgl_ui). Nothing else.
8. **NVS discipline** — Core 1 never blocks on flash; everything goes through `task_settings_saver`.
9. **No `%f` in `lv_label_set_text_fmt`.** Always `snprintf` to a local buffer first.
10. **Source-of-truth hierarchy:** blueprints (this doc) → bench logs → ad-hoc suggestions. v7.2 hardware doc overrides all firmware docs.

---

## 3. Mandatory performance directions (touch-latency post-mortem)

These six bind every relevant component below. They are requirements, not suggestions.

1. **Touch is interrupt-driven.** CST9217 INT (GPIO6) → GPIO-ISR → `xTaskNotifyFromISR` → `task_touch` reads via I2C in task context. **Never poll.**
2. **Touch coordinates bypass the broker mutex.** Single-producer (`task_touch`), single-consumer (LVGL indev callback): `xQueueOverwrite` of a `touch_point_t {x, y, pressed, ts_ms}`. No mutex on this path.
3. **`lv_disp_flush_ready()` is called from the DMA-complete callback**, never inline after starting the transfer.
4. **LVGL handler loop ≤5 ms.** `esp_lvgl_port` configured with `task_max_sleep_ms = 5`. Tile updates happen by *reaction* (broker-driven via a 50 Hz tick) or *demand* (UI events), not on the LVGL handler thread.
5. **Partial invalidation is mandatory.** Tiles invalidate only the widgets that changed. 410×502 RGB888 over QSPI@40 MHz is ~620 KB/frame ≈ 31 ms full-frame floor (~30 fps ceiling); full repaint on every touch is forbidden.
6. **Broker mutex is held for `memcpy` only.** Already true in v5; carry it forward and grep-enforce in CI.

---

## 4. Display + Touch pipeline (Mk I)

### 4.1 Display — CO5300 QSPI AMOLED 410 × 502 RGB888

- Bus: **SPI2 / FSPI** via IOMUX (GPIO 9–14, CS=10, TE=45, RST=3). Pixel clock target: **40 MHz** (datasheet supports up to 80 MHz; start at 40, bench, then raise).
- Init: command path `instruction 0x02` (1-wire), pixels `0x32` (4-wire QIO), `addr 0x003C00`, COLMOD `0x77`, column offset **22**.
- Sleep: `0x28 DISPOFF` → `0x10 SLPIN`. Wake: `0x11 SLPOUT` → full re-init → `0x29 DISPON` → repaint.
- Brightness: through panel reg `0x51 BRIGHT` (8-bit). Persisted to NVS as `g_saved_brightness`. **No LEDC; no PWM GPIO; no backlight task.**
- **Framebuffers:** two RGB888 buffers in PSRAM, each `LV_HOR_RES_MAX × LV_VER_RES_MAX × 3 = 617 460 B ≈ 603 KiB`. Total ~1.18 MiB of 8 MiB PSRAM. **DMA capable** — use `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA`.
- **Tearing:** the TE pin (GPIO45) wakes a TE-event task that calls `esp_lcd_panel_draw_bitmap` only between TE rising edges; LVGL gets `lv_disp_flush_ready` from the DMA-complete callback.
- Component: `components/co5300/` — `co5300.{c,h}` (panel driver), `co5300_te_isr.c` (TE pin handling), exposes `co5300_set_brightness(pct)` and `co5300_sleep()/co5300_wake()`.

### 4.2 Touch — CST9217 I2C 0x5A on bus 1

- Bus: **I2C bus 1** (GPIO1 SDA, GPIO2 SCL, 400 kHz — sensor bus stays at 400 kHz; v7.2 explicitly notes CST9217 supports 400 kHz).
- INT: **GPIO6** (RTC-wake-capable). RST: **GPIO44**.
- Path:
  1. `gpio_install_isr_service`, `gpio_isr_handler_add(GPIO6, cst9217_isr, …)`.
  2. ISR is one line: `xTaskNotifyFromISR(s_task_touch, 0, eNoAction, &xHigherPriorityTaskWoken)`.
  3. `task_touch` (Core 0, prio 8, stack 3 KB): wait on notify, take **bus 1 mutex**, read 0xD000 (ACK 0xAB), release mutex, `xQueueOverwrite(g_touch_q, &pt)`. **Touch task never holds the mutex during the LVGL flush.**
  4. LVGL indev `read_cb`: `xQueueReceive(g_touch_q, &pt, 0)` — non-blocking peek; LVGL drives at its own cadence.
- `task_touch` priority sits *above* every sensor task (8) and *above* the UI task (7) so its I2C window is taken first under contention.

### 4.3 LVGL port configuration

- `lvgl_port_cfg_t.task_priority = 7`, `task_stack = 8192`, `task_core_id = 1`, `task_max_sleep_ms = 5`.
- `lvgl_port_display_cfg_t`: `buffer_size = LV_HOR_RES × 80` (80-line slice; not full frame), `double_buffer = true`, `flags.buff_dma = true`, `flags.buff_spiram = true`. RGB888 — no `swap_bytes`.
- LVGL `LV_DEF_REFR_PERIOD = 16` ms (one VSYNC at ~60 fps for hand-feel responsiveness; the partial-invalidation cost dominates over the ceiling).
- Tile updates: on the 50 Hz broker tick (`task_ui_tick`, Core 1, prio 6, period 20 ms), only the currently visible tile's `update()` runs, and only widgets that changed are `lv_obj_invalidate`d. UI-Event queue is drained on the same tick.

> **Why 20 ms tick for tiles, but 5 ms LVGL handler.** The LVGL handler period is what the *port* sleeps for; it wakes on touch (via the indev callback) and on its own animation/redraw needs. Tile data updates are slow (broker reads at 1–10 Hz cadences typically); coalescing them at 50 Hz keeps the lvgl_port_lock window tight and predictable.

---

## 5. Two-bus I2C manager

### 5.1 Bus inventory

| Bus | SDA | SCL | Speed | Mutex | Devices |
|---|---|---|---|---|---|
| **Bus 1** | GPIO1 | GPIO2 | 400 kHz | `g_i2c1_mutex` | VEML6030 (0x10), LIS3MDLTR (0x1C), MAX-M10S (0x42), TMP117 (0x48), PCF85063 (0x51), MAX30101 (0x57), CST9217 (0x5A), LSM6DSV16X (0x6B), BME688 (0x76) |
| **Bus 2** | GPIO4 | GPIO5 | 400 kHz | `g_i2c2_mutex` | DRV2605 (0x5A), BQ25619 (0x6A) |

### 5.2 Ownership

- Both buses are created in `boot_hw_init.c` using the new API:
  ```c
  i2c_master_bus_config_t bus1_cfg = { .i2c_port = 0, .sda_io_num = 1, .scl_io_num = 2,
                                       .clk_source = I2C_CLK_SRC_DEFAULT,
                                       .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = false };
  i2c_master_new_bus(&bus1_cfg, &g_i2c1_bus);
  ```
- **No internal pull-ups** — v7.2 says 5.1 kΩ to 3V3 externally on every bus. Honor that.
- Each driver gets an `i2c_master_dev_handle_t` registered at `boot_hw_init` time and stores it locally. Drivers do not look the device up by address at runtime.
- **Per-bus mutex.** `g_i2c1_mutex` and `g_i2c2_mutex` are separate. A device declares its bus at registration; its driver acquires the matching mutex.

### 5.3 Contention rules

- **BME688 gas heater can hold bus 1 for 30–60 ms.** Mitigation:
  - Heater sequence runs at the *lowest* priority of any bus-1 owner (`task_env`, prio 2).
  - Heater issues transactions in *bursts* — never one long blocking transaction; configures the heater profile then releases the mutex while the chip cooks (datasheet-defined wait), then re-takes for the read.
  - Touch is on bus 1 and **must not be starved** by BME688: the touch task runs at prio 8, and the I2C mutex uses *priority inheritance* (FreeRTOS `xSemaphoreCreateMutex` does this automatically). A blocked high-prio task lifts the holder's priority transparently.
- **Charger I2C (bus 2) is slow but isolated.** BQ25619 transactions never affect bus 1.
- Never call into another driver's API while holding either mutex. Drivers `#include` only `<i2c_master.h>` + their own header + (where applicable) `data_broker.h` and `cross_driver.h`.

---

## 6. Module inventory (Mk I sensor suite)

Every module follows the 4-file pattern. Columns: bus / address / GPIOs / wake-source role / core.

| Module folder | Driver header | Tile | Bus | Addr | GPIOs (INT / RST / extra) | Wake role | Core (task) |
|---|---|---|---|---|---|---|---|
| `co5300/` | `co5300.h` | direct | — | — | TE=45, RST=3, CS=10, SCK=12, D0–D3=11/13/9/14 | none | C0 (no task, IO-driven) |
| `cst9217/` | `cst9217.h` | shared with LVGL indev | 1 | 0x5A | INT=6, RST=44 | **tap** (GPIO6 RTC wake) | C0 (`task_touch`, prio 8) |
| `bq25619/` | `bq25619.h` | `battery_tile.{c,h}` | 2 | 0x6A | BUTTON=16 (dual-wired to QON) | **button + ship-mode exit** | C0 (`task_charger`, prio 3, 1 Hz poll) |
| `pcf85063/` | `pcf85063.h` | `rtc_tile.{c,h}` | 1 | 0x51 | INT=15, CLKOUT=JP5 | **alarm** | C0 (`task_rtc`, prio 4, 1 Hz) |
| `max_m10s/` | `max_m10s.h` | `gps_tile.{c,h}` | UART1 | TX=GPIO17, RX=GPIO18 (NMEA + UBX) | 1PPS→GPIO46 | none (1 PPS is not RTC-wake) | C0 (`task_gps`, prio 4, 200 ms) |
| `lsm6dsv16x/` | `lsm6dsv16x.h` | `imu_tile.{c,h}` | 1 | 0x6B | INT1=8, INT2 not on GPIO | **raise-to-wake** | C0 (`task_imu`, prio 4, 20 ms or INT-driven) |
| `lis3mdl/` | `lis3mdl.h` | `compass_tile.{c,h}` | 1 | 0x1C | INT / DRDY not on GPIO | none | C0 (`task_mag`, prio 4, 100 ms) |
| `bme688/` | `bme688.h` | `env_tile.{c,h}` | 1 | 0x76 | none (heater on-chip) | none | C0 (`task_env`, prio 2, 2 s sample) |
| `max30101/` | `max30101.h` | `health_tile.{c,h}` | 1 | 0x57 | INT=7 | **PPG FIFO during sleep HR** | C0 (`task_hr`, prio 4, INT-driven) |
| `tmp117/` | `tmp117.h` | (subtile in health_tile) | 1 | 0x48 | TMP_ALRT not on GPIO | none | C0 (`task_skin_temp`, prio 2, 5 s) |
| `veml6030/` | `veml6030.h` | `light_tile.{c,h}` | 1 | 0x10 | VEML_INT not on GPIO | none | C0 (`task_light`, prio 3, 500 ms) |
| `drv2605/` | `drv2605.h` | `haptic_tile.{c,h}` | 2 | 0x5A | DRV_EN=GPIO0 (boot strap; FW output) | none | C0 (`task_haptic`, prio 3, on-demand) |
| `encoder/` | `encoder.h` | `system_tile` consumes | — | — | A=21 (RTC wake), B=43 | **crown turn (bonus)** | C0 (`task_encoder`, prio 5, ISR-driven via PCNT or anyedge GPIO ISR) |
| `ws2812/` | `ws2812.h` | (no tile, services others) | RMT | DIN=GPIO42 | — | none | C0 (`task_led`, prio 2, on-demand) |
| `flashlight/` | `flashlight.h` | `system_tile` consumes | LEDC | GPIO41 | — | none | C0 (no task; direct LEDC) |
| `sdcard/` | `sdcard.h` | (logs only — no tile in v1) | SDMMC | CLK=38, CMD=39, D0=40 | — | none | C0 (mount on demand) |
| `mic_pdm/` | `mic_pdm.h` | (no tile in v1) | I2S0 PDM | CLK=47, DAT=48 | — | none | C0 (`task_mic`, prio 4, DMA) |
| `qvar_ecg/` | `qvar_ecg.h` | `ecg_tile.{c,h}` (v2) | (inside LSM6DSV16X Qvar block) | — | electrodes on pogo pins | none (v1: manual trigger) | C0 (`task_ecg`, prio 4, on demand) |
| `fusion/` | `fusion.h` | (synthetic — read by other tiles) | — | — | — | — | C0 (`task_fusion`, prio 2, 1 Hz) |
| `alarm/` | `alarm.h` | `alarm_tile.{c,h}` | — | — | — | — | C0 (`task_alarm`, prio 2, 1 Hz check) |

### Infrastructure components (no hardware)

- `components/data_broker/` — same pattern as v5; expand for the new modules.
- `components/cross_driver/` — copy verbatim; add new event ordinals (touch-tap, encoder-turn, charger-event, …).
- `components/boot_logic/` — `boot_power.c`, `boot_hw_init.c`, `boot_display.c`, `boot_tasks.c` — all rewritten/ported per §10.
- `components/app_logic/` — `app_nvs.c` carried forward; `app_battery.c` deleted (replaced by `bq25619/`).
- `components/lvgl_ui/` — port; new resolution, new font set, new tile registry entries, ≤5 ms LVGL handler.

---

## 7. Boot / init order

```
app_main()
 ├─ 1. boot_power_init()          // GPIO16 input, GPIO0 (DRV_EN) output low,
 │                                 //   GPIO41 (flashlight) output low,
 │                                 //   PSRAM sanity check
 ├─ 2. broker_init()               // mutexes + queues + ui_event_q before anyone touches it
 ├─ 3. cross_driver_init()
 ├─ 4. app_nvs_init(&cal)          // NVS + calibration load
 ├─ 5. apply_settings_to_atomics() // theme, brightness, blue-light, TZ
 ├─ 6. boot_hw_init(&cal)
 │     ├─ i2c_master_new_bus(bus1) + bus2
 │     ├─ i2c1_scan() + i2c2_scan() + WHO_AM_I verify
 │     ├─ per-device init in v7.2 order:
 │     │     veml6030, lis3mdl, tmp117, pcf85063, max30101, lsm6dsv16x, bme688, max_m10s,
 │     │     drv2605, bq25619
 │     ├─ cst9217_init() + INT/RST GPIO setup + ISR install (no notifications armed yet)
 │     ├─ encoder_init() (PCNT + ISR)
 │     ├─ pcf85063 seed system clock with timegm() (UTC direct)
 │     ├─ fusion_init() + alarm_load_nvs()
 │     └─ cross_driver_register(…) — every XD listener
 ├─ 7. boot_display_init()         // CO5300 reset + init seq + RGB888 + TE wiring + LVGL port
 ├─ 8. lvgl_ui_init(&ui_cfg)       // inside lvgl_port_lock — build screens
 ├─ 9. ui_broker_init()            // NVS save queue
 ├─ 10. boot_tasks_start(save_q)   // every task; including arming the touch ISR notification target
 └─ return — FreeRTOS scheduler owns execution
```

**Why this order:**
- Power latch is gone — there is no race to GPIO-out before reset; `boot_power_init` is now mostly safety GPIO defaults (FW outputs low) and a PSRAM probe.
- I2C devices come up before the touch ISR is armed so the first INT edge has a fully-configured CST9217 to read.
- Display comes up after sensor init so the boot logo can already show real RTC time (when available).
- Tasks are the **last** thing — every primitive they need is alive.

---

## 8. Task table (canonical)

| Task | Core | Prio | Stack | Period / driver | Notes |
|---|---|---|---|---|---|
| `task_touch` | 0 | **8** | 3072 | INT-notified | Drains CST9217; produces `g_touch_q`. |
| `task_imu` | 0 | 4 | 4096 | 20 ms (or INT-driven via LSM_INT1) | Read-before-write; reuses imu_tile filter. |
| `task_gps` | 0 | 4 | 8192 | 200 ms drain | UART RX + NMEA + UBX-TIMEUTC; 1 PPS ISR is separate. |
| `task_rtc` | 0 | 4 | 4096 | 1 Hz | Reads PCF85063; alarm match logic. |
| `task_mag` | 0 | 4 | 4096 | 100 ms | LIS3MDLTR. |
| `task_mag_cal` | 0 | 2 | 4096 | on demand | Hard-iron min/max sweep + LRA-offset measure. |
| `task_charger` | 0 | 3 | 4096 | 1 Hz | BQ25619 — voltage, SoC, charging state, faults. |
| `task_haptic` | 0 | 3 | 4096 | on demand | DRV2605 on bus 2; sweep can hold bus 2 mutex across delays (no contention there). |
| `task_light` | 0 | 3 | 3072 | 500 ms | VEML6030 + auto-brightness. |
| `task_env` | 0 | 2 | 4096 | 2 s | BME688 — forced mode + gas heater. |
| `task_hr` | 0 | 4 | 4096 | INT-driven | MAX30101 FIFO via GPIO7. |
| `task_skin_temp` | 0 | 2 | 3072 | 5 s | TMP117. |
| `task_encoder` | 0 | 5 | 3072 | ISR + 1 ms debounce timer | Crown turn; feeds nav events. |
| `task_mic` | 0 | 4 | 4096 | DMA-driven | PDM mic (v2: VAD + Speex preproc). v1: capture only. |
| `task_fusion` | 0 | 2 | 4096 | 1 Hz | Altitude (GPS + baro + last-known), activity (IMU + HR). |
| `task_alarm` | 0 | 2 | 4096 | 1 Hz | RTC-match → haptic + UI event. |
| `task_ui_tick` | 1 | 6 | 4096 | 20 ms | Drives tile `update()`s, drains `g_ui_event_q`. |
| `task_lvgl_handler` | 1 | 7 | 8192 | ≤5 ms (`esp_lvgl_port`) | Owned by `esp_lvgl_port`; we don't author it. |
| `task_power_btn` | unpinned | 5 | 3072 | ISR + timer | BQ_BUTTON edges; short/long press, ship-mode. |
| `task_settings_saver` | unpinned | 2 | 2048 | blocked on queue | NVS async save (carry forward). |
| `task_led` | unpinned | 2 | 2048 | on demand | WS2812 status pixel (boot, charging, alerts). |

Total task count: 22 (v5 had 16). Stack budget: ~70 KiB. Comfortable on N16R8.

---

## 9. Sleep / wake design

### 9.1 Wake-source matrix (from v7.2 §Sleep wake sources)

| Source | GPIO | Trigger | Use case |
|---|---|---|---|
| Touch tap | 6 | edge (active-low INT) | "Show clock" tap-to-wake |
| PPG FIFO | 7 | edge | Background HR during sleep |
| Raise-to-wake | 8 | edge | LSM6DSV16X gesture (raise/tilt) |
| RTC alarm | 15 | edge | Scheduled alarms, hourly wakes |
| BQ button | 16 | edge | Power button, ship-mode exit |
| Encoder turn | 21 | edge (bonus) | Knob wake |

### 9.2 Power states (scoped, not implemented in v1)

| State | Description | Display | I2C | What stays running |
|---|---|---|---|---|
| **ACTIVE** | Normal use | full RGB888 | both buses 400 kHz | all tasks |
| **DOZE** | screen on, idle | dim (panel reg) | both buses 400 kHz | all sensors at half cadence |
| **STANDBY** | screen off, AMOLED SLPIN, no rendering | off (SLPIN) | bus 1 awake for INT-driven HR/IMU + RTC | `task_hr`, `task_rtc`, `task_imu` (raise-to-wake), ISR-only touch / button |
| **OFF** | BQ25619 ship-mode | off | off | nothing — wake only by BQ button (GPIO16) |

In v1 we implement ACTIVE and STANDBY only. DOZE is a brightness step. OFF is via BQ ship-mode entry (write to `REG_RST_BQ.bit3 = 1`); exit is the QON tactile.

### 9.3 Wake-source hooks per module (scope, no code)

- `bq25619`: hook `bq25619_enter_ship_mode()` invoked by `task_power_btn` after 3 s hold.
- `cst9217`: in STANDBY, INT line remains armed (chip stays in low-power gesture-detect mode); `task_touch` blocks on notify, no I2C traffic until a tap.
- `lsm6dsv16x`: in STANDBY, configured for wakeup-on-tilt; FW disables most ODR features to save current; INT1 wakes Core 0.
- `max30101`: in STANDBY only when "background HR" is enabled in settings; otherwise put to shutdown.
- `pcf85063`: alarm program written by `task_alarm`; INT15 wakes the SoC.
- `co5300`: SLPIN before sleep; on wake, full re-init + repaint the most-recently-shown tile.

---

## 10. Phased implementation plan

Each phase ends with a **bootable, flashable build**. No phase is open-ended; the gate is "you can hold the device in your hand and see the change."

### Phase 0 — Scaffold + display + touch bring-up

Goal: turn the watch on, see a single LVGL hello-screen, tap the screen and see a pixel toggle.

- Re-create `idf_component.yml` and `CMakeLists.txt` at the root and in `main/`.
- Carry forward `data_broker`, `cross_driver`, `lvgl_ui/tile_registry`, `lvgl_ui/ui_broker`, the LVGL theme/color helpers, `main/main.c` (orchestrator), and `app_nvs`.
- Write `components/co5300/` (display panel) + `components/cst9217/` (touch).
- Write the new `boot_logic` (two-bus I2C, new display init, new power init).
- Rebuild `lv_conf.h` for RGB888.
- Implement ≤5 ms LVGL handler, partial invalidation, DMA-complete flush callback, touch ISR → notify → I2C → queue.
- **Exit criterion:** boot logo + clock tick + smooth tap response with no full-screen redraws.

### Phase 1 — RTC + battery + power button

- Port `pcf85063/` (move INT to GPIO15, keep Phase-15 UTC fix).
- Write `bq25619/` (charger driver, BATSNS, button via QON, ship-mode helper).
- Rewrite `boot_power.c` around the new button model.
- Bring up `task_power_btn` (ISR + hold timing) and `task_settings_saver`.
- **Exit criterion:** time of day visible; battery percentage visible and updating with USB plug/unplug; long-press shuts down via ship-mode.

### Phase 2 — IMU + magnetometer + light

- Port `lsm6dsv16x/` (with raise-to-wake config) + `imu_tile`.
- Port `lis3mdl/` (with LRA-offset measurement during calibration) + `compass_tile`.
- Port `veml6030/` (auto-brightness logic from `light_tile`).
- **Exit criterion:** compass heading rotates with the device; auto-brightness modulates panel reg 0x51; raise-to-wake wakes the screen from STANDBY.

### Phase 3 — GPS + environment + heart rate + skin temp

- Port `max_m10s/` (drop the position-fix gate on `time_valid`; use UBX-TIMEUTC for time-before-fix). Add 1 PPS GPIO46 edge ISR (not for RTC discipline in v1 — just count it).
- Port `bme688/` (Bosch BME68x lib + forced + gas heater state machine + altitude into fusion).
- Port `max30101/` (INT-driven FIFO; SpO2 with the IR LED).
- Write `tmp117/` (5 s temperature poll on bus 1).
- Wire fusion altitude (GPS primary, baro fallback, last-known).
- **Exit criterion:** GPS time-of-day before fix; barometric altitude readable; resting HR visible; skin temp on the health tile.

### Phase 4 — Haptic + alarm + encoder + WS2812 + flashlight

- Port `drv2605/` (move to bus 2; DRV_EN on GPIO0 as FW output, **not** internal pull-up).
- Port `alarm/` (RTC match + haptic + UI event).
- Write `encoder/` (PCNT + edge ISR + debounce).
- Write `ws2812/` (RMT TX on GPIO42) and `flashlight/` (LEDC on GPIO41).
- **Exit criterion:** alarm fires haptic + overlay; crown changes tile selection; flashlight toggle in system tile; WS2812 reflects charge state when plugged in.

### Phase 5 — SD card, PDM mic, ECG, ML hooks (v2-leaning, optional in v1)

- `sdcard/` (SDMMC mount on demand, log rotation).
- `mic_pdm/` (I2S PDM capture; no UI in v1).
- `qvar_ecg/` (LSM6DSV16X Qvar block read; raw waveform display in a hidden debug tile).
- LSM6DSV16X MLC config blob upload (gesture / activity classifier).
- **Exit criterion:** SD logs accumulating; mic captures to SD on demand; ECG strip visible in debug; MLC fires an activity event into the broker.

### Phase 6 — Polish, power tuning, calibration UX

- Wire DOZE state (auto-dim after N seconds, AMOLED SLPIN after M minutes).
- Sleep / wake cycle bench (measure standby current from BQ25619 INPSRC).
- Add blue-light-on schedule (time-of-day in fusion).
- Trim every task stack to high-water-mark + 25 %.
- **Exit criterion:** v1 release candidate.

---

## 11. Explicit non-goals (Mk I firmware v1)

- **No WiFi.** Driver stays disabled in sdkconfig; BLE is the only RF path planned, and even that is deferred.
- **No BLE pairing UX.** v1 ships standalone; BLE bring-up is v1.5 or v2.
- **No OTA over BLE/WiFi.** USB CDC re-flash only for v1. Partition layout is OTA-ready; we just don't use it yet.
- **No cloud sync.** No telemetry. Period.
- **No ML inference on the SoC in v1.** The LSM6DSV16X MLC and FSM are usable from Phase 5; full DSP/PPG ML on the S3 is v2.
- **No dynamic frequency / light sleep.** 240 MHz fixed in v1.
- **No PCB v7.2 errata firmware workarounds in v1.** If the RF traces are wrong, we still build the firmware; we just don't certify range.
- **No music / audio playback / camera / fingerprint / cellular.** None of those are hardware-present.

---

## 12. What this document does *not* decide

- Exact LVGL widget set for each tile (Phase 0 will inform).
- Exact font choices for 410 × 502 (Montserrat 16/20/28 is a starting guess).
- BLE GATT profile shape (deferred).
- Fuel-gauge algorithm for BQ25619 SoC at low SoC (BQ has a built-in counter; v1 just trusts it).
- Whether the watchdog (RTC-WDT + INT-WDT) defaults from IDF are enough — re-evaluate after first stress test.

These belong in subsequent design docs once Phase 0 lands.

---
*End of blueprint.*
