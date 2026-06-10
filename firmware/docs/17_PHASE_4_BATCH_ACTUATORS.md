# PHASE 4 BATCH — ACTUATORS + INPUT (HAPTIC + LED + FLASHLIGHT + CROWN)
**Assigned:** 10 June 2026
**Firmware version:** iv7.2.f0.0
**Master instructions:** docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md (required reading)

---

## MODULES TO PORT (in order)

### Module 1: DRV2605 (Haptic Driver)

**Hardware spec from v7.2:**
- Bus: I2C bus 2 (GPIO4 SDA, GPIO5 SCL, 400 kHz)
- Address: 0x5A
- DRV_EN pin: GPIO0 (FW output, drives low to enable at runtime; see boot_power)
- LRA: ELV1411A, on daughter board below main PCB

**Code audit:**
Old code: DRV2605 copied verbatim, same chip. Only change: move from bus 1 to bus 2 mutex; DRV_EN GPIO0 handling (FW output, not internal pull-up).

**Implementation scope:**
- Copy old driver as-is (register map, waveform control)
- Confirm bus 2 mutex usage (g_i2c2_mutex, not g_i2c1_mutex)
- Confirm GPIO0 DRV_EN is driven low at boot (in boot_power.c)
- On-demand task: haptic_play(waveform_id) writes sequence to DRV2605
- Waveform library: simple patterns (click, buzz, confirm, alert)
- Profiling: boot init, per-waveform I2C overhead, waveform play time

**Test harness:**
- Can we initialize I2C bus 2 + DRV2605?
- Can we set GPIO0 low (DRV_EN)?
- Can we trigger a waveform (write to DRV2605 registers)?
- Measure: boot init, per-waveform I2C cost, play duration

**Deliverables:**
- `/docs/porting/DRV2605_2026-06-10_iv7.2.f0.0.md`
- `/components/drv2605/drv2605.{c,h}` (copied, bus 2 mutex, GPIO0 verify)
- `/components/drv2605/haptic.{c,h}` (waveform library, on-demand play)
- `/components/drv2605/haptic_tile.{c,h}` (test waveforms, manual trigger UI)
- `/components/drv2605/CMakeLists.txt`
- `/test/test_drv2605.c` (standalone, I2C bus 2 init + GPIO0 setup + waveform trigger)

---

### Module 2: Crown Encoder (GPIO21 / GPIO43)

**Hardware spec from v7.2:**
- GPIO21 (EC_SigA): crown encoder line A, edge ISR, RTC wake (bonus)
- GPIO43 (EC_SigB): crown encoder line B, boot-log TX edge (~3ms spurious activity), FW ignores pre-init
- Protocol: quadrature (A/B pulse sequences encode direction + count)
- Mechanical: rotary encoder with detents

**Code audit:**
New module, no old code.

**Implementation scope:**
- PCNT (Pulse Counter) on GPIO21/43: quadrature mode, counts edge transitions
- Edge ISR: debounce (1 ms timer resets on each new edge)
- Debounced rotation events feed `g_ui_event_q` (UI nav: scroll, page change)
- Direction detection: count sign (positive = CW, negative = CCW)
- Read PCNT counter periodically (or on timer) to detect motion
- Profiling: ISR latency, debounce delay, navigation event queue cost

**Test harness:**
- Can we initialize PCNT on GPIO21/43?
- Can we count quadrature edges (manual rotation simulation)?
- Does debouncing work (apply glitch, verify no spurious events)?
- Measure: ISR latency, debounce accuracy

**Deliverables:**
- `/docs/porting/CrownEncoder_2026-06-10_iv7.2.f0.0.md`
- `/components/encoder/encoder.{c,h}` (PCNT init, quadrature decode, debounce, nav event fire)
- `/components/encoder/CMakeLists.txt`
- `/test/test_encoder.c` (standalone, PCNT init + quadrature counting + debounce test)

---

### Module 3: WS2812B (Single Status LED, RGB)

**Hardware spec from v7.2:**
- GPIO42 (LED_Din): RMT TX, WS2812B protocol
- 1 pixel LED (not a string; just one)
- Colors: RGB 24-bit (0xFF0000 = red, 0x00FF00 = green, 0x0000FF = blue)

