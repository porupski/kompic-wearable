# Stage 29 -- Mk1b landscape rotation + Space Grotesk font family

**Date opened:** 2026-09-13
**Board:** Mk1b iv8.0.
**Firmware baseline:** `0.4.55` (Stage 28 close: RGB byte-order fixed
via in-place swap in `lvgl_flush_cb`, universal back-gesture pipeline
lives, lock narrowed to main screen only, LV_COLOR_DEPTH back at 16,
mode HUD deferred out of Stage 28).
**Status:** OPEN.

---

## 1. Why this stage exists

Stage 28 closed with the panel working, colors correct, and every
user-visible sleep / wake / navigation gesture behaving predictably.
Three polish-heavy items pushed out of Stage 28:

  1. **Text still visibly blurry.** The RGB byte-order swap fixed
     colors but not crispness. The blur is a second bug on the
     rendering path -- suspects are LVGL font AA against wrong
     background alpha, CO5300 gamma / brightness curve, our custom
     flush cb missing something Espressif's `lvgl_port_add_disp`
     handles for us, or the panel's own subpixel geometry / DPI
     assumption. Not solvable by one config knob; needs
     investigation.
  2. **Landscape rotation.** The panel is portrait-native; the
     wearable is meant to be worn landscape with `90° CW` from
     portrait per [[project_display_landscape_direction]]. Stage 25
     Batch H tried CO5300 MADCTL 0x60 and got green bars / sheared
     frames; that path is dead. LVGL 9 matrix rotation
     (`lv_display_set_rotation`) is the current lead; manual pixel
     transpose is the fallback if matrix rotation stutters.
  3. **Space Grotesk font family, everywhere.** Ivan wants Space
     Grotesk as the app-wide font (not just the clock), and the
     clock should be **2x its current 48 px** (~96 px). Stage 28.1's
     Montserrat 48 was a bridge; the target font source lives at
     `docs/build_info/reference_files/Space_Grotesk/
     SpaceGrotesk-VariableFont_wght.ttf`. Blocked on `lv_font_conv`
     which is Node.js and Node isn't installed on this box (see
     Stage 28 §4.1 return-point).

Also carried over as parked observations (not new work here, just
still open):

  - **TEMP_DUMP triggers spurious LSM WRIST_DOWN** (Stage 28 §4.3
    observation 1).
  - **spi_master polling error at LVGL init** (Stage 28 §4.3
    observation 2).
  - **LSM tap-double + touch double-tap redundant** (Stage 28 §4.5
    observation 3).
  - **DRV auto-cal IMU sweep needs LSM enabled** (Stage 28 §4.5
    observation 4).

Mode HUD (was Stage 28 §2.4) is DELIBERATELY not in this stage --
it needs the font family + landscape settled first so we don't
redraw the HUD twice.

---

## 2. Plan (per-item hypothesis + fix)

Ordered by dependency: font first (unblocks bigger clock + everywhere-
same-font redraws), then landscape (once font metrics are known),
then crispness (needs both prior in place so we're diagnosing on the
final canvas).

### 2.1 Batch 29.1 -- Space Grotesk font family + Node install

**Prereq:** Node.js on the dev box. Ivan's decision parked in Stage
28 §4.1 return-point. Cleanest path is conda:
`conda install -c conda-forge nodejs -y` into the existing miniforge
env -- no sudo, reversible, ~50 MB. Alternative: `sudo apt install
nodejs npm`. Ivan picks at stage-open.

**Font source:**
`docs/build_info/reference_files/Space_Grotesk/
SpaceGrotesk-VariableFont_wght.ttf` -- variable font, weight axis.
For LVGL C-file generation via `lv_font_conv`, pick a single weight
(Medium = 500 is a good default) and generate two files:

  1. **`space_grotesk_medium_20.c`** -- app-wide body font, replaces
     the Montserrat 20 / 22 usage under `UI_FONT_LABEL` and
     `UI_FONT_VALUE`. Symbol set: ASCII 0x20-0x7E, degree sign 0xB0,
     any icons currently used in labels. Estimated size: 8-15 KB
     flash.
  2. **`space_grotesk_medium_96.c`** -- watch-face clock, digits +
     colon only (`0123456789:`). Symbol set minimal -> flash cost
     tiny (~2-4 KB despite the size).

Add `LV_FONT_DECLARE` for both in `ui_theme_colors.h`, swap the
existing font aliases to point at the new files.

