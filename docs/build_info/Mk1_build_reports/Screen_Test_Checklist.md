# Screen Test Checklist — visual verification blocked until Mk1b PCB lands

**Purpose.** Every LVGL / display / touch item that could not be
verified headless during Stages 21-2X. Working blind against the CST9217
probe NAK on iv7.1, so anything past "the LVGL state moves" waits for
Mk1b to actually see pixels.

**How to use.** When Mk1b lands and boots, walk this file top-to-bottom.
Tick each row. Any surprise gets logged into the "Findings" column with
a Stage-XX pointer for the follow-up fix.

**Living doc.** New items land at the top of each subsection as later
stages (23 + 24 + ...) introduce more UI surface. Do not rearrange —
Ivan should recognise the row order from stage to stage.

---

## 0. Sanity — boot + no-regression pass

- [ ] Default boot: `LVGL_FORCE OFF`, no panel forced, sensors + CLI + shutdown all behave exactly as iv7.1 headless. STATUS shows `display = PRESENT (CO5300 up)  touch = CST9217 up`.
- [ ] Boot log includes `BOOT_DISP: CST9217 detected (0xAB @ 0xD000) -- display module present` and `CO5300 ready` and `CST9217 touch ready`.
- [ ] `LVGL_DISP` log line reports `panel=CO5300 touch=CST9217 buf=55920 B (40 rows × 466 px × 3B)`.
- [ ] `task_ui` appears in FreeRTOS task list; no watchdog trips in the first hour.

---

## 1. Display path (Stage 22 §4.1b)

- [ ] First frame ever renders. Boot animation / main screen visible.
- [ ] **RGB byte order** — draw a pure-red rectangle (via `TILE 4` after a photo-view swap, or a bench-only `SCREEN_FILL R G B` if we ever add one). Verify red = red, not blue. If swapped, add a byte-swap loop in `lvgl_flush_cb` before `co5300_write_pixels`.
- [ ] **Window addressing** — partial-refresh strips land in the correct panel region (top-left of a drawn area matches expected coords). If off, verify `co5300_set_window` handles column offset correctly.
- [ ] **Column offset** (`CO5300_COL_OFFSET = 22`) — left edge of every widget starts at logical x=0, not x=22. If widgets appear cut off on the right, offset is being applied twice.
- [ ] **Full-frame refresh** works when a tile switch invalidates the whole screen.
- [ ] **Backlight fade** — `backlight_set_brightness(0)` produces visible dark, `100` visible bright, linear in between.
- [ ] No tearing during 200 ms refresh cycle at normal panel activity.

## 2. Font legibility (all tiles)

- [ ] Every widget label readable at wrist-viewing distance.
- [ ] Data-row alignment consistent (Sats/HDOP/Time/Date/LAT/LON/ALT all left-aligned to the same x).
- [ ] Photo view (§4.6) — Montserrat_30 at wrist distance for lat/lon. If unreadable, upgrade to `LV_FONT_MONTSERRAT_48` (+50 KB) or JetBrainsMono (+40 KB, true monospace). Track in Stage 25 candidates.

## 3. Touch (Stage 22 §4.2)

- [ ] Single tap on empty tile area registers (`TOUCH` CLI verb shows event count increment).
- [ ] Coordinates in expected range: 0..410 X, 0..502 Y. If out of range, CST9217 report parsing in `cst9217.c` needs the actual datasheet vs the assumed CST816S layout (see `cst9217.h` layout comment).
- [ ] Tap on the ATOMIC SYNC button in the GPS tile fires the sync request (`g_gps_sync_requested = true`, log line "Atomic sync requested").
- [ ] Tap on the PHOTO button in the GPS tile swaps to photo view. Tap anywhere on photo returns to normal. Verify tap-to-exit doesn't require a specific spot.
- [ ] Tap on the power-toggle switch flips broker enable state.
- [ ] Long-press vs single-tap discrimination (if we ever wire long-press for encoder-less nav). CST9217 gesture bits are unverified — don't act on them yet.

## 4. Per-tile visual check (walk through all 10)

