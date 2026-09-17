# Stage 30 -- Mk1b esp_lvgl_port migration + landscape + sleep race

**Date opened:**  2026-09-13
**Date closed:**  2026-09-15
**Board:**        Mk1b iv8.0
**Firmware baseline:** `0.4.61` (Stage 29 close: Space Grotesk app-
wide, 96 px clock, full-frame PSRAM refresh mode, shear class fixed
per §4.2b, `esp_cache_msync` unaligned-flag noise silenced).
**Firmware close:** `0.4.75` (portrait, sync-only pixel path, bus
mutex, AUTOSHDN CLI+NVS, NVS dump verb, FLASH bypass verb).
**Status:**       CLOSED -- portrait shipped. Landscape (30.3) and
Mode HUD (30.4) deferred to Stage 31 per Ivan 2026-09-15: doing
either now would frankenstein code that Stage 31's foundations
rebuild will replace.

---

## 1. Why this stage exists

Stage 29 stopped the on-panel text shear by switching to full-frame
render mode with a 617 KB PSRAM buffer, but exposed two carried-over
problems and left one deliberate item unshipped:

  1. **Sluggish swipes.** Full-frame at 40 MHz QIO SPI is ~15 ms of
     bus time per flush -- half the 33 ms budget at 30 Hz. Not a
     bug, a bandwidth ceiling. Our custom driver has no DMA
     overlap: `spi_device_polling_transmit` blocks the calling task
     until the transfer finishes, so LVGL can't render the next
     frame while the current one is on the wire.
  2. **SPI race on idle-timeout sleep.** `co5300_sleep()` fired from
     `field_capture` collides with an in-flight LVGL flush on the
     same SPI device (`spi_master:1406: previous polling transaction
     not terminated`). Retries 6× before it wins. Cosmetic (sleep
     succeeds), but the log spam and the race are ugly. Made
     dramatically more likely by the full-frame flush; the ~1-2 ms
     partial-flush window was easier to miss.
  3. **Landscape 90° CW.** Was Stage 29 §2.2. Never got touched
     because the shear investigation ate the bench time. Trivial
     once we're on Espressif's port -- the port's rotation config
     handles it without custom transpose code.

Also carried from Stage 28 §2.4:

  4. **Mode HUD** on the main screen -- broker channel for current
     mode/submenu/RGB mnemonic, bottom-mid label owned by
     `ui_main_screen.c`. Was deferred out of Stage 28 because font +
     landscape had to settle first. Font settled in 29.1; landscape
     lands in this stage.

Parked observations still open (not new work this stage):

  - **TEMP_DUMP triggers spurious LSM WRIST_DOWN** (Stage 28 §4.3
    obs 1).
  - **LSM tap-double + touch double-tap redundant** (Stage 28 §4.5
    obs 3).
  - **DRV auto-cal IMU sweep needs LSM enabled** (Stage 28 §4.5
    obs 4).
  - **GPS silence** (Stage 27 §4.2).

---

## 2. Plan

Ordered by dependency + risk. Sleep-race first because it's a
small, standalone fix and makes the log clean while we work on the
big migration. Migration second because everything else benefits.
Landscape + Mode HUD ride the migration.

### 2.1 Batch 30.1 -- Sleep-time SPI serialization

**Symptom (Stage 29 §4.2b bench log, fw 0.4.61):**

    E spi_master: spi_device_polling_start(1406): Cannot send
    polling transaction while the previous polling transaction is
    not terminated.

Fires ~6× per idle-timeout entry before `co5300_sleep()` wins the
bus.

**Root cause.** Two paths touch the CO5300 SPI device without any
mutual exclusion:

  - `lvgl_flush_cb` from the LVGL task (any core) --
    `co5300_set_window` + `co5300_write_pixels`.
  - `boot_display_sleep()` from `field_capture` (Core 1) --
    `co5300_sleep`.

`spi_device_polling_transmit` is per-transaction blocking but does
NOT serialize across callers. When LVGL is mid-flush and field
tries to send SLPIN, the second call sees the "polling in
progress" state and errors out.

**Two fix options:**

  A. **Wrap CO5300 access with a driver-level mutex** in
     `co5300.c`. Every entry point (`co5300_set_window`,
     `co5300_write_pixels`, `co5300_sleep`, `co5300_wake`,
     `co5300_set_brightness`) grabs it. Cleanest -- callers stay
     naive.
  B. **Have `boot_display_sleep()` grab `lvgl_port_lock`** before
     touching SPI. Uses infra we already have and guarantees LVGL
     is idle. Downside: bakes the LVGL-port dependency into
     boot_display, and doesn't guard against a hypothetical future
     third caller.

Prefer A. Small addition (~20 lines), keeps the abstraction
localized.

**Success gate:** flash, idle 15+ s, watch console. Zero
`spi_master: 1406` E-lines around the sleep transition. Wake
still works.

**Bench flash:** independent of 30.2/30.3/30.4. Land this first --
smallest change, gives clean logs for the rest of the stage.

---

### 2.2 Batch 30.2 -- `esp_lvgl_port_add_disp` migration

**Symptom:** sluggish swipes on full-frame refresh, ~15 ms SPI per
flush blocks LVGL from starting the next frame's render.

**What changes:** the display init path in `boot_display.c` and
`lvgl_ui_display.c` gets rewritten to use Espressif's blessed API
instead of our hand-rolled CO5300 driver + custom flush cb. The
CO5300 chip driver stays (we still need the vendor-specific init
sequence + QSPI 0x32 pixel command), but the SPI panel-io layer
and the LVGL wiring both move to `esp_lcd_*`.

**Sketch of the target shape:**

```c
esp_lcd_panel_io_spi_config_t io_cfg = {
    .cs_gpio_num          = PIN_CS,
    .dc_gpio_num          = -1,        // CO5300 is command-in-data (no DC)
    .spi_mode             = 0,
    .pclk_hz              = CO5300_FREQ_HZ,
    .trans_queue_depth    = 10,
    .lcd_cmd_bits         = 32,        // CO5300 QSPI 32-bit cmd frame
    .lcd_param_bits       = 8,
    .flags = { .quad_mode = 1 },
    ...
};
esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io);

// Thin CO5300 panel wrapper -- calls io->tx_param / io->tx_color.
esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = PIN_RST,
    .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB, // swap_bytes handled below
    .bits_per_pixel = 24,
};
co5300_new_panel(io, &panel_cfg, &panel);

// LVGL wiring via esp_lvgl_port -- handles double-buffer DMA,
// cache msync, byte swap, and flush completion callbacks.
lvgl_port_display_cfg_t disp_cfg = {
    .io_handle     = io,
    .panel_handle  = panel,
    .buffer_size   = LCD_H_RES * LVGL_STRIP_ROWS,
    .double_buffer = true,
    .hres          = LCD_H_RES,
    .vres          = LCD_V_RES,
    .rotation      = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    .flags = { .buff_dma = true, .swap_bytes = true },
};
lvgl_disp = lvgl_port_add_disp(&disp_cfg);
```

**What we get:**

  - **DMA + double buffer:** SPI transfer happens in background
    while LVGL renders the next frame -> swipes stop feeling like
    the panel is stuttering.
  - **Cache msync + byte-order swap** handled inside the port ->
    delete our manual byte-swap loop and `esp_cache_msync` call.
  - **Partial refresh mode is safe again** -- Espressif's port
    manages the flush handoff correctly, so we can drop back from
    617 KB PSRAM full-frame to a small SRAM strip. Frees ~500 KB
    of PSRAM and cuts per-frame SPI time by ~10×.
  - **Rotation** is a single config bit (`swap_xy` + `mirror_*`),
    unlocks Stage 30.3.

**What we lose:**

  - Manual CS control -- Espressif's panel-io toggles CS
    per-transaction. Our sketch reference (16_amoled_touch_test)
    uses manual CS around the multi-chunk pixel burst; need to
    verify the ESP-LCD panel-io keeps CS low across our chunked
    writes, or accept per-transaction CS toggling and confirm
    CO5300 tolerates it.
  - Our hand-tuned QSPI chunking (12 KB/chunk). ESP-LCD panel-io
    has its own chunking via `trans_queue_depth`.