**Header (`ui_theme_colors.h`) knob updates:**

  - `UI_FONT_TITLE`    -> Space Grotesk medium 28 (or 32, judge on
    bench)
  - `UI_FONT_LABEL`    -> Space Grotesk medium 20 (or 22)
  - `UI_FONT_VALUE`    -> Space Grotesk medium 20
  - `UI_FONT_CHIP`     -> Space Grotesk medium 16 (or keep
    Montserrat if 16 px SG doesn't render well at that size --
    smaller sizes often need hinting adjustments the converter can't
    give)
  - `UI_FONT_TIME_XL`  -> Space Grotesk medium 96 (the 2x Ivan asked
    for)

If Space Grotesk at 16 px looks scrappy, keep `UI_FONT_CHIP` on
Montserrat as a per-tile deliberate fallback; document the trade-off
in `ui_theme_colors.h` comments.

**Success gate:** flash, all tiles render in Space Grotesk, clock is
noticeably larger than Stage 28's Montserrat 48. No missing-glyph
question marks. Overall flash size increase bounded by the two
generated files (target < 30 KB total).

**Bench flash:** independent of 29.2 and 29.3. Land this first --
biggest visual change, gives Ivan the "yes, right font, right size"
gate before we work on rotation.

---

### 2.2 Batch 29.2 -- Landscape 90° CW via LVGL matrix rotation

**Symptom:** Panel is portrait; needs to render as if rotated 90°
clockwise from portrait so the top-of-portrait maps to the right-of-
landscape (worn on the wrist).

**Hypothesis A -- LVGL 9 matrix rotation.** LVGL 9 supports software
transpose via `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`
without any panel-side MADCTL changes and without changing the draw
buffer shape. The CO5300 stays in native portrait; LVGL does the
transpose on flush. Cost: extra memory copy on each flush (~50 KB
transposed once per partial refresh). Manageable on ESP32-S3
internal SRAM.

**Hypothesis B -- Manual pixel transpose in flush cb.** If LVGL
matrix rotation stutters visibly on tile swipes (frame drops or
tearing), fall back to manual transpose: allocate a second scratch
buffer the same size, and in `lvgl_flush_cb` transpose from
`px_map` (source) to scratch (dest), then feed scratch to
`co5300_write_pixels` with the transposed window bounds. More CPU
but full control.

**Hypothesis C (dead) -- CO5300 MADCTL rotation.** Stage 25 Batch H
tried MADCTL 0x60 (MV=1 MX=1) and got green bars / sheared frames
on this silicon. Not pursued again.

**Proposed fix:**

Land A first (single-line change) and eyeball. If it works: done.
If it stutters: revert to portrait and land B. Log which one wins in
the stage doc so future stages don't rehash.

