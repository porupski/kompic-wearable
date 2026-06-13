# OPUS BATCH ASSIGNMENT INDEX
**Kompic̄ Mk I Firmware v7.2.f0.0**

---

## QUICK START

1. **One-time setup:** Paste `docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md` into your Opus chat. Opus reads it, acknowledges.

2. **Then run batches in order** (you can run one per day, one per week, or in parallel — your call):

| Phase | Batch file | Modules | Estimated effort | Status |
|-------|-----------|---------|------------------|--------|
| 0.1 | Already done | CO5300 (display) | ✓ | Complete |
| 0.2 | `PHASE_0_BATCH_2_TOUCH_RTC.md` | CST9217 (touch), PCF85063 (RTC) | 1–2 days | Ready |
| 1 | `PHASE_1_BATCH_POWER_BATTERY.md` | BQ25619 (charger), BootPower (button) | 1–2 days | Ready |
| 2 | `PHASE_2_BATCH_SENSORS_1.md` | LSM6DSV16X (IMU), LIS3MDLTR (compass), VEML6030 (light) | 2–3 days | Ready |
| 3 | `PHASE_3_BATCH_SENSORS_2.md` | MAX-M10S (GPS), BME688 (env), MAX30101 (HR), TMP117 (temp) | 2–3 days | Ready |
| 4 | `PHASE_4_BATCH_ACTUATORS.md` | DRV2605 (haptic), CrownEncoder, WS2812 (LED), Flashlight | 2–3 days | Ready |
| 5 | `PHASE_5_BATCH_ADVANCED.md` | SDCard, PDM Mic, Qvar ECG | 1–2 days | Complete |
| 6 | `19_PHASE_6_BATCH_INFRASTRUCTURE.md` | data_broker, cross_driver, app_logic, fusion, alarm, lvgl_ui | 3–5 days | Ready |

---

## HOW TO RUN A BATCH

**Example: Phase 0.2 (Touch + RTC)**

1. Copy the contents of `PHASE_0_BATCH_2_TOUCH_RTC.md`
2. Paste into your Opus chat (in the same conversation where you pasted the master prompt, or a fresh one)
3. Opus reads it, extracts module details, generates 2 dated .md files + driver skeletons + test harnesses
4. Opus commits at EOD

**Output:** 
- `/docs/porting/CST9217_2026-06-10_iv7.2.f0.0.md`
- `/docs/porting/PCF85063_2026-06-10_iv7.2.f0.0.md`
- `/components/cst9217/`, `/components/pcf85063/` (drivers + tiles)
- `/test/test_cst9217.c`, `/test/test_pcf85063.c` (test harnesses)
- One commit per module (or one commit at the end)

---

## RECOMMENDED WORKFLOW

**Option A: Sequential (safer, easier to debug)**
- Day 1: CO5300 (done)
- Day 2: Phase 0.2 (touch + RTC)
- Day 3: Phase 1 (power + battery)
- Day 4: Phase 2 (IMU + compass + light)
- Day 5: Phase 3 (GPS + env + HR + temp)
- Day 6: Phase 4 (haptic + LED + flashlight + crown)
- Day 7: Phase 5 (SD + mic + ECG, optional)

**Option B: Parallel (faster, needs coordination)**
- Give Opus multiple batches in parallel (one per separate chat or conversation thread)
- Merge into repo as they complete
- Risk: merge conflicts if they step on each other (unlikely, since they touch different folders)

**I recommend Option A** — one batch at a time, sequential. Easier to debug if something goes wrong.

---

## WHAT EACH BATCH OUTPUTS

Each batch produces:
- **1 dated .md per module** (e.g., `CST9217_2026-06-10_iv7.2.f0.0.md`) with:
  - Summary + hardware spec
  - Code audit (what's reusable)
  - Implementation skeleton
  - Test harness design
  - Profiling template (TBD for now, filled on real hardware)
  - Defects discovered
  - Open questions (if any)

- **Driver skeleton files** in `/components/[module]/`:
  - `[module].{c,h}` (Core 0 driver)
  - `[module]_tile.{c,h}` (Core 1 LVGL tile, if applicable)
  - `CMakeLists.txt`

- **Test harness** in `/test/test_[module].c`:
  - Standalone, runnable on any ESP32
  - No full firmware needed
  - Exercises the module in isolation
  - Measures basic performance (boot time, I2C latency, etc.)

- **One git commit per module** (or batched at the end)

---

## FILES TO DOWNLOAD / COPY TO YOUR REPO

All batch files are ready. Copy them to your repo:

```bash
# From the outputs of this task
cp PHASE_*.md ~/Kompic_Mk1_firmware/docs/
cp KICKOFF_CO5300_AMOLED_DISPLAY.md ~/Kompic_Mk1_firmware/docs/11_KICKOFF_CO5300_AMOLED_DISPLAY.md
cp HOW_TO_USE_OPUS_INSTANCE.md ~/Kompic_Mk1_firmware/docs/

# Already in your repo
# docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md
```

Then you're ready to start feeding batches to Opus.

---

## WHEN TO RUN EACH BATCH

- **Phase 0.2 (Touch + RTC):** Immediately after CO5300. Needed for display to be interactive + time display.
- **Phase 1 (Power + Battery):** After Phase 0.2. Needed to manage shutdown safely + monitor battery.
- **Phase 2 (Sensors 1):** After Phase 1. IMU for motion detection, light for auto-brightness (improves UX).
- **Phase 3 (Sensors 2):** After Phase 2. GPS, environment, HR all improve the device's utility.
- **Phase 4 (Actuators):** After Phase 3. Crown encoder, haptic, LED, flashlight complete the device.
- **Phase 5 (Advanced):** Optional for v1. SD + mic + ECG are nice-to-have, not essential.

---

## TROUBLESHOOTING

**Opus asks a hardware question?**
- Check `Kompic_Mk1_System_Instructions_v7.2.md` for the answer
- If v7.2 is silent, ask Ivan (post the question in the .md's "Open questions" section)
- Never have Opus guess

**Defects discovered?**
- Opus logs them in the .md as `[DEFECT-NN]`
- Review them; decide: block hardware test, or defer to later phase?
- Log disposition in the .md

**Module won't compile?**
- Likely a missing component or header file
- Opus should note it in "Open questions"
- Blocking issue → wait for clarification before moving to next batch

---

## RECAP

- **Master prompt** (one-time): sets the rules
- **Batch files** (6 total): each assigns 2–4 modules to Opus
- **Output** (per batch): dated .md files + driver skeletons + test harnesses + commits
- **Workflow:** sequential is safer; parallel is faster but riskier

You're ready. Pick a start date, give Opus the master prompt, then hand it batches one at a time (or all at once, if you're confident in parallel execution).

Go.
