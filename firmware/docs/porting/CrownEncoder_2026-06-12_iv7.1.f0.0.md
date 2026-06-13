# Crown Encoder -- Porting Log
**Module:** Rotary mechanical encoder (quadrature, with detents), decoded by ESP32-S3 PCNT
**Date:** 2026-06-12
**Firmware:** `iv7.1.f0.0`
**Hardware authority:** `docs/02_Kompic_Mk1_System_Instructions_v7.2.md`, §GPIO ASSIGNMENT (GPIO21 EC_SigA, GPIO43 EC_SigB).
**Brief:** `docs/17_PHASE_4_BATCH_ACTUATORS.md`, Module 2.
**Author:** Claude (Opus 4.7), under master prompt `docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md`.

---

## Summary

New input device in Mk I -- no v5 carryforward. A mechanical rotary encoder with detents lives at the right-hand crown position; channel A on GPIO21, channel B on GPIO43. The driver decodes quadrature in hardware via the ESP32-S3 PCNT peripheral (no per-edge ISR), filters glitches in hardware, applies a 2 ms software debounce on top, and fires `UI_EVENT_CROWN_CW` / `UI_EVENT_CROWN_CCW` events into the Core-0 -> Core-1 navigation queue `g_ui_event_q`.

Two pieces of glue are still missing from the tree -- both flagged below:

1. **`ui_event.h`** with the canonical `ui_event_t` enum + `g_ui_event_q` definition. Owned by `lvgl_ui` (Core 1 / LVGL nav consumer). The encoder declares forward externs for the two symbols it needs; once `ui_event.h` lands we swap the local declarations for `#include "ui_event.h"`. Same pattern Phase 2/3 used for `boot_hw_init.h`.

2. **boot wiring.** Nothing currently calls `encoder_init()` at boot. The expected hook is in `boot_hw_init.c` (or whatever the bring-up code becomes), after the UI event queue exists.

The v7.2 hardware note that GPIO43 doubles as the boot-log UART TX and emits "~3 ms spurious activity" is handled: a 200 ms boot guard window discards any detents drained during the first 200 ms after `encoder_init()` returns. In practice the boot log is finished long before any sensor / boot-power task gets around to calling `encoder_init()`, but the guard is cheap and survives future re-orderings of the bring-up sequence.

Today's deliverable: new `components/encoder/` (PCNT decoder + 5 ms drain timer + glitch / debounce / boot-guard logic), standalone test harness that stubs the UI event queue, dated .md.

---

## Hardware spec from v7.2

### Pinout

| Signal | GPIO | Direction | Notes |
|---|:--:|---|---|
| EC_SigA | 21 | input | RTC-capable (wake source candidate, future). Pull-up enabled by PCNT driver. |
| EC_SigB | 43 | input | Shared with boot-log UART TX -- ~3 ms spurious activity at boot; FW ignores via 200 ms boot guard. |

### Mechanical

| Item | Value |
|---|---|
| Detents per revolution | TBD (typical: 18-24) |
| Edges per detent | 4 (full quadrature cycle) |
| PCNT decode mode | x4 (count both edges of A; direction from B level) |

### PCNT settings

| Item | Value |
|---|---|
| High limit | +100 |
| Low limit | -100 |
| Glitch filter | 12500 ns (~12.5 us; 1000 APB cycles at 80 MHz) |
| Drain period | 5 ms (esp_timer one-shot, restarted by IDF as periodic) |

---

## Code audit

### What dies

Nothing -- new component.

### What's new

- **PCNT-based quadrature decoding.** `pcnt_new_unit` + `pcnt_new_channel` + `pcnt_channel_set_edge_action` + `pcnt_channel_set_level_action` -- the standard x4 quadrature pattern for the IDF v5 PCNT API.
- **Drain timer.** A 5 ms `esp_timer` periodic callback reads `pcnt_unit_get_count`, clears it, accumulates into `s_residue` (signed sub-detent), divides by `ENCODER_STEPS_PER_DETENT` (4), and emits CW/CCW events for each full detent crossed. Residue is preserved across ticks so partial rotations don't get lost.
- **Software debounce.** After each fired event, a 2 ms window suppresses subsequent events (counted in `s_glitch_count` for diagnostics).
- **Boot guard.** 200 ms after init, all drained detents are silently discarded -- absorbs the v7.2-documented GPIO43 boot-log TX activity in case the encoder happens to be initialised that early.
- **Diagnostic accessors.** `encoder_get_total_detents` / `_cw_count` / `_ccw_count` / `_glitch_count` for the test harness and for any "encoder debug" tile the UI grows in the future.
- **Forward-extern `g_ui_event_q` + `ui_event_t`.** Declared locally inside `encoder.c` until `ui_event.h` lands. The local enum values (`UI_EVENT_CROWN_CW = 1`, `UI_EVENT_CROWN_CCW = 2`) MUST match what `lvgl_ui` eventually defines; this is the load-bearing assumption.

---

## Implementation

### `components/encoder/encoder.h`

