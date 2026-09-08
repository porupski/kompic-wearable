# Stage 21 -- Screen Bring-up Prep (Mk1b arrival + GPS Day-1)

**Date opened:** 2026-09-08
**Board:** iv7.1 (Mk1 bench); Mk1b PCB in fab, screen test breakout received and pins cross-referenced -- pinout for the Mk1b display should be correct.
**Firmware baseline:** `iv7.1.f0.4.26` -> target `0.5.0` on Mk1b, `0.4.27+` on iv7.1.
**Status:** OPEN. Coding starts once the plan below is agreed.

---

## 1. Close-outs and defers

### 1.1 Stage 20 CLOSED at `0.4.26`

Bench flash landed. Ivan: "so much better now. sure there's still the
5 s wait, but at least the button is super responsive." The 5 s cold-
boot delay is BQ25619 QON wake + ESP cold boot + firmware init; not a
firmware-only fix (see Stage 20 §6.3 -- parked with the possible wins
listed).

- Press-edge haptic (0.4.25) confirmed firing.
- BATFET_DLY clear + BATFET_RST_WVBUS set (0.4.26) took ship-mode
  latency from ~9 s to ~1 s. Bench-verified.
- I2C mutex extern fix (0.4.26) puts BQ back on bus 1 alongside DRV.

Archive when convenient:

```
python docs/build_info/reference_files/make_archive.py stage-20-button-responsiveness
```

### 1.2 Stage 19 DEFERRED (parked, not cancelled)

Power-management refinement is a large, multi-week piece of work (park/
idle/off vocab, boot policy rewrite, profiling infra, per-driver park
helpers, CLI reorg). It is orthogonal to Mk1b bring-up and to the GPS
Day-1 deliverable. Deferring the whole thing.

- The Stage-11 -> Stage-18 ~17 mAh regression stays measured but
  unfixed. Runtime target is post-trip work.
- `battery_tag` NVS key (Stage 19 §7) also deferred; current `fw=`
  grouping is good enough.
- Stage 19 doc stays OPEN so the fresh-chat prompt at the top still
  fires cleanly when we come back to it.

Rationale: the trip needs a screen + GPS. Adding ~500 uA of gesture
drain or 30 % `rtos0/1` CPU_MAX doesn't matter if the operator can't
read a coordinate.

### 1.3 Everything else that was floating

- LVGL tile spec (Stage 18 §5): DONE, doc-only, ready to implement.
- Wrist gesture (Stage 18 §7.1, §9.1): landed, stays as-is.
- iv7.1 -> Mk1b delta plan (`docs/build_info/reference_files/`): DONE,
  Stage 21 executes against it.
- AMOLED touch sketch (`arduino/15_amoled_touch_test/`): audited,
  ready as first post-reflow-A flash on Mk1b.
- Photo-GPS Plan B (Waveshare fallback): dormant, only triggers if
  Mk1b screen fails after reflow-A.

---

## 2. Goal

**One firmware image, two hardware behaviours, with GPS + tile UI live
on Mk1b the moment it comes back from assembly.**

The image is built to Mk1b spec (CO5300 QSPI display, CST9217 touch,
GPS UART on GPIO18, tile registry seeded per Stage 18 §5.1). On iv7.1
the same image detects the missing display + missing touch controller
at boot and continues without them -- everything else (encoder, button,
LED, sensors, CLI, ship mode, recording modes) runs identically.

Why one image, not two:

- Ivan can iterate the tile code on iv7.1 today, before Mk1b arrives.
  Rendering happens against an LVGL buffer even without a panel;
  behaviour is testable via the CLI dump helpers.
- When Mk1b returns from reflow-A, we flash the same binary that was
  last-known-good on iv7.1 and the screen just lights up. No last-
  minute conditional-compile scramble on the bench.
- The "detect and skip" pattern is what we already do informally --
  GPS driver logs "no fix" and keeps going, sensors that fail probe
  log a warning and get parked. Extending this to the display closes
  the last hard-coded assumption.

---

## 3. What's already built (audit against the delta plan)

Grepped and confirmed on 2026-09-08.

