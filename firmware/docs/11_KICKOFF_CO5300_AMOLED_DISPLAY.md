# KICKOFF — CO5300 AMOLED DISPLAY DRIVER
**Assigned:** 10 June 2026
**Module:** CO5300 (QSPI AMOLED, 410×502, RGB888)
**Hardware authority:** `Kompic_Mk1_System_Instructions_v7.2.md` §DISPLAY
**Firmware version:** `iv7.2.f0.0`
**Master instructions:** `KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md` (required reading)

---

## BEFORE YOU START

Read **`KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md`** in full. That is your standing order for all porting work. This assignment is a specific instance of it.

---

## TASK

Port the CO5300 AMOLED display driver from scratch. The old ST7789 is completely different — use the datasheet as your only guide.

**Goal:** a dated, profiled, testable CO5300 driver skeleton + standalone test harness.

---

## STEPS

### 1. Read v7.2 §DISPLAY

Open `Kompic_Mk1_System_Instructions_v7.2.md` and extract every hardware detail:

- **QSPI pinout:** GPIO9–14, CS=10, TE=45, RST=3
- **Init sequence:** commands: 0x02 (instructions, 1-wire), 0x32 (pixels, 4-wire QIO)
- **COLMOD:** 0x77 (RGB888 mandatory; RGB565 lane mapping is broken on this chip)
- **Panel address:** 0x003C00
- **Column offset:** 22
- **Pixel clock target:** 40 MHz (start here; can raise after bench test)
- **Sleep/wake:** DISPOFF (0x28) → SLPIN (0x10) to sleep; SLPOUT (0x11) + full re-init to wake
- **Brightness:** register 0x51 (BRIGHT, 8-bit), no backlight PWM
- **Power rail:** 3V3 only (no separate VBAT)
- **TE (Tearing) pin:** GPIO45 (unused in v1; firmware placeholder only for now)

Cite the exact section numbers from v7.2 in your .md.

### 2. Create the .md document

Create: `/docs/porting/CO5300_2026-06-10_iv7.2.f0.0.md`

Fill in these sections:

#### **Summary**
One paragraph: what is the CO5300, what does it do, why it matters.

#### **Hardware spec from v7.2**
A table of pinout, addresses (N/A), I2C bus (N/A), SPI frequency, voltage rails, etc. Copy directly from v7.2.

#### **Code audit (N/A for CO5300)**
This is a new driver, not ported from old code. Write: "CO5300 is new for Mk I. Old project used ST7789 over SPI2 at 20 MHz with RGB565 — fundamentally different protocol and color depth. No code to reuse."

#### **Implementation**
Create the driver skeleton: `components/co5300/co5300.h` and `components/co5300/co5300.c`

**Header (`co5300.h`):**
- Register map (define every command/register cited in the v7.2 doc)
- Device handle struct (SPI bus handle, GPIO pins, state)
- Function stubs:
  - `co5300_init(config)` → initialize bus + panel
  - `co5300_write_command(h, cmd)` → send 0x02 command
  - `co5300_write_command_with_data(h, cmd, data, len)` → send command + data
  - `co5300_write_pixels(h, addr, pixels, count)` → send 0x32 frame data
  - `co5300_set_brightness(h, pct)` → set register 0x51
  - `co5300_sleep(h)` → DISPOFF + SLPIN
  - `co5300_wake(h)` → SLPOUT + full re-init
  - `co5300_deinit(h)` → cleanup
  - `co5300_get_status(h)` → read WHO_AM_I or equivalent

**Implementation file (`co5300.c`):**
- SPI transaction wrapper (taking bus handle, sending command + optional data)
- Register map constants
- Init sequence (from v7.2)
- Stub implementations of the above functions (return `ESP_OK` or `ESP_ERR_NOT_SUPPORTED` as appropriate; no real logic yet)
- Profiling hooks: `esp_timer_get_time()` before/after key operations

Don't implement the full driver yet — just the skeleton with every register documented and function signatures stubbed. The real implementation happens when hardware arrives.

#### **Test harness**
Create: `/test/test_co5300.c`

