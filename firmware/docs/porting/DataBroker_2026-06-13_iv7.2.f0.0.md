# DataBroker Porting — iv7.2.f0.0
**Date:** 2026-06-13
**Module:** `firmware/components/data_broker/`
**Phase:** 6.1 (Infrastructure)
**Ported by:** Opus
**Master prompt:** `firmware/docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md`
**Batch brief:** `firmware/docs/19_PHASE_6_BATCH_INFRASTRUCTURE.md`

---

## Summary

Central data broker — the single legal data path between Core 0 (sensor tasks) and
Core 1 (LVGL UI). Owns a single FreeRTOS mutex + per-module `broker_xxx_data_t`
slots. Generates 8 functions per module (write/read/get_status/set_enabled/
get_enabled/set_hw_status/hw_alive) via the `BROKER_MODULE_IMPL` macro. Also
creates `g_ui_event_q`, the Core 0 → Core 1 command queue.

No hardware dependency — broker is a pure RAM data structure plus a mutex and a
queue. Lives in SRAM.

## Pinout / bus from v7.2

N/A. The broker is hardware-agnostic.

## Code audit (v5 → v7.2)

| v5 #include       | v7.2 #include  | Why                                       |
|-------------------|----------------|-------------------------------------------|
| `tu10f.h`         | `max_m10s.h`   | GPS chip change (u-blox MAX-M10S)         |
| `qmc5883p.h`      | `lis3mdl.h`    | Magnetometer chip change (ST LIS3MDLTR)   |
| `app_battery.h`   | `bq25619.h`    | Battery moved into BQ25619 driver         |
| `bh1750.h`        | `veml6030.h`   | Light chip change (Vishay)                |
| `qmi8658.h`       | `lsm6dsv16x.h` | IMU chip change (ST)                      |
| `bme280_drv.h`    | `bme688_drv.h` | Env chip change (Bosch BME688)            |
| `max30102.h`      | `max30101.h`   | HR chip change (Maxim — same family)      |
| —                 | `tmp117.h`     | NEW: skin-temperature broker slot         |

All seven Phase 0–5 driver headers were verified to export the broker contract
(`broker_xxx_data_t` typedef + `BROKER_XXX_TIMEOUT_MS` macro) with the field names
the broker uses. No upstream chip-driver edits needed.

Mojibake from the v5 cp pass (`â†'`, `Â§`) replaced with the real UTF-8 glyphs
(`→`, `§`) in the file-header block comment.

## Implementation

### `data_broker.h` changes
- Seven `#include` swaps as per the table above.
- One new `#include "tmp117.h"` to pull in `broker_skin_data_t`.
- New 7-line API block: `broker_skin_*` (write/read/get_status/set_enabled/
  get_enabled/set_hw_status/hw_alive).
- File-header block-comment mojibake cleaned.

### `data_broker.c` changes
- `broker_state_t` gains `broker_skin_data_t skin;` member.
- `hw` flag struct gains `skin` bool.
- `BROKER_MODULE_IMPL(skin, SKIN, BROKER_SKIN_TIMEOUT_MS, true)` — skin is
  toggleable (user can disable continuous skin-temp polling for power).

### `CMakeLists.txt` rewrite
Old REQUIRES set (`tu10f qmc5883p bh1750 qmi8658 app_logic` + duplicates) replaced
with the v7.2 set: `pcf85063 drv2605 max_m10s lis3mdl veml6030 lsm6dsv16x bq25619
bme688 max30101 tmp117 alarm fusion`. `freertos` + `esp_timer` retained.

> NOTE: `app_logic` was dropped from REQUIRES. The broker no longer needs it —
> battery moved into BQ25619, and the broker only proxies data structs (no NVS).

## Test harness

`firmware/test/test_data_broker.c` — standalone ESP32 test:

1. Call `broker_init()` and assert the mutex + queue handles are non-NULL.
2. For each slot (gps, mag, rtc, battery, light, imu, env, hr, skin, haptic,
   fusion, alarm):
   - `set_hw_status(false)` → `get_status()` returns `SENSOR_OFFLINE`.
   - `set_hw_status(true)` + `set_enabled(false)` (where toggleable) → returns
     `SENSOR_DISABLED`.
   - `set_hw_status(true)` + `set_enabled(true)` + fresh `write()` → returns
     `SENSOR_ONLINE`.
   - Wait > `BROKER_XXX_TIMEOUT_MS` ticks → returns `SENSOR_STALE`.
