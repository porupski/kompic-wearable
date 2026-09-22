# LOG -- loose-ends journal

**Purpose.** Append-only chronological journal for stray observations,
half-baked ideas, and side-quest asks that don't yet belong in a
Stage_NN doc or a memory note. Everything here is a breadcrumb --
promote to a proper venue (Stage_NN, memory/*, ADR, dedicated doc)
when the item crosses the "actually a work item" threshold, and
leave a `-> promoted: <link>` note behind so the trail stays intact.

**Format.** Reverse-chronological date headers. Under each date,
bullets with:
  - The observation / ask itself (one sentence).
  - `source:` where it came from (convo mid-Stage-NN / commit hash /
    fw version / bench log line).
  - `owner:` where it'll be resolved (Stage_NN, memory slug, "TBD").

**Split rule.** When this file passes ~500 lines, split by year
(`LOG_2026.md`, `LOG_2027.md`). Not before. Weekly splits add
friction (which bucket does the Sunday-night item go in?), and
`git grep` across one file is easier than across twelve.

**Not for.** Real work items, driver docs, stage plans, memory
notes. Those all have proper homes. This is the "I noticed X"
scratchpad.

---

## 2026-09-15

- **Boot ordering feels off.** Ivan wants a rebuild starting from
  core things -> per-device boots -> UI up first so the user sees
  progress -> drv-final buzz for finish. Not this stage.
  `source:` convo mid-Stage-30. `owner:` Stage 30.6 or Stage 31.
- **UI is a frankenstein.** Per-tile duplication + parallel
  broker/refresh pipelines. Rebuild scheduled.
  `source:` convo mid-Stage-30 2026-09-15.
  `owner:` Stage 31 -- [[project_lvgl_foundations_rebuild_planned]].
- **Encoder dead on iv8.0 unit.** `ENC` verb shows cw=0 ccw=0 net=0
  during active rotation; polled loop is alive; hardware suspect.
  Ivan flashing Arduino field-demo sketch to A/B hw-vs-fw next week.
  `source:` bench log fw 0.4.71-4.74.
  `owner:` Stage 30.5 -- [[project_encoder_dead_on_iv80]].
- **I2C hw_fsm_reset crash under rapid sleep/wake stress.** Core 0
  stuck in `i2c_hw_fsm_reset` -> `vPortExitCritical` during a
  `max17048_read16` after ~6+ double-tap cycles. ESP-IDF v5.5.2
  legacy I2C driver latent issue, not Stage 30.
  `source:` crash log fw 0.4.68 + fw 0.4.73.
  `owner:` i2c_master API migration (Stage 22b groundwork) --
  [[project_i2c_hw_fsm_reset_crash]].
- **Landscape via MADCTL + SW-swap is double-transformed.** Rebuild
  or use LVGL SW rotation.
  `source:` bench log fw 0.4.73 -- Stage_30 §4.3a.
  `owner:` Stage 30.3b (LVGL SW rotation, path A/B in §4.3a).
- **Touch may not reset 30 s idle-sleep.** Ivan flagged
  "feels like 30 s and its done". CST9217 press-edge already calls
  `field_capture_kick_activity()` but he hasn't retested after
  fw 0.4.74's mutex fix. Verify next flash.
  `source:` convo 2026-09-15. `owner:` next bench pass.
- **Diag pattern still ON.** 6 colored squares + white band on the
  main screen from Stage 30.2 §4.2g. Remove once landscape lands
  and rendering stays stable.
  `source:` fc 0.4.70 -- ui_main_screen.c `UI_MAIN_DIAG_PATTERN`.
  `owner:` next Stage 30 batch (after landscape decision).
- **BROKER: rtc_read: mutex timeout** intermittent warning during
  active navigation. Isolated occurrence during Stage 30 close-out
  bench; not blocking. Likely RTC read timing out against I2C
  contention window under UI activity.
  `source:` bench log fw 0.4.74, `data_broker/*` (tag `BROKER`).
  `owner:` monitor -- promote if recurring.
- **FLASH CLI verb landed (fw 0.4.75).** `FLASH [ON|OFF|<0..100>]`
  bypasses FCM_FLASHLIGHT mode-cycle so the LED works while the
  encoder is dead. Direct wrap around `flashlight_set_brightness`.
  Ivan's "least code" bridge to demo readiness.
  `source:` convo 2026-09-15. `owner:` shipped.
- **Log-tag legend landed.** `docs/build_info/LOG_TAGS.md` --
  canonical list of every ESP_LOG TAG in the tree, grouped by
  subsystem. New components add their tag there in the same
  commit. Rule: [[feedback-log-line-taxonomy]].
  `source:` convo 2026-09-15. Two known drifts flagged:
  `co5300_panel` + `co5300_pio` (lowercase), `XD` (mystery).
- **`XD` mystery tag.** Grep finds `static const char *TAG = "XD"`
  somewhere; origin unknown, unclear which C file. Hunt on next
  opportunistic pass through that area.
  `source:` LOG_TAGS.md build 2026-09-15. `owner:` TBD.

## 2026-09-18

- **28 px data-row font shears on env tile (fw 0.4.79).** Header
  ("BME688 T/H/P/Gas") at UI_FONT_TITLE renders cleanly, but the
  5 data rows (same font, `theme_subtext` colour, subject-driven
  content) render with thin white outline, blur, italic-like shear
  and antialiasing artefacts. Same chronic shear class as main-
  clock italic. Possible contributors: partial-refresh strip
  boundary alignment, subtext colour interacting with sub-pixel
  rendering, subject-driven repaint vs. static content. Same
  bucket as the recurring main-clock italic issue; both need a
  proper render-pipeline audit at some point.
  `source:` bench log fw 0.4.79. `owner:` chronic display-render
  issue (grouped with main-clock italic).
- **Env-tile header should be chip-name only (not name + desc).**
  "BME688 T/H/P/Gas" is too wide at 28 px and overlaps the newly-
  larger switch. Just "BME688" fits cleanly. Same probably applies
  to every other sensor tile header (Stage 32 sweep). Small +
  cosmetic; deferred.
  `source:` convo 2026-09-18. `owner:` next env_tile touch.
- **Tile order reordered (System=0, Env=1) fw 0.4.80.** New
  workflow rule: the tile under active iteration moves to Col 1
  so bench access is one swipe-down; DEFAULT_TILE_COL=1 already
  auto-lands there. Codified as
  [[feedback_active_tile_at_col_1]].
  `source:` convo 2026-09-18. `owner:` shipped.
- **Stage 31.2 landed cleanly on fw 0.4.78.** Env tile now
  subject-bound; label content driven by ui_subjects drain timer
  at 200 ms cadence. But bench-invisible: the tile power switch is
  cropped by AMOLED corner arc so Ivan can't turn BME688 ON to see
  live values. Sensor tiles across the board still ignore the 15%
  corner-safe zone -- env is the first fix (0.4.79). Every other
  tile inherits the same pattern until Stage 32 sweep.
  `source:` bench log fw 0.4.78.
  `owner:` Stage 32 for the systemic sweep;
  env done opportunistically in 31.2b (fw 0.4.79).
- **CLI-parity for tile toggles pending.** Ivan wants
  `BME688 [ON|OFF|PARK]` (and per-sensor variants) so headless bench
  can drive the sensors without needing to hit a touchscreen switch.
  Skipping now that the switch is being made reachable in 0.4.79;
  promote if it comes back.
  `source:` convo 2026-09-18. `owner:` TBD, small verb per sensor.
- **i2c_hw_fsm_reset crash: fourth instance.** fw 0.4.78 same
  backtrace (bq25619_read_reg -> i2c_hw_fsm_reset -> IRQ WDT),
  triggered mid-navigation ("swipe slower" per Ivan). Not going to
  keep logging fresh instances -- the pattern is confirmed. Root
  fix is [[project_i2c_hw_fsm_reset_crash]] i2c_master migration.
  `source:` bench log fw 0.4.78.
- **Stage 31.4a bench findings + polish items (fw 0.4.85).**
  - GPS tile subject-bound + robust status line: works well.
    "no data -- check antenna/wiring" and "acquiring..."
    variants tested by having chip silent.
  - **ATOMIC SYNC button not pressable at all** on env-run
    bench. Not blocking (needs GPS fix to be useful anyway,
    which needs bench hw fix). Suspect: button style /
    z-order / disabled-state stuck. Investigate when GPS is
    actually alive. `owner:` post-hw-fix bench pass.
  - **Switch responsiveness slightly finicky** system-wide
    -- occasional missed tap, sometimes double-fire from a
    single tap. Not new to 31.4a; probably CST9217 debounce
    or LVGL indev tuning. Batch with other UI polish later.
    `owner:` polish sweep during Stage 33 (`ui_notif_overlay` /
    shutdown_overlay pass).
  - Tile registry TILE_NAMES in fc_cli.c is stale (reflects
    old order pre-reorder). Cosmetic; TILE `<n>` still works.
    `owner:` next fc_cli touch.
- **Stage 31.3 = DONE fw 0.4.83.** Drawer opens + closes cleanly
  via unified cb_main_gesture; GPS sub-tile still works; alarm
  still works. Stage 31 rollup in Stage_31 doc §10-11.
  `source:` bench log fw 0.4.83. `owner:` shipped.
- **MAX-M10S wiring: 2 pins wrong on this Mk1b unit.** Manual
  §4.1.4 review: VIO_SEL was GND (forces 1V8 mode on a 3V3
  design), should FLOAT for 3V3. SAFEBOOT_N was tied to 3V3
  (undefined + fights internal 1kΩ to TIMEPULSE), should
  FLOAT. Ivan bench-fixing now. Correct wiring durably captured
  in [[project_max_m10s_wiring]]. Earlier hallucination from
  Claude was the source of both errors -- apologies.
  `source:` MAX-M10S Integration Manual review 2026-09-18.
  `owner:` Ivan bench fix, then Stage 31.4 to exercise.
- **GPS integration manual review complete.** Driver-side is
  fine (no "wake up" command needed, chip auto-acquires on
  power). Firmware issue is unlikely; more likely the wiring
  errors above + antenna. Diagnostic adds recommended for
  31.4: UBX-MON-RF (antenna status), UBX-NAV-PVT (fix
  telemetry), UBX-NAV-SAT (per-sat C/N0), dynamic model
  CLI verb (wrist/pedestrian/etc). Details in Stage_31 §11.
  `source:` PDF review agent 2026-09-18.
- **LVGL 9 gesture routing gotcha (worth remembering).**
  `LV_OBJ_FLAG_GESTURE_BUBBLE` is a DEFAULT flag on every
  non-screen object (`lv_obj.c:593`). Gesture dispatch walks
  UP the parent chain while the flag is set, firing on the
  first object WITHOUT it. Only screens lack the flag by
  default. So event cbs registered on any non-screen object
  for `LV_EVENT_GESTURE` receive NOTHING unless you explicitly
  clear `GESTURE_BUBBLE` on the object. Bit us on Stage 31.3
  drawer close (two flash cycles chasing a phantom guard bug).
  `source:` bench log fw 0.4.81/0.4.82 + LVGL source dive
  (lv_indev.c:1781, lv_obj.c:593). `owner:` reference; may
  save future memory if it recurs.
- **31.3 drawer opens but won't close (fw 0.4.81) -> hotfix
  in 0.4.82.** Swipe-up opens drawer cleanly (slide anim
  visible, main under drawer). Swipe-down on Env row 0 doesn't
  close it -- 0.4.82 rewrites the sub-tile guard to use explicit
  tile_registry.subtile_handle comparison instead of
  `lv_obj_get_y > 0` (which may be racy vs. tileview's gesture
  handler timing), plus adds an ENTRY LOGD so we can bench-trace
  every dispatch. `source:` bench log fw 0.4.81.
  `owner:` Stage_31 §9.3b.
- **i2c crash: 5th instance.** GPS-nav-triggered but same
  `bq25619_read_reg -> i2c_hw_fsm_reset` backtrace. Not going
  to log further instances -- pattern is fully confirmed.
  Real fix stays i2c_master API migration; escalating from
  "eventual" to "blocker for stress-tolerance" since it now
  fires on every meaningful bench session.
  `source:` bench log fw 0.4.81.
- **Encoder registered rotations mid-fiddle on fw 0.4.77.** `FIELD:
  mode -> alarm/compass/alarm/fl (top)` fired during unplanned
  handling; polled loop is picking up detents when it wants to. So
  firmware IS reading the pin toggles, root cause is at the wire /
  contact level (intermittent open). Arduino sketch A/B still worth
  doing but hardware-suspect list now clearly narrows to soldering
  or a partial-lift on the encoder's shield-pin. `source:` bench
  log fw 0.4.77. `owner:` -- [[project_encoder_dead_on_iv80]]
  (memory needs update: FW confirmed alive when it registers;
  swap suspect to solder joint / wire fatigue).
- **fw 0.4.77 sleep-skip prophylactic: didn't help this crash.**
  Second bench cycle crashed after ~10 s awake activity (2 swipes +
  1 double-tap ignore), no sleep in the trigger sequence. Confirms
  the crash isn't sleep-triggered per se -- it's cumulative bus
  stress under fast UI navigation. Sleep-skip stays landed (still
  reduces unnecessary bus load when panel is off), but it's clearly
  not the right knob. Real fix stays i2c_master API migration.
  `source:` bench log fw 0.4.77. `owner:` --
  [[project_i2c_hw_fsm_reset_crash]].
- **Blue-light overlay reads too-dark orange on DARK theme.** On
  LIGHT theme the amber tint looks correct; on DARK it renders as a
  muddy dark orange because the overlay opacity + blend was tuned
  against the light bg. Needs per-theme opacity or a different blend
  mode for DARK. Not blocking, do next time we're in
  `light_tile_create_overlay()`.
  `source:` bench log fw 0.4.76 (Stage 31.1 verify pass).
  `owner:` opportunistic when touching light_tile.
- **Italic/shear on main-screen time + battery still recurring.**
  Random, intermittent, transient -- classic display refresh quirk.
  Not caused by Stage 31.1 (labels only changed text-color inheritance).
  Present since Stage 29 / 30 full-frame render. Ivan flag: "at
  earliest convenience when along the way; not blocking but kinda
  critical". `source:` bench log fw 0.4.76.
  `owner:` next display-touching stage; candidate root causes: LVGL
  partial-refresh strip boundary + font atlas alignment.
- **Stage 31.1 plumbing verified on fw 0.4.76.** `PANE_STYLE: init: 5
  pane styles created` fires once at boot; `PANE_STYLE:
  reapply_theme: theme=DARK -- 5 styles updated, screen invalidated`
  fires at UI init. Main + settings render clean; swipes responsive;
  tile titles noticeably crisper (side-effect of centralising
  pad/radius on style_pane_root). Visual DARK<->LIGHT toggle test
  incomplete -- see i2c crash below.
  `source:` bench log fw 0.4.76. `owner:` promoted to Stage_31 §9.1.
- **i2c_hw_fsm_reset crash: third instance.** IRQ WDT on Core 0 mid-
  navigation after ~3 swipes + 2 double-tap sleep/wake cycles + 3
  more swipes. Victim this time: bq25619_read_reg (task_battery_fn),
  not max17048. Same backtrace shape as 0.4.68 / 0.4.73. Confirms
  this is bus-level (any I2C reader can be the victim), not a per-
  driver bug. Escalation: bump priority of i2c_master API migration.
  `source:` bench log fw 0.4.76. `owner:` --
  [[project_i2c_hw_fsm_reset_crash]] (memory updated with third data
  point).

---

## 2026-09-16

- **Boot-mode force-hardcode landed as stopgap.** `field_capture.c`:
  right after `nvs_load()`, `s_mode = FCM_FLASHLIGHT; s_in_submenu =
  false;` -- does NOT call `nvs_save_mode()` so persisted mode is
  untouched. Only there because encoder is dead and there's no other
  way to select a mode. Delete this block once the encoder is fixed;
  device will resume remembering mode normally.
  `source:` convo 2026-09-16. `owner:` revert after
  [[project_encoder_dead_on_iv80]] resolves.

---






I (4464) BOOT_DISP: boot_logic v0.4.1
I (4465) BOOT_DISP: CST9217 ACK @ 0x5A -- display module present
I (4466) co5300_pio: co5300_panel_io ready (queue=8, chunk=32768B, cs=10)
I (4466) co5300_panel: co5300_panel v0.1.3 created (rst=3, rotate90=0)
I (5156) BOOT_DISP: CO5300 ready (410x502 logical, brightness ~80%)
E (5376) gpio: gpio_install_isr_service(530): GPIO isr service already installed
I (5376) CST9217: CST9217 init OK @ 0x5A, boot=220489 us
I (5377) BOOT_DISP: CST9217 touch ready (task_touch_fn will run on Core 0)
I (5377) MAIN: display ready
I (5377) LVGL_DISP: lvgl_ui_display v0.2.5 (force_no_panel=0)
I (5378) LVGL: Starting LVGL task
I (5451) LVGL_DISP: LVGL touch indev bound to g_touch_q
I (5452) LVGL_DISP: LVGL up -- panel=CO5300 touch=CST9217 strip=410x60 px (73800 B x2)
I (5453) APP_NVS: UI settings loaded: theme=0 bright=20 bluelit=0 autobr=0
I (5453) UI_BROKER: UI settings queue created
I (5453) UI_SUBJ: init: env(5) + gps(9 str + 2 int) subjects created; g_env_q + g_gps_q up
I (5454) PANE_STYLE: init: 5 pane styles created (root/drawer/row/card/subtext)
I (5463) STATUS_BAR: Status bar init OK — 5 dots
I (5470) MAIN_SCR: DIAG pattern on: 6 squares R G B W Y C (L->R) + white band
I (5471) MAIN_SCR: Main screen built
E (5502) temperature_sensor: temperature_sensor_install(136): Already installed
I (5502) SYS_TILE: Internal temp sensor: ESP_ERR_INVALID_STATE (already owned by esp_ts_ensure_init, tile shows N/A)
I (5503) SYS_TILE: System tile init OK
I (5536) ENV_TILE: BME688 tile init OK
I (5554) HEALTH_TILE: MAX30101 tile init OK
I (5562) HAPTIC_TILE: haptic_tile_init
I (5588) HAPTIC_TILE: DRV2605 tile init OK
I (5593) LIGHT_TILE: light_tile_init
I (5615) LIGHT_TILE: VEML6030 tile init OK
I (5636) GPS_TILE: MAX-M10S tile init OK (subject-bound, normal + photo views)
I (5650) GPS_TILE: GPS sub-tile init OK
I (5659) RTC_TILE: PCF85063A tile init OK
I (5677) COMPASS_TILE: LIS3MDLTR tile init OK
I (5686) IMU_TILE: imu_tile_init
I (5708) SETTINGS_SCR: Settings screen built — 9 tiles, start col 1
I (5723) ALARM_TILE: Alarm screen built (3 slots, lazy overlay)
I (5723) UI_NAV: Main screen registered
I (5723) UI_NAV: Settings drawer registered (gestures dispatch via main cb)
I (5723) UI_NAV: Alarm screen registered
I (5724) DISP_SLEEP: Display sleep init OK
I (5727) SHUTDOWN_OVL: Shutdown overlay init OK (lv_layer_sys)
I (5729) LIGHT_TILE: Blue-light overlay created on lv_layer_top()
I (5732) LVGL_UI: Initial tile status update complete (9 tiles)
I (5733) PANE_STYLE: reapply_theme: theme=DARK -- 5 styles updated, screen invalidated
I (5737) LVGL_UI: UI init complete - 9 tiles + alarm screen, theme=DARK, brightness=20%
I (5737) UI_SUBJ: start_drain: LVGL drain timer running (200 ms)
I (5837) LVGL_DISP: screens built (theme=0 bright=20%)
I (5838) CST9217: Task started on Core 0
I (5838) LVGL_DISP: LVGL tasks up -- UI_REFRESH + SETTINGS_SAVER + TOUCH
I (5838) BOOT_TASKS: Creating 16 tasks...
I (5838) BME688: Task started on Core 0
I (5839) LSM6DSV16X: Task started on Core 0
I (5839) LIS3MDL: Task started on Core 0
I (5839) LIS3MDL: Calibration task started on Core 0
I (5839) TMP117: Task started on Core 0
I (5840) VEML6030: Task started on Core 0
I (5840) BQ25619: Task started on Core 0
I (5841) PCF85063: Task started on Core 0
I (5842) HAPTIC: Haptic task started (Core 0)
I (5844) ALARM: Alarm task started (1 Hz poll, Stage C active)
I (5845) MAX_M10S: Task started on Core 0
I (5845) FIELD: driver v0.3.2
I (5846) FC_SHDN: shutdown watcher armed: hold 4000 ms to ship (buzz warn at 2000 ms). auto-shdn cap = 30 min (1800 s).
I (5846) BOOT_TASKS: All tasks created:
I (5865) BOOT_TASKS:   Core 0: ENV | IMU | MAG | MAG_CAL | HR | SKIN | LIGHT | BAT | RTC | HAPTIC | ALARM | GPS
I (5866) BOOT_TASKS:   Core 1: FIELD_CAPTURE
I (5866) BOOT_TASKS:   Unpinned: SHUTDOWN_WATCHER (priority double-click ship mode) | RTC_CLI (stdin SET_TIME/GET_TIME)
I (5866) MAIN: Phase 2 boot complete - field_capture running on Core 1
I (5867) main_task: Returned from app_main()
I (5846) FC_BATT: BLACKBOX: disabled by NVS flag -- task exiting
I (5868) FC_COMMON: boot_seq=221 restored mode=usb (submenu=0)
I (5868) SDCARD: Mounting SDMMC 1-bit at /sd (CLK=38 CMD=39 DAT0=40)
I (5928) SDCARD: Card mounted: name=SDABC, capacity=59645 MiB
I (5939) FC_COMMON: SD mounted (59542 MiB free)
I (5939) FIELD: field_capture_init OK (mode=fl boot_seq=221)

[RTC] Boot-time state:
  [RTC] 2026-09-18T23:52:25 UTC
  Commands:
    AUTOSHDN [<min>|OFF]             auto-shutdown minutes (0/OFF = perma-on, default 120, reboot to apply)
    BATT_TEST [ON|OFF]               enter battery-test mode on next boot (no arg = state)
    BLACKBOX [ON|OFF]                background telemetry logger (reboot to start/stop)
    BLACKBOX_CADENCE <s>             sample cadence in seconds (default 10, range 1..3600)
    BQ [EN|BOOST [ON|OFF] | SHIPMODE] BQ25619 command surface (no arg = dump)
    DISP [ON|OFF]                    CO5300 panel sleep/wake (batt-test tool; no arg = state)
    ENC [RESET]                      encoder status + reg dump (RESET = zero counters)
    FLASH [ON|OFF|<0..100>]          flashlight LED (bypass encoder; no arg = show state)
    FS_CAT </sd/path>                dump a file to console
    FS_LS [/sd/path]                 list SD directory
    FUEL                             MAX17048 VERSION + VCELL + fuel % (needs cell attached)
    GESTURE                          dump wrist-gesture state + LPF value
    GET_TIME [-v]                    read RTC now (-v also dumps NVS + RAM_byte)
    GPS_DYNMODEL [PORT|STAT|PED|AUTO|SEA|AIR1|AIR2|AIR4|WRIST|BIKE]  set MAX-M10S dynamic model (no arg = show)
    GPS_MONRF                        dump UBX-MON-RF snapshot (antenna status, noise, AGC)
    GPS_VIEW [normal|photo|toggle]   switch GPS tile between telemetry + photo layouts (no arg = state)
    HAPTIC [EN|PLAY <n>|CAL|SWEEP START|STOP|UI [<n>]]  (no arg = dump)
    HELP                             this list
    LIGHT [EN|AUTO|BLUE [ON|OFF] | BR [<0..100>]]       (no arg = dump)
    LOGLEVEL [OFF|E|W|I|D|V|AUTO]    runtime esp_log level (no arg = show current)
    LVGL_FORCE [ON|OFF]              force LVGL up on next boot even without a panel (no arg = state)
    LVGL_SCREENSHOT                  capture active screen -> /sd/lvgl_fb/*.png
    NVS                              dump every persisted NVS record (on-demand)
    NVS_PRINT [ON|OFF]               toggle boot-time NVS printout (no arg = dump)
    PM_DUMP                          dump PM lock inventory now
    REBOOT                           esp_restart() -- clean SW reset
    REC_AUDIO [ON|OFF]               5 s voice annotation before ENV/MOTION/SKIN (no arg = state)
    RGB <r> <g> <b> | AUTO           bench-poke WS2812 / release override (no arg = dump)
    RTC                              PCF85063A status + register dump
    RTC_DUMP                         hex dump of all 18 PCF85063A registers
    SET_TIME YYYY-MM-DDTHH:MM:SS     write UTC + persist to NVS + PCF RAM_byte
    SHIPMODE                         drop BATFET now (escape when button stuck)
    STATUS                           one-shot state dump (uptime, sensors, batt, heap)
    TEMP_DUMP                        read every onboard temp source (waits for stable, non-zero)
    TILE [list | <n> | <name>]       jump tileview to a tile (bench-testable under LVGL_FORCE)
    TOUCH                            dump CST9217 touch state (last x/y, event count, pressed)
    WHOAMI                           I2C sensor identification + hw_alive status
I (13106) UI_NAV: → Settings (drawer opens)
[GPS] MON-RF: no snapshot yet -- either MON-RF not enabled
             or chip is silent (check antenna/wiring first).
I (60479) FIELD: [DISP] reason=idle-timeout (30000 ms idle)
I (60479) BOOT_DISP: sleep -- panel to DISPOFF+SLPIN
Guru Meditation Error: Core  0 panic'ed (Interrupt wdt timeout on CPU0). 

Core  0 register dump:
PC      : 0x400559d5  PS      : 0x00060234  A0      : 0x8037875f  A1      : 0x3fc9ec60  
--- 0x400559d5: _xtos_set_intlevel in ROM
A2      : 0x00060223  A3      : 0x00000001  A4      : 0x00060221  A5      : 0x00000000  
A6      : 0x60027000  A7      : 0x00000000  A8      : 0x00000000  A9      : 0x3fc9ec10  
A10     : 0x3c140b28  A11     : 0xffffffff  A12     : 0x3fc9ec58  A13     : 0x00060023  
A14     : 0x00000001  A15     : 0x0000cdcd  SAR     : 0x0000001e  EXCCAUSE: 0x00000005  
EXCVADDR: 0x00000000  LBEG    : 0x40056f5c  LEND    : 0x40056f72  LCOUNT  : 0xffffffff  
--- 0x40056f5c: memcpy in ROM
--- 0x40056f72: memcpy in ROM
Core  0 was running in ISR context:
EPC1    : 0x40041a6b  EPC2    : 0x400559e0  EPC3    : 0x00000000  EPC4    : 0x400559d5
--- 0x40041a6b: ets_delay_us in ROM
--- 0x400559e0: _xtos_set_intlevel in ROM
--- 0x400559d5: _xtos_set_intlevel in ROM


Backtrace: 0x400559d2:0x3fc9ec60 0x4037875c:0x3fc9ec70 0x4037805d:0x3fc9ec90 0x4037875c:0x3fccfdb0 0x403824be:0x3fccfdc0 0x4208b36e:0x3fccfde0 0x4208b559:0x3fccfe00 0x4208c69b:0x3fccfe60 0x4207340e:0x3fccfea0 0x42073476:0x3fccfed0 0x42012b97:0x3fccff00 0x40382141:0x3fccff40
--- 0x400559d2: _xtos_set_intlevel in ROM
--- 0x4037875c: vPortClearInterruptMaskFromISR at /home/ivan/.espressif/v5.5.2/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h:560
--- (inlined by) esp_pm_impl_isr_hook at /home/ivan/.espressif/v5.5.2/esp-idf/components/esp_pm/pm_impl.c:1077
--- 0x4037805d: _xt_lowint1 at /home/ivan/.espressif/v5.5.2/esp-idf/components/xtensa/xtensa_vectors.S:1240
--- 0x4037875c: vPortClearInterruptMaskFromISR at /home/ivan/.espressif/v5.5.2/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h:560
--- (inlined by) esp_pm_impl_isr_hook at /home/ivan/.espressif/v5.5.2/esp-idf/components/esp_pm/pm_impl.c:1077
--- 0x403824be: vPortClearInterruptMaskFromISR at /home/ivan/.espressif/v5.5.2/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h:560
--- (inlined by) vPortExitCritical at /home/ivan/.espressif/v5.5.2/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/port.c:514
--- 0x4208b36e: i2c_hw_disable at /home/ivan/.espressif/v5.5.2/esp-idf/components/driver/i2c/i2c.c:266
--- 0x4208b559: i2c_hw_fsm_reset at /home/ivan/.espressif/v5.5.2/esp-idf/components/driver/i2c/i2c.c:726
--- 0x4208c69b: i2c_master_cmd_begin at /home/ivan/.espressif/v5.5.2/esp-idf/components/driver/i2c/i2c.c:1648
--- 0x4207340e: max17048_read16 at /home/ivan/Projekti/Elektronika/Kompic-Wearable/kompic-wearable/firmware/esp-idf/components/max17048/max17048.c:45
--- 0x42073476: max17048_read_soc_pct100 at /home/ivan/Projekti/Elektronika/Kompic-Wearable/kompic-wearable/firmware/esp-idf/components/max17048/max17048.c:66
--- 0x42012b97: task_battery_fn at /home/ivan/Projekti/Elektronika/Kompic-Wearable/kompic-wearable/firmware/esp-idf/components/bq25619/bq25619.c:391
--- 0x40382141: vPortTaskWrapper at /home/ivan/.espressif/v5.5.2/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/port.c:139


Core  1 register dump:
PC      : 0x4037bd26  PS      : 0x00060434  A0      : 0x8037f4f0  A1      : 0x3fca9770  
--- 0x4037bd26: esp_cpu_wait_for_intr at /home/ivan/.espressif/v5.5.2/esp-idf/components/esp_hw_support/cpu.c:64
A2      : 0x00060423  A3      : 0x00000000  A4      : 0x00060420  A5      : 0x3fca96a0  
A6      : 0x00060820  A7      : 0x3fca995c  A8      : 0x8037935d  A9      : 0x3fca9730  
A10     : 0x3fc9e028  A11     : 0x3fca9760  A12     : 0x00060820  A13     : 0x00060423  
A14     : 0x60023000  A15     : 0x0000abab  SAR     : 0x0000001d  EXCCAUSE: 0x00000005  
EXCVADDR: 0x00000000  LBEG    : 0x00000000  LEND    : 0x00000000  LCOUNT  : 0x00000000  


Backtrace: 0x4037bd23:0x3fca9770 0x4037f4ed:0x3fca9790 0x40381208:0x3fca97b0 0x40384c92:0x3fca97d0 0x40382141:0x3fca97f0
--- 0x4037bd23: xt_utils_wait_for_intr at /home/ivan/.espressif/v5.5.2/esp-idf/components/xtensa/include/xt_utils.h:82
--- (inlined by) esp_cpu_wait_for_intr at /home/ivan/.espressif/v5.5.2/esp-idf/components/esp_hw_support/cpu.c:55
--- 0x4037f4ed: esp_pm_impl_waiti at /home/ivan/.espressif/v5.5.2/esp-idf/components/esp_pm/pm_impl.c:1087
--- 0x40381208: esp_vApplicationIdleHook at /home/ivan/.espressif/v5.5.2/esp-idf/components/esp_system/freertos_hooks.c:56
--- 0x40384c92: prvIdleTask at /home/ivan/.espressif/v5.5.2/esp-idf/components/freertos/FreeRTOS-Kernel/tasks.c:4350
--- 0x40382141: vPortTaskWrapper at /home/ivan/.espressif/v5.5.2/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/port.c:139




ELF file SHA256: cb90fa04d

Rebooting...
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0xc (RTC_SW_CPU_RST),boot:0x2f (SPI_FAST_FLASH_BOOT)
Saved PC:0x403813e5
--- 0x403813e5: esp_restart_noos at /home/ivan/.espressif/v5.5.2/esp-idf/components/esp_system/port/soc/esp32s3/system_internal.c:164
SPIWP:0xee
mode:DIO, clock div:1
load:0x3fce2820,len:0x173c
load:0x403c8700,len:0xe80
--- 0x403c8700: _stext at ??:?
load:0x403cb700,len:0x31cc
entry 0x403c8948
--- 0x403c8948: call_start_cpu0 at /home/ivan/.espressif/v5.5.2/esp-idf/components/bootloader/subproject/main/bootloader_start.c:25
I (24) boot: ESP-IDF v5.5.2-dirty 2nd stage bootloader
I (24) boot: compile time Sep 10 2026 21:17:19
I (24) boot: Multicore bootloader
I (25) boot: chip revision: v0.2
I (25) boot: efuse block revision: v1.3




Okay, so it managed to flash even tho you got cut off.

crashed when i tryed to type gps_dynmode after gps_monrf. Ok so bench is immanent blocker for this.
what is crashing i2c again? we are still missing the button for dynmode

I (35052) BOOT_DISP: sleep -- panel to DISPOFF+SLPIN
I (109209) MAX_M10S: DYNMODEL -> AIR<1G (6) queued to UART
[GPS] DYNMODEL <- AIR<1G (6) : ESP_OK
      watch DEBUG log for UBX-ACK-NAK if not supported.
[GPS] DYNMODEL = AIR<1G (6)

Also, if safeboot is held high, would that keep the chip off? I think i floated everything, vcc and vio to 3v3, only safeboot is left high at 3v3. or would the chip show signs of life even if safeboot held high?

seems to work fine when not core panicking. Pick up where you left, close stage 31 after i flash and confirm, and prepare 32md file for a fresh session