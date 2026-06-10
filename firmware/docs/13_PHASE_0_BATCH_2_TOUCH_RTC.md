# PHASE 0 BATCH 2 — TOUCH + RTC
**Assigned:** 10 June 2026
**Firmware version:** iv7.2.f0.0
**Master instructions:** docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md (required reading)

---

## MODULES TO PORT (in order)

### Module 1: CST9217 (Capacitive Touch Controller)

**Hardware spec from v7.2:**
- Bus: I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
- Address: 0x5A
- INT pin: GPIO6 (RTC wake: tap)
- RST pin: GPIO44
- Register base: 0xD000
- ACK value: 0xAB

**Code audit:**
Old code: CST816S (similar protocol, ISR pattern reusable). New chip is CST9217 — register-compatible family, same I2C handshake, same interrupt model.

**Implementation scope:**
- Register map from datasheet (0xD000 register, touch coordinates, gesture flags)
- ISR-driven path: GPIO6 INT → `xTaskNotifyFromISR` → I2C read in task context
- Single-producer single-consumer queue: `xQueueOverwrite(g_touch_q, &point)`
- Touch coordinates bypass broker mutex (lock-free fast path)
- Stub: no tile yet (tile is LVGL's indev callback, not a driver concern)

**Test harness:**
- Can we initialize I2C + CST9217?
- Can we read touch coordinates (0xD000 register)?
- Does INT fire (GPIO6) when screen is touched?
- Measure: I2C transaction time @ 400 kHz, ISR latency

**Deliverables:**
- `/docs/porting/CST9217_2026-06-10_iv7.2.f0.0.md`
- `/components/cst9217/cst9217.{c,h}` (register map, ISR handler, queue output)
- `/components/cst9217/CMakeLists.txt`
- `/test/test_cst9217.c` (standalone, probes I2C + INT)

---

### Module 2: PCF85063 (Real-Time Clock)

**Hardware spec from v7.2:**
- Bus: I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
- Address: 0x51
- INT pin: GPIO15 (RTC wake: alarm)
- CLKOUT pin: GPIO via JP5 (optional, not Phase 0)

**Code audit:**
Old code: PCF85063 copied verbatim, same chip. v5 code has Phase-15 UTC fix (timegm() seed) — carry it forward as-is. No hardware changes.

**Implementation scope:**
- Copy old driver as-is (register map, read/write time, alarm logic)
- Boot-time UTC seed: `timegm()` (not mktime + TZ env hack)
- RTC tile: display local time (TZ offset from NVS, loaded at boot)
- Profiling: boot init time, per-register I2C cost

**Test harness:**
- Can we initialize I2C + PCF85063?
- Can we read/write time (set to known value, read back)?
- Does timegm() seed work (UTC math correct)?
- Does INT fire (GPIO15) on alarm match?
- Measure: I2C transaction time, boot-to-ready time

**Deliverables:**
- `/docs/porting/PCF85063_2026-06-10_iv7.2.f0.0.md`
- `/components/pcf85063/pcf85063.{c,h}` (copied from v5, no changes except confirm timegm path)
- `/components/pcf85063/rtc_tile.{c,h}` (resize for 410×502, display local time)
- `/components/pcf85063/CMakeLists.txt`
- `/test/test_pcf85063.c` (standalone, I2C init + time read/write + UTC math verification)

---

## OUTPUT

You will produce **2 dated .md files** in `/docs/porting/`:
- CST9217_2026-06-10_iv7.2.f0.0.md
- PCF85063_2026-06-10_iv7.2.f0.0.md

Plus driver skeletons in `/components/` and test harnesses in `/test/` for each.

**Profiling template must be filled** (even if TBD — measure on real hardware later):
- Boot-time cost (init I2C + device init)
- Per-operation cost (I2C read/write times)
- Memory (stack high-water, heap usage)
- Current draw estimate (or TBD)

**Defects logged** as `[DEFECT-NN]` in each .md.

**Commit strategy:** one commit per module (2 total), or one commit at the end. Your choice.

---

## REMEMBER

- Reference v7.2 §GPIO ASSIGNMENT, §I2C BUS ASSIGNMENT, §DISPLAY for pinout/bus/address.
- No speculation. Datasheet + v7.2 only.
- Profiling is real: use `esp_timer_get_time()`, don't guess.
- Test harnesses are standalone and reproducible.
- Live in the .md. All work is visible, all decisions are documented.

Go.
