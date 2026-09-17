# LOG_TAGS -- canonical log-tag legend

**Date opened:**  2026-09-15
**Date closed:**  (open -- living reference)
**Status:**       OPEN

**Purpose.** Every `ESP_LOGx` and CLI `printf` line in this firmware
carries an ALL_CAPS component tag so `git grep <TAG>` locates the
origin in one hop. This file is the authoritative list. Per
[[feedback-log-line-taxonomy]], when a new component lands, add its
tag here in the same commit. Existing lines migrate opportunistically,
never in a mass-rename pass.

**Format.** Tag ↔ source component ↔ one-line purpose. Grouped by
subsystem for scan-ability, not alphabetically.

---

## Core / boot / app layer

| Tag         | Source                                     | Purpose |
| ----------- | ------------------------------------------ | ------- |
| BOOT_HW     | boot_logic/boot_hw_init.c                  | Always-on hardware bring-up (I2C, sensors, BQ). |
| BOOT_DISP   | boot_logic/boot_display.c                  | Display + panel_io + panel init, sleep/wake. |
| BOOT_TASKS  | boot_logic/boot_tasks.c                    | FreeRTOS task spawning. |
| BOOT_PM     | boot_logic/boot_pm.c                       | Power-management lock inventory dump. |
| BOOT_POWER  | boot_logic/boot_power.c                    | Reset-reason + wakeup source classification. |
| APP         | app_core/* (post-Stage-34.1 rename)        | Main mode state machine + FCM_* dispatch. |
| FIELD       | field_capture/field_capture.c              | LEGACY -- migrating to APP. |
| FC_COMMON   | field_capture/fc_common.c                  | Button + encoder polled state machines. |
| FC_MODES    | field_capture/fc_modes.c                   | Recording modes (ENV, MOT, QVAR, etc.). |
| FC_MODES_LSM| field_capture/fc_modes_lsm.c               | LSM submenu recording paths. |
| FC_REC      | field_capture/fc_rec.c                     | CSV recording plumbing. |
| FC_SHDN     | field_capture/fc_shutdown.c                | Shutdown watcher + auto-shdn timer. |
| FC_BATT     | field_capture/fc_battery_test.c            | Battery-test mode CSV logger. |
| FC_PPG_BCG  | field_capture/fc_ppg_bcg.c                 | PPG+BCG combined recording. |
| FC_CLI      | field_capture/fc_cli.c                     | USB-CDC command surface. |
| APP_NVS     | app_nvs/*                                  | NVS flash init + partition mount. |
| NVS_CFG     | nvs_cfg/*                                  | Per-driver NVS config load/save. |

## Data + UI infrastructure

| Tag         | Source                             | Purpose |
| ----------- | ---------------------------------- | ------- |
| BROKER      | data_broker/*                      | Cross-core sensor data-flow bus. |
| UI_BROKER   | ui_broker/*                        | UI-side settings + state channels. |
| UI_NAV      | lvgl_ui/ui_navigation.c            | Screen + gesture routing. |
| LVGL_UI     | lvgl_ui/lvgl_ui.c                  | Top-level LVGL init + tile registry. |
| LVGL_DISP   | lvgl_ui/lvgl_ui_display.c          | LVGL port + display setup. |
| LVGL_SHOT   | lvgl_ui/lvgl_ui_screenshot.c       | Frame-buffer PNG capture. |

## Screens + tiles

| Tag           | Source                            | Purpose |
| ------------- | --------------------------------- | ------- |
| MAIN_SCR      | lvgl_ui/ui_main_screen.c          | Watch-face + clock + battery. |
| STATUS_BAR    | lvgl_ui/ui_status_bar.c           | 5-dot state row. |
| SETTINGS_SCR  | lvgl_ui/ui_settings_screen.c      | Tileview + settings screen. |
| ALARM_TILE    | lvgl_ui/alarm_screen.c            | Alarm slot overlay. |
| NOTIF_OVL     | lvgl_ui/notification_overlay.c    | Toasts / overlays. |
| SHUTDOWN_OVL  | lvgl_ui/shutdown_overlay.c        | Shutdown-hold LED-mirror overlay. |
| DISP_SLEEP    | lvgl_ui/display_sleep.c           | Panel sleep/wake helpers. |
| HEALTH_TILE   | lvgl_ui/health_tile.c             | MAX30101 PPG tile. |
| HAPTIC_TILE   | lvgl_ui/haptic_tile.c             | DRV2605 haptic tile. |
| LIGHT_TILE    | lvgl_ui/light_tile.c              | VEML6030 ambient-light tile. |
| BAT_TILE      | lvgl_ui/battery_tile.c            | BQ25619 + MAX17048 tile. |
| SYS_TILE      | lvgl_ui/system_tile.c             | Heap + uptime + die-temp tile. |
| GPS_TILE      | lvgl_ui/gps_tile.c                | MAX-M10S GPS tile. |
| RTC_TILE      | lvgl_ui/rtc_tile.c                | PCF85063 clock tile. |
| ENV_TILE      | lvgl_ui/env_tile.c                | BME688 environment tile. |
| COMPASS_TILE  | lvgl_ui/compass_tile.c            | LIS3MDL compass tile. |
| IMU_TILE      | lvgl_ui/imu_tile.c                | LSM6DSV16X IMU tile. |

## Drivers

| Tag           | Source                            | Purpose |
| ------------- | --------------------------------- | ------- |
| BQ25619       | bq25619/bq25619.c                 | Charger IC. |
| BQ_CMD        | bq25619/bq25619_cmd.c             | BQ CLI surface. |
| MAX17048      | max17048/max17048.c               | Fuel gauge. |
| BME688        | bme688/*                          | Env sensor. |
| LSM6DSV16X    | lsm6dsv16x/*                      | 6-axis IMU. |
| LSM_EMB       | lsm6dsv16x/lsm_embedded.c         | LSM MLC / embedded funcs. |
| LIS3MDL       | lis3mdl/*                         | Magnetometer. |
| VEML6030      | veml6030/veml6030.c               | Light sensor. |
| VEML_CMD      | veml6030/veml6030_cmd.c           | VEML CLI surface. |
| TMP117        | tmp117/*                          | Skin temp sensor. |
| MAX30101      | max30101/*                        | PPG (heart-rate LEDs). |
| MAX_M10S      | max_m10s/*                        | GPS. |
| PCF85063      | pcf85063/pcf85063.c               | RTC. |
| PCF_CMD       | pcf85063/pcf85063_cmd.c           | PCF CLI surface. |
| DRV2605       | drv2605/drv2605.c                 | Haptic driver. |
| DRV_CMD       | drv2605/drv2605_cmd.c             | DRV CLI surface. |
| HAPTIC_TILE   | drv2605/haptic_tile.c             | (Owned by drv2605 comp.) |
| WS2812        | ws2812/ws2812.c                   | RGB LED. |
| WS2812_CMD    | ws2812/ws2812_cmd.c               | RGB CLI surface. |
| RGB_POLICY    | rgb_policy/*                      | LED colour policy. |
| FLASHLIGHT    | flashlight/*                      | LEDC-PWM flashlight. |
| CST9217       | cst9217/*                         | Touch controller. |
| MIC_PDM       | mic_pdm/*                         | PDM microphone. |
| SDCARD        | sdcard/*                          | SD host / mount. |
| USB_MSC       | usb_msc/*                         | USB mass-storage. |
| ALARM         | alarm/*                           | Alarm task. |
| ENCODER       | encoder/encoder.c                 | Rotary encoder driver. |
| ENC_CMD       | encoder/encoder_cmd.c             | Encoder CLI surface. |
| FUSION        | fusion/*                          | Sensor fusion (mag+IMU). |

## Panels (Stage 30 additions)

| Tag              | Source                            | Purpose |
| ---------------- | --------------------------------- | ------- |
| co5300_panel     | co5300/co5300_panel.c             | CO5300 esp_lcd panel wrapper. **Non-conforming (lowercase)** -- rename to `CO5300_PANEL` next time this file is edited. |
| co5300_pio       | co5300/co5300_panel_io.c          | CO5300 custom panel_io. **Non-conforming** -- rename to `CO5300_PIO`. |

## Known-drifting / TBD

| Tag  | Note |
| ---- | ---- |
| XD   | Mystery tag surfaced by grep. Origin unknown; hunt on next opportunistic pass. |
| FIELD | Legacy; migrates to `APP` when field_capture -> app_core rename lands (Stage 34.1). |