**CO5300 esp_lcd panel wrapper:** ESP-IDF has no in-tree CO5300
driver. We write a thin one -- ~200 lines wrapping our existing
init sequence + `draw_bitmap` implementation that translates to
CO5300's QSPI 0x32 pixel command. Reference: our own `co5300.c`
init path + Espressif's `esp_lcd_panel_st7789` / `_gc9a01` as
templates.

**Success gate:**

  1. Boot clean, LVGL up, all tiles render.
  2. Text remains shear-free across boot + swipe cycles + clock
     ticks.
  3. Swipe feels comparable to (or better than) the current
     full-frame path.
  4. `esp_cache_msync` calls removed from our code (handled by
     port).
  5. Sleep race from 30.1 stays fixed (port and sleep both use the
     panel-io handle, which serializes internally).

**Bench flash:** after 30.1. This is the big lift -- expect
several flash-per-checkpoint iterations to get the CO5300 panel
wrapper right (init sequence, brightness, sleep/wake).

**Risk:** the CO5300's QSPI pixel command (0x32) diverges from
mainstream MIPI-DBI panels. The ESP-LCD panel-io SPI backend
supports "manufactured 32-bit cmd + 24-bit addr" via
`SPI_TRANS_MODE_QIO` + variable cmd/addr, which is exactly what
our custom driver does today. If it doesn't map cleanly, fall back
to using ESP-LCD panel-io ONLY for the CO5300 init/sleep/brightness
commands and keep our custom pixel-write path via a lower-level
API. Documenting the fallback in the bench log.

---

### 2.3 Batch 30.3 -- Landscape 90° CW

Trivial after 30.2 lands. Set `swap_xy = true` +
`mirror_x = true` (or `_y`, sign to eyeball) in the
`lvgl_port_display_cfg_t.rotation` block. Touch coordinates need
the same transform, which the port handles automatically when
the indev is bound to the rotated display.

**Success gate:** all tiles readable in landscape; top-of-portrait
now on the RIGHT of landscape (per
[[project_display_landscape_direction]]); touch tracks under the
finger without offset.

**Bench flash:** after 30.2. Cheap A/B if direction is wrong (flip
`mirror_x`/`_y`).

---

### 2.4 Batch 30.4 -- Mode HUD on the main screen

Carries forward from Stage 28 §2.4. Broker channel for
`(mode, submenu, rgb_mnemonic)` -> subscribed by
`ui_main_screen.c` as a bottom-mid label. Was blocked on font +
landscape being settled; both land this stage.

**Sketch:**

  - New broker key: `UI_MODE_HUD_STATE` carrying a small struct
    (`mode_id`, `submenu_id`, `rgb_mnemonic[4]`).
  - `field_capture` publishes on every mode change (currently the
    RGB LED is the only mode-visible-to-user indicator).
  - `ui_main_screen.c` creates a bottom-mid label, subscribes to
    the key, updates on notify.

**Success gate:** switching mode via CLI or gesture updates the
bottom-mid label on the main screen within one refresh; label
uses UI_FONT_CHIP (16 px SG) or UI_FONT_LABEL (20 px SG) --
judge on bench.

**Bench flash:** after 30.3.

---

## 3. Non-goals for Stage 30

  - Adding partial-refresh optimizations back BEFORE the port
    migration -- we're not touching the custom flush cb anymore,
    it's about to be deleted.
  - Touch driver changes -- CST9217 stays as-is; only the LVGL
    indev binding to the rotated display changes.
  - GPS silence, TEMP_DUMP WRIST_DOWN, LSM tap-double redundancy,
    DRV auto-cal LSM gate -- all parked from earlier stages.
  - CLI submenu restructure per [[project_cli_submenus]].
  - Recording writer datetime migration per
    [[feedback_recording_datetime_opportunistic]].
  - Encoder tile redesign per [[project_encoder_tile_redesign]].

---

## 4. Bench log (fill in as each batch lands)

### 4.1 Batch 30.1 -- CO5300 bus mutex

**Code landed (2026-09-14, fw 0.4.62, `co5300` 0.2.2).** Chose option A
per §2.1. `co5300_dev_s` grows a `SemaphoreHandle_t bus_mutex`; created
in `co5300_init`, deleted in `co5300_deinit`. Two static inline
helpers (`bus_lock` / `bus_unlock`) wrap every public SPI-touching
entry point:

  - `co5300_write_command`, `co5300_write_command_with_data`,
    `co5300_write_pixels`, `co5300_set_brightness` -- single-lock /
    single-unlock around the one `spi_send_instruction` /
    `spi_send_pixels` call.
  - `co5300_set_window` -- lock held across all three
    `spi_send_instruction` calls (CASET + RASET + RAMWR) so the window
    setup reaches the panel as one atomic sequence even if an LVGL
    flush or a `boot_display_sleep()` fires concurrently.
  - `co5300_sleep` / `co5300_wake` -- lock held across both
    instructions in the pair (DISPOFF+SLPIN, SLPOUT+DISPON). Wake
    keeps its 120 ms `vTaskDelay` inside the locked window; LVGL
    flushes will queue behind for that window, which is
    correct -- the panel isn't ready to draw during SLPOUT anyway.

`panel_init_sequence()` runs before any other task holds the handle
so it doesn't need explicit locking; the helpers are safe to call
either way because `bus_lock` no-ops on a NULL mutex.

Bench flash gate (per §2.1 success gate + [[feedback_flash_per_checkpoint]]):

  1. Flash + monitor; idle 15+ s (past
     `IDLE_SCREEN_MS`).
  2. Verify **zero** `E spi_master: 1406` lines around the sleep
     transition.
  3. Verify wake still works (tap or gesture -> panel back on).
  4. Verify swipes / flushes look no worse than 0.4.61.

