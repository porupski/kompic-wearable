# 03 — Migration Map (every old file → new path → verdict)

**Source root:** `11_SmartWatch_v5_project_only/`
**Target root:** `NEW_PROJECT_FROM_SCRATCH/`

**Verdicts:** **COPY** = drop in as-is or with trivial edit · **PORT** = copy as skeleton, internals to be rewritten · **REWRITE** = file conceptually replaced (the new file lives at the new path; the old one is *not* copied) · **DROP** = delete; nothing carries forward.

> "New path" reflects where the file lands inside `NEW_PROJECT_FROM_SCRATCH/`. Where the verdict is REWRITE, no file is copied — the new path is listed as a placeholder for where the rewrite will live.

---

## Root

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 1 | `CMakeLists.txt` | `CMakeLists.txt` | **COPY** | Top-level CMake stub, project name aside, is generic. |
| 2 | `partitions.csv` | `partitions.csv` | **COPY** | 16 MB OTA layout still valid; no change. |
| 3 | `sdkconfig` | (regen) | **DROP** | Old defaults reference deprecated I2C and ST7789; regenerate from IDF default via `idf.py menuconfig` and re-apply PSRAM-octal/80 MHz + LVGL-RGB888 + FREERTOS_HZ=1000. |
| 4 | `sdkconfig.ak` | — | **DROP** | Snapshot of an old config; obsolete. |
| 5 | `sdkconfig.old` | — | **DROP** | Snapshot of an older config; obsolete. |
| 6 | `dependencies.lock` | (regen) | **DROP** | Will be regenerated when `idf_component.yml` is re-curated. |
| 7 | `0i0_Project_Scrubber_Copier.py` | `0i0_Project_Scrubber_Copier.py` | **COPY** | Tooling script — only relevant for the dev workflow, not the firmware. |
| 8 | `print_project_files.txt` | — | **DROP** | Stale generated listing. |

## `main/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 9 | `main/main.c` | `main/main.c` | **PORT** | Orchestrator pattern correct; init order changes per Blueprint §7 (two-bus, new display init, new power init). |
| 10 | `main/lv_conf.h` | `main/lv_conf.h` | **PORT** | Switch to `LV_COLOR_DEPTH=24`, drop `LV_COLOR_16_SWAP`, enable `LV_DRAW_SW_SUPPORT_RGB888`, raise default refresh period. |
| 11 | `main/CMakeLists.txt` | `main/CMakeLists.txt` | **COPY** | One-liner; component list will diverge but template is fine. |
| 12 | `main/idf_component.yml` | `main/idf_component.yml` | **PORT** | Re-curate managed deps: drop `esp_lcd_touch_cst816s`, add (or vendor) CO5300 / CST9217 helpers, Bosch BME68x, etc. |

## `components/data_broker/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 13 | `data_broker/CMakeLists.txt` | same | **COPY** | Trivial. |
| 14 | `data_broker/data_broker.h` | same | **COPY** | API + macro pattern is the design we keep; add new module includes during the port. |
| 15 | `data_broker/data_broker.c` | same | **COPY** | Macro-based broker; expand for new modules with `BROKER_MODULE_IMPL`. |
| 16 | `data_broker/power_flags.h` | same | **PORT** | Strip dead `g_screen_locked` comment; reframe around AMOLED SLPIN sleep state and BQ button. |
| 17 | `data_broker/ui_event.h` | same | **COPY** | Cross-core command queue contract — already canonical. Extend the enum. |

## `components/cross_driver/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 18 | `cross_driver/CMakeLists.txt` | same | **COPY** | Trivial. |
| 19 | `cross_driver/cross_driver.h` | same | **COPY** | Event enum extends with new ordinals (touch tap, encoder, charger). Never reorder. |
| 20 | `cross_driver/cross_driver.c` | same | **COPY** | Dispatcher is correct, small, Core-0 only. |