A standalone test that can run on any ESP32 (with CO5300 on a breakout):
1. Initialize the SPI bus (GPIO9–14 hardcoded from v7.2)
2. Initialize CO5300 via `co5300_init()`
3. Attempt to read WHO_AM_I or a status register (confirm chip is alive)
4. Write DISPON command (turn display on)
5. Set brightness to 50%, then 100% (register 0x51)
6. Log all times and results

The test is **not** pretty — it's a diagnostic. Output should be:
```
I (123) test_co5300: SPI bus init: 1.2 ms
I (124) test_co5300: CO5300 init: 5.3 ms
I (125) test_co5300: WHO_AM_I read: 0xXX (expected 0xXX?)
I (200) test_co5300: DISPON command: sent
I (201) test_co5300: Brightness set to 50%: register write 1.1 ms
I (202) test_co5300: Brightness set to 100%: register write 1.1 ms
I (203) test_co5300: All checks passed.
```

#### **Profiling**
Fill in the template from the master prompt:

| Phase | Component | Time (ms) | Method |
|-------|-----------|-----------|--------|
| Init | SPI bus setup | _to be measured_ | `esp_timer_get_time()` in test harness |
| Init | CO5300 init | _to be measured_ | same |
| Op | I2C-equivalent register write (brightness) | _to be measured_ | same |
| Op | Command send (DISPON) | _to be measured_ | same |

Memory: `heap_caps_get_free_size()` before/after init.

Current draw: TBD (measure on real hardware).

**Important:** Use `esp_timer_get_time()` in the test harness to log actual timings. This is not guesswork — measure it.

#### **Defects discovered**
List any issues you find or foresee. Examples:
- `[DEFECT-001] WHO_AM_I register unknown | datasheet silent on this | LOW | No known chip ID to verify; may need to add a handshake via GPIO status pin instead.`
- `[DEFECT-002] TE pin (GPIO45) is unused | v7.2 says TE is freed up for firmware | MED | Firmware placeholder only; no actual tearing sync in v1.`

#### **Open questions**
Ask, don't assume. Examples:
- Is there a WHO_AM_I register for CO5300, or do we verify the chip via GPIO feedback?
- Should the init sequence happen once at boot, or can it be re-run on wake from SLPIN?
- The COLMOD 0x77 command — is this part of the init sequence, or does it get sent once per frame?

---

### 3. Add to repo + commit

Create the .md in `/docs/porting/`.
Create the driver skeleton in `/components/co5300/` with a minimal `CMakeLists.txt`.
Create the test harness in `/test/test_co5300.c`.

Commit:
```
[CO5300] Porting: AMOLED display driver, iv7.2.f0.0

- Hardware spec extracted from v7.2 §DISPLAY
- Driver skeleton: register map, SPI wrapper, init sequence
- Test harness: SPI init, chip handshake, brightness control, profiling
- Profiling: boot times TBD (requires hardware)
- [DEFECT-002] TE pin placeholder; no real tearing sync in v1
- [DEFECT-001] WHO_AM_I verification TBD (datasheet silent)

See: docs/porting/CO5300_2026-06-10_iv7.2.f0.0.md
```

---

## DELIVERABLES AT EOD

- [ ] `/docs/porting/CO5300_2026-06-10_iv7.2.f0.0.md` (complete, all sections filled, open questions posted if any)
- [ ] `/components/co5300/co5300.h` (register map + function stubs)
- [ ] `/components/co5300/co5300.c` (SPI wrapper, skeleton implementations, profiling hooks)
- [ ] `/components/co5300/CMakeLists.txt` (minimal, just the .c/.h)
- [ ] `/test/test_co5300.c` (standalone harness, runnable on any ESP32)
- [ ] Commit pushed with message as above

---

## REMEMBER

- **No speculation.** Everything from v7.2 or the datasheet.
- **Profiling is real.** Use `esp_timer_get_time()`, don't guess.
- **Live in the .md.** The .md is the work; code is documentation.
- **Test harness is not pretty.** It's a diagnostic. Just facts.
- **If you get stuck on a hardware question, ask Ivan.** Post the "Open questions" section and wait.

You're building the skeleton. Hardware arrives in a few weeks. When it does, this skeleton becomes the real driver, and the test harness becomes the first thing we flash.

Go.