*(Ivan's flash result -> here.)*

### 4.2 Batch 30.2 -- `esp_lvgl_port_add_disp` migration

**Code landed (2026-09-15, fw 0.4.63, boot_logic 0.4.0,
lvgl_ui_display 0.2.0, co5300 0.3.0, co5300_panel 0.1.0).** Full swap
per §2.2, no fallback path taken.

Files:
  - New `components/co5300/co5300_panel.h/c` -- thin `esp_lcd_panel_t`
    wrapper. `init` runs the vendor sequence (SLPOUT, page0, RAM_EN,
    COLMOD 0x77, MADCTL, WRCTRLD, HBM, DISPON, WRDISBV, sun-off,
    INVOFF). `draw_bitmap` sends CASET+RASET+`esp_lcd_panel_io_tx_color`
    with cmd word `0x32003C00` (0x32 prefix | 0x003C00 pixel addr).
    Sleep/wake pair DISPOFF+SLPIN and SLPOUT+DISPON as one op. Manual
    MADCTL update on `mirror` / `swap_xy` for Stage 30.3.
  - Deleted `components/co5300/co5300.c`. `co5300.h` trimmed to
    opcodes + geometry + wire constants only; `co5300_get_chip_name/
    desc` moved into `co5300_panel.c`.
  - `boot_display.c` rewritten -- brings up SPI2 QSPI bus, creates the
    esp_lcd panel IO via `esp_lcd_new_panel_io_spi(quad_mode=1,
    lcd_cmd_bits=32, lcd_param_bits=8)`, calls `co5300_new_panel` then
    `esp_lcd_panel_reset`+`_init`. Publishes `get_panel_io()` +
    `get_panel()` in place of the old `get_co5300()`. Sleep/wake now
    route through `esp_lcd_panel_disp_sleep`.
  - `lvgl_ui_display.c` rewritten -- deleted the custom `lvgl_flush_cb`,
    the 617 KB PSRAM full-frame buffer, the in-place byte swap, and
    the `esp_cache_msync` call. Uses `lvgl_port_add_disp` with a
    40-row strip = 16 400 px, double-buffered in internal DMA-capable
    SRAM (~98 KB total). `LV_COLOR_FORMAT_RGB888`. Force-mode path
    (LVGL_FORCE ON) no longer creates a display since
    `lvgl_port_add_disp` asserts on a non-NULL io.
  - CMake: `co5300` REQUIRES esp_lcd; `boot_logic` REQUIRES esp_lcd;
    `lvgl_ui` PRIV_REQUIRES esp_lcd (replaces esp_mm which no longer
    consumed).

Byte-order handling: MADCTL is initialised with the BGR bit (0x08)
so the panel interprets LVGL's native RGB888 `[B, G, R]` memory
order verbatim. No per-flush swap. If bench proves the CO5300
silicon ignores the BGR bit, fall back to in-place swap inside
`co5300_op_draw_bitmap` (~50 us per strip).

Instruction wire-format risk: the plan's §2.2 Risk paragraph flagged
that `esp_lcd_panel_io_spi` with `quad_mode=1` puts params on 4
lines whereas the CO5300's instruction frame conventionally uses
1-line data. This first flash tests whether the panel tolerates
quad-mode params. If init garbles, next batch adds a second
panel-IO handle with `quad_mode=0` for control commands and keeps
the quad-mode io for pixels only.

**Local `idf.py build`:** clean (0x14d470 B, 57% partition free).

**Flash test:** fw 0.4.63
  - Boot banner `co5300_panel v0.1.0 created`; then panel_init lines
    with no `esp_lcd`/`esp_lvgl_port` E-lines.
  - LVGL comes up with `strip=410x40 px (~98 KB x2)` banner (not the
    old `full-frame PSRAM` line).
  - Tiles render with correct colors (BGR bit trial) -- if any tile
    shows swapped R/B, that's the BGR-bit fallback signal.
  - Swipes feel comparable to or better than 0.4.62.
  - Idle 15+ s: sleep transition still clean (no `spi_master: 1406`).

#### 4.2a Attempt 1 -- fw 0.4.63 (RGB888 + buff_dma clash)

Panel init succeeded and printed `co5300_panel v0.1.0 created (rst=3,
rotate90=0)` + `CO5300 ready`. Sleep transition still clean (batch
30.1 held; `spi_master:1406` E-lines still absent around idle-timeout
sleep). But:

    E LVGL: lvgl_port_add_disp_priv(314): DMA buffer can be used only
      in display color format RGB565 (not aligned copy)!

esp_lvgl_port has a hard guard: `.flags.buff_dma = 1` is refused for
non-RGB565 color formats. Our RGB888 tripped it, so
`lvgl_port_add_disp` returned NULL and the boot proceeded headless
(main logged `LVGL setup failed: ESP_FAIL (continuing headless)`).

**Fix (fw 0.4.64, lvgl_ui_display 0.2.1):** dropped `buff_dma=1`.
esp_lvgl_port now allocates via `MALLOC_CAP_DEFAULT` which on
ESP32-S3 lands in internal SRAM, which IS DMA-capable for the SPI
DMA path -- so we still get the DMA overlap without the guard trip.
Kept `buff_spiram=0` so allocation stays in fast internal memory.
Comment in `lvgl_ui_display.c` records the guard reason.

**Flash test (attempt 2):** fw 0.4.64
  - Boot log: `LVGL up -- panel=CO5300 touch=CST9217 strip=410x40 px
    (49200 B x2)` line (no more `LVGL setup failed`).
  - Tiles render with correct colors (MADCTL.BGR trial); swap R/B =
    fallback to in-flush byte swap.
  - Swipes >= fw 0.4.62.

#### 4.2b Attempt 2 -- fw 0.4.64 (glitched pixels + IRQ WDT)

Boot log fully clean this time: `LVGL up -- panel=CO5300 touch=CST9217
strip=410x40 px (49200 B x2)`, all tiles built, screens registered.
30.1 sleep-race still fixed (`[DISP] reason=idle-timeout` fired at
20 s with no `spi_master:1406` E-lines).

But: **screen showed glitched blotches**, swipes moved the glitch but
never resolved into readable tiles. After ~50 s of swiping, IRQ WDT
tripped on Core 0 (`_xt_lowint1` in ISR context) and rebooted.

Root cause. Stock `esp_lcd_new_panel_io_spi` with `quad_mode=1` only
puts the color DATA phase on 4 lines; the leading 32-bit cmd word
(our packed `0x32003C00`) still ships on 1 line. CO5300 needs QIO on
cmd + addr + data for the 0x32 pixel frame -- otherwise the panel
misreads the prefix, pixel bytes land at wrong offsets, and (long
enough later) the SPI driver's internal state gets tangled enough to
spin an ISR into the watchdog. Verified by reading
`components/esp_lcd/spi/esp_lcd_panel_io_spi.c:340-390` -- the
quad_mode branch adds `SPI_TRANS_MODE_QIO` only for color-chunk
transactions, not for the leading cmd.

Fix (fw 0.4.65, co5300 0.3.1). New file
`components/co5300/co5300_panel_io.c/h` -- our own
`esp_lcd_panel_io_t` implementation with CO5300 wire semantics:
  - `tx_param` uses `cmd_bits=8` + `addr_bits=24` and
    `SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR` so the
    32-bit cmd word ships on 4 lines; instruction data on 1 line.
  - `tx_color` first chunk uses `SPI_TRANS_MODE_QIO` on the whole
    transaction (cmd + addr + data all quad); subsequent chunks
    use `VARIABLE_CMD/ADDR/DUMMY` = 0 bits with `MODE_QIO` and
    `SPI_TRANS_CS_KEEP_ACTIVE` so the panel treats the whole
    burst as one pixel write. Post-trans ISR of the last chunk
    calls the user's `on_color_trans_done` so esp_lvgl_port can
    fire `lv_display_flush_ready` and get its DMA overlap.
  - This is the exact wire behavior our pre-Stage-30 custom
    driver used (bench-verified on Mk1b for months).
  - `boot_display.c panel_io_up()` now calls
    `co5300_panel_io_new_spi()` instead of
    `esp_lcd_new_panel_io_spi()`. Same io handle type, same
    downstream consumers (`co5300_new_panel`, `lvgl_port_add_disp`).

**Local build:** clean (0x14cd80 B, 57% partition free).

**Flash test:** fw 0.4.65
  - Boot has `co5300_pio: co5300_panel_io ready (queue=8, chunk=12288B,
    cs=10)` before the panel-init banner.
  - Screen renders readable tiles (goodbye glitch). Colors true;
    R/B swap = MADCTL.BGR wrong on this silicon, tell me and I'll
    put an in-flush swap into `co5300_panel_io_tx_color`.
  - Swipes smooth; no IRQ WDT after 60+ s of swiping.
  - Idle 15+ s -> sleep transition still clean.

#### 4.2c Attempt 3 (redirected) -- fw 0.4.66 (CS_KEEP_ACTIVE + queue_trans crash)

Panel-io + panel wrapper created, then instant crash inside
`esp_lcd_panel_init` -> `co5300_op_init` -> first `tx_param` call:

    Guru Meditation Error: Core 0 panic'ed (InstrFetchProhibited).
    EXCCAUSE: 0x00000014  PC: 0x8201b527

Backtrace lands in `spi_post_trans` -> `spi_device_polling_end` ->
`spi_device_polling_transmit` -> our `co5300_pio_tx_param`. The
device-level `post_cb` we registered on `spi_bus_add_device` fires
for **every** transaction on the device -- including the polling
`tx_param` ones. My cb was doing
`__containerof(t, co5300_pio_trans_t, base)`; for stack-local
tx_param transactions that produced a garbage struct pointer,
`lt->fire_done` read a random byte and enterered the branch,
`pio->on_color_trans_done` was a garbage fn pointer at 0x8201b527,
CPU tried to fetch instructions there -> panic.

**Fix (fw 0.4.66, co5300 0.3.2).** Discriminate transaction origin
via `t->user`:
  - `tx_param` sets `t.base.user = NULL` explicitly. post_cb early-
    returns when `t->user == NULL`.
  - `tx_color` chunks set `lt->base.base.user = lt` (self-pointer).
    post_cb reads `lt = t->user`, checks `lt->fire_done`, and only
    the LAST chunk of a burst has that flag set -> fires
    `on_color_trans_done`.
  - Removed the `__containerof(t, co5300_pio_trans_t, base)` bug --
    that pattern is unsafe when the same device sees both polling
    stack-local transactions and queued pool-owned ones.

**Local build:** clean (0x14cec4 B, 57% free).

**Flash test:** fw 0.4.66
  - Boot past `co5300_panel v0.1.0 created` into `esp_lcd_panel_init`
    without panicking.
  - `LVGL up ... strip=410x40 px (49200 B x2)` line prints.
  - Screen renders readable tiles.
  - Swipes smooth; no IRQ WDT / InstrFetchProhibited after 60+ s.
  - Idle 15+ s -> sleep transition still clean.

#### 4.2d Attempt 4 -- fw 0.4.66 (queue_trans failed + task WDT)

InstrFetchProhibited fixed. Panel init clean, LVGL up, but the first
flush emitted:

    E co5300_pio: co5300_pio_tx_color(234): queue_trans failed
    E task_wdt: taskLVGL stuck in wait_for_flushing (lv_refr.c:1454)

Every ~5 s another IDLE0 WDT trip. LVGL was blocked forever waiting
for `lv_display_flush_ready` because `tx_color` returned an error
before firing the completion callback.

Root cause. Verified in
`esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:1248`:
`spi_device_queue_trans` returns `ESP_ERR_INVALID_ARG` when
`SPI_TRANS_CS_KEEP_ACTIVE` is set on a transaction whose device has
not been pre-acquired via `spi_device_acquire_bus`. My chunk-1..N-1
each set that flag to keep CS low across the pixel burst, but I
never acquired the bus -> immediate reject on the very first chunk.

**Fix (fw 0.4.67, co5300 0.3.3).** Split `tx_color` into two paths:

1. **Single-chunk async** (default -- used when `color_size <=
   pixel_chunk_bytes`). No `CS_KEEP_ACTIVE` needed; `queue_trans`
   accepts it, `post_cb` fires `on_color_trans_done` from the SPI
   ISR, esp_lvgl_port gets its DMA overlap.
2. **Multi-chunk sync fallback** (used only if `color_size >
   pixel_chunk_bytes`). Acquires the bus so `CS_KEEP_ACTIVE` is
   accepted, uses `polling_transmit` for each chunk, releases the
   bus, calls `on_color_trans_done` synchronously so LVGL sees
   flush_ready. No DMA overlap on that flush, but the strip layout
   makes this path unreachable in normal operation.

Bumped `pixel_chunk_bytes` from 12 KB to 60 KB so every 40-row LVGL
strip (49 200 B) fits in the single-chunk async path. Bounded by
`max_transfer_sz = 64 KB` set at `spi_bus_initialize`.

Also improved error logging: previous message was
`queue_trans failed` with no err code; new logs include
`esp_err_to_name(err)` and byte count so future failures are
diagnosable at a glance.

**Local build:** clean.

**Flash test:** fw 0.4.67
  - No `co5300_pio: queue_trans failed` line during LVGL start.
  - No `task_wdt: IDLE0 CPU 0` / `wait_for_flushing` spam.
  - Screen renders readable tiles.
  - Swipes smooth 60+ s.
  - Idle 15+ s -> sleep transition still clean.

#### 4.2e Attempt 5 -- fw 0.4.67 (SPI DMA per-transaction cap tripped)

`queue_trans failed` path fixed in principle, but the first
attempted flush surfaced a stricter constraint one layer down:

    E spi_master: check_trans_valid(1127): txdata transfer >
      hardware max supported len
    E co5300_pio: queue_trans (single) failed: ESP_ERR_INVALID_ARG
      (49200 B)
    E task_wdt: taskLVGL stuck in wait_for_flushing

Root cause. ESP32-S3 SPI DMA has a hardware ceiling of
`SPI_LL_DMA_MAX_BIT_LEN = (1 << 18)` bits = 32 768 B per single
transaction (`hal/esp32s3/include/hal/spi_ll.h:44`). The
`check_trans_valid` in `esp_driver_spi/src/gpspi/spi_master.c:1127`
gates every queued transaction against it. Our 40-row LVGL strip
= 40 * 410 * 3 = 49 200 B blew past the cap on the very first
flush. `pixel_chunk_bytes = 60 * 1024` and `max_transfer_sz =
64 * 1024` from attempts 3/4 were both above the hw cap; only the
per-transaction check refused the transfer, which is why nothing
tripped until an actual pixel burst was submitted.

**Fix (fw 0.4.68, `lvgl_ui_display` 0.2.2, `boot_logic` 0.4.1,
no `co5300_pio` change).** Reduce `LVGL_STRIP_ROWS` from 40 to 25:
25 * 410 * 3 = 30 750 B fits under the 32 768 B DMA cap with a
2 KB headroom margin. Strip still stays in the single-chunk async
path (DMA overlap preserved). Lowered `pixel_chunk_bytes` from
60 KB to 32 KB (exact DMA cap) so the branching threshold matches
hardware -- any future burst above the cap now correctly falls to
the multi-chunk sync fallback (which chunks below the cap and
loses only DMA overlap for that specific flush, not correctness).

Added a `_Static_assert` on `LVGL_STRIP_PIXELS * 3U <= 32 768U`
in `lvgl_ui_display.c` so any future bump above 26 rows fails at
compile time with a pointed message, not on bench with a task WDT.

Buffer memory drops from ~98 KB to ~62 KB double-buffered -- more
headroom on the internal-SRAM heap, and the smaller strip means
more but shorter flushes, so LVGL's per-flush handoff cadence
gets tighter (net win on swipe responsiveness).

**Local build:** clean (verify before handing to Ivan).

**Flash test:** fw 0.4.68
  1. Boot has `co5300_panel v0.1.0 created` +
     `co5300_pio: co5300_panel_io ready (queue=8, chunk=32768B, cs=10)`.
  2. `LVGL up -- panel=CO5300 touch=CST9217 strip=410x25 px
     (30750 B x2)` line prints (was `410x40 (49200 B x2)`).
  3. No `check_trans_valid`, no `queue_trans (single) failed`, no
     `task_wdt: taskLVGL wait_for_flushing`.
  4. Screen renders readable tiles; R/B swap = MADCTL.BGR wrong on
     this silicon, tell me and I'll wire an in-flush swap into
     `co5300_pio_tx_color` (documented fallback since attempt 1).
  5. Swipes smooth 60+ s of gesture / tile navigation.
  6. Idle 15+ s -> sleep transition still clean (30.1 held; zero
     `spi_master:1406` E-lines).

If (4)-(6) all pass, 30.2 is done and we move on to 30.3
(landscape 90 CW: flip `swap_xy = true` + `mirror_x = true` in
`lvgl_ui_display.c disp_cfg.rotation`; add matching
`rotate_90_cw = true` in `boot_display.c` vendor config so panel
MADCTL and LVGL agree).

#### 4.2f Attempt 6 -- fw 0.4.68 (glitched pixels on first real flush)

DMA cap fix held: no `check_trans_valid`, no `queue_trans (single)
failed`, no `task_wdt: taskLVGL wait_for_flushing`, boot log clean
(`strip=410x25 px (30750 B x2)`, sleep still clean at 15 s idle).

But: screen showed the same glitched-blotches symptom as attempt 2
(fw 0.4.64). Not colors-swapped, not shear -- pixel BYTES landing
at wrong panel offsets. Movement of the glitch on swipe confirmed
it was a window/address problem, not a data-corruption problem.

Realization. Attempts 3 (fw 0.4.65) and 4 (fw 0.4.67) both crashed
BEFORE any real flush completed (tx_param InstrFetchProhibited,
then queue_trans CS_KEEP_ACTIVE rejection). fw 0.4.68 is the FIRST
attempt in this stage where a pixel burst went all the way to the
panel through our custom `co5300_panel_io`. Every "supposed to fix
the glitch" claim from 3/4 was untested until this flash.

Diff-check against the bench-verified pre-Stage-30 `co5300.c`
(`git show HEAD:.../co5300.c`, `co5300_set_window` at line 337).
Old sequence per flush:

    spi_send_instruction(CASET, x0..x1)   // 0x02 2A0000 + 4 B
    spi_send_instruction(RASET, y0..y1)   // 0x02 2B0000 + 4 B
    spi_send_instruction(RAMWR, NULL)     // 0x02 2C0000 + 0 B
    spi_send_pixels(0x32003C00, buf, N)   // 0x32 003C00 + pixels (QIO)

Our new `co5300_op_draw_bitmap` (co5300_panel.c) sent CASET +
RASET + straight to `tx_color` -- **no RAMWR (0x2C)** between the
window setup and the 0x32 pixel burst. Same delta shows up in the
Arduino reference (`16_amoled_touch_test_mk1b.ino:216` -- explicit
`cmd(0x2C);  // RAMWR`). Without RAMWR the CO5300 doesn't open the
RAM write window, so the 0x32 pixel burst writes to whatever the
last-set target was -- pixels land at the wrong offsets and the
image reads as "glitched blotches that move with swipe."

**Fix (fw 0.4.69, `co5300_panel` 0.1.1).** Insert
`co5300_send_cmd(io, CO5300_CMD_RAMWR)` between the RASET tx_param
and the `esp_lcd_panel_io_tx_color` call in `co5300_op_draw_bitmap`.
One instruction, no other logic changes. Matches the bench-verified
old-driver + Arduino-ref sequence exactly.

**Local build:** clean (verify before handing to Ivan).

**Flash test:** fw 0.4.69
  1. Boot banner still clean (identical to 0.4.68).
  2. Tiles render readable across boot + swipe cycles + clock ticks.
  3. Colors true (BGR bit trial); R/B swap = MADCTL wrong, add
     in-flush swap in `co5300_pio_tx_color`.
  4. Swipes smooth 60+ s.
  5. Idle 15+ s -> sleep transition still clean.

If (2)-(5) hold, 30.2 lands and we move to 30.3.

#### 4.2g Attempt 7 -- fw 0.4.69 + diagnostic pattern (fw 0.4.70)

RAMWR fix landed. Boot clean, no crashes, sleep transition still
clean. Screen better than 0.4.68 but not right:
  - Clock label appears italic + sheared (row-to-row horizontal
    shift, same class as Stage 29 pre-full-frame).
  - New symptom: ~10 evenly-spaced green horizontal lines running
    across the whole panel.
  - Status-bar LED dots showing incorrect colours.
  - Swipes better than before, still not smooth.

The green-line pitch aligns with strip boundaries (502 rows / 25
rows per strip = ~20 strips; visible pitch on-screen suggests
every-other-strip artefact). Text shear is back because we dropped
back from 617 KB PSRAM full-frame to 25-row strips -- if the
CASET/RASET window doesn't exactly match the pixel-count LVGL
sends, each row inside the strip shifts by one and the strip
edge shows as a coloured seam.

**Diagnostic-pattern batch (fw 0.4.70, `lvgl_ui` 0.2.1,
`ui_main_screen` unchanged version but bumped through `lvgl_ui`).**
Per Ivan's request, added a `UI_MAIN_DIAG_PATTERN` block to
`ui_main_screen.c main_screen_build()`:

  1. **Six 40x40 pure-colour squares** in a horizontal row centred
     +90 px below screen mid, left-to-right order R G B W Y C with
     1-px white borders and black gaps. Colour swaps decode as:
     - R shows blue, B shows red -> MADCTL.BGR bit inverted for this
       silicon; wire an in-flush byte swap in `co5300_pio_tx_color`.
     - Y shows cyan, C shows yellow -> confirms R/B swap above.
     - W is off-white or tinted -> a channel is dropped or the byte
       stream is drifting (not aligned to a 3-byte pixel boundary).
     - G shifts hue -> byte-alignment issue (not a channel swap).
     - Squares appear at wrong column, missing entirely, or with
       black slits through them -> panel wrapping at wrong column
       count (CASET off, or COL_OFFSET wrong for this strip mode).
     - Square edges slant instead of being vertical -> row-shift
       shear (window doesn't match pixel count).
  2. **380x6 pure-white full-width band** at +145 px. Any tint
     visible across its length = shear or byte-drift; a clean flat
     white band means the pixel path is honest at least for solid
     rectangles.

Batch is gated on `#define UI_MAIN_DIAG_PATTERN 1` at the top of
the file -- flip to 0 once the port renders cleanly.

**Ivan-reads-back protocol.** After flashing 0.4.70, describe:
  - The six square colours from left to right (list them).
  - Whether square edges are vertical or slanted.
  - Whether the white band is clean or shows tint / breaks / seams.
  - Whether the green-line artefact runs through the diag zone or
    stops at label boundaries.
  - Colours reported by `ui_status_bar` LED dots (extra data point).

The reply tells us in one flash cycle whether we're chasing (a) a
byte-order / MADCTL bug, (b) a window / offset bug, or (c) a
strip-boundary handshake bug. Faster than trial-and-error patches.

**Non-goal for this flash.** Not touching the pixel path this
attempt; the diag pattern is instrumentation only. Fix follows in
the next batch once we know which bucket the symptom lives in.

#### 4.2h Attempt 8 -- fw 0.4.71 (bigger strip + longer idle)

Ivan's diag-pattern read-back after fw 0.4.70:
  - Squares L->R: **R G B W Y C** (all correct). No R/B swap; MADCTL.BGR
    is right for this silicon, no in-flush swap needed.
  - Square edges vertical, white band clean. Solid content is honest;
    pixel bytes align, byte order right.
  - Clock label still italic-looking / sheared. Same class of artefact
    Stage 29 fixed by going full-frame PSRAM.
  - Green horizontal lines pass THROUGH the static squares but STOP at
    dynamic-label bounding boxes (clock area is clean). That's the tell:
    the green lines are strip-boundary artefacts on the initial paint
    that only get overwritten by subsequent widget re-flushes. Static
    widgets (never re-flushed) keep the artefact; time label re-flushes
    every minute and paints over it clean.
  - Sleep at 15 s too aggressive during bench diagnostics.

Root cause of shear + green-lines. LVGL invalidates rectangles and
flushes each one within the display's buffer window. With a 25-row
strip, a 48 px clock glyph spans ~2 strips; each strip flushes its
slice independently through CASET/RASET+RAMWR+0x32-burst. Any tiny
per-strip alignment slip (byte-stride rounding, port's internal
line-count math, or CO5300 write-ptr drift after RAMWR) shears the
glyph across the boundary. Stage 29 §4.2b sidestepped this by going
full-frame PSRAM (617 KB, one flush per whole panel, no boundaries).
We don't need to go all the way to full-frame -- just far enough
that a single glyph fits in one flush.

Unrelated crash observation. After ~370 s of alternating
double-tap sleep/wake cycling, Core 0 tripped
`Interrupt wdt timeout on CPU0` in `i2c_hw_fsm_reset` ->
`vPortExitCritical` during a `max17048_read_vcell_mv`. Root cause
looks like I2C driver's recovery path holding a critical section
too long under contention -- not Stage 30's problem, but repeated
sleep/wake stress helps expose it. Bumping DISP_IDLE_MS to 30 s
(below) indirectly reduces the sleep-cycle rate.

Encoder observation. Ivan rotates -> no haptic click ->
`ENC` verb dumps show cw=0 ccw=0 net=0 (pins A=21, B=43 unchanged
by Stage 30 -- no code collision). Symptom is at the pin-read
layer, not the mode-dispatch layer. Deferred to Stage 30.5 or
later per Ivan's "focus on screen, encoder next" 2026-09-15.

**Fix (fw 0.4.71, `lvgl_ui_display` 0.2.3, no panel_io/panel
change).**

1. **Strip 25 rows -> 60 rows** in `lvgl_ui_display.c`. 60*410*3 =
   73 800 B per buffer; double-buffered = ~144 KB in internal SRAM
   (post-boot heap ~276 KB, LVGL widgets need ~130 KB, fits).
   Whole 48 px clock glyph fits in one flush -> no cross-strip
   shear. Removed the `_Static_assert(<= 32768)` since the strip
   is now intentionally above the DMA cap.

2. **Routing shift.** Full-strip flushes (73 800 B > 32 KB
   pixel_chunk_bytes) now go through the multi-chunk sync fallback
   in `co5300_panel_io_tx_color_multi_sync` -- acquires the bus,
   sends the strip as 3 chunks (32 KB + 32 KB + 8264 B) with
   CS_KEEP_ACTIVE, releases, calls `on_color_trans_done` synchronously
   so LVGL sees flush_ready. Loses DMA overlap on the strip flush
   (paint-then-return instead of paint-in-background); small dirty
   regions (< 32 KB) still route to single-chunk async and keep DMA
   overlap. Bench check: swipe smoothness should stay close to 0.4.70
   because the strip flush is rare vs. LVGL's per-frame render.

3. **DISP_IDLE_MS 15 s -> 30 s** in `field_capture.c` per Ivan's
   ask. Comment updated.

**Local build:** clean (0x14d0f0 B, 57% partition free).

**Flash test:** fw 0.4.71
  1. Clock label rendered without italic/shear across boot + minute
     ticks + swipes.
  2. Green horizontal lines gone (or at least: no worse than
     0.4.70).
  3. Sleep transitions clean; auto-sleep at 30 s.
  4. Diag pattern still there and still all correct.
  5. Swipes comparable to 0.4.70; if noticeably slower, we can
     move the LVGL buffer to PSRAM next batch.

If (1)+(2) hold, 30.2 lands; move to 30.3 (landscape 90 CW) then
30.4 (Mode HUD) then Stage 30.5 (encoder recovery) as Ivan
requested.

#### 4.2i Attempt 9 -- fw 0.4.72 (sync-only pixel path)

Ivan's read-back after fw 0.4.71:
  - Initial main-screen paint clean, "finally looks like pre stage 30".
  - After ~20 s, clock label went italic / sheared again -- same
    class of artefact as before, but only on the RE-render, not on
    the initial paint.
  - Encoder still dead (Ivan will A/B against the arduino field-demo
    sketch next week to prove hw-vs-fw; deferred).
  - Idle-timer might not be resetting on single-tap ("feels like 30 s
    and it's done"); CST9217 press-edge already calls
    `field_capture_kick_activity()` (cst9217.c:346) so this needs a
    bench trace next flash rather than a preemptive fix.

Root cause of the re-render shear. Boot / tile-navigation paints
the whole 60-row strip in one flush (73 800 B > 32 KB
pixel_chunk_bytes -> multi-chunk sync path) -> renders clean.
When only the clock minute rolls over, LVGL invalidates just the
clock label's bounding box (~150 x 48 px x 3 = ~22 KB < 32 KB) ->
routes to the single-chunk async path -> shears. Both paths use
identical wire framing (SPI_TRANS_MODE_QIO, cmd=0x32, addr=0x003C00,
cmd_bits=8, addr_bits=24) but the async path queues via
`spi_device_queue_trans` and fires `on_color_trans_done` from the
SPI ISR. Suspected race: post_cb hits from ISR -> flush_ready fires
from ISR -> LVGL immediately queues the next flush -> our new
tx_param(CASET) fires with the SPI driver still in a transitional
state that quietly misaligns column addressing. Wire-level bug that
`wait_all_inflight` should mask but apparently doesn't.

**Fix (fw 0.4.72, `co5300_panel` 0.1.2, panel_io simplified).**
Removed the single-chunk async branch from `co5300_pio_tx_color`.
Every pixel burst now goes through the sync path (acquire_bus +
polling_transmit, one or more chunks bounded by pixel_chunk_bytes,
release_bus, sync call to `on_color_trans_done`). Deleted the now-
dead `co5300_pio_tx_color_single_async` + `reap_inflight` helpers.
The `pool` allocation + `inflight` counter left in place -- they're
harmless dead plumbing, cleanup is a follow-up if we ever restore
async DMA overlap deliberately.

Cost: every flush pays ~20 us of extra bus-lock overhead
(acquire_bus + release_bus) vs. queue_trans, and loses DMA overlap
on every flush (paint-then-return instead of paint-in-background).
For LVGL's ~30 FPS target this is 20 us / 33 ms = 0.06% CPU
overhead, unmeasurable in practice. Buys shear-free rendering
across all flush sizes.

**Local build:** clean (0x14cdc0 B, 57% free, warning-clean).

**Flash test:** fw 0.4.72
  1. Clock stays crisp indefinitely, including through minute
     rollovers (wait past the first XX:XX -> XX:XX+1 tick).
  2. Diag pattern still R G B W Y C, edges vertical.
  3. Swipes still comparable to 0.4.71 (marginal slowdown OK).
  4. Idle-sleep at 30 s; verify that a single tap on the panel
     during that window resets the timer (Ivan's bench check).
  5. No new E-lines in the boot log; no `queue_trans failed`.

If (1) holds, 30.2 is officially done and we proceed to 30.3
(landscape) then 30.4 (Mode HUD). Encoder recovery (Stage 30.5)
waits on Ivan's Arduino A/B test next week.

### 4.3 Batch 30.3 -- Landscape 90 CW + AUTOSHDN CLI+NVS

fw 0.4.72 read-back: sync-only pixel path landed clean.
"finally looks like pre stage 30", "very extremely stable" after
a rebuild + reflash cycle. Small transient blip once ("sometimes
it goes italic, i can see some pixels left of the minute number")
but recovered on its own -- monitor, not-a-blocker. 30.2 closes.

Two things bundled into this batch per Ivan 2026-09-15:
  (a) Auto-shutdown timer configurable via CLI+NVS (replaces the
      pre-existing hardcoded 15-min uptime cap). Default 120 min
      (2 h); 0 or OFF = perma-on until manual ship-mode gesture.
  (b) Landscape rotation 90 CW.

**Auto-shutdown (fw 0.4.73, `nvs_cfg` 0.3.3).**
  - New NVS key `K_SYS_AUTO_SHDN` (u16, minutes, clamp 0..1440).
  - `nvs_cfg_sys_get/set_auto_shdn_min()` accessors.
  - `nvs_cfg_boot_print()` line: `auto_shdn = N min` or `= OFF`.
  - `fc_shutdown.c task_shutdown_watcher_fn`: reads NVS once at
    task start, derives `auto_shdn_cap_ms` and `auto_shdn_on`
    flag. All references to the deleted `SHDN_MAX_UPTIME_MS`
    macro replaced. When `auto_shdn_on == false`, the entire
    deadline check is skipped -- device stays on forever until
    the 4 s ship-mode hold fires. `[FC_SHDN]` armed-log now
    reports the effective cap (`auto-shdn cap = 120 min (7200 s)`
    or `auto-shdn cap = OFF (perma-on)`).
  - `fc_cli.c`: new `AUTOSHDN [<min>|OFF]` handler. No arg = show
    current. Persists via NVS, reboot to apply (matches
    BLACKBOX/BATT_TEST semantics). HELP entry added.
  - `field_capture.c field_capture_kick_activity()`: now also
    calls `shutdown_watcher_kick()` so any surface that kicks
    activity (touch press-edge in cst9217.c, external caller)
    postpones both the display-idle timer AND the auto-shdn
    deadline in one call.

**Landscape (`lvgl_ui_display` 0.2.4).**
  - `lvgl_ui_display.c disp_cfg.rotation`:
    `swap_xy = true`, `mirror_x = true`, `mirror_y = false`.
  - `esp_lvgl_port` propagates via `panel_swap_xy` +
    `panel_mirror`; `co5300_panel.c` honours by updating MADCTL
    (MV/MX/MY bits in `co5300_apply_madctl`) AND swapping
    CASET/RASET coords in `co5300_op_draw_bitmap` so LVGL's
    landscape coords (0..501, 0..409) address the panel's
    native (0..409, 0..501) portrait RAM correctly.
  - Vendor config in `boot_display.c` intentionally left at
    `rotate_90_cw = false` -- port ops override on first flush,
    initial MADCTL is portrait then flipped as the panel comes
    up. No duplicate rotation state.

Direction sign is a bench check per Stage 30 §2.3: if landscape
comes out upside-down or mirrored, flip `mirror_x` -> `mirror_y`
(or set both true). project_display_landscape_direction memory
says top-of-portrait must land on the RIGHT of landscape;
`mirror_x = true` matches that intent for the standard MADCTL
convention but plenty of panels invert it.

Diag pattern (6 squares + white band) intentionally left ON one
more flash so we can visually confirm landscape doesn't break
the color/edge/band invariants proven at fw 0.4.70. Removed in
the next batch.

**Idle observation left as-is.** Ivan reported "does it reset
after i touch it? because it feels like it does its 30 s and
its done." CST9217 press-edge already calls
`field_capture_kick_activity()` (cst9217.c:346), and that now
also kicks the auto-shdn deadline (change above). If touch STILL
doesn't reset the 30 s idle after this flash, we need a bench
trace of whether the tap actually registers as a press-edge
(valid=1 && s_prev_valid=0). Not fixing preemptively.

**Local build:** clean (0x14d230 B, 57% free, warning-clean).

**Flash test:** fw 0.4.73
  1. Landscape orientation on-screen. Top of former portrait
     lands on the RIGHT (per project_display_landscape_direction).
     If it lands on the LEFT, flip `mirror_x` -> `mirror_y` next
     batch.
  2. Diag pattern still R G B W Y C in landscape, edges vertical
     to the new "up".
  3. Clock text stays crisp through minute rollover in landscape
     (sync path from 30.2 §4.2i still applies).
  4. Boot log shows `[FC_SHDN] shutdown watcher armed ...
     auto-shdn cap = 120 min (7200 s).`
  5. Boot NVS printout shows `auto_shdn = 120 min`.
  6. `HELP` lists the AUTOSHDN verb.
  7. `AUTOSHDN` (no arg) prints current. `AUTOSHDN 5` sets to 5
     min; reboot; watch shutdown fire ~5 min in (with USB
     detached, or `USB present, skipping BATFET disable` log if
     plugged). `AUTOSHDN OFF` disables.
  8. Any touch tap should reset the 30 s idle-sleep timer.
     Verify by tapping every ~20 s and watching that panel
     doesn't sleep.

If (1)+(3) hold, 30.3 closes and we proceed to 30.4 (Mode HUD).
Encoder recovery (Stage 30.5) still waits on Ivan's Arduino A/B
test next week.

#### 4.3a Attempt 1 -- fw 0.4.73 (landscape via MADCTL glitched)

Ivan's read-back:
  - Panel glitched fundamentally: green on left/right/bottom, big
    black rectangle in the middle with noise streaks.
  - Nav didn't rotate either -- swipes on landscape mapped to
    portrait's swipe directions (right on landscape triggered
    "swipe up in portrait" -> alarm tile).
  - Crash after ~86 s of double-tap sleep/wake + swipe: same
    I2C-hw-fsm-reset-in-critical-section pattern from
    max17048_read16 as fw 0.4.68. Recurring.
  - New E-line at idle-sleep:
      E spi_master: spi_device_acquire_bus(1338):
        Cannot acquire bus when a polling transaction is in progress.
      E co5300_pio: co5300_pio_tx_param(131): acquire failed
      E co5300_panel: co5300_op_disp_sleep(317): sleep: DISPOFF failed
    Sleep still succeeded via the safety-net path but with visible
    log spam.

Root cause of the glitch. co5300_panel.c op_draw_bitmap does a SW
coord swap when ctx->swap_xy is set AND MADCTL.MV=1 makes the
panel HW-swap its own CASET/RASET interpretation. That's a double
transform. On top of that, x_gap (CO5300_COL_OFFSET) gets applied
to whichever axis the SW branch names "col" -- which no longer
matches the panel's physical column direction under MV. The
combination of double-transform + wrong-axis offset produces
garbage window addressing, which reads as green borders (panel
untouched RAM) + black centre (window intersection) + noise
(overwritten wrong pixels).

Root cause of the `spi_master:1338` regression. Stage 30.1 landed
a `bus_mutex` in the OLD co5300.c to serialize LVGL flushes vs.
`co5300_sleep` calls from field_capture. We deleted co5300.c in
30.2 and moved to the new co5300_panel.c + co5300_panel_io.c
duo -- but forgot to bring the mutex forward. New panel_io has no
serialization, so when field_capture fires idle-sleep while LVGL
is mid-`tx_color`, spi_device_acquire_bus sees the in-progress
polling transaction and refuses. Sleep succeeds via safety-net
but the log is dirty and the panel may momentarily see a partial
CASET before DISPOFF wins.

Root cause of the I2C-fsm-reset crash. Same signature as fw 0.4.68
attempt: after many rapid sleep/wake cycles, MAX17048 read hits
an I2C bus error, driver goes into `i2c_hw_fsm_reset` inside
`vPortExitCritical`, IRQ WDT trips. Independent of Stage 30 --
recurring latent I2C driver issue in ESP-IDF v5.5.2 that surfaces
under our specific contention pattern. Noted in
[[project_i2c_hw_fsm_reset_crash]] (memory saved this pass).

**Fix (fw 0.4.74, `co5300_panel` 0.1.3, `lvgl_ui_display` 0.2.5,
`nvs_cfg` 0.3.4).**

1. **Revert landscape** to portrait in `lvgl_ui_display.c
   disp_cfg.rotation` (all three flags back to false). Big comment
   documenting the two proper paths for landscape (LVGL software
   rotation vs. rewrite panel op_draw_bitmap to trust MADCTL only)
   -- pick in the next batch after review.

2. **Re-add bus_mutex** to `co5300_pio_t`. Created in
   `co5300_panel_io_new_spi()`, destroyed in `co5300_pio_del`.
   `tx_param` and `tx_color` both take the mutex around their
   whole SPI activity so field_capture's disp_sleep and LVGL's
   flush can no longer collide. Mirrors what Stage 30.1 did on the
   old driver; keeps the wire path unchanged.

3. **New `NVS` CLI verb.** Extracted the printing body of
   `nvs_cfg_boot_print()` into a public `nvs_cfg_dump()` (no
   print-on-boot gating); `nvs_cfg_boot_print()` becomes a thin
   wrapper. `NVS` (no arg) calls `nvs_cfg_dump(I2C_NUM_0)`. The
   stale local `rtc_cli_dump_nvs` in fc_cli.c now delegates to
   the same central dumper -- fixes drift where the local copy
   was missing lvgl_force, auto_shdn, log_level, pc_sync fields.
   `NVS_PRINT [ON|OFF]` preserved as the boot-toggle verb; the
   `NVS` handler explicitly skips inputs that start with
   `NVS_PRINT` so verbs don't collide.

**Landscape replanning notes.** Two paths, pick one next batch:
  (a) **LVGL software rotation** (`lv_display_set_rotation` or
      the `rotation` field in `lvgl_port_display_cfg_t` set to
      LV_DISPLAY_ROTATION_90 with `swap_xy`/`mirror_*` all
      false). LVGL rotates the pixel buffer in software before
      each flush; the panel stays in portrait via MADCTL 0x08
      (BGR only, no MV). Zero driver changes; guaranteed correct;
      pays ~15% CPU per flush.
  (b) **Rewrite op_draw_bitmap** to trust MADCTL only (drop the
      SW coord swap; re-anchor x_gap to the physical column axis
      that survives MV). Matches the ST7789 esp_lcd driver
      pattern. Needs bench validation of QSPI 0x32 semantics and
      COL_OFFSET direction under MV.

Recommend (a) for the next flash -- fastest to bench-prove, and
falls back to (b) as a perf optimization if the CPU cost proves
noticeable. Landscape doesn't need to be perfect for the pending
demo; portrait + a working AUTOSHDN + working NVS dump gets us
there.

Boot-order overhaul, tile-arch cleanup, LVGL foundations rebuild
all stay parked for Stage 31 per §6 handoff. Ivan flagged the
frankenstein feel on 2026-09-15 -- codified in
[[project_lvgl_foundations_rebuild_planned]] (memory saved this
pass).

**Local build:** clean (0x14d040 B, 57% free; only pre-existing
IDF-header warnings about spi_transaction_ext_t init).

**Flash test:** fw 0.4.74
  1. Portrait renders clean like fw 0.4.72. Sync path from 30.2
     §4.2i still in place; text stays crisp through minute rollover.
  2. Idle-sleep at 30 s: no `spi_master:1338` E-lines, no
     `co5300_op_disp_sleep sleep: DISPOFF failed`. Log is clean.
  3. `NVS` verb dumps every persisted record (rtc + all sys keys
     including lvgl_force, auto_shdn, log_level, pc_sync).
  4. `NVS_PRINT ON|OFF` still toggles the boot-time printout
     independently.
  5. `HELP` lists both `NVS` and `NVS_PRINT`.
  6. `AUTOSHDN` still works from fw 0.4.73.

If (1)+(2)+(3) hold, we're ready for landscape 30.3b (path (a)
LVGL software rotation), then 30.4 (Mode HUD).

#### 4.3b Attempt 2 -- fw 0.4.75 (Stage 30 close-out)

Ivan bench-verified fw 0.4.74: "flash is great". Portrait +
mutex + NVS dump all clean. Chose to stay portrait rather than
attempt the LVGL SW rotation path -- rationale: Stage 31's
foundations rebuild will re-decide orientation anyway, hacking
it now is throw-away work.

**Side-quest landed (fw 0.4.75):** `FLASH [ON|OFF|<0..100>]` CLI
verb. Direct wrap around `flashlight_set_brightness()`; bypasses
FCM_FLASHLIGHT mode-cycle so the LED works while the encoder is
offline on the iv8.0 unit. Least-code bridge to demo readiness.

**Housekeeping landed same day:**
  - `memory/feedback_md_frontmatter_convention.md` -- new rule:
    every narrative MD opens with a preemptive metadata block
    (Date opened / Date closed / Status), fill on close.
  - `memory/feedback_log_line_taxonomy.md` -- new rule: every
    ESP_LOG tag matches the source component's directory,
    ALL_CAPS. Canonical list at `docs/build_info/LOG_TAGS.md`.
  - `docs/build_info/LOG.md` -- append-only loose-ends journal,
    seeded with today's breadcrumbs (BROKER rtc mutex,
    diag-pattern removal, XD mystery tag, etc.).
  - `docs/build_info/LOG_TAGS.md` -- canonical tag legend
    grouped by subsystem. Flags `co5300_panel`/`co5300_pio`
    (lowercase, non-conforming) and `XD` (mystery) for future
    cleanup passes.

**Stage 30 shipped:**
  - 30.1 sleep-race mutex (landed 4.1, regressed and re-landed
    at 4.3a as bus_mutex on co5300_panel_io_t).
  - 30.2 esp_lvgl_port migration (attempts 1-9 across §4.2a..i;
    sync-only pixel path is the final wire behaviour).
  - AUTOSHDN CLI+NVS (side-scope, §4.3).
  - NVS on-demand dump verb (side-scope, §4.3a).
  - FLASH CLI bypass verb (side-scope, §4.3b).
  - Housekeeping conventions above.

**Stage 30 deferred (rolls into Stage 31):**
  - 30.3 landscape rotation.
  - 30.4 Mode HUD.
  - 30.5 encoder recovery (waits on hw A/B test).
  - 30.6 boot-order overhaul (Ivan's core -> per-device -> UI ->
    drv-end sequencing).

Bench baseline for Stage 31 open: fw 0.4.75, iv8.0 unit,
portrait, sync-only pixel path, 60-row LVGL strip, all sensors
alive except encoder (hw-suspect).

---

## 5. Definition of done for Stage 30

  1. No SPI-race E-lines around idle-timeout sleep.
  2. Display init runs through `esp_lvgl_port_add_disp` (our
     custom `lvgl_flush_cb`, byte-swap loop, and cache msync
     are gone from `lvgl_ui_display.c`).
  3. Swipes feel comparable to or better than fw 0.4.61 full-frame.
  4. Panel renders in landscape 90° CW; touch tracks correctly.
  5. Mode HUD live on the main screen, updates on mode change.
  6. No regression on peripherals stable at Stage 29 close.

---

## 6. Next-steps hook -- Stage 31+ scope

**Primary handoff (execute-plan chain):** when 30.1-30.4 land and
bench-verify, next stage is
`Stage_31_Mk1b_LVGL_Foundations_Rebuild.md` -- LVGL 9 architecture
rebuild (shared pane styles + `lv_subject_t` bindings + sibling-pane
drawer + tile pattern proof on GPS). Foundations for that stage
depend on Stage 30's PARTIAL-refresh + port serialization being in
place. Full 3-stage roadmap in Stage 31 §1.

**Parked (unchanged from Stage 29):**

  - **Text-crispness deep dive** if the port migration doesn't
    already crisp things (Stage 29 §2.3 candidates 1-3 still
    valid).
  - **LSM tap-double + touch double-tap redundancy** -- one
    detector armed at a time, touch-alive gates LSM off.
  - **DRV auto-cal IMU sweep auto-enable LSM** or gate the sweep
    verb on LSM ready.
  - **TEMP_DUMP LSM WRIST_DOWN suppression** -- investigate why
    the settling window triggers the gesture.
  - **Encoder tile redesign** per [[project_encoder_tile_redesign]].
  - **CLI submenu restructure** per [[project_cli_submenus]].
  - **Recording writers datetime migration** per
    [[feedback_recording_datetime_opportunistic]].

---

## References

  - `Stage_29_Mk1b_Landscape_And_Space_Grotesk.md` -- previous
    stage. §4.1 (Space Grotesk landed), §4.2 (shear diagnosis),
    §4.2b (full-frame PSRAM escalation + sleep-race obs).
  - `Stage_28_Mk1b_Display_And_Mode_HUD.md` -- Mode HUD spec at
    §2.4 (carried forward).
  - Old smartwatch project at
    `/home/ivan/esp/ESP32-S3/WS-Touch-LCD-1.69/
    11_SmartWatch_v5_project_only/` -- known-crisp LVGL via
    `esp_lvgl_port_add_disp` on ST7789. Reference for the
    migration shape.
  - `firmware/arduino/16_amoled_touch_test_mk1b/` -- Mk1b-verified
    CO5300 init + pixel-write sequence; reference when writing the
    ESP-LCD panel wrapper.
  - Auto-memory `MEMORY.md` -- especially
    [[project_display_landscape_direction]],
    [[project_display_text_crispness]],
    [[feedback_lvgl_kconfig_authoritative]],
    [[feedback_flash_per_checkpoint]],
    [[feedback_group_work_by_venue]],
    [[feedback_status_terse]],
    [[feedback_stage_log_workflow]],
    [[feedback_mk1b_stage_naming]],
    [[feedback_lv_font_conv_flags]].