## `components/fusion/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 21 | `fusion/CMakeLists.txt` | same | **COPY** | Trivial. |
| 22 | `fusion/fusion.h` | same | **PORT** | Struct shape correct; add ENV branch wiring + (eventually) HR/IMU activity. |
| 23 | `fusion/fusion.c` | same | **PORT** | Skeleton + altitude arbitration reusable; uncomment + finish ENV branch for BME688. |

## `components/boot_logic/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 24 | `boot_logic/CMakeLists.txt` | same | **COPY** | Trivial. |
| 25 | `boot_logic/boot_hw_init.h` | same | **REWRITE** | New API surface: two buses, two bus handles, two mutexes. |
| 26 | `boot_logic/boot_hw_init.c` | same | **REWRITE** | New `i2c_master`, two buses, new device table, no inline touch config. |
| 27 | `boot_logic/boot_display.h` | same | **REWRITE** | CO5300 QSPI constants; no LEDC backlight; new GPIO map. |
| 28 | `boot_logic/boot_display.c` | same | **REWRITE** | QSPI panel init, RGB888 framebuffers, DMA-complete `lv_disp_flush_ready`, ≤5 ms LVGL handler, partial invalidation. |
| 29 | `boot_logic/boot_power.h` | same | **PORT** | Rebuild around BQ_BUTTON GPIO16 + ship-mode; drop GPIO41 latch. |
| 30 | `boot_logic/boot_power.c` | same | **PORT** | Rebuild button task as ISR-driven; keep haptic-on-press, shutdown-overlay, hold-3s patterns. |
| 31 | `boot_logic/boot_tasks.h` | same | **PORT** | API stays the same. |
| 32 | `boot_logic/boot_tasks.c` | same | **PORT** | Replace task rows per Blueprint §8; add new task rows for touch / charger / encoder / mic / etc. |

## `components/app_logic/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 33 | `app_logic/CMakeLists.txt` | same | **COPY** | Trivial; drop `app_battery.c` from sources list during port. |
| 34 | `app_logic/app_battery.h` | — | **DROP** | ETA6098 + ADC inference dies; replaced by `components/bq25619/`. |
| 35 | `app_logic/app_battery.c` | — | **DROP** | Same reason. |
| 36 | `app_logic/app_nvs.h` | same | **COPY** | Pure NVS layer; grow `app_calibration_t` for new chips during port. |
| 37 | `app_logic/app_nvs.c` | same | **COPY** | Solid; no migration blockers. |

