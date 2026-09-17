# Stage 26 -- Mk1b display polish + fuel gauge + tile layout

**Date opened:** 2026-09-11
**Board:** Mk1b iv8.0
**Firmware baseline:** `0.4.40` (Stage 25 close, portrait working).
**Status:** OPEN.

**Stage-open bench observations (fw 0.4.40, no battery, USB power):**

| Config                                             | Draw     |
|----------------------------------------------------|----------|
| Dark theme, screen at max brightness               | 70-73 mA |
| Dark theme, screen at min brightness               | 66-68 mA |
| Light theme, screen at min brightness              | 66-68 mA |
| Light theme, screen at MAX brightness              | ~175 mA  |
| During swipe transition (either theme)             | +30-50 mA transient, 1-2 s, then relaxes back |

Light-theme max is by far the highest steady-state cost so far -- worth
keeping in mind when we tune the auto-brightness curve and blue-light
overlay in the batches below. Swipe transient is noted but not a
concern (LVGL layer composite during the animation).

---

## 1. Why this stage exists

Stage 25 landed a stable Mk1b display + touch + GPS stack in **portrait**
(410 wide x 502 tall, MADCTL 0x00, no rotation math). Def-of-done §1-§4
met. The rest of Ivan's original Stage 25 asks -- landscape orientation,
sharper text, correct blue-light filter tint, MAX17048 fuel gauge stub,
and tile layout polish under the bigger fonts -- carry forward to
Stage 26 because they need proper investigation, not more shortcuts.

Two failed landscape attempts in Stage 25 that this stage must not
repeat:

  - **Batch G**: `lv_display_set_rotation(LV_DISPLAY_ROTATION_90)` alone
    only swapped the reported hor/ver res; it did NOT rotate the pixel
    data (requires `LV_DRAW_TRANSFORM_USE_MATRIX` to be compiled in).
    LVGL rendered landscape strips, flush wrote them at native-portrait
    RAM addresses, resulting in sheared frames + green "last quarter"
    band from stale RAM past native col 466.

  - **Batch H**: CO5300 MADCTL 0x60 (MV=1, MX=1) with swapped set_window
    args and the `+22` COL_OFFSET moved from CASET to what LVGL calls Y.
    Panel returned green bars + noise. Either the actual CO5300 register
    interpretation isn't what the ST7789-family conventions suggest, OR
    the offset math is wrong, OR the two need to combine with something
    else (RAMWR direction, framebuffer clear, etc). Landscape was
    reverted in Batch I and Stage 25 closed portrait.

---

## 2. Plan (batches; strictly one thing per flash unless bundled)

### Batch 26.1 -- MAX17048 stub driver

Isolated from display work; safe to land first while landscape research
happens. Same spec as the deferred Stage 25 Batch B:

**New files**
  - `components/max17048/{CMakeLists.txt, max17048.c, max17048.h}`
    - I2C addr 0x36 on `I2C_NUM_1` (bus 2 -- confirmed by Ivan Stage 25
      open: on top with BQ, not with the sensor group).
    - WHOAMI = read `VERSION` register `0x08` (2 bytes MSB first,
      upper nibble should be 0x1 per MAX17048 family).
    - `max17048_read_vcell()` and `max17048_read_soc()` synchronous
      accessors that grab `g_i2c2_mutex` and return raw values. No task,
      no broker channel yet -- cell not attached.

**Files touched**
  - `boot_hw_init.c` -- probe 0x36 on I2C1 in `bringup_bus1()`; on ACK
    call `max17048_init(I2C_NUM_1)` and log the VERSION register.
  - `boot_logic/CMakeLists.txt` -- REQUIRES += `max17048`.
  - `fc_cli.c` -- new `FUEL` verb printing `VER=0x%04X  VCELL=%.3f V
    SOC=%.1f %%`. Per `feedback-cli-first-testability`.
  - `firmware_version.h` bump.

**Success**: boot log shows `MAX17048 0x36 OK  VER=0x001x`, `FUEL` verb
prints three numbers without crashing. Values are noise until Ivan
attaches a cell.

### Batch 26.2 -- LV_COLOR_DEPTH 16 -> 32 for font sharpness