| Piece                                | State                                                         |
|:-------------------------------------|:--------------------------------------------------------------|
| CO5300 driver (`co5300.c` 265 LOC)   | Written; QSPI init + pixel push code present                  |
| CST9217 driver (`cst9217.c` 263 LOC) | Written; probe reg 0xD000 -> ACK 0xAB documented              |
| LVGL port + `lvgl_ui.c`              | 25-file component ready; tile_registry pattern in place       |
| `tile_registry.{c,h}`                | Descriptor + build API ready; not called from anywhere yet    |
| Per-sensor tile files                | ALL PRESENT: alarm, env, battery, haptic, compass, imu,       |
|                                      | health, gps, rtc, ecg, light, system                          |
| `gps_tile.c` (487 LOC)               | Under-the-hood view built out; photo view TBD                 |
| `max_m10s.c` (642 LOC)               | Full driver; task_gps_fn defined, broker plumbing ready       |
| `IV71_TO_MK1B_FIRMWARE_DELTA.md`     | 9-step checklist ready to execute                             |
| `15_amoled_touch_test.ino`           | Audited (Stage 18 §7.2); verbatim first-flash for Mk1b        |
| Home tile spec (Stage 18 §5.3)       | Documented; `system_tile.c` is the implementation             |
| GPS tile spec (Stage 18 §5.2a/b)     | Two-mode spec documented; under-the-hood partially coded      |

## 4. What still needs building

### 4.1 `boot_display.c` (missing)

Only `boot_display.h` exists today, and it's a stub -- `LCD_H_RES/V_RES`
constants plus a no-op `backlight_set_brightness()`. `main.c` references
`boot_display_init()` in a commented-out block; there is no
implementation.

Deliverable: `boot_display.c` that

1. Configures the CO5300 QSPI pins (GPIO9-14 per master pinout v20).
2. Drives DISP_RST (GPIO3) low->high per CO5300 timing.
3. Calls the CO5300 init sequence (COLMOD 0x77, instruction 0x02 for
   commands, opcode 0x32 for pixels -- proven in the Arduino sketch).
4. **Probes the panel via a CO5300 read-ID (RDID / RDDID) before
   binding LVGL.** On failure: log `[BOOT_DISP] no CO5300 -- skipping
   LVGL`, return `ESP_ERR_NOT_FOUND`. `main.c` skips the LVGL port
   creation, tile registry build, and touch bind.
5. On success: bind CO5300 to `esp_lvgl_port_add_disp()`, allocate the
   LVGL draw buffer (partial refresh, size TBD by RAM budget), start
   the LVGL port task on Core 1.
6. Wires `backlight_set_brightness()` to the CO5300 dimming path
   (register 0x51 per datasheet) -- the current stub becomes real.

### 4.2 Touch indev bind

`cst9217.c` exists but nothing calls it from `lvgl_ui`. Deliverable:

1. `boot_touch_probe_and_init()` -- I2C probe at `0x5A`, read
   `CST9217_REG_BASE (0xD000)`, expect first byte `0xAB`. On mismatch:
   log and return without arming the ISR. Rest of firmware continues.
2. On success: attach `cst9217_lvgl_indev_read()` (already declared in
   `cst9217.h`) via `lvgl_port_add_touch()`, install INT ISR on GPIO6.
3. Same pattern as display -- probe-then-bind is the only place the
   Mk1-vs-Mk1b divergence lives.

### 4.3 Tile registry seed (Day-1 order)

`tile_registry.c` has the descriptor plumbing but no descriptors are
registered. Decide (proposal below) and land one place -- ideally
`tile_registry_build()` reads a compile-time array in a single spot.

**Proposed Day-1 registration order** (matches Stage 18 §5.1 columns):

```
col 0: system_tile      (home: time + date + battery status strip)
col 1: gps_tile         (under-the-hood default; encoder click -> photo view)
```

Rest stays unregistered until Day-2. Everything compiles because the
tile .c files are already in the graph; they just don't get added to
the tileview.

### 4.4 `fc_battery_test.c` Vbat ADC channel (Mk1b only)

Delta plan §2: iv7.1 override reads Vbat on GPIO18 / ADC2_CH7; Mk1b
schematic wires it to ADC1_CH8 (GPIO9). This is a Mk1b-only change.

**Handle with a compile-time switch on `KOMPIC_HW_VERSION`.** When
`iv7.1` is set, keep the ADC2/GPIO18 path (with the Wi-Fi caveat comment
intact). When `iv7.2` is set (Mk1b), switch to ADC1_CH8. Zero runtime
overhead, single source of truth.

Also frees GPIO18 for its intended use on Mk1b: GPS UART RX
(`MAX_M10S_RX_PIN` in `max_m10s.h`).

### 4.5 GPS enable (already gated behind one comment)

`boot_tasks.c:59` -- `extern void task_gps_fn(void *arg);` is
commented out with "GPS module offline (broken connector)". Un-comment
it and the paired `xTaskCreatePinnedToCore`. Also add
`broker_gps_set_enabled(true)` to `boot_hw_init.c` next to the
existing always-on brokers so the tile has data from boot.

