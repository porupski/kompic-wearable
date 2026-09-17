# Stage 25 -- Mk1b display bring-up + GPS wiring + MAX17048 stub

**Date opened:** 2026-09-10
**Date closed:** 2026-09-11
**Board:** Mk1b iv8.0 (physical unit on Ivan's bench)
**Firmware baseline:** `0.4.32` (Stage 24 close) -> shipped at `0.4.40`.
**Status:** CLOSED. Portrait display + touch + GPS stable. Landscape,
font sharpness, blue-filter color, MAX17048, and layout polish carry
over to Stage 26 (`Stage_26_Mk1b_Display_Polish.md`).

---

## 1. Why this stage exists

Stage 24 closed with the panel dark. The CST9217 probe fix
(`cst9217_reset()` before `cst9217_probe_ack()`) was in tree but never
flashed. Stage 25 asks, in Ivan's priority order:

  1. Something visible on the screen. React to touch.
  2. GPS coordinate tile updating (MAX-M10S is soldered on Mk1b, untested).
  3. MAX17048 fuel gauge driver stub so the chip ACKs on the bus. No
     battery attached yet — SOC/Vcell reads are noise.

Ivan re-confirmed at open-of-stage: **MAX17048 is on I2C bus 2 (`I2C_NUM_1`)
alongside BQ25619**, not on I2C0 with the sensors.

Standalone observation carried forward: Mk1b quiescent is ~40 mA (headless)
vs iv7.1 ~100 mA. Almost certainly a small short healed between board
revs (SD not on Mk1b either). Noted, no action.

---

## 2. Batches landed (chronological, keep for backtrace)

### Batch A -- GPS wired + Stage 24 §2.7 display fix ride-along (fw `0.4.33`)

**Files:** `boot_hw_init.c` (max_m10s_init + broker enable),
`boot_tasks.c` (task_gps_fn added to task_table[]),
`boot_logic/CMakeLists.txt` (REQUIRES += max_m10s),
`firmware_version.h`.

**Flash result** (2026-09-10 evening): screen still dark, touch offline.
Stage 24 §2.7 reset-before-probe did NOT unblock the panel — boot log
short-circuited at CST9217. GPS side overshadowed and not evaluated.

### Sketch fall-back -- 16_amoled_touch_test_mk1b

Copied `firmware/arduino/15_amoled_touch_test/` →
`16_amoled_touch_test_mk1b/`. Header banner + no-battery ship-mode note
only; every display + touch pin is identical on iv7.1 and Mk1b per
master pinout v20 (double-checked in `IV71_TO_MK1B_FIRMWARE_DELTA.md`
§2 and Stage 24 §2.7). Vbat divider was on GPIO18 (ADC2_CH7) as an
iv7.1 prototype override per `project_vbat_on_gpio18.md`; on Mk1b
GPIO18 is `MAX_M10S_RX_PIN`. Sketch touches neither.

**Sketch flash result:** panel + touch worked cleanly — color-test
squares, yellow touch stamps that fade, current draw jumped 40 mA →
**82 mA**. Confirmed panel + touch hardware is fine; failure is in the
ESP-IDF integration.

### Batch C -- align ESP-IDF path with sketch (fw `0.4.34`)

Three sketch/driver divergences closed in one flash:
  1. `touch_module_present()` used `0xAB @ 0xD000` register-read; sketch
     uses bare `i2c_ping`. New `cst9217_ack_probe()` exposed and used.
  2. `CST9217_RST_LOW_MS` 5 → 20, `CST9217_RST_HIGH_MS` 50 → 200 (match
     sketch's 20/200).
  3. `co5300_init` didn't call `spi_device_acquire_bus`; sketch does.
     Added + `release_bus` in `spi_bus_down`.

**Batch C flash result:** panel came up **but rendered wrong**:
  - Green background, white-on-black text blocks.
  - LED-dot icons wrong colors, no symbols.
  - "Sheared / captcha-like" render on minute change (recovered).
  - Screen was portrait (correct for Mk1b) but skewed inside.
  - **`CST9217 init failed: ESP_ERR_NOT_FOUND -- panel OK, touch disabled`**
    because `cst9217_init` still checked `0xAB @ byte 0` from `0xD000`
    (my `ack_probe` only fixed the presence path in `boot_display.c`).
  - Boot log flooded with **12 x `spi_master: check_trans_valid(1127):
    txdata transfer > hardware max supported len`** per full-screen
    render.
  - Current draw ~72 mA (panel bright, flushes failing).

### Batch D -- chunk pixels + fix CST9217 report layout (fw `0.4.35`)

Root causes from the Batch C log:

  - ESP32-S3 SPI DMA cap = `SPI_LL_DMA_MAX_BIT_LEN = 2^18 bits = 32 KB
    per transaction` (confirmed at
    `~/.espressif/v5.5.2/esp-idf/components/hal/esp32s3/include/hal/
    spi_ll.h:44`). LVGL strip is `466 * 40 * 3 = 55 920 B` — over cap.
    Sketch works because `fill_pixels()` chunks pixels into 1024-pixel
    (3 KB) bursts with CS held low across chunks: first chunk carries
    cmd `0x32` + addr `0x003C00`, subsequent chunks use
    `SPI_TRANS_VARIABLE_CMD/ADDR/DUMMY = 0` so the QIO stream
    continues without a fresh command frame.
  - CST9217 report layout the driver assumed (ACK at byte 0, x/y at
    4..7) is CST816S-family, NOT CST9217. Sketch's actual layout:
    byte 0 low nibble = touch status (`0x06` = pressed), bytes 1/2/3
    pack the coordinates (byte 1 = x[11:4], byte 2 = y[11:4], byte 3
    splits x[3:0] / y[3:0]), byte 5 low 7 bits = finger count, **byte 6
    = 0xAB validity marker**, total report = 12 bytes. Sketch also
    writes `0xAB` back to `0xD000` after each read as a handshake —
    chip needs this to clear its INT.

Landed:
  1. `co5300.c` `spi_send_pixels` — chunks into 12 288-byte (4096-pixel)
     bursts, first-chunk pattern from sketch. `CO5300_PIXEL_CHUNK_BYTES`
     defined at 12 KB, comfortably under the 32 KB HW cap.
  2. `cst9217.h` — `CST9217_REPORT_LEN` 8 → 12; report-layout comment
     rewritten to reflect the sketch-verified fields.
  3. `cst9217.c` `cst9217_init` — drop the byte-0 `0xAB` check, use
     `cst9217_ack_probe` (bare I2C ACK).
  4. `cst9217.c` `task_touch_fn` — decode per sketch. New
     `cst9217_touch_ack()` helper writes `0xAB @ 0xD000` after every
     report to clear INT.

**Batch D flash result:**
  - **No SPI errors anywhere in the log.** Pixel chunking works.
  - **CST9217 init OK.** `LVGL touch indev bound to g_touch_q`.
    `LVGL up -- panel=CO5300 touch=CST9217`. **Touch works.**
  - **BUT image is skewed, sliced, zig-zag, torn.** Barely usable — Ivan
    could tell where he was by swiping but not read text.
  - Date "kinda blurry", time "went captcha" during a swipe and stayed
    that way.
  - `E task_wdt: Task watchdog got triggered ... CPU 0: taskLVGL`,
    repeating every 5 s after the first swipe into Settings. Backtrace:
    `lv_tlsf_malloc → lv_draw_layer_alloc_buf → dispatch →
    lv_display_refr_timer`.

### Batch F -- LVGL heap → PSRAM via libc malloc (fw `0.4.37`)

Root cause from Batch E: `task_wdt` on IDLE0 still triggered when Ivan
navigated a few tiles in (sys tile suspected). Backtrace two flashes in
a row: `lv_tlsf_free → lv_draw_buf_create_ex → lv_draw_layer_alloc_buf`
then `lv_tlsf_malloc → block_prepare_used → block_can_split`.

**Load-bearing discovery mid-Batch F:** `sdkconfig` has
`CONFIG_LV_CONF_SKIP=y`, which means the ESP-IDF managed LVGL component
IGNORES `firmware/esp-idf/main/lv_conf.h` at build time and uses Kconfig
entries only. Every prior `lv_conf.h` tweak in this project has been a
no-op. In particular, Batch E's `LV_MEM_SIZE = 256 * 1024U` never landed
-- LVGL was running with `CONFIG_LV_MEM_SIZE_KILOBYTES = 64`, exactly the
old baseline. That explains why Batch E's WDT looked identical to
Batch D's.

Real fix: `sdkconfig` `CONFIG_LV_USE_BUILTIN_MALLOC=y` →
`CONFIG_LV_USE_CLIB_MALLOC=y`. LVGL now uses libc `malloc()`; esp-idf's
`CONFIG_SPIRAM_USE_MALLOC=y` + `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`
routes any allocation ≥ 16 KB straight to PSRAM. Widget trees (many small
allocations) stay in internal SRAM. LVGL's 8 MB PSRAM ceiling >> any
composite layer the sys tile can dream up.

Landed:
  1. `sdkconfig` `CONFIG_LV_USE_BUILTIN_MALLOC` → not set,
     `CONFIG_LV_USE_CLIB_MALLOC=y`. Removed the stale
     `LV_MEM_SIZE_KILOBYTES / _POOL_EXPAND / _ADR` lines (they depend on
     BUILTIN in Kconfig).
  2. `sdkconfig.defaults` — same change appended with an explanatory
     block so it survives a menuconfig round-trip.
  3. `lv_conf.h` — comment rewritten to flag `CONFIG_LV_CONF_SKIP=y`;
     the file's contents are documentation-only under the current Kconfig.
  4. `firmware_version.h` 0.4.36 → 0.4.37.

**What Batch F's flash should show**
  - No `task_wdt` no matter how many tiles Ivan visits, no matter how
    long he swipes.
  - `heap_caps` metrics: PSRAM utilisation climbs by a few hundred KB
    once the sys tile has been visited; internal SRAM heap stays stable.
  - Everything else identical to Batch E (no color / rotation change).

### Batch G -- landscape rotation + fonts + safe-area + log cleanup (fw `0.4.38`)

Batch F confirmed: no `task_wdt` on any tile Ivan visited, VEML6030
turned out fine (auto-brightness works after `LIGHT EN`), GPS RTC-sync
fired at first fix. Def-of-done §1 met.

Ivan's Batch G asks:
  - **Landscape orientation** (Mk1b is mounted landscape; Batch E's
    portrait was my misread of his earlier note).
  - **Font size bump** ≥ 2x -- current UI_FONT_* are 10/12/14/16, way
    too small on a high-DPI 502x410 landscape panel.
  - **Mandatory 25 px safe-area margin** on all edges -- AMOLED corners
    are arced, sys tile's switch was hidden behind the arc.
  - **Investigate blurry text** (may not be gone in Batch G).

Landed:
  1. `boot_display.h` -- split LCD constants:
     - `LCD_NATIVE_W = 410`, `LCD_NATIVE_H = 502` (panel physical, only
       consumed by lvgl_ui_display for lv_display_create).
     - `LCD_H_RES = 502`, `LCD_V_RES = 410` (landscape LOGICAL, used by
       every widget / tile / overlay for layout).
  2. `lvgl_ui_display.c` -- `lv_display_create(LCD_NATIVE_W, LCD_NATIVE_H)`
     then `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`. LVGL
     swaps hor/ver internally; widget code sees 502x410, flush cb
     receives pixels in NATIVE 410x502 orientation -- co5300 driver
     works unchanged.
  3. `ui_theme_colors.h` fonts: `TITLE 16→28`, `LABEL 14→22`,
     `VALUE 12→20`, `CHIP 10→16`. All sizes 8..30 already enabled in
     Kconfig so no font-map add needed.
  4. `ui_theme_colors.h` padding: `UI_TILE_PAD_H 10→25`,
     `UI_TILE_PAD_V 8→25`. Widget code that uses these for corner
     alignment now clears the arc automatically. Tiles that hardcode
     x/y offsets (system_tile, some sensor tiles) still need per-tile
     audits -- Batch H.
  5. `system_tile.c` -- demoted the "Internal temp sensor init failed"
     line from WARN to INFO with a clear explanation. The install was
     already done by `main.c::esp_ts_ensure_init()` for the STATUS verb,
     so `ESP_ERR_INVALID_STATE` here is expected and STATUS still shows
     `soc_temp` fine.

**What Batch G's flash should show**
  - Screen orientation: **landscape** (long axis horizontal). Widgets
    laid out in a 502 wide × 410 tall canvas.
  - Fonts noticeably bigger; ideally sharper too but the blur is a
    16-bit color-depth artifact and may persist -- Batch H if it does.
  - Sys tile switch fully visible (25 px inset from the arced corner).
  - Boot log: no more `SYS_TILE: Internal temp sensor init failed`
    warning. Info line instead: "already owned by esp_ts_ensure_init".
  - No new `task_wdt`, no `spi_master check_trans_valid` errors.

**Deferred to Batch H (if needed after Batch G flash)**
  - `CONFIG_LV_COLOR_DEPTH_16` → `_32` (ARGB8888) if text still looks
    blurry. Doubles the LVGL internal pixel size but PSRAM heap under
    Batch F absorbs it. Anti-aliasing goes from 6-bit-per-channel to
    8-bit, which is the classic cause of "readable but blurry" text on
    high-density panels.
  - Per-tile alignment audit for tiles that ignore UI_TILE_PAD_H/V.
  - Sluggish left/right swipe -- Batch F PSRAM alloc may have already
    fixed this; if not, look at LV_DRAW_LAYER_MAX_MEMORY.

### Batch H -- CO5300 MADCTL landscape (fw `0.4.39`)

Batch G flash: swipes were landscape-oriented (LVGL knew about the
rotation) but the panel showed torn / sheared frames with a green
"last fourth" band. Fonts noticeably bigger + roughly correct sizes,
still blurry as expected (color depth is Batch I work).

Root cause: `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`
alone does NOT rotate pixel data unless
`LV_DRAW_TRANSFORM_USE_MATRIX` is compiled in AND
`lv_display_set_matrix_rotation(disp, true)` is called. Without those,
LVGL just swaps the reported hor/ver res; the flush callback receives
pixel data laid out in LOGICAL landscape order but writes to native
portrait RAM with landscape coords → geometry mismatch, torn frames,
CASET overflow past native col 466 → panel wraps / discards → green
band from the previous frame buffer content.

Landed:
  1. `co5300.h` -- new `bool rotate90` field on `co5300_config_t`;
     `CO5300_CONFIG_DEFAULT()` sets it to true.
  2. `co5300.c` `panel_init_sequence` -- MADCTL 0x60 (MV=1, MX=1) when
     `rotate90`, else 0x00. Panel scans its native 410x502 RAM as a
     502x410 landscape image.
  3. `co5300.c` `co5300_set_window` -- when `rotate90`, swap the
     (x0,y0,x1,y1) inputs and move the +22 `COL_OFFSET` from CASET to
     what LVGL calls Y (which is native COL after MADCTL swap). Math:
     `native_col = y_lvgl + OFFSET`, `native_row = x_lvgl`.
  4. `lvgl_ui_display.c` -- reverted Batch G's
     `lv_display_set_rotation()`. LVGL now treats the display as
     native landscape 502x410 directly (LCD_H_RES/V_RES = 502/410 from
     Batch G stays). `lv_display_create(LCD_H_RES, LCD_V_RES)`.
  5. `firmware_version.h` 0.4.38 -> 0.4.39.

**What Batch H's flash should show**
  - Full-screen landscape image, no tearing, no green fringe.
  - If the image appears MIRRORED horizontally, change MADCTL 0x60 →
    0xA0 (MV=1, MY=1 instead of MX=1).
  - If the image appears OFFSET by ~22 px vertically instead of
    covering the full panel, the col-offset needs to stay on CASET
    (undo step 3 partially -- keep +22 on x0/x1).

**Deferred to Batch I (if still needed after Batch H)**
  - Blue-light filter overlay looks bright blue instead of yellow-tint
    per Ivan's Batch G observation. Suspect RGB/BGR channel order
    mismatch in the light_tile's overlay style or in LVGL's RGB888
    byte-order convention. Verify: draw a solid RED rectangle from LVGL,
    check if it shows red on panel; if blue, LVGL and CO5300 disagree on
    byte order.
  - LV_COLOR_DEPTH bump to 32 (from 16) if fonts still blurry.
  - Per-tile alignment audit for tiles that hardcode x/y instead of
    using UI_TILE_PAD_*.

### Batch E -- resolution fix + LVGL heap bump (fw `0.4.36`)

### Batch E -- resolution fix + LVGL heap bump (fw `0.4.36`)

Root causes from Batch D:

  - `LCD_H_RES = LCD_V_RES = 466` in `boot_display.h`. Panel is
    **410 × 502** per `co5300.h` and per the working sketch (which uses
    `LCD_WIDTH=410`, `LCD_HEIGHT=502`, `COL_OFFSET=22`). The 466x466
    "square addressable region" comment in the header traced to an
    incorrect Espressif-demo assumption. Every LVGL flush computed
    pixel addresses off the 466x466 grid; the driver's set_window then
    added `+22` col offset and sent to the panel — pixels landed at
    wrong panel coordinates, hence the tearing.
  - `LV_MEM_SIZE = 64 KB` (per memory `feedback_lvgl_heap_too_small_
    for_pixels.md`). LVGL 9's `lv_draw_layer_alloc_buf` for
    swipe-animation composite layers exceeds 64 KB. `lv_tlsf_malloc`
    spins the free-list search when no block fits → `taskLVGL` never
    yields → IDLE0 misses WDT reset.

Landed:
  1. `boot_display.h` `LCD_H_RES` 466 → 410, `LCD_V_RES` 466 → 502.
     Comment rewritten to reflect the sketch-verified geometry.
  2. `lv_conf.h` `LV_MEM_SIZE` `64 * 1024U` → `256 * 1024U`. Comment
     explains the Batch D WDT observation.
  3. `firmware_version.h` 0.4.35 → 0.4.36.

**What Batch E's flash should show**
  - Panel geometry correct: no tearing, tiles render in the right area.
  - Widgets designed for the old 466x466 square may now look off-center
    or clipped on the 410x502 portrait — that's expected and layout
    polish is a follow-up batch, not a Stage 25 blocker.
  - No `task_wdt` after swipes into Settings, tile-to-tile navigation,
    or minute changes.
  - Current draw should climb into the sketch-baseline ~80-82 mA range.
  - GPS init still logs (Batch A ride-along); GPS tile picks up a fix
    when the antenna is connected.

---

## 3. Deferred behind display work

  - **Batch G (landscape + fonts + layout)** — LVGL 90° rotation, font
    size bump ≥ 2x, tile layout audit for 502x410 landscape (not 410x502
    portrait as the Batch E code has it now). Lands after Batch F flash
    confirms no more WDT.
  - **Batch B (MAX17048 stub)** — driver at I2C1 @ 0x36, `FUEL` CLI
    verb, ACK-only probe, no task, no cell yet. Lands after Batch G.
  - **Post-battery batch** — broker channel, 1 Hz polling, SOC in the
    battery tile. Waits for Ivan to attach a cell.
  - **VEML6030 diagnosis** — auto-brightness read looked dead on Batch
    E. Might be hardware (VEML6030 fried on Mk1b) rather than firmware.
    Isolated CLI verb `LIGHT` already prints raw lux; use it to
    corroborate before touching driver code.

---

## 4. Non-goals for Stage 25

  - DRV2605 auto-cal retry (still failing STATUS=0xEC on cold-cal
    without the motor properly tuned).
  - SD path (SD not on Mk1b).
  - CLI submenu restructure (queued per `project-cli-submenus`).

---

## 5. Definition of done

Stage 25 closes when:

  1. Panel wakes on every boot, no CST9217 probe skip, no SPI error
     lines, no `task_wdt`.
  2. Main clock face renders correctly. Swipe → Settings tileview
     works, taps navigate to individual tiles, TILE `<n>` verb jumps.
  3. GPS tile updates when the antenna is connected and a fix arrives.
  4. Everything underneath the display path stays stable: sensor tasks,
     RTC, BQ25619 charging, haptic, WS2812 — nothing regressed by the
     display work.

MAX17048 (Batch B) is NOT in the definition of done; it's the last
Stage 25 item but the stage doesn't block on the fuel gauge.

Once §5.1-4 hold across a fresh cold boot AND a soft REBOOT, Ivan can
pack the board off the bench (SD + battery + encoder + button + GPS
antenna install phase). Firmware iteration continues remotely after
that.

---

## 6. Bench log (chronological)

### §6.A  Batch A flash

Screen dark, touch offline. Stage 24 §2.7 reset-before-probe alone did
not unblock the CST9217 probe. GPS side not evaluated.

### §6.C  Batch C flash

Panel up but wrong render (green bg, LED dots wrong, text blocks visible
but wrong colors, minute-change shear). Touch disabled with
`CST9217 init failed`. Log flooded with `check_trans_valid(1127)`
errors, 12 per render cycle. Draw ~72 mA.

### §6.D  Batch D flash

No SPI errors. CST9217 init OK, touch bound to LVGL indev, touch
actually works (`I UI_NAV: → Alarm (swipe right)` etc.). Image sliced /
torn / zig-zag — can barely tell tiles apart. Blurry date, "captcha"
time after swipe. `task_wdt` triggered on IDLE0 after swipe to Settings
(t = 92 s), repeating. LVGL task stuck in
`lv_tlsf_malloc → lv_draw_layer_alloc_buf`.

Benign log note (not a bug):
`E gpio: gpio_install_isr_service(530): GPIO isr service already
installed` — `cst9217_int_install` handles `ESP_ERR_INVALID_STATE` as
success; log line is Espressif's default level.

### §6.E  Batch E flash

Display geometry correct, no more tearing, tiles navigable. WDT on
IDLE0 still triggers when Ivan gets a few tiles in (sys tile suspected).
Backtrace path identical to Batch D: `lv_tlsf_*` inside
`lv_draw_layer_alloc_buf`. Root cause traced to
`CONFIG_LV_CONF_SKIP=y` making `lv_conf.h` a no-op -- Batch E's
`LV_MEM_SIZE=256 KB` never took effect; LVGL was still on
`CONFIG_LV_MEM_SIZE_KILOBYTES=64`. Fonts noted as tiny + blurry, screen
now confirmed **landscape** (not portrait as I wrote in Batch E), VEML6030
auto-brightness looks dead.

### §6.F  Batch F flash

No `task_wdt` anywhere in the transcript across sys tile visit, IMU
gesture toggles, VEML6030 enable/disable, GPS view switching, and
multiple swipes into Settings and back. Def-of-done §1 met.

VEML6030 works fine (Ivan turned it on manually with `LIGHT EN`;
auto-range kicked in `range set: gain=2 → gain=3` as ambient changed).
Fuel gauge / MAX17048 still deferred behind display polish (no cell).

`GPS UTC sync: 01:01:00 2026-09-11` fired on the first fix -- cross-driver
event wiring works end-to-end.

Remaining Ivan complaints logged for Batch G:
  - Fonts still tiny AND blurry (Batch F heap change had no visible
    effect on blur, as expected -- heap size doesn't touch AA).
  - Sys tile switch hidden behind the AMOLED arced corner.
  - Orientation is portrait but SHOULD be landscape.

Cosmetic log lines noted (not bugs):
  - `E gpio: gpio_install_isr_service(530): GPIO isr service already
    installed` -- benign, CST9217 install_isr treats ESP_ERR_INVALID_STATE
    as success.
  - `E temperature_sensor: temperature_sensor_install(136): Already
    installed` + `W SYS_TILE: Internal temp sensor init failed` -- the
    sys tile is racing main.c's esp_ts_ensure_init (which installs first
    for STATUS). STATUS still shows soc_temp OK because the earlier
    install succeeded. Demoted in Batch G to INFO with explanation.

### §6.G  Batch G flash

Fonts bigger (roughly correct sizes now, still blurry). Swipes
respect landscape orientation but rendering is torn / sheared with a
green "last fourth" band. `lv_display_set_rotation` alone doesn't
actually rotate pixels without `LV_DRAW_TRANSFORM_USE_MATRIX`; only
swaps reported hor/ver res. Batch H switches to HW MADCTL rotation.

Also flagged (not blocking Batch H): blue-light filter overlay renders
as bright blue instead of a soft yellow-tint filter -- suspected
RGB/BGR channel swap somewhere in LVGL 9 -> CO5300 output path.
Diagnosis is Batch I work.

### §6.H  Batch H flash

Worse than Batch G: green bars everywhere, previous content shown as
noise. HW MADCTL rotation on this specific CO5300 silicon apparently
doesn't do what the datasheet's family behaviour would suggest, OR the
`+22` col-offset interaction with MV=1 is not what I assumed.
Ivan called it: shortcuts aren't going to close landscape.

### §6.I  Batch I flash -- portrait revert, Stage 25 close (fw `0.4.40`)

Reverted the display geometry back to the Batch F working state:
  - `boot_display.h` LCD_H_RES / LCD_V_RES = 410 / 502 (portrait, panel
    native). Removed the LCD_NATIVE_W/H split from Batch G (unused
    without LVGL rotation).
  - `co5300.h` `CO5300_CONFIG_DEFAULT()` `.rotate90 = false`. The field
    stays defined so Stage 26 can resume without re-plumbing.
  - `co5300.c` panel_init still selects MADCTL from `rotate90` (0x00
    when false, matches sketch). set_window still branches on
    `rotate90`. Both paths ready for Stage 26 experimentation.
  - `lvgl_ui_display.c` `lv_display_create(LCD_H_RES, LCD_V_RES)` in
    portrait dims -- no set_rotation call. Comment rewritten with the
    Stage 26 hand-off note.
  - Font bumps (`UI_FONT_*` 16/14/12/10 -> 28/22/20/16) KEPT from
    Batch G -- still readable in portrait.
  - Safe-area padding (`UI_TILE_PAD_H/V` 10/8 -> 25/25) KEPT.
  - `system_tile.c` temp-sensor init log demotion (W -> I with
    explanation) KEPT.
  - `firmware_version.h` 0.4.39 -> 0.4.40.

Def-of-done audit at Stage 25 close:
  1. Panel wakes on every boot, no CST9217 probe skip, no SPI error
     lines, no task_wdt.                                        **MET**.
  2. Main clock face renders correctly, swipe -> Settings works,
     taps navigate to individual tiles, TILE `<n>` verb jumps. **MET**
     (portrait; some tile-level clipping under bigger fonts, to be
     tuned in Stage 26).
  3. GPS tile updates when antenna connected and fix arrives.  **MET**
     (RTC sync fired 01:01:00 2026-09-11 UTC in Batch F log).
  4. Everything underneath the display path stays stable.      **MET**
     (BATCH F log clean across BME688 / LSM6DSV / LIS3MDL / VEML6030
     / PCF85063 / BQ25619 / GPS / haptic / rgb_policy /
     field_capture; only failure is SD which is expected -- no card).

---

## 7. Current-draw log for Stage 25

| Config                                    | Draw     |
|-------------------------------------------|----------|
| Headless boot, no LVGL, no panel           | 40 mA   |
| Batch C — panel bright, flushes erroring   | 72 mA   |
| Sketch 16 running (target baseline)        | 82 mA   |
| Batch D — panel bright, torn render        | ~72 mA (assumed) |
| Batch E — target                           | 80-82 mA |

Mk1b idle is ~60 mA less than iv7.1 at comparable config; noted 2026-09-10
during Stage 24 bring-up, no follow-up planned.

---

## 8. Next-steps hook -- Stage 26 scope

`docs/build_info/Mk1b_build_reports/Stage_26_Mk1b_Display_Polish.md`
opens at Stage 25 close. Batches queued there:

  - **Landscape orientation (real this time).** Two candidates:
    (a) enable `LV_USE_MATRIX` + `LV_DRAW_TRANSFORM_USE_MATRIX` in
    Kconfig, then `lv_display_set_matrix_rotation(disp, true)` after
    the existing `lv_display_set_rotation(90)`. This is LVGL's
    documented path and should Just Work; risk is bloat + unknown
    interactions with our PSRAM heap.  (b) manual pixel transpose in
    the flush callback -- deterministic, but costs ~600 us per strip.
    Try (a) first.
  - **Font sharpness.** `CONFIG_LV_COLOR_DEPTH_16` -> `_32` (ARGB8888).
    Anti-aliasing goes from 5/6/5 quantized to full 8-bit-per-channel.
    Batch F PSRAM heap absorbs the doubled internal pixel size.
  - **Blue-light filter looks bright blue not yellow-tint.** Likely
    RGB/BGR channel-order swap somewhere in LVGL 9's RGB888 output vs
    CO5300 expectation. Diagnose by drawing a solid RED rectangle:
    if it shows blue, byte order is swapped. Fix could be MADCTL bit 3
    (RGB/BGR) or setting `LV_COLOR_FORMAT_BGR888` on the display.
  - **MAX17048 fuel gauge stub (Stage 25 Batch B).** Never landed
    behind display work. Driver at I2C1 @ 0x36, `FUEL` CLI verb,
    ACK-only probe, no task, no cell yet. Small, self-contained.
  - **Per-tile layout audit under bigger fonts.** Some tiles hardcode
    x/y offsets (system_tile uses 12/8/34) and will look off with the
    new 28/22/20/16 pt fonts. Once landscape lands, walk every tile.
  - **Sluggish left/right swipe.** Batch F PSRAM heap may have already
    fixed this; verify after Stage 26 lands, and if not, look at
    `CONFIG_LV_DRAW_LAYER_MAX_MEMORY`.

Non-Stage-26 items (opportunistic, not required):
  - DRV2605 auto-cal retry once the LRA motor is properly tuned.
  - GPS antenna soldering + real-world fix test.
  - VEML6030 auto-brightness verification with actual light variation.
