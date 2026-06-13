# CrossDriver Porting — iv7.2.f0.0
**Date:** 2026-06-13
**Module:** `firmware/components/cross_driver/`
**Phase:** 6.2 (Infrastructure)
**Ported by:** Opus
**Master prompt:** `firmware/docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md`
**Batch brief:** `firmware/docs/19_PHASE_6_BATCH_INFRASTRUCTURE.md`

---

## Summary

Cross-driver (XD) event bus — Core 0–only publish/subscribe for driver-to-driver
interactions that should not pass through the broker. Static-table dispatcher:
zero malloc, no FreeRTOS primitives, registrations are boot-time only.

This module is chip-agnostic by design. Phase 6 work is **(a)** comment refresh
to name the v7.2 chip set and **(b)** new event ordinals to anticipate v7.2-only
producers/consumers (TMP117, crown encoder, alarm, boot-button long press,
SD card, PDM mic, Qvar ECG).

## Pinout / bus from v7.2

N/A. RAM-only.

## Code audit (v5 → v7.2)

- `XD_EVENT_ENV_UPDATED` comment: `(BME280/BME688, future)` → `(BME688)`.
- `XD_EVENT_HR_UPDATED` comment: `(MAX30101, future)` → `(MAX30101)`.
- `XD_EVENT_LIGHT_UPDATED` comment: `(BH1750, future)` → `(VEML6030)`.
- Seven new ordinals appended at the **end** of the enum (ordinal values are
  array indices into `s_slots` — never reorder, never insert):
  - `XD_EVENT_SKIN_UPDATED      = 8`
  - `XD_EVENT_CROWN_TURN        = 9`
  - `XD_EVENT_ALARM_FIRED       = 10`
  - `XD_EVENT_BUTTON_LONG       = 11`
  - `XD_EVENT_SD_MOUNTED        = 12`
  - `XD_EVENT_MIC_FRAME_READY   = 13`
  - `XD_EVENT_ECG_FRAME_READY   = 14`
- `s_event_names[]` extended with the seven new strings — designated initialisers,
  so order does not matter but every ordinal must be present or
  `cross_driver_event_name()` returns NULL and prints "UNNAMED" in logs.

## Implementation

No behavioural changes. Pure additive port:
- `cross_driver.h` enum extended; comments refreshed.
- `cross_driver.c` event-name table extended.
- `CMakeLists.txt` unchanged.

Memory growth: `s_slots` array grows from 8 to 15 entries; each slot is
`sizeof(cross_driver_cb_t) * 4 + uint8_t` ≈ 17 bytes on 32-bit pointers. Total
extra static cost ≈ 7 × 17 = 119 bytes. Negligible.

## Test harness

`firmware/test/test_cross_driver.c`:

1. Call `cross_driver_init()`.
2. Register one callback for `XD_EVENT_GPS_FIX_VALID`; fire with a synthetic
   payload; assert callback observed it.
3. Register 4 callbacks for `XD_EVENT_ALARM_FIRED` (max); assert all 4 fire and
   observe the payload in registration order.
4. Fire a 0-listener event (`XD_EVENT_MIC_FRAME_READY`); assert no crash and
   dispatch returns quickly.
5. Iterate the event-name table — every ordinal `0..XD_EVENT_COUNT-1` must
   return a non-NULL string.
6. (Optional / commented out by default) negative test: a 5th
   `cross_driver_register()` for the same event must hit `configASSERT`.

## Profiling

### Boot-time cost
| Phase | Component | Time (µs) | Method |
|-------|-----------|-----------|--------|
| Init  | `cross_driver_init()` (memset) | TBD | `esp_timer_get_time()` |

### Per-operation cost
| Operation | Time (µs) | Notes |
|-----------|-----------|-------|
| `cross_driver_fire` 0 listeners | TBD | early-out, no callback loop |
| `cross_driver_fire` 1 listener  | TBD | one indirect call |
| `cross_driver_fire` 4 listeners | TBD | indirect call × 4 |
| `cross_driver_register`         | TBD | array append, asserts |

### Memory
| Metric | Value | Notes |
|--------|-------|-------|
| `s_slots` (15 events × 4 cb × ptr + count) | ≈ 255 bytes (32-bit ptrs) | static |
| `s_event_names` | 15 × ptr ≈ 60 bytes | static |
| Stack high-water (dispatch) | TBD | callbacks own their own stack |

### Current draw
N/A — RAM-only dispatcher.

### Notes
- Dispatch is synchronous and Core 0–local — callbacks must stay short
  (the master rule). Adding events does not relax this.
- The new `XD_EVENT_MIC_FRAME_READY` / `XD_EVENT_ECG_FRAME_READY` payloads
  reference driver-defined frame structs that are **not yet declared**
  (mic_pdm.h, qvar_ecg.h ship without frame typedefs today). Producers
  can fire NULL until the structs land — consumers will need to defer.

## Defects discovered

[DEFECT-001] MIC and ECG frame typedefs not declared in driver headers | components/mic_pdm/mic_pdm.h + components/qvar_ecg/qvar_ecg.h | LOW | `XD_EVENT_MIC_FRAME_READY` and `XD_EVENT_ECG_FRAME_READY` reference `mic_pdm_frame_t*` and `qvar_ecg_frame_t*` respectively in the event comment, but neither header declares such a type today. Disposition: cosmetic. Producers will fire with whatever payload shape the driver actually exposes when the consumer pattern is needed; the comment is forward-looking guidance, not a contract.

[DEFECT-002] No automated wakeup if a producer fires before any consumer registers | cross_driver.c:74-90 | LOW | If a Core 0 task is created and fires events before all boot_hw_init.c `cross_driver_register()` calls have run, those events are silently dropped (`slot->count == 0` early-out). The master rule already says "register before xTaskCreate" so this is correct-by-convention, not a bug. Disposition: keep the comment in cross_driver.h explicit; add a runtime warning if a fire happens with 0 listeners on an event that has at least one *expected* consumer (future v2 idea).

## Open questions

None. Module is additive-only; no behavioural risk.