**Compile-time gate this on `iv7.2`.** iv7.1 has no GPS chip; enabling
the task there wastes a UART allocation and clogs the log with
"waiting for fix" chatter.

### 4.6 GPS photo view (Stage 18 §5.2b -- not yet coded)

Second view mode inside `gps_tile.c`. Encoder click toggles between
under-the-hood (default) and photo. Layout per Stage 18 §5.2b:

- Full black bg.
- Very large white lat / lon rows, monospace, filling screen width.
- Time + date row above at ~35 % of coord row height.
- Nothing else. No fix icon, no battery.

Data update 1 Hz, no animation. Solid white on solid black so the
camera does not hunt for focus.

### 4.7 "Detect and skip" wiring in `main.c`

Once §4.1 and §4.2 land, `main.c` becomes:

```c
if (boot_display_init() == ESP_OK) {
    lvgl_port_lock(portMAX_DELAY);
    lvgl_ui_init(&ui_cfg);
    tile_registry_build();
    boot_touch_probe_and_init();   // safe to call even if display absent
    lvgl_port_unlock();
    ui_broker_init();
}
```

If display is absent, none of the LVGL-side code runs. The rest of
`main.c` (sensor init, task start, CLI) is unchanged.

---

## 5. Mk1b arrival bring-up sequence (checklist)

Assumes the AMOLED touch sketch (post-reflow-A) has already lit the
screen. From that point:

1. `firmware_version.h` -- bump `KOMPIC_HW_VERSION` to `"iv7.2"`,
   `KOMPIC_FW_VERSION` to `"0.5.0"`.
2. Build. Compile-time gate in `fc_battery_test.c` and `boot_tasks.c`
   flips Vbat + GPS on automatically.
3. Flash Mk1b. Expected first-boot log lines:
   - `[BOOT_DISP] CO5300 detected (RDID = 0xXX) -- LVGL bound`
   - `[BOOT_TOUCH] CST9217 detected (0xAB @ 0xD000) -- ISR armed`
   - `[GPS] task_gps_fn started on UART_NUM_1 (TX=17 RX=18 PPS=46)`
   - Home tile draws time / date / battery. Encoder scroll -> GPS
     tile (under-the-hood view). Touch works.
4. Take outside for a real fix. Confirm lat / lon populates, encoder
   click switches to photo view, photo view is readable in sunlight.
5. Reflow phase B (GPS, USB connector, bottom side per Stage 18 §3).
6. Bring-up B: verify GPS UART traffic (already covered by step 3-4),
   DRV auto-cal, SD mount.

For every step above, on iv7.1 the same binary would log:
- `[BOOT_DISP] no CO5300 -- skipping LVGL`
- `[BOOT_TOUCH] CST9217 probe failed -- skipping indev`
- `[GPS] disabled at compile-time (iv7.1)` (if gated per §4.5)
- Everything else identical.

---

## 6. Order of work

Proposal, in landing order. Each step is one flash cycle on iv7.1.

1. **§4.1 boot_display.c with probe** -- most surface area, land
   first so subsequent work has a real display init to attach to.
   Verifies "no panel -> log + skip" on iv7.1.
2. **§4.7 main.c wiring** -- gate LVGL side on §4.1's return code.
   iv7.1 boot log should show the skip line and everything else
   should be unchanged.
3. **§4.3 tile registry seed** (home + GPS only). LVGL renders to
   its buffer on iv7.1 with no visible effect; on Mk1b day-1 the
   tiles appear.
4. **§4.6 GPS photo view** -- pure LVGL, no hardware dependency.
   Can be developed against an emulator / bench flash-and-inspect.
5. **§4.2 touch probe + indev bind** -- last of the hardware-
   probing steps; only meaningful on Mk1b.
6. **§4.4 Vbat compile-time switch** -- Mk1b-only, keep the diff
   small and gated.
7. **§4.5 GPS enable** -- Mk1b-only compile-time gate.

Steps 1-4 are all safe to develop and flash on iv7.1. Steps 5-7 are
Mk1b-only but land in the same binary via compile-time selection.

---

## 7. What is explicitly NOT in Stage 21

- Day-2 tiles (health, env, compass, imu, light, ecg, recording,
  settings, flashlight). Framework will accept them; adding is one
  line in the registry per tile once the day-1 pair is proven.
- Notification overlay producer wiring (`ui_notif_overlay.h` has the
  consumer; nothing publishes yet).