**Direction check on first flash:** LVGL 9 rotation direction is
counter-clockwise-by-default in the standard convention. If
`LV_DISPLAY_ROTATION_90` puts the top-of-portrait on the LEFT of
landscape (wrong direction per Ivan's ask), flip to
`LV_DISPLAY_ROTATION_270`. Cheap two-flash A/B.

**Also touch coordinates:** the CST9217 x/y need to be transformed
by the same rotation so touch stays under the finger. LVGL 9's
`lv_display_set_rotation` handles indev coords automatically when
the indev is bound to the rotated display -- verify at bench (tap a
corner, confirm the event lands under the finger).

**Success gate:** all tiles readable in landscape orientation, no
green bars / tearing / missing rows, touch tracks correctly.

**Bench flash:** after 29.1. The Space Grotesk font metrics need to
be known before we lock in landscape tile layouts.

---

### 2.3 Batch 29.3 -- Text crispness deep dive

**Symptom:** Text edges look blurry despite the RGB fix in Stage
28.4. The old WS-Touch-LCD-1.69 project renders crisp text on
ST7789 at DEPTH_16 via Espressif's `esp_lvgl_port_add_disp` -- known
baseline for what "right" looks like.

**Candidates ranked (from Stage 28 research spike + this stage's
new context):**

  1. **`esp_lvgl_port_add_disp` migration.** Rewrite the display
     init to use Espressif's blessed path: `esp_lcd_panel_io_spi_
     config_t` -> `esp_lcd_panel_dev_config_t` -> `esp_lcd_panel_*`
     for CO5300, then `lvgl_port_add_disp(&disp_cfg)` for LVGL. That
     path handles DMA, cache, flush timing, and byte order via
     `swap_bytes` flag. If this fixes crispness, the ROOT was
     something our homegrown flush cb misses (probably a subtle
     timing / cache detail). BIG batch -- essentially rewriting
     `boot_display.c` + `lvgl_ui_display.c`. Stage-sized.
  2. **Font BPP audit.** Confirm Montserrat / Space Grotesk are
     compiled at 4-bpp (grayscale AA) not 1-bpp (aliased). Kconfig
     doesn't expose per-font BPP; it's baked into the built-in
     Montserrat files. `lv_font_conv` output for Space Grotesk
     defaults to 4-bpp -- verify.
  3. **CO5300 gamma / brightness curve.** COLMOD 0x77 is set but no
     gamma table is loaded. Sketch 16 also skips gamma -- but sketch
     16 renders SOLID color blocks, not text on gradients. AA
     against a mid-tone background exercises gamma linearity in
     ways solid blocks don't.
  4. **DPI-vs-actual mismatch.** `CONFIG_LV_DPI_DEF=130` in both
     sdkconfigs; actual panel DPI ~226. LVGL uses DPI for
     text-antialiasing kernel width. Underestimated DPI -> wider AA
     halo -> perceived blur. Try `CONFIG_LV_DPI_DEF=220`.
  5. **Flush window off-by-one.** Ruled out by Stage 28 research;
     recheck if all else fails.

**Proposed sequence (least-invasive first):**

  1. **First flash: DPI bump alone** (`CONFIG_LV_DPI_DEF=220`,
     one Kconfig line). If crispness improves, done. Two-flash A/B
     with 220 vs current 130 to be sure.
  2. **If DPI didn't help: font BPP verification** on the Space
     Grotesk files generated in 29.1. `lv_font_conv --bpp 4` (the
     default) should be right; a `--bpp 1` output would be aliased.
     Grep the generated .c for `bpp`.
  3. **If nothing works: research spike on `esp_lvgl_port_add_disp`
     migration** as a Stage 30 candidate. That's a rewrite, not a
     batch.

**Success gate:** text on tiles matches Ivan's crispness bar
(subjective; Ivan judges). If DPI alone gets us there, great; if we
need the migration, that becomes Stage 30's headline job.

**Bench flash:** last in Stage 29. Land 29.1 and 29.2 first so the
"blur" judgment is on the final canvas.

---

## 3. Non-goals for Stage 29

  - Mode HUD -- deferred deliberately from Stage 28. Needs font +
    landscape settled first; will get its own stage (30+).
  - LSM tap-double / touch double-tap redundancy fix -- parked
    observation from Stage 28 §4.5.
  - DRV auto-cal IMU sweep needs LSM enabled -- parked observation
    from Stage 28 §4.5.
  - TEMP_DUMP triggers spurious LSM WRIST_DOWN -- Stage 28 §4.3
    observation 1.
  - spi_master polling error at LVGL init -- Stage 28 §4.3
    observation 2. Only worth chasing if it recurs.
  - GPS silence -- still parked per Stage 27 §4.2.
  - CLI submenu restructure per [[project_cli_submenus]].
  - Recording writer datetime migration per
    [[feedback_recording_datetime_opportunistic]].
  - `esp_lvgl_port_add_disp` migration -- staged out of 29.3
    conditionally; only becomes work if DPI + font AA don't crisp
    text on their own.

---

## 4. Bench log (fill in as each batch lands)

### §4.1  Space Grotesk font family landed  (fw 0.4.56, 2026-09-13)

**Prereqs done at stage-open:**

  - Node.js `26.8.2` + npm `11.19.1` installed into the `pcb_dev`
    mamba env via `mamba install -n pcb_dev -c conda-forge nodejs -y`
    (no sudo, reversible).
  - `lv_font_conv` installed globally into that env via `npm i -g
    lv_font_conv`.

**Font source:** `docs/build_info/reference_files/Space_Grotesk/
static/SpaceGrotesk-Medium.ttf` (static Medium file, avoids variable-
font wght-axis quirks in lv_font_conv).

**Generation commands** (run from
`firmware/esp-idf/components/lvgl_ui/fonts/`):

```bash
FONT=".../Space_Grotesk/static/SpaceGrotesk-Medium.ttf"

# Body sizes: full ASCII + degree sign
for SZ in 16 20 28; do
  lv_font_conv --font "$FONT" --size $SZ --bpp 4 --no-compress \
               --format lvgl -r 0x20-0x7E -r 0xB0 \
               --lv-include lvgl.h \
               --lv-font-name space_grotesk_medium_${SZ} \
               -o space_grotesk_medium_${SZ}.c
done

# Clock: digits + colon only
lv_font_conv --font "$FONT" --size 96 --bpp 4 --no-compress \
             --format lvgl --symbols "0123456789:" \
             --lv-include lvgl.h \
             --lv-font-name space_grotesk_medium_96 \
             -o space_grotesk_medium_96.c
```

**Output (source .c size, not flash size — flash is ~1/3 of this;
uncompressed bitmaps make these bigger than a compressed run):**

  - `space_grotesk_medium_16.c`  --  49.1 KB
  - `space_grotesk_medium_20.c`  --  62.3 KB
  - `space_grotesk_medium_28.c`  --  94.1 KB
  - `space_grotesk_medium_96.c`  -- 111.3 KB

BPP verified = 4 (grayscale AA) per the generated file, matching Stage
29 §2.3 candidate #2 ruled-out condition.

**Gotchas noted (first + second flash fail):**

  1. Without `--lv-include lvgl.h`, the tool emits `#include
     "lvgl/lvgl.h"` which doesn't match this project's managed-component
     layout (rest of the codebase uses plain `"lvgl.h"`, see
     `ui_theme_colors.h:25`).
  2. Without `--no-compress`, `lv_font_conv` emits compressed bitmaps
     (`.bitmap_format = 1` on the `lv_font_fmt_txt_dsc_t`). Our
     `sdkconfig` has `# CONFIG_LV_USE_FONT_COMPRESSED is not set`, so
     LVGL 9 can't decode them — the font **loads without error but every
     glyph silently renders as nothing**. Text areas end up blank with
     no log line. `fa5_select_14.c` (the working reference) uses
     `.bitmap_format = 0`. Always pass `--no-compress`.

Regen recipe above includes both fixes; documented here so no future
stage re-hits either.

**Wiring:**

  - `ui_theme_colors.h` — `LV_FONT_DECLARE` for all four faces added;
    aliases repointed:
      * `UI_FONT_TITLE`   → `space_grotesk_medium_28`
      * `UI_FONT_LABEL`   → `space_grotesk_medium_20`  (was Montserrat 22)
      * `UI_FONT_VALUE`   → `space_grotesk_medium_20`
      * `UI_FONT_CHIP`    → `space_grotesk_medium_16`
      * `UI_FONT_TIME_XL` → `space_grotesk_medium_96`  (2× the Stage 28.1
        Montserrat 48)
  - `lvgl_ui/CMakeLists.txt` — four `fonts/space_grotesk_medium_*.c`
    entries appended under `SRCS`.
  - `firmware_version.h` — bumped `0.4.55` → `0.4.56`.

**Deviation from §2.1 plan:** plan said "two files (20 + 96)". Bumped
to four (16 / 20 / 28 / 96) because the theme has five distinct alias
sizes and collapsing all four body aliases onto a single 20 px face
would have visibly shrunk titles and grown chips — layout churn we
don't want on top of the font swap. `UI_FONT_LABEL` and `UI_FONT_VALUE`
still share the 20 px face (matches plan intent). Flash cost slightly
above the 30 KB budget; measured at the next flash.

**Independent from 29.2 and 29.3** — bench-flash this batch alone
first per §2.1 gate, judge the "right font, right size" verdict
before locking in landscape.

**Bench verdict:**

  - [ ] Boot clean, no OOM with the four new font files linked.
  - [ ] All tiles render Space Grotesk (no missing-glyph `?` boxes).
  - [ ] Main-screen clock ≈ 2× Stage 28.1 size.
  - [ ] `UI_FONT_CHIP` at 16 px readable — if scrappy, revert CHIP to
        Montserrat 16 per §2.1 fallback and re-flash.
  - [ ] Flash growth reported (target: bounded, ideally < 80 KB total
        for the four files' `.rodata`).

*(Ivan fills the checklist above once flashed.)*

**Bench verdict (2026-09-13, after fix #2):** fonts render, Space
Grotesk visible, clock ~2× prior. NEW artifact surfaced (see §4.2) --
text on-panel is "sliced horizontally in the middle, top half italic,
bottom half stretched italic", intermittently recoverable by swiping
between tiles. `LVGL_SCREENSHOT` output is clean, so LVGL's render
buffer is correct -- bug is downstream in the flush cb -> panel path.
The Stage 28 blur was a subset of this; Space Grotesk 96 px just made
the shear visible.

---

### §4.2  Flush-path shear diagnosis  (fw 0.4.57, 2026-09-13)

**Symptom recap:** on-panel glyphs are sheared/italic-looking with a
horizontal discontinuity around mid-screen (top and bottom halves have
different shear slopes). `LVGL_SCREENSHOT` dumps the LVGL framebuffer
as PNG and the PNG is correct -- so the bug is between `lvgl_flush_cb`
and pixels on the CO5300, not in LVGL rendering.

**Ruled out immediately:**

  - Font-generation (Stage 29 §4.1 gotcha) -- fonts have
    `.bitmap_format = 0`, screenshot proves the glyphs render right.
  - AA (Ivan's initial guess) -- AA would be identical every frame,
    not intermittent. Recovery-on-swipe is the tell.
  - Custom stride math in LVGL 9 partial mode --
    `LV_DRAW_BUF_STRIDE_ALIGN=1` so rows should be tightly packed
    (`w × bpp` bytes, no padding). If this is wrong, `count * 3` in
    the flush cb under-reads the buffer.

**Hypotheses to check against the log:**

  H1. LVGL emits multiple invalidated areas per frame with different
      widths (e.g. narrow status bar + wide tile body), and each width
      lands at a different byte-offset per row on the panel -->
      different shear slopes on top vs bottom.
  H2. `px_map` is padded to something other than `w * 3` (contradicts
      the LVGL 9 source read but worth verifying at runtime).
  H3. The CO5300 window-set command doesn't actually reset the RAM
      pointer between flushes, so a partial flush lands wherever the
      RAM cursor happens to be.
  H4. Byte-swap-in-place trips on non-3-byte-aligned counts (should
      not happen with RGB888 but worth ruling out).

**Instrumentation landed (`lvgl_ui_display.c:lvgl_flush_cb`):**

  For the first 40 flushes after boot, log at INFO:

    flush[N] x=X1..X2 y=Y1..Y2 w=W h=H count=CNT pre=BB GG RR|BB GG RR

  where `pre=` is the first two pixels of `px_map` (pre-swap, so in
  LVGL's [B,G,R] order). After 40 flushes the log goes silent so it
  doesn't drown other traffic. `FLUSH_DIAG_MAX=40` covers boot's
  initial full-screen paint plus a couple of tile swipes.

**Bench-capture recipe:**

  1. Flash + monitor.
  2. Let boot settle (~5 s).
  3. Trigger the artifact (swipe left+right until sheared text
     appears).
  4. Copy the `flush[N]` lines from the monitor log.
  5. Paste them below.

**Log capture (fw 0.4.57, boot at 16:23:53, sleep at 20.6 s):**

Boot paint (`flush[0..12]` and `[13..25]`): 13 full-width strips of
`w=410 h=40 count=16400` (last strip trims to `h=22 count=9020`).
Every count = w * h exactly -- **no stride padding, no byte-swap
miscount**. So H2 and H4 ruled out on the wire.

Widget partial paints (`flush[26..39]`):

    flush[26] w=209 h=78 count=16302 -- tile body
    flush[27] w=209 h=27 count=5643  -- tile body cont.
    flush[28] w=37  h=32 count=1184  -- clock digit(s)
    flush[29] w=34  h=25 count=850   -- status-bar icon
    flush[30..34] w=10 h=10 count=100 x 5 -- five status dots
    flush[35..37] w=304 h=53,53,12   -- another tile
    flush[38] w=48  h=25 count=1200
    flush[39] w=10  h=10 count=100

Ivan reports: on-panel text was CLEAN at boot, then sheared "a few
secs before disp locking" (~20 s in). Our 40-cap cut off before any
periodic label updates (clock rollover, RTC 1 Hz poll, etc.). The
shearing flushes are **beyond flush[39]**, still uncaptured.

**Refined instrumentation (fw 0.4.58):**

  - Removed the 40-flush cap.
  - Added flush index (`seq`) and delta-us since last flush -- lets
    us spot periodic updates by their cadence (clock: ~1000000 us
    apart, RTC tick similar, battery poll longer).
  - Dedupe consecutive identical-area flushes into one line
    (`flush[..N] (prev area repeated K more times)`) so the log
    stays readable during animations.

**Second capture recipe:**

  1. Flash + monitor.
  2. Boot, verify clean text, note wall-time.
  3. Wait until text visibly shears (should take 10-20 s per fw 0.4.57
     observation).
  4. Ctrl+T then Ctrl+H to trigger idf-monitor's help / save-log,
     or just Ctrl+] to quit and grep the terminal for `LVGL_DISP: flush`.
  5. Paste the last dozen flushes (the ones just before the shear
     appeared) below.

**Log capture (fw 0.4.58, ~1100 flushes, terminal saved manually):**

Boot + steady-state (`flush[0..697]`): full-width strips + small
periodic status-bar updates (5-dot repaint at ~200 ms cadence).
Text was CLEAN throughout.

Swipe up to Settings at 21.6 s (`flush[699..706]`, `[707..745]`) --
full-frame animation, no shear.

Settings-tile refresh at 22.0 s onward (`flush[746..978]`, ~200 ms
periodic 9-flush cycle):

    w=34 h=34  -- icon
    w=82 h=78  -- gauge / dial
    w=89 h=25  -- label
    w=18 h=8  x4 -- chip elements
    w=117 h=32 / w=123 h=32 -- labels

**None of these sheared** on-panel per Ivan.

Swipe down to Main at 25.975 s (`flush[979..1021]`) -- full-frame
animation.

**Main-tile refresh at 26.4 s onward (`flush[1022..1109]`, ~200 ms
periodic 8-flush cycle) -- THIS IS WHERE THE SHEAR APPEARED:**

    flush[1022] x=71..339 y=149..208 w=269 h=60 count=16140  <-- top half
    flush[1023] x=71..339 y=209..266 w=269 h=58 count=15602  <-- bot half
    flush[1024] x=350..388 y=21..45  w=39  h=25 count=975    -- status
    flush[1025..1029] w=10 h=10 count=100 x5                 -- 5 dots

**Diagnosis.** The clock label area (invalidated as 269 x 118 pixels,
rows 149..266) is bigger than the strip buffer can hold in one pass.
LVGL 9's `get_max_row()` computes:

    stride  = area_w * bpp = 269 * 3 = 807 bytes
    max_row = buf_size / stride = 49200 / 807 = 60 rows

So LVGL splits the invalidation into two consecutive flushes: rows
149..208 (60 rows) then 209..266 (58 rows). The split boundary at
row 208/209 is **exactly** where Ivan sees the horizontal slice, and
"top half italic, bottom half stretched italic" is the two halves
landing at slightly different x-offsets on the CO5300.

The Settings-tile flushes DIDN'T shear because each of their
invalidations fits in a single flush (largest is 82 x 78 = 6396
pixels, well under the 16400-pixel buffer capacity at 82 wide).

Root cause of the x-shift between two back-to-back partial flushes
is still unknown -- the flush cb itself is straightforward (set
window, byte-swap, write pixels), and the CO5300 driver re-issues
CASET+RASET+RAMWR at the start of every flush. Candidates: SPI
QIO-mode state leakage between transactions, RAMWR-vs-0x32 pixel-cmd
handoff losing the write cursor, or a subtle CS-toggle timing detail.
**Not fixed in-stage** -- pushed to Stage 30 for the
`esp_lvgl_port_add_disp` migration per Stage 29 §2.3 fallback plan.

**Fix landed (fw 0.4.59):** bumped `LVGL_STRIP_ROWS` 40 -> 80. The
strip buffer grows from 49 200 B to 98 400 B (both internal DMA RAM,
no PSRAM needed). At 80 rows for a 269-wide partial, `max_row =
98400 / 807 = 121` -- the whole 118-row clock area fits in ONE flush.
Full-width invalidations still split (98400 / 1230 = 80 rows per
strip vs 40 previously) but full-width strips paint cleanly (verified
at boot §4.1). Diagnostic instrumentation reverted -- terminal
un-cluttered.

**Verdict (fw 0.4.59):**

  - [ ] Boot clean, no OOM with the 98 KB draw buffer.
  - [ ] Main-screen clock label renders without horizontal slice /
        shear across 30 s of idle observation.
  - [ ] Swipe up/down between Main and Settings, return to Main,
        wait for clock refresh -- no shear on the periodic 8-flush
        refresh cycle.
  - [ ] No new artifacts elsewhere (peripheral tiles, status bar).

Note for future: if any tile ever introduces a partial invalidation
wider than ~270 px AND taller than ~121 rows, it will split again
and the shear will come back. Track that class of change to
`project_display_text_crispness` or the queued Stage 30 migration.

---

**Bench verdict (fw 0.4.59):** 96 px clock stopped shearing. But
smaller-font labels still sheared after a swipe-up-and-back cycle
("8:04 1" italic instead of "18:04"). So bumping the strip fixed the
clock-specific split but the class of partial-flush misalignment is
broader -- either other widgets still split at 80 rows, OR consecutive
partials interact through some SPI/panel state we can't see.

### §4.2b  Full-frame PSRAM refresh escalation  (fw 0.4.60)

**Change:** switched from `LV_DISPLAY_RENDER_MODE_PARTIAL` (49-98 KB
strip in internal DMA RAM) to `LV_DISPLAY_RENDER_MODE_FULL` (617 KB
full-frame in PSRAM). One flush per frame -> no partial-split class,
no consecutive-partial x-shift possible.

Code changes (all in `lvgl_ui_display.c`):

  - Removed `LVGL_STRIP_ROWS` macro; added
    `LVGL_FULL_FRAME_BYTES = LCD_H_RES * LCD_V_RES * 3 = 617 460 B`.
  - `heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM |
    MALLOC_CAP_DMA)` -- 64-byte cache-line alignment for ESP32-S3
    external-RAM DMA.
  - `lv_display_set_buffers(..., LV_DISPLAY_RENDER_MODE_FULL)`.
  - `esp_cache_msync(px_map, count*3, ESP_CACHE_MSYNC_FLAG_DIR_C2M)`
    in the flush cb before the SPI write -- flushes CPU writes to
    PSRAM so DMA sees fresh data.
  - Include `esp_cache.h`.

**Perf budget:** full-frame SPI burst = 617 KB / (40 MHz * 4-bit
QIO / 8) = ~15.4 ms per flush. LVGL refreshes at up to 30 Hz
(33 ms/frame) -- fits with headroom for widget rendering time.

**Verdict:**

  - [ ] Boot clean, PSRAM alloc succeeds (log line reports
        `buf=617460 B (410 × 502 × 3B, full-frame PSRAM)`).
  - [ ] No visible shear on ANY text after boot, after swipe cycles,
        after clock refresh.
  - [ ] No perceptible FPS drop or lag on swipe animations.
  - [ ] No new artifacts.

If the shear persists at 0.4.60: bug is in the SPI/panel layer
itself (state corruption independent of flush size), and Stage 30
`esp_lvgl_port_add_disp` migration is warranted -- Espressif's path
handles panel-side details we may be getting wrong.

**Bench verdict (fw 0.4.60):** ✅ shear GONE on both 96 px clock and
smaller-font labels, across boot / swipe cycles / clock refreshes.
Root cause confirmed to be in the partial-refresh flush machinery
(split flushes and/or consecutive-partial state carry-over) --
kicking one flush per frame kills it.

**Side observation (fw 0.4.60):** `esp_cache_msync` logged
E-level errors continuously:

    E (12486) cache: esp_cache_msync(112): start address:
     0x3c141b40, or the size: 0x96bf4 is(are) not aligned with
     cache line size (0x20)B

The buffer address (0x3c141b40) IS 32 B-aligned; the SIZE (617460
B = 0x96bf4) is not (`617460 % 32 = 20`). IDF's msync still writes
back all full lines overlapping the range, but the E log spams once
per flush (~30 Hz) and adds visible per-log latency on USB-JTAG --
Ivan noted swipe felt sluggish, plausibly from this.

**Follow-up (fw 0.4.61):** added `ESP_CACHE_MSYNC_FLAG_UNALIGNED`
alongside `ESP_CACHE_MSYNC_FLAG_DIR_C2M`. Tells IDF "I know it's
unaligned, best-effort sync". Silences the logs, no functional
change to the writeback.

**Bench verdict (fw 0.4.61):** ✅ cache errors gone. Ivan's read:
"swiping still a bit sluggish". Root cause is the honest 617 KB per
frame at 40 MHz QIO SPI = ~15 ms of solid bus time per flush --
using ~half the 33 ms/30 Hz frame budget on pixels. Not a bug, a
bandwidth ceiling. `esp_lvgl_port_add_disp` migration (Stage 30)
supports proper DMA double-buffering and hardware-accelerated
flush overlap, which is the real cure.

**New observation (fw 0.4.61) -- sleep-time SPI race:**

    I (36168) FIELD: [DISP] reason=idle-timeout (15000 ms idle)
    E (36168) spi_master: spi_device_polling_start(1406): Cannot
    send polling transaction while the previous polling transaction
    is not terminated.
    (6× retries, then finally)
    I (36198) BOOT_DISP: sleep -- panel to DISPOFF+SLPIN

`co5300_sleep()` fired from field_capture's idle-timeout collides
with an in-flight LVGL full-frame flush on the same SPI device.
`spi_device_polling_transmit` is per-call synchronous but not
thread-safe across callers; the flush task on Core 1(ish) is
mid-transaction when field_capture tries to send SLPIN. Retries
until the flush finishes, then succeeds. Cosmetic (goes to sleep
correctly, just spams 6 E-lines). **Deferred to Stage 30 §2.1**:
either wrap CO5300 access with a mutex in the driver, or have
sleep grab `lvgl_port_lock` before touching SPI. Full-frame flushes
made this race way more likely to hit than the partial-refresh path
where each flush was only ~1-2 ms.

---

## Stage 29 close (fw 0.4.61)

**Delivered:**
  1. Space Grotesk Medium is the app-wide font on every tile (§4.1).
     UI_FONT_TITLE=28, LABEL=VALUE=20, CHIP=16, TIME_XL=96. Two
     `lv_font_conv` gotchas documented +
     [[feedback_lv_font_conv_flags]] memory added.
  2. Clock face on the main screen at Space Grotesk 96 -- ~2× the
     Stage 28.1 Montserrat 48 as Ivan asked.
  3. Panel-side shear fixed via full-frame PSRAM render mode (§4.2b).
     Root-cause of the partial-flush class deferred to Stage 30.
  4. No regression on peripherals stable at Stage 28 close.

**NOT delivered (moved to Stage 30):**
  - Landscape 90° CW rotation (Stage 29 §2.2) -- didn't touch this
    stage because the shear investigation ate all bench time.
    Trivial once the display port migration lands.
  - Text-crispness cheap-knob attempts (Stage 29 §2.3) -- moot for
    now, text looks crisp after the shear fix.
  - `esp_lvgl_port_add_disp` migration (Stage 29 §2.3 fallback) --
    **promoted to Stage 30 headline.** Full-frame flush works but
    is bandwidth-bound (~sluggish swipes). Migration gives DMA
    double-buffering + hardware-assisted flush overlap.
  - Sleep-time SPI race (new obs above) -- Stage 30 §2.1 quick fix
    (SPI serialization) even if we defer the full migration.





---

## 5. Definition of done for Stage 29

  1. Space Grotesk medium is the visible app-wide font on every tile
     (or documented per-tile exceptions).
  2. Clock face on the main screen reads Space Grotesk medium 96 (or
     nearest bench-judged best) -- roughly 2x the Stage 28.1
     Montserrat 48.
  3. Panel renders in landscape 90° CW from portrait, tiles
     readable, touch tracks under the finger without offset.
  4. Text edges look crisp to Ivan's eye at arm's length OR the
     `esp_lvgl_port_add_disp` migration is queued as a Stage 30
     headline with a clear plan.
  5. No regression on peripherals stable at Stage 28 close, no OOM
     at boot with the new font files linked in.

---

## 6. Next-steps hook -- Stage 30+ scope

  - **Mode HUD** on the main screen -- broker channel for current
    mode / submenu / RGB mnemonic, subscribed by
    `ui_main_screen.c` as a bottom-mid label. Ivan's spec from
    Stage 28 §2.4. Waits for font + landscape final.
  - **`esp_lvgl_port_add_disp` migration** if Stage 29.3 doesn't
    crisp text with the cheap knobs. Full display-init rewrite,
    matches the WS-Touch-LCD-1.69 baseline.
  - **LSM tap-double + touch double-tap redundancy** -- one
    detector armed at a time, touch-alive gates LSM off.
  - **DRV auto-cal IMU sweep auto-enable LSM** or gate the sweep
    verb on LSM ready.
  - **TEMP_DUMP LSM WRIST_DOWN suppression** -- investigate why
    the settling window triggers the gesture.
  - **Encoder tile redesign** per [[project_encoder_tile_redesign]].
  - **CLI submenu restructure** per [[project_cli_submenus]].
  - **Recording writers datetime migration**.

---

## References

  - `Stage_28_Mk1b_Display_And_Mode_HUD.md` -- previous stage. §4.1
    return-point (Space Grotesk font source path), §4.4 (RGB byte
    swap landed), §4.5 (lock narrowed to main screen, closed
    2026-09-13 on fw 0.4.55).
  - `firmware/arduino/16_amoled_touch_test_mk1b/` -- Arduino sketch
    reference (colors + touch known-good).
  - `Screen_Test_Checklist.md` -- visual verification list.
  - `Module_Blueprint.md` -- CLI + screen contract.
  - Old smartwatch project at
    `/home/ivan/esp/ESP32-S3/WS-Touch-LCD-1.69/
    11_SmartWatch_v5_project_only/` -- known-crisp LVGL text via
    Espressif `esp_lvgl_port_add_disp`. Reference for 29.3 fallback
    plan.
  - Auto-memory `MEMORY.md` -- especially
    [[project_display_landscape_direction]],
    [[project_display_text_crispness]],
    [[feedback_lvgl_kconfig_authoritative]],
    [[feedback_flash_per_checkpoint]],
    [[feedback_group_work_by_venue]],
    [[feedback_status_terse]].