3. Spawn two tasks: one writes broker_imu @ 100 Hz, one reads broker_imu @ 200 Hz.
   Assert no mutex-timeout warnings appear over a 10-second window (`ESP_LOGW`
   capture).
4. `xQueueSend(g_ui_event_q, ...)` × 8 (fill) + `xQueueReceive` × 8 (drain) round
   trip. Assert all 8 events recovered in order.

## Profiling

### Boot-time cost
| Phase | Component | Time (ms) | Method |
|-------|-----------|-----------|--------|
| Init  | `broker_init()` (mutex + queue) | TBD | `esp_timer_get_time()` before/after |
| Init  | First per-slot `set_hw_status` (12×) | TBD | sum across slots |

### Per-operation cost
| Operation | Time | Notes |
|-----------|------|-------|
| `broker_xxx_write` (memcpy + lock) | TBD | timeout = 10 ms |
| `broker_xxx_read` (memcpy + lock) | TBD | timeout = 10 ms |
| `broker_xxx_get_status` (lock + age compute) | TBD | timeout = 5 ms |
| `ui_event_send` (xQueueSend, non-blocking) | TBD | depth 8 |

### Memory
| Metric | Value | Notes |
|--------|-------|-------|
| `broker_state_t` | TBD | sum of 12 `broker_xxx_data_t` + mutex handle |
| `hw` flag struct | 12 bytes (one bool per module) | static |
| `g_ui_event_q` | 8 × sizeof(ui_event_t) ≈ 832 bytes | depth 8 |
| Stack high-water on producer task | TBD | `uxTaskGetStackHighWaterMark` |

### Current draw
| State | Current (mA) | Notes |
|-------|--------------|-------|
| Mutex acquire | negligible | RAM-only |
| Queue send | negligible | RAM-only |

### Notes
- Mutex contention is the only realistic bottleneck. Hold time is bounded by
  `memcpy(sizeof(broker_xxx_data_t))` — all slots are <128 bytes so this is
  microseconds.
- Sole heavy memory cost is `g_ui_event_q` (832 bytes). Acceptable in SRAM.

## Defects discovered

[DEFECT-001] BQ25619 NVS namespace ownership unclear | components/bq25619/bq25619.c:138-153 | MED | The v5 "battery" NVS namespace (lifetime min/max voltage curve) is currently RAM-only inside bq25619.c (`s_vbat_min_mv`, `s_vbat_max_mv` static globals; TODO comment at line 147 confirms persistence not yet wired). app_battery.c is dead. The lifetime-extreme tracking is therefore lost across reboots. Disposition: file as Phase-1 BQ25619 follow-up; do NOT silently absorb into app_logic. See also AppNVS_2026-06-13.md DEFECT-002.

[DEFECT-002] No tile descriptor for skin temperature | components/tmp117/ | LOW | TMP117 has `broker_skin_data_t` + `BROKER_SKIN_TIMEOUT_MS` and the broker now proxies them, but there is no `temp_tile.{c,h}` exposing a `tile_desc_t`. The chip is driver-only at present; UI surfacing deferred to v2 per Ivan's Phase 6 decision. Disposition: tracked; intentional.

[DEFECT-003] `fusion` is REQUIRED by data_broker but `fusion.h` itself includes data_broker indirectly | components/data_broker/data_broker.h:91 + components/fusion/fusion.c:21 | LOW | Compile-order works today because `broker_fusion_data_t` is defined in `fusion.h` with no broker include, but the dependency graph is brittle. If future fusion features need `broker_xxx_get_status` *from inside fusion.h's typedef block*, the build will cycle. Disposition: monitor; document the "fusion.h must stay header-only typedef-wise" rule in the file header comment in a later pass.

## Open questions

None blocking. Confirmation on DEFECT-001 vs. opening a Phase-1 follow-up commit
is the only outstanding item — see AppNVS doc for the matching note.

## Profiling timestamps

To be filled when first ESP32 board is in hand. All "TBD" entries above are placeholders.