- Wrist-raise -> screen-wake gate. Screen stays on-when-LVGL-up.
- Any Stage 19 power work. Screen + LVGL will regress the drain
  further; that is expected and post-trip.
- BQ telemetry `(fake!)` label cleanup (Delta plan §8). Post-trip
  cosmetic.

---

## 8. Results

_(populated per landed change)_

### 8.1 boot_display.c + CO5300 SPI plumbing (fw `0.4.27`)

Landed the foundation for §4.1 in one batch. LVGL binding stays for §4.1b
so this iteration is safe to flash and observe on iv7.1 without a panel.

**`components/co5300/co5300.c` -- filled in every `TODO(hw)` block:**

- `spi_bus_up()` -- configures CS + RST as manual GPIOs (RST idle high, CS
  idle high), then `spi_bus_initialize(SPI2_HOST, ...)` with
  `SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS`, QSPI wp/hd
  wired to D3/D2. `spi_bus_add_device()` with cmd_bits=8, addr_bits=24,
  mode 0, half-duplex, spics=-1 (manual CS), 40 MHz.
- `spi_send_instruction()` -- `SPI_TRANS_MULTILINE_CMD|MULTILINE_ADDR`,
  cmd=0x02, addr=`(cmd_op << 8)`, inline `tx_data` for <=4-byte payloads
  else external `tx_buffer`. Manual CS pulse around each transaction.
- `spi_send_pixels()` -- `SPI_TRANS_MODE_QIO`, cmd=0x32, addr=panel addr,
  external tx_buffer. Single-shot; the LVGL flush path will refactor to
  keep CS low across multi-chunk bursts if throughput demands it.
- `reset_panel()` -- HIGH 50 ms, LOW 200 ms, HIGH 300 ms per Arduino
  reference sketch. Different from the old stub's "LOW 10 ms, HIGH 120 ms"
  comment; trust the proven sequence.
- `panel_init_sequence()` -- full sequence per
  `15_amoled_touch_test.ino`: SLPOUT (120 ms delay), 0xFE 00 (page 0),
  0xC4 80 (SPI RAM write en), 0x3A 77 (COLMOD RGB888), 0x36 00 (MADCTL
  normal), 0x53 20 (WRCTRLD brightness ctrl on), 0x63 FF (HBM max),
  DISPON, 0x51 D0 (~80 %), 0x58 00 (sunlight off), INVOFF.
- `co5300_set_brightness()` -- now actually writes 0x51 via SPI (was a
  no-op stub before).
- `co5300_set_window()` -- gains the trailing `RAMWR (0x2C)` so
  callers can immediately push pixels after the CASET/RASET pair.
- `co5300_get_status()` -- kept as `ESP_ERR_NOT_SUPPORTED` with a comment
  pointing at the CST9217 I2C probe in `boot_display.c` as the real
  presence check. Half-duplex RDID readback is untested on this chip;
  add once Mk1b bench-verifies a real read.
- `CO5300_MAX_TRANSFER_BYTES` set to 4104 (panel init + modest partial
  refresh). LVGL flush lands in §4.1b -- bump if needed.

**`components/boot_logic/boot_display.h`:**

- Added `boot_display_init(void)` returning `esp_err_t`
  (`ESP_ERR_NOT_FOUND` = no display module).