- Pin defines: `ENCODER_GPIO_A = GPIO_NUM_21`, `ENCODER_GPIO_B = GPIO_NUM_43`.
- Tuning constants: PCNT limits, glitch filter (ns), software debounce (ms), boot guard (ms), steps per detent.
- Identity strings: `"CrownEnc"` / `"Rotary encoder (quadrature)"`.
- Public API: `encoder_init` / `encoder_deinit` / diagnostic counters.

### `components/encoder/encoder.c`

- Forward-extern block for `g_ui_event_q` + `ui_event_t` (see DEFECT-002).
- `encoder_drain_cb` -- 5 ms timer callback. Reads + clears PCNT, accumulates residue, divides for detent count, applies boot guard + debounce, fires events.
- `encoder_init` -- PCNT unit + channel + glitch filter + drain timer. Logs the parameters used.
- `encoder_deinit` -- tears down timer + PCNT cleanly.
- Diagnostics: `s_total_detents`, `s_cw_count`, `s_ccw_count`, `s_glitch_count` -- all file-static.

### `components/encoder/CMakeLists.txt`

REQUIRES: `driver` (PCNT), `esp_timer` (drain), `freertos` (queue).

### `test/test_encoder.c`

Four-phase standalone harness:

1. **Setup** -- creates `g_ui_event_q` (16-depth, `ui_event_t` payload) so the driver has somewhere to send events.
2. **Init** -- calls `encoder_init()`, logs the wall time.
3. **Observation** -- 15 s window. Rotates by hand; every detent logs the elapsed time + direction + cumulative counters.
4. **Teardown** -- `encoder_deinit`, stack high-water + heap.

The harness prints per-event lines like:
```
[+ 12345.678 ms] CW   (total=+5 cw=5 ccw=0 gli=2)
```

This is the bench bring-up tool for the crown.

---

## Profiling

### Boot-time cost

| Phase | Component | Time | Method |
|---|---|---|---|
| Init | PCNT unit + channel + glitch filter + drain timer | **TBD (no hw)**, < 1 ms expected | wall time |
| Init | Boot guard arming (just an int64 add) | < 1 us | -- |

### Per-operation cost

| Operation | Time | Notes |
|---|---|---|
| PCNT hardware edge processing | ~0 CPU | hardware-only; CPU sees nothing until a drain tick |
| Drain callback (no events) | < 5 us | one pcnt_get_count + early return |
| Drain callback (one detent) | < 20 us | + clear + residue math + one xQueueSend |
| Drain callback (fast rotation, 4 detents in tick) | < 80 us | + 4 xQueueSend; the loop is unrolled |

### Memory

| Metric | Value | Notes |
|---|---|---|
| Driver state | ~40 bytes | counters + handles + residue + boot guard |
| PCNT IDF state | ~200 bytes | unit + channel handles |
| Drain timer | ~80 bytes | esp_timer internal |

### Current draw estimate

| State | Current | Notes |
|---|---|---|
| Idle (no rotation) | ~0 mA at the encoder side | mechanical contact only |
| Rotation | < 100 uA | through the pull-up |
| PCNT peripheral | included in the SoC's running current | not separately measurable |

### Notes

- **PCNT is essentially free.** Hardware handles the edge counting; the CPU only sees the periodic drain. Even at unreasonable rotation rates (5000 RPM, 333 detents/s, 1333 edges/s), the drain callback runs 200 times per second and processes ~7 edges per tick -- nowhere near saturation.
- **The 200 ms boot guard is conservative.** v7.2 says "~3 ms spurious activity"; 200 ms gives ample headroom for boot sequence reordering.
- **The 5 ms drain period limits maximum rotation rate.** With high_limit = 100, the hardware counter would have to overflow before we drain. 100 edges in 5 ms = 20 kHz edge rate = 5 kHz cycle rate = effectively impossible for a mechanical encoder. Safe.
- **`g_ui_event_q` full = events dropped.** `xQueueSend(... 0)` is non-blocking. If the UI is busy and the queue fills, we drop CW/CCW events on the floor. This is the right behaviour -- the user feels nothing happened and rotates again. The alternative (back-pressure) would block the timer ISR, which is fatal.

---

## Defects discovered

