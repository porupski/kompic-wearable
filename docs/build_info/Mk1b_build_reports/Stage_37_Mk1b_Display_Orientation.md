---
Date opened: 2026-10-02
Date closed: 2026-10-04
Status: ABANDONED (both Flash A SW rotate + Flash B HW MADCTL came out
mangled; reverted to fw 0.4.97 pre-Stage-37 portrait. Next session
re-plans landscape from scratch.)
---

# Stage 37 -- Mk1b landscape orientation + text crispness + tile reflow

Board: Mk1b iv8.0. Firmware baseline: 0.4.97 (Stage 36 close).

Two prior landscape attempts failed (Stage 29 §2.2 revert, Stage 30 §4.3
double-transform glitch). This stage takes the third approach -- **path
(a) LVGL software rotation** -- which was the fallback plan documented in
the lvgl_ui_display.c:187-215 comment block.

Scope expanded on open per bench feedback: ride the orientation flip to
also land (b) text crispness (DPI + colour depth), and (c) the tile
layout reflow for the 502x410 landscape canvas with the ~15% corner
deadzone per [[project_display_corner_cutoff]]. User complaint:
titles/switches overlap and are bunched up, haptic tile is too tiny,
text still looks AA-smeared.

Direction convention: top-of-portrait on the RIGHT of landscape per
[[project_display_landscape_direction]] = 90 degrees clockwise rotation.

## 1. Why path (a)

Panel stays native-portrait (zero co5300_panel.c changes). LVGL rotates
the pixel buffer in software before each flush via `lv_draw_sw_rotate`
on draw_buffs[2] (allocated per sw_rotate=1 in esp_lvgl_port). Panel
sees portrait coords verbatim so CO5300_COL_OFFSET=22 keeps applying to
the correct axis -- the whole class of Stage 30 §4.3 bugs is
sidestepped.

Trade-offs: CPU cost per flush for `lv_draw_sw_rotate` (ESP32-S3 has no
PPA), plus a third draw buffer (~72 KB internal SRAM for the 60-row x
410 x 3 bytes rotation staging). Internal SRAM headroom goes from
~276 KB to ~200 KB free post-boot; still comfortable.

Alt (b) is the panel-driver MADCTL.MV + x_gap rework. Deferred until
(a) proves user-visible insufficient (bench-perceived flush time hurts
UX). Not expected on this screen size (60-row strips @ ~24 kpx each).

## 2. Plan

Three flashes, each a bench checkpoint per
[[feedback_flash_per_checkpoint]].

### 2.1 Flash 1 -- SW rotation

1. `lvgl_ui_display.c`:
   - Set `.flags.sw_rotate = 1` in disp_cfg.
   - Keep `.hres = LCD_H_RES`, `.vres = LCD_V_RES` (these are PHYSICAL
     resolution; esp_lvgl_port needs the native panel size).
   - After `lvgl_port_add_disp`, call
     `lv_display_set_rotation(s_lv_disp, LV_DISPLAY_ROTATION_90)` so
     LVGL exposes a 502x410 logical canvas to widgets.
   - In `lvgl_touch_read_cb`, transform CST9217 portrait coords into
     landscape coords:
       `data->point.x = LCD_V_RES - 1 - pt.y;`  (= 501 - pt.y)
       `data->point.y = pt.x;`
     LVGL 9 does NOT auto-rotate indev points; the display's
     horizontal_resolution() after rotation = 502, which is what the
     indev x range gets validated against.

2. Audit sweep for hardcoded portrait geometry (`lv_display_get_horizontal_
   resolution(NULL)` / `_vertical_` for anything full-canvas):
   - `ui_settings_screen.c:88,90,102` -- drawer pane + tileview sized by
     `LCD_H_RES x LCD_V_RES`.
   - `ui_lock_screen.c:73` -- absorber.
   - `ui_shutdown_overlay.c:77` -- overlay.
   - `ui_notif_overlay.c:125` -- alarm overlay.
   - `ui_animations.c:81-102` -- slide_in/out offsets.
   - `ui_main_screen.c` -- diag pattern (hardcoded 396 wide) -- disable
     `UI_MAIN_DIAG_PATTERN` to 0; its diagnostic purpose was Stage 30
     bringup and is now in the way.

3. Non-goals this flash:
   - Do NOT touch co5300_panel.c (panel stays portrait).
   - Do NOT touch any `*_tile.c` (reserved for Flash 3).
   - Do NOT re-attempt MADCTL path (b).

**Flash gate:** rotation renders cleanly in landscape, touch tracks the
visible pixel. Clock + date + battery label visible on main screen.
Settings drawer slides up to full landscape canvas. If any area
glitches at the strip boundary, pause and report BEFORE touching
co5300_panel.c.

### 2.2 Flash 2 -- Text crispness

Stage 29 §2.3 ranked candidates; Stage 30 landed candidate #1 (migration
to esp_lvgl_port). Remaining cheap knob is DPI.