**Code audit:**
New module, no old code.

**Implementation scope:**
- RMT peripheral: NRZ (8-bit tones) encoding for WS2812B protocol
- Color library: simple RGB macros (RED, GREEN, BLUE, YELLOW, MAGENTA, CYAN, WHITE, OFF)
- On-demand API: `ws2812_set_color(r, g, b)` fires RMT TX
- States: off (init/idle), charging (pulsing blue), charged (steady green), alert (pulsing red)
- Profiling: RMT timing accuracy, color output latency

**Test harness:**
- Can we initialize RMT on GPIO42?
- Can we set colors (red, green, blue, white, off)?
- Do the colors appear correct (visual inspection required, or use logic analyzer)?
- Measure: RMT timing precision

**Deliverables:**
- `/docs/porting/WS2812_2026-06-10_iv7.2.f0.0.md`
- `/components/ws2812/ws2812.{c,h}` (RMT driver, color set, state machine)
- `/components/ws2812/CMakeLists.txt`
- `/test/test_ws2812.c` (standalone, RMT init + color set)

---

### Module 4: Flashlight (GPIO41, LEDC PWM)

**Hardware spec from v7.2:**
- GPIO41 (GPIO_FLASHLIGHT): LEDC PWM output
- Duty cycle: 0–100% brightness
- No hardware enable; GPIO low = LED off, GPIO high = LED on at programmed brightness

**Code audit:**
New module, no old code. (Old design used GPIO41 as power latch; now it's the flashlight.)

**Implementation scope:**
- LEDC: Timer 0, channel 0 (or configurable), frequency ~1 kHz (avoid flicker), 8-bit resolution
- API: `flashlight_set_brightness(pct)` sets duty cycle (0–100)
- Simple on/off: `flashlight_on()`, `flashlight_off()`
- Profiling: LEDC overhead, duty cycle accuracy, current draw vs brightness

**Test harness:**
- Can we initialize LEDC on GPIO41?
- Can we set brightness (0%, 50%, 100%)?
- Does duty cycle correspond to perceived brightness?
- Measure: LEDC frequency/resolution accuracy, current draw

**Deliverables:**
- `/docs/porting/Flashlight_2026-06-10_iv7.2.f0.0.md`
- `/components/flashlight/flashlight.{c,h}` (LEDC driver, brightness set)
- `/components/flashlight/CMakeLists.txt`
- `/test/test_flashlight.c` (standalone, LEDC init + brightness sweep)

---

## OUTPUT

You will produce **4 dated .md files** in `/docs/porting/`:
- DRV2605_2026-06-10_iv7.2.f0.0.md
- CrownEncoder_2026-06-10_iv7.2.f0.0.md
- WS2812_2026-06-10_iv7.2.f0.0.md
- Flashlight_2026-06-10_iv7.2.f0.0.md

Plus driver skeletons in `/components/` and test harnesses in `/test/` for each.

**Profiling template must be filled:**
- Boot-time cost
- Per-operation cost (waveform play, color set, brightness change)
- Memory
- Current draw (especially flashlight vs duty cycle)

**Defects logged** as `[DEFECT-NN]` in each .md.

**Commit strategy:** one commit per module (4 total), or one commit at the end. Your choice.

---

## DEPENDENCY NOTES

- DRV2605 requires GPIO0 setup from boot_power (must be driven low at boot)
- Crown encoder feeds `g_ui_event_q` (Core 0 → Core 1 command path), requires data_broker + ui_event.h
- WS2812 and flashlight have no dependencies (pure GPIO outputs)
- Haptic is called on-demand from boot_power (button press) and from alarm tile (alarm trigger)

---

## REMEMBER

- Reference v7.2 §GPIO ASSIGNMENT (GPIO0 DRV_EN, GPIO21 EC_SigA, GPIO43 EC_SigB, GPIO42 LED, GPIO41 flashlight)
- DRV2605 datasheet for waveform codes
- WS2812B datasheet for RMT bit timing (critical)
- LEDC driver frequency: ~1 kHz avoids flicker but may be audible; adjust if needed
- Profiling: current draw is key for power budget (flashlight can draw significant current)
- Test harnesses are standalone and reproducible
- Live in the .md. All work is visible

Go.