Root cause hypothesis for the "blurry text" Ivan reported since Batch
D: LVGL renders anti-aliasing internally at `CONFIG_LV_COLOR_DEPTH=16`
= RGB565 (5/6/5 bits per channel). When the flush converts to RGB888
for the panel, sub-pixel shades get requantised. Text edges look
noticeably fuzzy on high-DPI panels.

**Changes**
  - `sdkconfig` + `sdkconfig.defaults`:
    - `CONFIG_LV_COLOR_DEPTH_16` -> not set,
      `CONFIG_LV_COLOR_DEPTH_32=y`, `CONFIG_LV_COLOR_DEPTH=32`.
  - `lvgl_ui_display.c` -- keep `lv_display_set_color_format(disp,
    LV_COLOR_FORMAT_RGB888)`. LVGL renders internally as ARGB8888 and
    converts to RGB888 on flush; the flush cb code stays the same.
  - Draw-buffer alloc stays 3 bytes/pixel (output format is RGB888).
    Only the internal LVGL layer buffers grow -- absorbed by the PSRAM
    heap under Batch F.

**Success**: subjective -- fonts read as sharp, not fuzzy. Ivan will
know at a glance.

### Batch 26.3 -- Blue-light filter color diagnosis

Ivan reported that turning the blue-light filter ON in the light tile
made the screen "bright blue" instead of a warm yellow-tint. Filter is
supposed to overlay a semi-transparent yellow, which reduces blue
content. Getting bright blue instead = RGB/BGR channel swap.

**Diagnosis first, then fix**
  - Add a temporary `LIGHT TEST` CLI verb (or hijack an existing verb)
    that draws a full-screen solid RED rectangle for 2 seconds. If it
    shows blue on the panel, LVGL and CO5300 disagree on byte order.
  - If swap confirmed, options:
    - `lv_display_set_color_format(disp, LV_COLOR_FORMAT_BGR888)` --
      LVGL reorders on flush.
    - CO5300 MADCTL bit 3 (RGB/BGR) -- flip to 0x08 in the MADCTL
      value the panel init sends.
  - If NOT confirmed, look at the overlay style code in
    `veml6030/light_tile.c::light_tile_create_overlay` for a wrong
    fill color / bad alpha.

### Batch 26.4 -- Landscape rotation (real, this time)

Only start after Batches 26.1/26.2/26.3 land -- don't stack risky
changes.

**Preferred path: LVGL matrix rotation**
  - `sdkconfig` + `sdkconfig.defaults`:
    - `CONFIG_LV_USE_MATRIX=y`
    - `CONFIG_LV_DRAW_TRANSFORM_USE_MATRIX=y`
  - `lvgl_ui_display.c` -- after `lv_display_set_rotation(disp,
    LV_DISPLAY_ROTATION_90)`, add
    `lv_display_set_matrix_rotation(disp, true)`.
  - `boot_display.h` LCD_H_RES/V_RES back to 502/410 landscape;
    `lv_display_create(LCD_NATIVE_W, LCD_NATIVE_H)` with native
    portrait.
  - Flush cb should still receive native coords + native pixels
    (per the LVGL 9 matrix path).

**Fallback: manual pixel transpose in flush cb**
  - Keep LVGL in portrait (LCD 410x502); expose `LCD_HOR_LOGICAL=502,
    LCD_VER_LOGICAL=410` for widget code; widget code layouts stay
    portrait until Ivan actively wants to move them.
  - No, that's a rabbit hole. If matrix rotation fails, the correct
    fallback is: LVGL landscape 502x410 logical + flush cb transposes
    each pixel to a scratch buffer + writes native. Scratch buffer =
    ~60 KB PSRAM. Cost: ~600 us per strip.

**Success**: full-screen landscape image, no shearing, no green bars.
If mirrored, flip MADCTL bit / rotation direction. If offset, adjust
COL_OFFSET side.

### Batch 26.6 -- Stop the stale GPIO18 vbat_adc read

Small, self-contained. Landed alongside whichever bench session is
already touching the boot path. See §4.0 above for the diagnosis.