- Added `boot_display_is_present(void)` for §4.1b LVGL gating.
- Documented the CST9217-probe-as-presence-signal contract.
- `LCD_H_RES / LCD_V_RES` unchanged (still 466x466 logical -- the
  CO5300's 22-column offset is added inside `co5300_set_window`).

**`components/boot_logic/boot_display.c` (new):**

- `touch_module_present()` -- takes `g_i2c_mutex`, calls
  `cst9217_probe_ack(I2C_NUM_0, ...)`, checks byte == `0xAB`. Returns
  false on any I2C error or signature mismatch. Presence of the touch
  chip on bus 0 is the proxy for whether the display FPC is populated
  (touch + panel share the FPC).
- `boot_display_init()` -- if probe fails, logs
  `no CO5300 module -- skipping display bring-up` and returns
  `ESP_ERR_NOT_FOUND` without touching QSPI pins. Otherwise runs
  `co5300_init` with `CO5300_CONFIG_DEFAULT()` (SPI2_HOST + master
  pinout v20 pins) and stores the handle in `s_disp`.
- `backlight_set_brightness()` -- new real implementation. Forwards to
  `co5300_set_brightness(s_disp, pct)` when present; no-op otherwise.
  Callers (`ui_lock_screen.c`, `veml6030/light_tile.c`) see the same
  signature, no changes needed on their side.

**`components/boot_logic/boot_hw_init.c`:**

- Removed the Phase-1 `backlight_set_brightness` stub; replaced with a
  three-line comment pointing at `boot_display.c`.

**`components/boot_logic/CMakeLists.txt`:**

- Added `boot_display.c` to SRCS.
- Added `co5300` and `cst9217` to REQUIRES.

**`main/main.c`:**

- Uncommented `#include "boot_display.h"`.
- Replaced the old "TODO restore when display returns" block with a
  real `boot_display_init()` call. Return code is inspected only for
  the boot-log line: `display ready`, `display absent -- running
  headless`, or the raw `esp_err_to_name(...)` on driver failure.
- Placeholder comment noting §4.1b LVGL binding still to come.
- Note: `boot_cst816d_configure()` from the old commented-out block
  was NOT restored -- Mk1b uses CST9217, not CST816D.

**`components/field_capture/firmware_version.h`:**

- `0.4.26` -> `0.4.27`.

**iv7.1 expected boot log:**

```
I (xxxx) BOOT_DISP: no CST9217 on bus 0 (i2c err=ESP_ERR_TIMEOUT)
I (xxxx) BOOT_DISP: no CO5300 module -- skipping display bring-up
I (xxxx) MAIN: display absent -- running headless
```

Rest of boot (sensors, tasks, CLI, ship mode, recording modes) is
unchanged. If any of the CO5300 driver code introduced a hang or
crash, it would show up here; on iv7.1 the code never runs past the
probe.

**Mk1b expected boot log (post-reflow-A, panel connected):**

```
I (xxxx) BOOT_DISP: CST9217 detected (0xAB @ 0xD000) -- display module present
I (xxxx) co5300: init OK in NNNNN us, heap delta = NNN bytes
I (xxxx) BOOT_DISP: CO5300 ready (466x466 logical, brightness ~80%)
I (xxxx) MAIN: display ready
```

Panel should light up (backlight ~80 %) and stay in the "no LVGL, no
drawing" state. That is expected -- §4.1b adds the LVGL flush that
actually paints pixels. For a pre-§4.1b sanity test on Mk1b, call
`co5300_set_window()` + `co5300_write_pixels()` from a CLI verb or a
one-shot in boot to draw a solid rectangle.

**Not touched (deferred to §4.1b):**

- `main/CMakeLists.txt` `esp_lvgl_port` require -- still omitted.
- LVGL port `add_disp` / draw buffer / flush callback.
- CST9217 driver init + LVGL indev bind (§4.2).
- Tile registry seed (§4.3).

### 8.2 main.c gating + skip path

_(TBD)_

### 8.3 tile_registry seed

_(TBD)_

### 8.4 GPS photo view

_(TBD)_

### 8.5 CST9217 probe + indev bind

_(TBD)_

### 8.6 Vbat compile-time switch

_(TBD)_

### 8.7 GPS compile-time enable

_(TBD)_

---

## 9. Next steps

_(fill at wrap)_

Once Mk1b PCB is in hand, this stage closes and any Mk1b-specific
follow-up work opens under `docs/build_info/Mk1b_build_reports/`
(new folder). Anything pre-arrival stays here.

---

## References

- `docs/build_info/reference_files/IV71_TO_MK1B_FIRMWARE_DELTA.md` --
  9-step delta plan; Stage 21 §4 executes against it.
- `docs/build_info/Mk1_build_reports/Stage_18_Log_Audit_PCF_Tile_Spec.md`
  §5 -- tile spec (home + GPS day-1, GPS two-mode).
- `docs/build_info/Mk1_build_reports/Stage_20_Button_Responsiveness.md`
  §6.3 -- 5 s cold-boot analysis (parked).
- `docs/build_info/Mk1_build_reports/Stage_19_Power_Refinement.md` --
  deferred; fresh-chat prompt still valid when we return.
- `firmware/esp-idf/components/co5300/{co5300.c,co5300.h}` --
  panel driver ready for `boot_display.c` to call.
- `firmware/esp-idf/components/cst9217/{cst9217.c,cst9217.h}` --
  touch driver with probe register 0xD000 -> ACK 0xAB documented.
- `firmware/esp-idf/components/lvgl_ui/tile_registry.h` -- descriptor
  API waiting for §4.3 seed.
- `firmware/arduino/15_amoled_touch_test/15_amoled_touch_test.ino` --
  first post-reflow-A flash on Mk1b.