## `components/lvgl_ui/`  *(every file ports — UI is rebuilt on the new resolution but shape and patterns carry forward)*

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 38 | `lvgl_ui/CMakeLists.txt` | same | **COPY** | Sources list to be edited as files are ported. |
| 39 | `lvgl_ui/lvgl_ui.h` | same | **PORT** | API trimmed (no `boot_cst816d_configure` cross-call); event-queue drain stays. |
| 40 | `lvgl_ui/lvgl_ui.c` | same | **PORT** | Refresh model rebuilt around 50 Hz tick + reactive partial invalidation (Blueprint §4.3). |
| 41 | `lvgl_ui/ui_broker.h` | same | **COPY** | NVS async save queue + Core-1 globals; correct. |
| 42 | `lvgl_ui/ui_broker.c` | same | **PORT** | Stop hardcoding `g_tz_offset_hours`; load from NVS. |
| 43 | `lvgl_ui/tile_registry.h` | same | **COPY** | Plug-in registry contract. |
| 44 | `lvgl_ui/tile_registry.c` | same | **PORT** | New rows for new tiles; remove old (compass→LIS, light→VEML, env→BME688, …). |
| 45 | `lvgl_ui/ui_main_screen.h` | same | **PORT** | Resize to 410 × 502; new font set; tile slot geometry. |
| 46 | `lvgl_ui/ui_main_screen.c` | same | **PORT** | Same. |
| 47 | `lvgl_ui/ui_status_bar.h` | same | **PORT** | New icon ladder (charge state via BQ; SD card icon; mic icon). |
| 48 | `lvgl_ui/ui_status_bar.c` | same | **PORT** | Same. |
| 49 | `lvgl_ui/ui_navigation.h` | same | **PORT** | Tile order changes; encoder-as-input adds a new event source. |
| 50 | `lvgl_ui/ui_navigation.c` | same | **PORT** | Same. |
| 51 | `lvgl_ui/ui_lock_screen.h` | same | **PORT** | "Lock screen" is now "wake screen" (no lock concept); rebrand. |
| 52 | `lvgl_ui/ui_lock_screen.c` | same | **PORT** | Same. |
| 53 | `lvgl_ui/ui_settings_screen.h` | same | **PORT** | Add new settings (TZ, mic, SD logging, blue-light schedule). |
| 54 | `lvgl_ui/ui_settings_screen.c` | same | **PORT** | Same. |
| 55 | `lvgl_ui/ui_notif_overlay.h` | same | **COPY** | UI event-driven overlay; pattern stays. |
| 56 | `lvgl_ui/ui_notif_overlay.c` | same | **PORT** | Adjust geometry for 410 × 502. |
| 57 | `lvgl_ui/ui_shutdown_overlay.h` | same | **PORT** | Now displays the ship-mode confirmation flow. |
| 58 | `lvgl_ui/ui_shutdown_overlay.c` | same | **PORT** | Same. |
| 59 | `lvgl_ui/ui_theme_colors.h` | same | **COPY** | Color tokens still valid (RGB888 widens the palette but the named tokens don't need to change). |
| 60 | `lvgl_ui/system_tile.h` | same | **PORT** | Now exposes flashlight + WS2812 + SD eject; firmware version. |
| 61 | `lvgl_ui/system_tile.c` | same | **PORT** | Same. |
| 62 | `lvgl_ui/fa5_select_14.c` | same | **PORT** | Re-export with the larger glyph set we want at 410 × 502. |

## `components/alarm/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 63 | `alarm/CMakeLists.txt` | same | **COPY** | Trivial. |
| 64 | `alarm/alarm.h` | same | **COPY** | Pattern table + match API stays. |
| 65 | `alarm/alarm.c` | same | **COPY** | Pure software; no hardware dep. |
| 66 | `alarm/alarm_tile.h` | same | **PORT** | Resize for 410 × 502. |
| 67 | `alarm/alarm_tile.c` | same | **PORT** | Same. |

## `components/pcf85063/`  *(same chip on Mk I)*

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 68 | `pcf85063/CMakeLists.txt` | same | **COPY** | Trivial. |
| 69 | `pcf85063/pcf85063.h` | same | **COPY** | Same chip; Phase-15 UTC API preserved. |
| 70 | `pcf85063/pcf85063.c` | same | **COPY** | Add (later) 1 PPS-driven trim; not Phase 1. |
| 71 | `pcf85063/rtc_tile.h` | same | **PORT** | Resize. |
| 72 | `pcf85063/rtc_tile.c` | same | **PORT** | Same. |

## `components/drv2605/`  *(same chip, moves to bus 2)*

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 73 | `drv2605/CMakeLists.txt` | same | **COPY** | Trivial. |
| 74 | `drv2605/drv2605.h` | same | **COPY** | Same chip + address. |
| 75 | `drv2605/drv2605.c` | same | **PORT** | Move from bus 1 to bus 2 mutex; DRV_EN GPIO changes (GPIO0); JP12-aware. |
| 76 | `drv2605/haptic.h` | same | **COPY** | Wrapper API. |
| 77 | `drv2605/haptic.c` | same | **PORT** | Update bus selection + DRV_EN handling. |
| 78 | `drv2605/haptic_tile.h` | same | **PORT** | Resize. |
| 79 | `drv2605/haptic_tile.c` | same | **PORT** | Same. |

## `components/bh1750/` → renamed `components/veml6030/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 80 | `bh1750/CMakeLists.txt` | `veml6030/CMakeLists.txt` | **PORT** | Rename targets. |
| 81 | `bh1750/bh1750.h` | `veml6030/veml6030.h` | **PORT** | New chip; reuse tile types. |
| 82 | `bh1750/bh1750.c` | `veml6030/veml6030.c` | **PORT** | Rewrite low-level register access; keep task shape. |
| 83 | `bh1750/light_tile.h` | `veml6030/light_tile.h` | **COPY** | Tile API portable. |
| 84 | `bh1750/light_tile.c` | `veml6030/light_tile.c` | **PORT** | Update unit conversion + auto-brightness ramp for VEML lux range. |

## `components/bme280/` → renamed `components/bme688/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 85 | `bme280/CMakeLists.txt` | `bme688/CMakeLists.txt` | **PORT** | Vendor library swap (Bosch BME68x). |
| 86 | `bme280/bme280.h` | — | **DROP** | Bosch BME280 vendor header replaced by BME68x. |
| 87 | `bme280/bme280.c` | — | **DROP** | Same. |
| 88 | `bme280/bme280_defs.h` | — | **DROP** | Same. |
| 89 | `bme280/bme280_drv.h` | `bme688/bme688_drv.h` | **PORT** | Same role; new chip family. |
| 90 | `bme280/bme280_drv.c` | `bme688/bme688_drv.c` | **PORT** | Same role; new chip family. |
| 91 | `bme280/env_tile.h` | `bme688/env_tile.h` | **COPY** | Tile API portable. |
| 92 | `bme280/env_tile.c` | `bme688/env_tile.c` | **PORT** | Add gas / VOC fields; keep pressure→altitude path. |

## `components/max30102/` → renamed `components/max30101/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 93 | `max30102/CMakeLists.txt` | `max30101/CMakeLists.txt` | **PORT** | Rename. |
| 94 | `max30102/max30102.h` | `max30101/max30101.h` | **PORT** | Register-compatible family; add 3rd LED control. |
| 95 | `max30102/max30102.c` | `max30101/max30101.c` | **PORT** | INT → GPIO7; INT-driven FIFO. |
| 96 | `max30102/health_tile.h` | `max30101/health_tile.h` | **COPY** | Tile API portable. |
| 97 | `max30102/health_tile.c` | `max30101/health_tile.c` | **PORT** | Add SpO2 path; integrate TMP117 skin temp subtile. |

## `components/qmc5883p/` → renamed `components/lis3mdl/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 98 | `qmc5883p/CMakeLists.txt` | `lis3mdl/CMakeLists.txt` | **PORT** | Rename. |
| 99 | `qmc5883p/qmc5883p.h` | `lis3mdl/lis3mdl.h` | **PORT** | New chip header (address 0x1C, ST device ID 0x3D). |
| 100 | `qmc5883p/qmc5883p.c` | `lis3mdl/lis3mdl.c` | **PORT** | New chip body; keep task shape + read-before-write. |
| 101 | `qmc5883p/compass_tile.h` | `lis3mdl/compass_tile.h` | **COPY** | Tile API portable. |
| 102 | `qmc5883p/compass_tile.c` | `lis3mdl/compass_tile.c` | **PORT** | Calibration UX adds LRA-offset step. |

## `components/qmi8658/` → renamed `components/lsm6dsv16x/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 103 | `qmi8658/CMakeLists.txt` | `lsm6dsv16x/CMakeLists.txt` | **PORT** | Rename. |
| 104 | `qmi8658/qmi8658.h` | `lsm6dsv16x/lsm6dsv16x.h` | **PORT** | New chip header (ST WHO_AM_I 0x70). |
| 105 | `qmi8658/qmi8658.c` | `lsm6dsv16x/lsm6dsv16x.c` | **PORT** | New chip body; INT1 GPIO8 raise-to-wake; MLC config block (later). |
| 106 | `qmi8658/imu_tile.h` | `lsm6dsv16x/imu_tile.h` | **COPY** | Tile API portable. |
| 107 | `qmi8658/imu_tile.c` | `lsm6dsv16x/imu_tile.c` | **PORT** | Recompute scaling for ±4 g / 2000 dps default; keep complementary filter. |

## `components/tu10f/` → renamed `components/max_m10s/`

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 108 | `tu10f/CMakeLists.txt` | `max_m10s/CMakeLists.txt` | **PORT** | Rename. |
| 109 | `tu10f/tu10f.h` | `max_m10s/max_m10s.h` | **PORT** | Same NMEA struct + add UBX-TIMEUTC path. |
| 110 | `tu10f/tu10f.c` | `max_m10s/max_m10s.c` | **PORT** | Keep NMEA + timegm fix; add UBX parser; UART pins 17/18; 1 PPS ISR on GPIO46 (new file? — embed it here for now). |
| 111 | `tu10f/gps_tile.h` | `max_m10s/gps_tile.h` | **COPY** | Tile API portable. |
| 112 | `tu10f/gps_tile.c` | `max_m10s/gps_tile.c` | **PORT** | UBX-TIMEUTC means time-before-fix UX changes; update display. |

## Net-new components (no source in old tree)

| # | Old path | New path | Verdict | Reason |
|---|---|---|---|---|
| 113 | (none) | `components/co5300/{co5300.c,co5300.h,co5300_te_isr.c,CMakeLists.txt}` | **REWRITE** | CO5300 QSPI AMOLED panel driver. |
| 114 | (none) | `components/cst9217/{cst9217.c,cst9217.h,CMakeLists.txt}` | **REWRITE** | CST9217 ISR-driven touch driver. |
| 115 | (none) | `components/bq25619/{bq25619.c,bq25619.h,battery_tile.{c,h},CMakeLists.txt}` | **REWRITE** | BQ25619 charger + fuel-gauge + ship-mode driver; replaces `app_battery`. |
| 116 | (none) | `components/tmp117/{tmp117.c,tmp117.h,CMakeLists.txt}` | **REWRITE** | Skin-temperature sensor on daughter board. |
| 117 | (none) | `components/encoder/{encoder.c,encoder.h,CMakeLists.txt}` | **REWRITE** | Crown encoder via PCNT + edge ISR. |
| 118 | (none) | `components/ws2812/{ws2812.c,ws2812.h,CMakeLists.txt}` | **REWRITE** | Single-LED status pixel (RMT, GPIO42). |
| 119 | (none) | `components/flashlight/{flashlight.c,flashlight.h,CMakeLists.txt}` | **REWRITE** | LEDC PWM on GPIO41. |
| 120 | (none) | `components/sdcard/{sdcard.c,sdcard.h,CMakeLists.txt}` | **REWRITE** | SDMMC on GPIO38/39/40. |
| 121 | (none) | `components/mic_pdm/{mic_pdm.c,mic_pdm.h,CMakeLists.txt}` | **REWRITE** | PDM mic via I2S0 on GPIO47/48. |
| 122 | (none) | `components/qvar_ecg/{qvar_ecg.c,qvar_ecg.h,ecg_tile.{c,h},CMakeLists.txt}` | **REWRITE** | Qvar ECG channel inside LSM6DSV16X. Phase 5. |

---

## Summary roll-up

| Verdict | Count (rows) | Notes |
|---|---|---|
| COPY | **24** | Mostly broker / cross_driver / NVS / alarm / pcf85063 / drv2605 headers / tile headers. |
| PORT | **57** | Most sensor `.c` files, every `_tile.c`, lvgl_ui body files, fusion. |
| REWRITE | **15** | Display + touch + charger + main.c-orchestrator step changes + 10 net-new components. |
| DROP | **9** | ETA6098 ADC battery, BME280 vendor files, stale sdkconfigs, stale lock + listing. |
| **Total old files counted** | **112** | Matches the `~113` quoted in the task brief (we have 112 in `components/` + `main/` + root configs; the rest are net-new files that don't appear in the old tree). |

---
*End of migration map.*