**Files touched**
  - `firmware/esp-idf/components/field_capture/fc_battery_test.c` --
    remove (or #ifdef-guard on an `IV71_PROTOTYPE` symbol) the ADC2_CH7
    init that logs
    `FC_BATT: Vbat ADC armed on GPIO18 (ADC2_CH7), 5k1-5k1 divider`.
  - `STATUS` verb (`fc_cli.c`) -- drop the `vbat_adc = XXXX mV` line, or
    replace with "vbat_adc: n/a (iv7.1 override retired on Mk1b)".
  - `firmware_version.h` bump.

**Success**: no `FC_BATT: Vbat ADC armed on GPIO18` line in boot log;
`STATUS` no longer shows a bogus `vbat_adc = 3590 mV` value; GPS UART
reception is no longer periodically corrupted by ADC pin reconfig.

### Batch 26.5 -- Per-tile layout audit under bigger fonts

Once landscape lands (or is deferred long-term), walk every tile and
fix hardcoded x/y offsets that clash with the 28/22/20/16 pt fonts and
the arced corners. Suspects: system_tile (uses 12/8/34), some sensor
tiles that hand-position widgets instead of using `UI_TILE_PAD_H/V`.

---

## 3. Non-goals for Stage 26

  - DRV2605 auto-cal retry (deferred until Ivan tunes the LRA
    profile).
  - SD path (still not on Mk1b).
  - GPS antenna install (mechanical, not firmware -- pending Ivan's
    bench).
  - Broker channel for MAX17048 + battery-tile SOC display (needs a
    cell first).

---

## 4. Bench log (fill in as each batch flashes)

### §4.0 Stage-open bench diagnosis (fw 0.4.40, pre-Batch 26.1)

Ivan flashed sketch `17_max17048_fuelgauge_mk1b` and attached a partially
discharged cell (~3.2 V) to the Mk1b BAT+/- pads. Observations captured
2026-09-11:

**MAX17048 fuel gauge -- NO ACK without battery (expected)**
  - MAX17048 is powered directly from CELL (VBAT+). No cell = no chip
    power = no ACK. This is per datasheet, NOT a soldering issue.
  - Ivan added a cell; re-run the sketch and confirm ACK before
    concluding rework is needed. If still NO ACK with a cell present,
    then rework the MAX17048 pads (or the cell isn't reaching BAT+).

**Charging path is broken between BQ25619 BAT pin and the cell**
  - `STATUS` shows `charging = 1` (BQ25619 REG_STATUS says it's driving
    charge) but cell voltage isn't moving from ~3.2 V.
  - Board dies **immediately** when USB is unplugged, DESPITE a 3.2 V
    cell attached. That means BATFET isn't passing power BAT->SYS
    either. So the entire BAT<->BQ path is broken, not just the
    charge direction.
  - Suspect: cold joint on BQ25619's BAT pad (same class as Stage 24
    §3.5 reseat). Also possible: battery lead/tab not making metal
    contact.
  - Button/QON is NOT the blocker here -- VBUS presence auto-exits
    ship mode on BQ25619. Button matters only for entering ship-mode
    or waking from it after unplug.
  - **Action:** rework BQ25619 BAT joint on next bench session. Once
    the physical path is restored, re-check MAX17048 ACK too -- both
    depend on the cell reaching the top-side ICs.

**`bq_v (fake!) = 3.184 V` -- hardcoded, not a real read**
  - The `STATUS` line still shows a fake constant instead of the
    BQ25619's actual REG_VBAT read. Opportunistic follow-up: wire the
    real BQ voltage read via I2C so we can distinguish "not charging"
    from "already at target voltage".

**`vbat_adc = 3590 mV` on GPIO18 is bogus on Mk1b**
  - `FC_BATT: Vbat ADC armed on GPIO18 (ADC2_CH7)` from the boot log
    is a carry-over from the iv7.1 prototype override
    ([[project_vbat_on_gpio18]]). On Mk1b, GPIO18 is `MAX_M10S_RX`.
  - The 3.59 V reading is the GPS TX line idling high (3.3 V UART
    idle) plus ADC noise. NOT a battery reading.
  - **Secondary problem:** every ADC read on GPIO18 briefly
    reconfigures the pin as an analog input, which can trash GPS UART
    reception. So this is a real bug, not just cosmetic.
  - **Action (Batch 26.6 candidate):** stop the ADC init on Mk1b.
    Either delete the ADC path entirely (now that BQ25619 will
    provide real Vbat over I2C) or #ifdef-guard it behind an
    iv7.1-only flag.

**GPS test sketch written -- no antenna required**
  - `firmware/arduino/18_max_m10s_gps_test_mk1b/` opens Serial1 on
    GPIO17/18 @ 9600 baud, decodes NMEA + UBX frames, polls
    UBX-MON-VER every 10 s, counts 1PPS edges on GPIO46.
  - Success without antenna = any NMEA line at all + a UBX-MON-VER
    reply (surfaces SW / HW version strings). Chip liveness only, not
    fix quality.
  - No firmware overlap: sketch does NOT touch GPIO18 as ADC, so
    UART reception is clean.

**Firmware boot banner is stale: `hw = iv7.1`**
  - `MAIN: KOMPIC hw=iv7.1 fw=0.4.40` in the boot log -- the string is
    hardcoded from the iv7.1 baseline. Board is Mk1b iv8.0. Update
    opportunistically when touching `main.c` /
    `firmware_version.h` per [[project_mk1_shelved]].

**GPS sketch flash result -- chip silent, needs bench diagnosis**
  - `18_max_m10s_gps_test_mk1b` printed `[STAT] 5000 ms: 0 B (0 NMEA,
    0 UBX)` across multiple cycles; MON-VER polls got no reply. Chip
    should auto-emit NMEA at 1 Hz on power-up regardless of antenna.
  - Root causes to check tomorrow: (a) VCC to the M10S module (power
    trace / cold joint), (b) TX/RX swap somewhere between ESP GPIO17/18
    and the module, (c) module actually reflowed OK -- Stage 24 Phase B
    only recorded "soldered; not yet exercised".
  - **Action:** add "M10S continuity + power check" to the tomorrow's
    rework list alongside the BQ BAT joint and MAX17048 pads.

### §4.1 Batch 26.6 -- vbat_adc retired + hw banner refreshed (part of fw 0.4.42)

Coded batch, awaiting flash-verify. Bundled with §4.2 (Batch 26.1) in
one build; if Ivan wants to bisect, revert the max17048 changes and
rebuild.

**Landed:**
  - `fc_battery_test.c`: `vbat_adc_ensure_init()` is now a one-shot info
    log ("retired on Mk1b (GPIO18 is GPS RX); use BQ25619 REG_VBAT").
    `vbat_adc_read_mv()` returns 0. Both symbols retained so the
    BATT_TEST + BLACKBOX CSV writers keep compiling; their `vbat_adc_mv`
    column now writes zeros until the BQ-backed reader is wired up.
    Removed `esp_adc/adc_oneshot.h`, `adc_cali.h`, `adc_cali_scheme.h`
    includes; deleted `s_vbat_adc` / `s_vbat_cali` / `s_vbat_init_tried`
    statics.
  - `field_capture/CMakeLists.txt`: dropped `esp_adc` from REQUIRES.
  - `fc_cli.c` STATUS: dropped `vbat_adc = XXXX mV`; the line now reads
    `soc_temp = XX.X C   (vbat_adc retired on Mk1b -- use BQ REG_VBAT)`.
  - `firmware_version.h`: `KOMPIC_HW_VERSION "iv7.1" -> "iv8.0"`.

**Success criteria for the flash:**
  - Boot banner reads `MAIN: KOMPIC hw=iv8.0 fw=0.4.4x`.
  - No `FC_BATT: Vbat ADC armed on GPIO18` line -- replaced with
    `FC_BATT: Vbat ADC: retired on Mk1b ...`.
  - `STATUS` no longer shows `vbat_adc = 3590 mV`.
  - GPS UART reception (once the M10S is alive) is no longer periodically
    trashed by ADC pin reconfig on GPIO18.

### §4.2 Batch 26.1 -- MAX17048 stub driver + FUEL verb (part of fw 0.4.42)

Coded batch, awaiting flash-verify. Bundled with §4.1 (Batch 26.6).

**New component:** `components/max17048/{CMakeLists.txt, max17048.c,
max17048.h}`.
  - I2C @ 0x36 on I2C_NUM_1; shares `g_i2c2_mutex` with BQ25619 + DRV2605.
  - Accessors: `max17048_read16()`, `max17048_read_vcell_mv()`,
    `max17048_read_soc_pct100()` (returns percent x 100 for one-decimal
    display without float in the driver).
  - `max17048_init()` probes VERSION and confirms family nibble 0x1.
  - No task, no broker channel yet -- that layer comes once Ivan has a
    real cell attached and voltage/SOC curves to fit.

**Files touched**
  - `boot_hw_init.c` bringup_bus1: probe 0x36 after BQ25619; on ACK call
    `max17048_init(I2C_NUM_1)` + log. NAK without a cell is expected +
    logged as such.
  - `boot_logic/CMakeLists.txt`: added `max17048` to REQUIRES.
  - `field_capture/CMakeLists.txt`: added `max17048` to REQUIRES so
    fc_cli.c can call it.
  - `field_capture/fc_internal.h`: added `extern SemaphoreHandle_t
    g_i2c2_mutex;` (bus 0 mutex was already there; bus 2 was missing).
  - `fc_cli.c`: new `FUEL` verb prints
    `VER=0x%04X  VCELL=%u mV  SOC=%u.%02u %%`. HELP row added. WHOAMI
    grew a row for MAX17048 (`I2C1 0x36  MAX17048  0x08  0xXXXX  fam0x1`).

**Success criteria for the flash (with a cell attached):**
  - Boot log: `MAX17048 0x36 OK` + `MAX17048 init OK @ 0x36
    (VERSION=0x001x, family=0x1)`.
  - `FUEL` returns three numbers without crashing.
  - `WHOAMI` shows the new MAX17048 row with `OK`.

**Without a cell**: chip stays unpowered so both ACK and register reads
fail; boot log shows `MAX17048 0x36 NAK (absent, no cell, or bus fault)`
and `FUEL` prints `MAX17048 not responding ... no cell attached?`. That
matches what Ivan sees today.

### §4.3 Batch 26.7 -- STATUS refactor + Mk1b feedback rules (fw 0.4.43)

Ivan's Batch 26.1 flash surfaced two style issues that got landed in
this follow-up:

  - `STATUS` was showing the retired `vbat_adc` label + the fake
    `bq_v (fake!)` line. Both are noise -- retired fields belong
    deleted, not annotated; fake-labelled fields undermine the whole
    dump. Ivan's reaction was blunt.
  - **Landed:** `fc_cli.c` `rtc_cli_dump_status()` now prints just
    `soc_temp = XX.X C` (no parenthetical) and replaces `bq_v (fake!)`
    with a real `vcell = XXXX mV   soc = XX.XX %` line pulled from
    MAX17048 when the fuel gauge is present. If the gauge is absent,
    the line collapses to `vcell = n/a`.
  - **Landed memory:** [[feedback_status_terse]] codifies the "no
    explanatory parentheticals, no fake-labels" rule so future
    printouts don't ship with the same noise.
  - **Landed memory:** [[project_bq_no_vbat_adc]] records that
    `bq25619_read_vbat_mv()` on this hardware is speculative; MAX17048
    VCELL is the authoritative cell voltage source.

### §4.4 BQ25619 death + replacement (fw 0.4.43-flash session, 2026-09-11)

The BQ25619 originally installed on Mk1b died during Stage 24-26 bench
work. The `bq_v = 3.18 V` static reading + stuck `CHRG_STAT =
PRE_CHARGE` + `VSYS_STAT = 1` combo across every flash was the top-side
symptom; multiple bench events (finger touches, battery-lead touches,
brownout on battery insert, board dying on USB disconnect with a cell
attached) traced to the same chip.

**Diode-check protocol used (board OFF, USB unplugged, cell unplugged):**

Only two readings turned out to be genuinely chip-specific:

| Reading | Dead chip | Fresh chip | Interpretation |
|---------|-----------|------------|----------------|
| BAT ↔ SYS body diode | OL both ways | 0.8 V forward, ~1.8 V reverse | BATFET body diode present on fresh chip |
| STATUS register | 0x6D (CHRG_STAT=PRE, VSYS_STAT=1) | 0x74 (CHRG_STAT=FAST, VSYS_STAT=0) | Fresh chip exits PRE_CHARGE and stops holding SYS at VSYSMIN |

`VBUS ↔ SYS OL both ways` and `SYS ↔ GND weird bidirectional drops`
turned out to be **normal** for a populated PCB, not chip-fault
signatures -- BQ25619's input stage is reverse-blocking back-to-back
FETs (VBUS↔SYS reads OL both ways by design), and SYS↔GND is dominated
by TPS62840 VIN ESD clamp + bulk cap charging under the DMM's ~1 mA
drive. Both got misread as damage during the diagnosis; corrected mid-
session and codified in [[feedback_diode_check_parallel_paths]].

**Root cause hypothesis:** electrical over-stress event (reverse-current
episode, mis-polarised battery contact, or a solder blob short during
one of the earlier bench sessions), compounded by ≥5 reflows across
Stage 24 and Stage 26. Chip died with body diodes destroyed on both
BATFET and input FET.

**Fix:** BQ25619 swapped out for a fresh part. Diode checks on the
fresh chip look normal (BAT↔SYS body diode present). Register dump
shows healthy transitions.

**Verified after swap (fw 0.4.43-flash 2026-09-11):**
  - Battery attach with USB present -> total draw 330 mA, of which
    ~300 mA is charge current into the cell (30 mA headless baseline).
  - Unplug USB -> battery holds the board running. RGB verified on
    battery-only, no brownout.
  - `STATUS = 0x74`, `FAULT = 0x00`, `IINDPM = 0x77` (bit 6 flip vs
    dead chip's 0x57 is normal auto-DPDM re-detection).
  - `WHOAMI` shows `MAX17048 0x36 OK`, `FUEL` returns
    `VER=0x0012  VCELL=4185 mV  SOC=100.00 %  (reads=OK/OK)`.
  - Datasheet cross-check: VERSION reset value = `0x001_` (low nibble =
    production revision). `0x0012` = revision 2, valid.

### §4.5 Batch 26.8 -- MAX17048 VERSION mask fix (fw 0.4.44)

`WHOAMI` at fw 0.4.43 printed `MAX17048 ... MISMATCH` for a valid
`VERSION = 0x0012` because my init check compared only the upper nibble
(bits 15-12) against `0x1`. Datasheet actually specifies the family
mask as bits 15-4 (`0x001_`) with the low nibble being the production
revision.

**Landed:**
  - `max17048.c` `max17048_init()`: `(version & 0xFFF0) == 0x0010`; log
    now reads `prod_rev=0xX`.
  - `fc_cli.c` WHOAMI row: same mask; label column shows `0x001_`
    instead of the misleading `fam0x1`.
  - `firmware_version.h` -> 0.4.44.

---

## 5. Definition of done for Stage 26 -- final status

  1. Landscape orientation -- **DEFERRED**. Panel not mated on this
     unit yet (Mk1b assembly still in progress); landscape lands in a
     future stage once display is physically connected.
  2. Font sharpness improved -- **DEFERRED**. Same reason.
  3. Blue-light filter shows correct yellow tint -- **DEFERRED**. Same
     reason.
  4. MAX17048 ACKs on the bus, `FUEL` CLI verb reads without crashing
     -- **MET** (fw 0.4.44). VCELL + SOC read correctly, VERSION mask
     matches datasheet.
  5. No new WDT, no new SPI errors, no regression on sensor tasks
     stable at Stage 25 close -- **MET**. All sensors present at Stage
     25 remain OK across the fw 0.4.42/0.4.43/0.4.44 flashes.

**Bonus wins not in the original DoD:**
  - vbat_adc retired (Batch 26.6) -- GPIO18 UART no longer contested.
  - Boot banner refreshed to `hw=iv8.0` (Batch 26.6).
  - STATUS output trimmed to real data only (Batch 26.7 + memory).
  - BQ25619 replacement -- charging + battery-run both verified.
  - Diode-check methodology documented + memorised
    ([[feedback_diode_check_parallel_paths]]).

**Items carried forward to Stage 27:**
  - Display / touch / landscape / fonts / blue-filter -- physical
    display bring-up first, polish after.
  - GPS antenna install + MAX-M10S alive-check (chip was silent
    across sketch `18_max_m10s_gps_test_mk1b`; needs rework or
    continuity investigation regardless of antenna).
  - Encoder install + electrical bring-up.
  - Button install (dual-wire to BQ /QON).
  - Haptic motor install + DRV2605 auto-cal retune (currently
    `STATUS=0xEC` FAIL every boot).
  - SD card insert + `FS_LS` verify.
  - Real-cell soaking on `broker_battery_data_t` -- swap the fake
    `bq25619_read_vbat_mv()` for MAX17048 VCELL so `battery_tile.c`
    shows real numbers.

Stage 26 CLOSED as of 2026-09-11 with fw 0.4.44 flashed.