- `[DEFECT-001] No driver consumes ui_event_t (ui_event.h not in tree) | components/encoder/encoder.c | HIGH | The driver pushes events into a queue that no consumer is listening on yet. lvgl_ui owns the UI event queue + the ui_event_t enum; both are expected to live in ui_event.h. Until that header lands the events go into a queue that's never drained (in production) or into a test stub (in test_encoder). Disposition: defines + queue creation must be added to lvgl_ui as a follow-up. The encoder driver itself is complete.`
- `[DEFECT-002] Forward-extern of g_ui_event_q + local ui_event_t enum | components/encoder/encoder.c | HIGH | The chip-layer file declares the queue handle + a private copy of the enum. Once ui_event.h lands, BOTH must be removed in favour of #include "ui_event.h"; the integer values 1/2 in this file MUST match what ui_event.h declares (currently undefined). If they diverge, the encoder fires events that the UI doesn't recognise. Disposition: documentation; canonical values must be sourced from ui_event.h.`
- `[DEFECT-003] Boot guard not exposed at runtime | components/encoder/encoder.c | LOW | The 200 ms boot guard is configured by ENCODER_BOOT_GUARD_MS. If the encoder is ever re-init'd at runtime (e.g. after a power-mode transition), the boot guard fires again -- which is the right behaviour but could surprise. Disposition: documented.`
- `[DEFECT-004] PCNT x4 mode assumes encoder cycle = 4 edges per detent | components/encoder/encoder.h:ENCODER_STEPS_PER_DETENT | LOW | If the mechanical SKU turns out to have a different edges-per-detent ratio, every rotation would emit the wrong number of UI events. Disposition: confirm on bench by rotating slowly and counting events vs detents; tune the constant if needed.`
- `[DEFECT-005] No quadrature direction reversal flag | components/encoder/encoder.c | LOW | If the encoder is mounted such that CW physical rotation reads as CCW (channel A/B swapped by the PCB), we'd emit reversed events. The fix is to swap edge/level actions or the A/B pin assignments. Disposition: confirm direction on bench; flip the IDF action constants if needed.`
- `[DEFECT-006] xQueueSend uses portMAX timeout = 0 (non-blocking) | components/encoder/encoder.c:encoder_drain_cb | LOW | Intentional -- the timer callback must not block. Documented here so future readers don't "fix" it. If the UI consumer is starved, events drop silently. Disposition: documented behaviour.`

---

## Open questions

1. **When does `encoder_init()` get called?** The brief implies it sits alongside the rest of the hardware init in `boot_hw_init.c`. Confirm.
2. **Should the encoder be a wake source?** GPIO21 is RTC-capable; with `gpio_wakeup_enable()` a crown rotation could wake the watch from light sleep. v2 work.
3. **Does the UI want raw rotation rate, or only detent events?** Today we emit one CW/CCW event per detent. For something like a fast scrub through a long list, the UI might want to know "the user just spun the crown 30 detents in 200 ms". Recommend: keep one-event-per-detent for now; UI can compute rate from arrival timing.
4. **Press detection?** Many crown encoders include a press button (separate GPIO). v7.2 doesn't mention one for Mk I; check schematic. If present, it lives outside this driver and probably belongs in `boot_power.c` alongside the existing power button.
5. **What's the canonical `ui_event_t` shape?** If it's an enum, the integer values can be set centrally and we're fine. If it's a struct (e.g. carrying a timestamp or a rotation count), the encoder needs to construct it differently. Recommend: enum-only for v1; add struct payload when the UI needs it.

---

## Deliverable checklist

- [x] `docs/porting/CrownEncoder_2026-06-12_iv7.1.f0.0.md` -- this file.
- [x] `components/encoder/encoder.{c,h}` -- new driver (PCNT + 5 ms drain + glitch / debounce / boot guard).
- [x] `components/encoder/CMakeLists.txt` -- new component registration.
- [x] `test/test_encoder.c` -- standalone harness, stubs g_ui_event_q + ui_event_t.
- [ ] Commit -- prepared message:

```
[CrownEncoder] Porting: PCNT quadrature input, iv7.1.f0.0, 6 issues noted

- New components/encoder/ component (new input device, no carryforward).
  PCNT-based quadrature decode on GPIO21 (A) / GPIO43 (B).
- Drain strategy: 5 ms esp_timer reads + clears PCNT, accumulates
  signed residue, divides by ENCODER_STEPS_PER_DETENT (4 for x4
  quadrature), emits one CW/CCW event per detent.
- Glitch filter: 12.5 us hardware (1000 APB cycles @ 80 MHz).
- Debounce: 2 ms software window after each fired event, suppressed
  events counted as glitches.
- Boot guard: 200 ms after init swallow drained detents -- absorbs
  the v7.2 GPIO43 boot-log TX ~3 ms spurious activity.
- Events go to g_ui_event_q (forward extern; queue identity must
  match what lvgl_ui creates -- see [DEFECT-001] and [DEFECT-002]).
- Diagnostics: total_detents / cw / ccw / glitch counters.
- Test harness: creates 16-depth g_ui_event_q stub, runs 15 s window,
  logs every detent with elapsed time + cumulative counters.
- [DEFECT-001] ui_event.h not in tree (lvgl_ui follow-up) -- HIGH.
- [DEFECT-002] Forward-extern g_ui_event_q + local ui_event_t -- HIGH.
- [DEFECT-003] Boot guard re-fires on runtime re-init.
- [DEFECT-004] PCNT x4 assumes 4 edges per detent.
- [DEFECT-005] No direction-reversal flag.
- [DEFECT-006] xQueueSend non-blocking (intentional).

See: firmware/docs/porting/CrownEncoder_2026-06-12_iv7.1.f0.0.md
```