For each tile: swipe/encoder to it, verify layout, verify widget values match `STATUS` / broker snapshot.

- [ ] `system` (default) — clock + date + battery status strip.
- [ ] `gps` — telemetry view + PHOTO button; sub-tile swipe-up shows NMEA.
- [ ] `rtc` — UTC + local + date + uptime + GPS sync timestamp.
- [ ] `env` — temp / humidity / pressure / altitude / delta height. ZERO HEIGHT button.
- [ ] `compass` — needle rotation matches heading; CALIBRATE button.
- [ ] `imu` — accel XYZ + gyro XYZ + roll/pitch + temp columns.
- [ ] `light` — lux + auto-brightness + brightness slider + blue-light filter toggle.
- [ ] `haptic` — LED + effect roller + IMU CAL + SET FREQ buttons.
- [ ] `health` — LED + BPM + signal bar + SpO2 + finger-detect. **PPG only — no ECG on Kompic** (`feedback_no_ecg_on_kompic.md`).
- [ ] ~~`ecg`~~ — **stub tile, delete during Stage 23/24 cleanup** (NO ECG on Kompic).
- [ ] `alarm` (standalone, not in tileview) — swipe-right from main.

## 5. Photo view (Stage 22 §4.6)

- [ ] Enter photo via PHOTO button OR `GPS_VIEW PHOTO` OR `TILE gps` + button.
- [ ] Black background actually black (LV_OPA_COVER holds).
- [ ] Lat/lon rows large + centred; decimal points visually aligned (or acceptable given proportional font).
- [ ] Time+date row above readable at Montserrat_16.
- [ ] Sat count + ALT row at bottom.
- [ ] "tap to exit" hint visible but subdued.
- [ ] Tap anywhere on photo returns to normal. Verify normal view rebuilds cleanly (no orphan widgets, no bg colour bleed).

## 6. Navigation (Stage 22 gestures + Stage 23 encoder)

- [ ] Swipe up from main → settings tileview.
- [ ] Swipe down from settings → main.
- [ ] Swipe right from main → alarm screen.
- [ ] Swipe left from alarm → main.
- [ ] Tileview horizontal swipe navigates through 10 tiles.
- [ ] Encoder rotate → tileview column changes (**Stage 23 §2**).
- [ ] Encoder click → per-tile action (**Stage 23 §2**). First user: GPS tile click = `gps_tile_cmd_view_toggle()`.

## 7. LVGL_SCREENSHOT (Stage 23 §3)

- [ ] `LVGL_SCREENSHOT` verb dumps to `/sd/lvgl_fb/s<seq>.png` (or `.rgb`).
- [ ] PNG opens on laptop, matches what's on the panel.
- [ ] Consecutive screenshots go into rotating session files, not overwriting.

## 8. Long-running / stress

- [ ] Overnight: LVGL side up + tile refresh at 200 ms + touch idle. No memory leak (heap free doesn't drift down).
- [ ] Battery drain with LVGL on vs LVGL off (compare against the Stage 23 batt_test baseline).
- [ ] Sleep behaviour — does the panel dim / go to sleep after idle? (Depends on `ui_lock_screen.c` state machine + `backlight_set_brightness`.)

## 9. Regression checks (must still work post-Mk1b)

- [ ] All CLI verbs unchanged.
- [ ] `LVGL_FORCE ON` on Mk1b (redundant but shouldn't break anything — should just override the probe).
- [ ] `SHIPMODE` still drops BATFET.
- [ ] Double-click power button still triggers ship mode.
- [ ] SD write path unaffected (recording still lands rows).

---

## Findings log (fill during Mk1b bring-up)

| Date | Row | Finding | Fix / Stage |
|:-----|:----|:--------|:------------|
|      |     |         |             |

---

## References

- `docs/build_info/Mk1_build_reports/Stage_22_LVGL_Binding.md` — where each item was landed blind.
- `docs/build_info/Module_Blueprint.md` — the two-outlet contract these visual checks validate.
- `docs/build_info/reference_files/IV71_TO_MK1B_FIRMWARE_DELTA.md` — Mk1b delta plan.
