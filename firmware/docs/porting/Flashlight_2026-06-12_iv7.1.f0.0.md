# Flashlight LED -- Porting Log
**Module:** High-power LED driven by LEDC PWM, GPIO41
**Date:** 2026-06-12
**Firmware:** `iv7.1.f0.0`
**Hardware authority:** `docs/02_Kompic_Mk1_System_Instructions_v7.2.md`, §GPIO ASSIGNMENT (GPIO41 GPIO_FLASHLIGHT).
**Brief:** `docs/17_PHASE_4_BATCH_ACTUATORS.md`, Module 4.
**Author:** Claude (Opus 4.7), under master prompt `docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md`.

---

## Summary

New output device in Mk I -- no v5 carryforward. (v5's GPIO41 was the system power latch; Mk I repurposes it as the flashlight PWM line.) The driver is the simplest module of the entire Phase 4 batch: configure one LEDC timer + one LEDC channel, expose `set_brightness(pct)` / `on()` / `off()`, done.

LEDC config: 1 kHz / 8-bit on timer 0 channel 0 (low-speed mode). 1 kHz is the sweet spot: above the eye's flicker-fusion threshold (no perceptible flicker at any duty cycle) and below the audible band (no inductor whine, which is a real concern at the typical 2-10 kHz LEDC frequencies). 8-bit resolution gives 256 duty steps -- finer than the eye can resolve as brightness change.

No gamma correction. High-power LEDs at moderate duty cycles are roughly linear in perception vs duty; gamma here would just waste low-end resolution.

Today's deliverable: new `components/flashlight/` (LEDC driver + thin brightness API), standalone test harness with brightness sweep + pulse pattern, dated .md.

---

## Hardware spec from v7.2

### Pinout

| Signal | GPIO | Direction | Notes |
|---|:--:|---|---|
| GPIO_FLASHLIGHT | 41 | output | LEDC PWM. HIGH (duty > 0) lights the LED through its driver transistor / FET. |

### LEDC

| Item | Value |
|---|---|
| Timer | 0 |
| Channel | 0 |
| Speed mode | LOW_SPEED |
| Frequency | 1000 Hz |
| Resolution | 8 bits (256 duty levels) |
| Clock source | LEDC_AUTO_CLK |

### Migration note

In v5, GPIO41 was the latched power-on line driven by boot_power. Mk I re-uses the pin for the flashlight. boot_power.c MUST NOT continue to drive GPIO41 as a power latch on Mk I. Out of scope for this driver to enforce; flagged here so the boot_power review catches it.

### Power

- LED forward voltage: TBD (depends on which LED is fitted -- typical 3-3.5 V for a white emitter).
- Current at 100% duty: TBD; typical 100-500 mA for a "flashlight on a wearable" LED.
- Driver expects an external transistor / FET; the ESP GPIO cannot source the LED directly.

---

## Code audit

### What dies

Nothing -- new component. (v5's GPIO41-as-power-latch code lives in `boot_power.c` and is out of this driver's scope.)

### What's new

- **LEDC timer config.** 1 kHz / 8-bit / LOW_SPEED / AUTO_CLK. Auto-clock selection lets the IDF pick between APB and the on-chip RC oscillator depending on the target.
- **LEDC channel config.** GPIO41, timer 0, intr disabled, initial duty 0.
- **Idempotent init.** Calling `flashlight_init()` twice is a no-op after the first success.
- **Implicit init on first use.** `flashlight_set_brightness(pct)` calls `flashlight_init()` if not yet initialised. Convenient for the flashlight tile to just call `set_brightness(50)` without worrying about ordering.
- **Linear pct -> duty mapping.** `duty = (pct * 255) / 100`, with `pct` clamped to 0..100.
- **No gamma curve.** Documented decision; LEDs in this regime are roughly linear in perception.
- **No FreeRTOS task, no timer.** Pure on-demand. The driver does nothing between API calls.

---

## Implementation

### `components/flashlight/flashlight.h`

- Pin: `FLASHLIGHT_GPIO = GPIO_NUM_41`.
- LEDC params: timer 0, channel 0, 1 kHz, 8-bit.
- `FLASHLIGHT_DUTY_MAX` = 255 (derived from RES_BITS).
- Identity: `"Flashlight"` / `"LEDC PWM, GPIO41"`.
- Public API: `_init` / `_deinit` / `_set_brightness(pct)` / `_on` / `_off` / `_get_brightness`.

### `components/flashlight/flashlight.c`

- `flashlight_init()` -- ledc_timer_config + ledc_channel_config (duty=0). Sets `s_initialised`.
- `flashlight_deinit()` -- duty=0, ledc_stop. Resets `s_initialised`.
- `flashlight_set_brightness(pct)` -- clamps, computes duty, ledc_set_duty + ledc_update_duty.
- `flashlight_on/off` -- one-liner wrappers.
- `flashlight_get_brightness` -- returns the last commanded pct.

### `components/flashlight/CMakeLists.txt`

REQUIRES: just `driver` (LEDC is part of the IDF driver component).

### `test/test_flashlight.c`

Five-phase standalone harness:

1. **Phase 1** -- init.
2. **Phase 2** -- 0 -> 100% sweep in 10% steps, 500 ms dwell, per-step API timing + readback.
3. **Phase 3** -- bracket: off / on / off via convenience helpers.
4. **Phase 4** -- 5x 200/200 ms pulse pattern -- intentional check for LEDC inductor whine at 1 kHz (none expected; if you hear it, that's a hardware-side issue).
5. **Phase 5** -- off + deinit, stack high-water + heap.

The bench operator pairs this with a current meter on the LED rail to characterise current draw vs duty -- the only profiling signal that matters for the battery budget.

---

## Profiling

### Boot-time cost

| Phase | Component | Time | Method |
|---|---|---|---|
| Init | LEDC timer + channel config | **TBD (no hw)**, < 1 ms expected | wall time |

### Per-operation cost

| Operation | Time | Notes |
|---|---|---|
| `flashlight_set_brightness(pct)` | **TBD**, ~20 us expected | ledc_set_duty + ledc_update_duty |
| `flashlight_on/off` | Same as above | wrapper around set_brightness |

### Memory

| Metric | Value | Notes |
|---|---|---|
| Driver state | 2 bytes (s_initialised + s_current_pct) | -- |
| LEDC IDF state | ~100 bytes | timer + channel |

### Current draw estimate

| State | Current at the LED | Notes |
|---|---|---|
| OFF (duty=0) | < 1 uA | leakage only |
| 25% duty | TBD ~ 25 mA | depends on LED + driver |
| 50% duty | TBD ~ 50 mA | -- |
| 100% duty | TBD ~ 100-500 mA | dominates the battery budget when on |

### Notes

- **The LED is by far the highest peak current sink in the firmware.** At 100% duty a typical flashlight LED draws 200+ mA -- comparable to the entire rest of the watch combined. Battery life with flashlight on continuously is single-digit hours.
- **1 kHz LEDC is safe for the eye AND the ear.** Higher frequencies (5-10 kHz) sit in the audible range and can whine through a poorly-damped inductor; lower frequencies (< 200 Hz) start to flicker visibly during head movement.
- **No on-time limit.** The driver does not auto-disable the LED after some interval. If the user UI leaves the flashlight on indefinitely the battery dies. Should be flagged on the flashlight tile, not enforced here.

---

## Defects discovered

- `[DEFECT-001] No coordination with boot_power.c about GPIO41 ownership | components/flashlight/flashlight.c | HIGH | In v5, GPIO41 was the power-latch line driven by boot_power.c. On Mk I that pin is the flashlight. If boot_power continues to configure GPIO41 as a power-latch output, the flashlight driver will fight it -- whichever wrote last wins, leading to either no flashlight (boot_power drives LOW) or unexpected boot behaviour (flashlight sets LEDC). Disposition: boot_power.c must be reviewed to ensure it no longer touches GPIO41 on Mk I. This driver does not enforce it.`
- `[DEFECT-002] No auto-timeout / runaway protection | components/flashlight/flashlight.c | MED | A user-initiated "flashlight on" with no UI dismissal can drain the battery in a few hours. The driver provides no timeout; that policy belongs in the flashlight tile. Disposition: the flashlight tile (Phase 4+ UI work) should auto-disable after N minutes or display a prominent "ON" indicator.`
- `[DEFECT-003] LEDC freq 1 kHz [DSV vs ear] | components/flashlight/flashlight.h:FLASHLIGHT_FREQ_HZ | LOW | 1 kHz is the chosen value: above flicker fusion, below audibility. If the bench unit's LED + driver inductor whines at 1 kHz, bump to ~500 Hz or ~2 kHz to dodge the resonance. Disposition: tune on bench.`
- `[DEFECT-004] Linear pct -> duty (no gamma) | components/flashlight/flashlight.c:flashlight_set_brightness | LOW | Documented design choice. If the LED looks "too bright too fast" at low pct values (the eye is logarithmic at low intensities), add a gamma 2.2 LUT here. Disposition: subjective; tune on bench if needed.`
- `[DEFECT-005] LEDC channel 0 may conflict with display backlight | components/flashlight/flashlight.h:FLASHLIGHT_LEDC_CHANNEL | MED | The CO5300 display backlight likely uses an LEDC channel too. If both pick channel 0 timer 0, second init wins. Disposition: confirm display backlight's LEDC channel assignment; if it's also 0/0, move flashlight to channel 1 (or another unused timer).`
- `[DEFECT-006] No PSU rail dependency declared | components/flashlight/flashlight.c | LOW | The LED probably runs off a higher-voltage rail (or a dedicated boost) than 3V3. If that rail is sequenced separately, flashlight_set_brightness(100) before the rail is up just produces a dark LED. Disposition: documented; coordination lives in boot_power.`

---

## Open questions

1. **Which LEDC channel does the display backlight use?** If channel 0 / timer 0, the flashlight collides -- see DEFECT-005. Quick fix is moving flashlight to channel 1 (or timer 1). Need a peek at the CO5300 driver.
2. **Where does the flashlight tile live?** Probably `components/flashlight/flashlight_tile.{c,h}` analogous to other tiles. Out of scope today (driver-only batch).
3. **Strobe / SOS modes?** Some flashlights expose a Morse SOS or a high-frequency strobe. Easy to add at the tile layer using `flashlight_set_brightness(0/100)` from a timer callback. Defer to tile work.
4. **Brightness persistence across reboot?** Save the last brightness to NVS so the user's preferred level survives? Recommend: NO -- safety property says the flashlight should default to OFF every boot.
5. **Integration with WS2812 status LED?** When the flashlight is on, should the WS2812 status LED light up green (or similar) so the user sees the watch is "active"? UX decision; doesn't affect the driver.

---

## Deliverable checklist

- [x] `docs/porting/Flashlight_2026-06-12_iv7.1.f0.0.md` -- this file.
- [x] `components/flashlight/flashlight.{c,h}` -- new driver (LEDC + thin brightness API).
- [x] `components/flashlight/CMakeLists.txt` -- new component registration.
- [x] `test/test_flashlight.c` -- standalone harness with brightness sweep + pulse pattern.
- [ ] Commit -- prepared message:

```
[Flashlight] Porting: LEDC PWM flashlight driver, iv7.1.f0.0, 6 issues noted

- New components/flashlight/ component. LEDC timer 0 channel 0 on
  GPIO41, 1 kHz / 8-bit. 1 kHz is above flicker fusion and below
  audibility.
- API: flashlight_init / set_brightness(0..100) / on / off / get_brightness.
- Implicit init on first set_brightness call -- the flashlight tile
  doesn't need to coordinate init ordering.
- Linear pct -> duty mapping (no gamma); LEDs in this regime are
  approximately linear in perception.
- Migration note: v5 GPIO41 was the system power latch. On Mk I it
  is the flashlight. boot_power.c MUST be reviewed to ensure it no
  longer drives GPIO41 ([DEFECT-001]).
- No FreeRTOS task, no esp_timer -- pure on-demand.
- Test harness: 0..100% sweep in 10% steps, off/on/off bracket,
  5x 200/200 ms pulse pattern (whine check), per-step API timing.
- [DEFECT-001] GPIO41 ownership conflict with boot_power -- HIGH.
- [DEFECT-002] No auto-timeout / runaway protection (tile-layer).
- [DEFECT-003] 1 kHz LEDC frequency tunable.
- [DEFECT-004] Linear pct -> duty (no gamma) by design.
- [DEFECT-005] LEDC channel 0 may collide with display backlight.
- [DEFECT-006] No PSU rail dependency declared.

See: firmware/docs/porting/Flashlight_2026-06-12_iv7.1.f0.0.md
```