- `sdkconfig`: `CONFIG_LV_DPI_DEF=130` -> `220` (panel is ~226 DPI from
  502 px / 2.2" diagonal). LVGL uses DPI to size the text AA kernel;
  underestimated DPI widens the halo and reads as "blur".

- If still blurry: `CONFIG_LV_COLOR_DEPTH 16 -> 32` per
  [[project_display_text_crispness]]. 32-bit (ARGB8888) lv_color_t
  preserves blending precision when the display buffer is RGB888.
  Memory cost: 2x widget bitmap storage. Acceptable with our current
  heap.

Flash gate: subjective -- Ivan judges "crisp enough".

### 2.3 Flash 3 -- Tile layout reflow for 502x410

Audit says haptic + gps + system tiles are the crowding risk. GPS is
already "good" per Ivan, so leave alone. Haptic reflow + 15% deadzone
discipline is the headline work. Per
[[project_display_corner_cutoff]], the four corners (~15% each side)
are usable only for mode-color chrome / RGB mirroring -- no text or
touch targets land there.

Specific touches (candidate list; refine on bench):
- `drv2605/haptic_tile.c` -- enlarge roller + buttons, spread the stack
  along the longer horizontal axis.
- `lvgl_ui/system_tile.c` -- list height currently 220 px assumes
  502-tall; recalc for 410-tall landscape.
- Any flex/row layout that benefits from the wider canvas gets re-flowed
  via `lv_obj_set_flex_flow(LV_FLEX_FLOW_ROW)`.
- Hoist repeated widget sizes/padding into `ui_pane_styles.c` where it
  makes sense; don't invent abstractions beyond what the reflow uses.

Non-goal: do NOT change tile semantics or add/remove widgets. Pure
geometry pass.

Flash gate: no overlap, no clipping, ergonomic touch targets.

## 3. References

- `firmware/esp-idf/components/lvgl_ui/lvgl_ui_display.c:187-215` --
  two-paths comment block (path (a) vs path (b)).
- `Stage_29_Mk1b_Landscape_And_Space_Grotesk.md` §2.2 (first revert) +
  §2.3 (text crispness candidate ranking).
- `Stage_30_Mk1b_Display_Port_Migration.md` §4.3 (path (b) failure root
  cause).
- `esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:449-475, 676-733` --
  sw_rotate buffer alloc + `lv_draw_sw_rotate` flush path.
- Memory: [[project_display_landscape_direction]],
  [[project_display_corner_cutoff]],
  [[project_display_text_crispness]].

## 4. Bench log

(filled in-place per flash per [[feedback_stage_log_workflow]])

### 4.1 Flash 1 -- SW rotation (fw 0.4.98)

**Bench observation (2026-10-02):**
  - Display: "all diagonal artifacts, can't see shit on it how messed
    up it and jumbled it all looks."
  - Touch/gesture: swipe RIGHT-TO-LEFT (physical landscape) opens
    the drawer that was swipe-UP in portrait. 90-ish-degree gesture
    offset.

**Touch bug root-caused + fixed in source (not yet flashed):**
  LVGL 9 auto-rotates indev points. See
  `managed_components/lvgl__lvgl/src/indev/lv_indev.c:743`:
  `lv_display_rotate_point(i->disp, &data->point)` is called inside
  `indev_pointer_proc` BEFORE the point is handed to widgets. It uses
  the display's PHYSICAL hor/ver_res to transform raw -> logical. My
  `lvgl_touch_read_cb` was pre-rotating with the same formula, so
  LVGL rotated again -> 180-deg offset. For a physical landscape
  swipe-UP (portrait right -> portrait left), raw_y stays constant
  and raw_x decreases; double-rotated this reads as landscape x
  INCREASING = DIR_RIGHT. The user's "swipe-right-to-left opens
  swipe-up drawer" exactly matches a 90-deg CCW remap. Fix: hand raw
  CST9217 coords to LVGL unchanged; delete the
  `map_touch_portrait_to_landscape` helper. Already in local build
  (fw 0.4.98 unflashed). Save this lesson as feedback memory for
  next session.

**Diagonal artifacts: unresolved.** Code review of
`esp_lvgl_port_disp.c:449-475, 676-733` + `lv_draw_sw_rotate` ->
`rotate90_rgb888` found no obvious stride/alignment/dst-size bug.
Buffer sizing fits: 24,600-px partial render respects the sw-rotate
draw_buffs[2] cap. Not pinned without bench instrumentation.

**Decision for next session (Ivan 2026-10-02):** no revert. Commit
to doing it right. Three paths on the table:
  (A) Flash touch fix only; see if the "diagonals" were actually
      aggravated touch confusion. Low risk, high info.
  (B) Pivot to HW rotation (path b): esp_lvgl_port
      `rotation = {swap_xy=true, mirror_x=true}` + fix
      `co5300_op_draw_bitmap` to drop its SW swap AND re-anchor
      COL_OFFSET=22 to RASET under MV=1 (physical col axis =
      LVGL "row" axis after MV). Stage 30.3 crashed on exactly
      this -- but with the known failure mode in hand, the fix is
      concrete: `caset_offset = swap_xy ? y_gap : x_gap;`.
  (C) Dig into SW rotate: wire a scope on the SPI to confirm the
      pixel burst length matches CASET/RASET window after rotation.
      Only if (A) proves SW rotate truly is corrupt.

**Carried into next session:**
  - `lvgl_ui_display.c` -- touch fix landed, sw_rotate=1 still on.
  - `ui_main_screen.c` -- DIAG pattern off.
  - Overlays + animations -- LCD_H/V_RES swapped for
    `lv_display_get_horizontal/vertical_resolution(NULL)`. Benign
    either way.
  - FW 0.4.98 built, not flashed.

### 4.2 Flash 2 -- Text crispness
Never reached.

### 4.3 Flash 3 -- Tile layout reflow
Never reached.

### 4.4 Flash B -- HW MADCTL rotation (fw 0.4.99)

Config landed:
  - `lvgl_ui_display.c`: sw_rotate=0, disp_cfg.rotation={0,0,0}.
    No lv_display_set_rotation() call.
  - `co5300_panel.c`:
      - op_swap_xy / op_mirror no longer re-emit MADCTL (only update
        ctx flags).
      - co5300_apply_madctl deleted (unused).
      - co5300_new_panel initial MADCTL = BGR | MV | MY (0xA8) when
        rotate_90_cw=true (ctx->swap_xy=true, mirror_y=true,
        mirror_x=false).
  - `boot_display.c`: vendor.rotate_90_cw = true.
  - `firmware_version.h`: 0.4.99.
  - Hypothesis: MV flips pixel-fill to column-first so LVGL's
    row-major landscape stream lands on correct physical cells after
    op_draw_bitmap SW-swaps CASET/RASET; MY reverses row-fill so
    landscape(0,0) maps to portrait physical bottom-left = user's
    landscape top-left. Stage 30.3 used MV|MX (0x68) which was the
    wrong combo on this panel.

Bench observation (2026-10-04): "completely messed up, I think you
better start from scratch." Flash B gave garbage output too.

### 4.5 Full revert to pre-Stage-37 (fw 0.4.97)

All Stage 37 Flash A + Flash B code changes reverted in-source
(2026-10-04). Preserved intact:
  - This Stage 37 doc as post-mortem reference.
  - [[feedback_lvgl_auto_rotates_indev]] memory (valid LVGL 9 lesson
    independent of rotation strategy).
  - Ivan's Stage 32-36 WIP in all touched files.

Fw builds clean at 0.4.97 with the pre-Stage-37 config:
  - Panel init MADCTL = BGR only (0x08), portrait.
  - LVGL rotation = 0 (default), no SW rotate.
  - Touch cb returns raw CST9217 coords (equivalent to LVGL
    auto-rotating with rotation=0 = identity, so correct either way).
  - UI_MAIN_DIAG_PATTERN = 1 restored.

## 5. Lessons for next landscape attempt

1. **LVGL 9 auto-rotates indev points** via lv_display_rotate_point
   in indev_pointer_proc. Captured in
   [[feedback_lvgl_auto_rotates_indev]]. Any future custom touch cb
   on a rotated display must return raw device coords.

2. **esp_lvgl_port SW rotate for RGB888 was not clean on this
   stack.** Flash A: clock widget rendered fine, every other widget
   came out diagonally sliced. Not root-caused -- suspects were the
   partial-render dirty-area shape fighting the 24,600 px strip
   buffer, or an lv_draw_sw_rotate rgb888 stride edge case. If SW
   rotate is reconsidered, do NOT trust it blindly; instrument the
   per-flush area dimensions first.

3. **HW MADCTL rotation on CO5300 is not purely a MADCTL flip.** Two
   different MADCTL combos (0x68 in Stage 30.3, 0xA8 in Stage 37
   Flash B) both failed visually. Something else about the pipeline
   (COL_OFFSET under MV, CASET/RASET axis meaning, panel driver's
   SW swap interaction) is wrong. Suggested next move: build a
   headless Arduino sketch per [[feedback_arduino_sketch_pattern]]
   that pokes the panel at MADCTL combinations with a known test
   pattern BEFORE wiring landscape into the ESP-IDF stack. That
   isolates MADCTL semantics from everything else.

4. **The next Stage NN landscape plan should**:
   - Start with the Arduino sketch to characterize CO5300 under
     MV=1 (does CASET address physical cols or rows? Does
     COL_OFFSET=22 still apply to CASET or move to RASET?).
   - Only after MADCTL semantics are empirically verified, port
     the result into ESP-IDF with a precise MADCTL + panel driver
     combo.
   - Keep Flash gates tight per [[feedback_flash_per_checkpoint]];
     do NOT batch landscape with text crispness or tile reflow.
     Isolating variables is the lesson of Stage 37.
