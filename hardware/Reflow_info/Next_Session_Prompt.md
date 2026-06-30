# Next-session prompt — copy/paste into a fresh chat

---

I'm working on the Kompic Mk I wearable (hardware iv7.1, firmware baseline iv7.1.f0.0). The board is on the bench, I've been through stages 1, 2, and 3 of bring-up with a previous Claude session. The full session history is captured in `hardware/Reflow_info/Stage_3_Build_Report.md`. **Read that report before doing anything else** — it documents 5 critical PCB-level bugs found during stage 3, how each was diagnosed, and what was bench-fixed vs left open.

## Where I am now

Stage 3 closed with 4/5 critical bugs resolved on the bench and **1 unresolved: the SD card.** SDMMC 1-bit mode never completes init. I've already swept every passive variable I can:

- Drive strength bumped to GPIO_DRIVE_CAP_3
- Pull-ups taken 10 kΩ → 5.1 kΩ → 3 kΩ on CMD/DAT0
- Clock swept 0.4 / 1 / 4 MHz
- Three different cards (128 MB SDSC, 32 GB SDHC, 64 GB SDXC), all healthy on a PC reader
- GPIOs bit-bang-verified clean (chip itself is fine)
- Init always dies at ACMD51 (`send_scr` 0x107 timeout) or occasionally later at FAT `mount_to_vfs`

Borderline bus signal integrity. Read `§4.1` of the Stage 3 report for the full table of what was tested. **The conclusion was: SDMMC 1-bit on this PCB is unrecoverable without either a respin or moving to SPI mode.**

## What I want to do next session

**Switch the SD card to SPI mode.** Arduino-esp32 `<SD.h>` library, SPI bus pinned to the existing SDMMC traces (CLK→SCK, CMD→MOSI, DAT0→MISO), plus one CS pin. I have no free GPIOs — the plan is to **free GPIO0** by tying DRV2605L's EN pin permanently to +3V3 via a 0 Ω jumper, and bodge a wire from GPIO0 to socket pin 2 (currently DAT3/unused). Trade-off is losing firmware-controlled DRV2605L shutdown for low-power modes, which I'm fine with for v0.

Specifically I want you to:

1. **Write a focused SPI SD test sketch** at `firmware/test/sd_spi_pump_test_mk1/sd_spi_pump_test_mk1.ino`. Same retry-loop structure as the existing `firmware/test/sd_pump_test_mk1/sd_pump_test_mk1.ino` (which is the SDMMC version) — repeated `SD.begin()` attempts with logging, smooth LED breathe heartbeat on GPIO41, no other peripherals. Use 1 MHz SPI clock to start. Reuse the same scope-friendly format.
2. **Once SPI works**, integrate the SPI SD path into the main stage-3 smoke sketch at `firmware/test/3_smoke_test_mk1_hotair/3_smoke_test_mk1_hotair.ino`, replacing the SDMMC path. Keep the boot-summary log line writing to `/smoke_log.txt` and the `.txt` file counting in the dwell loop.

## Other follow-ups (in priority order, do these AFTER SD is sorted)

1. **Replace MAX30101.** Currently dead — fried by the +1V8 overvoltage event documented in §4.8 / §4.9. Now that the LDO and VDDIO rework is done and +1V8 measures clean 1.8 V, it's safe to fit a fresh chip. The smoke sketch already has its probe + dwell — should just light up green on next boot.
2. **DRV2605L auto-cal still trips OC_DETECT.** Currently fails with DIAG_RESULT=1 despite the dropped RATED_VOLTAGE / OD_CLAMP values. The motor still vibrates on library effects (chip falls back to open-loop). I want to know if the cal can be made to PASS by dropping the values further, or if there's something else going on. See §4.4.
3. **Encoder polarity not yet bench-verified.** Sketch has `ENC_INVERT = 0`; might need flipping after testing rotation.
4. **R26 restock** (47 Ω 0402, flashlight series resistor). Once back in place, lift `LED_MAX_DUTY` cap from 64 → 255 in the sketch.

## Things to know about working with me

- **The project memory file at `~/.claude/projects/-home-ivan-Projekti-Elektronika-Kompic-Wearable-kompic-wearable/memory/project_kompic_mk1.md`** has the project context (iv7.1 hardware, fw baseline, commit policy, etc.). Read that too.
- **I commit my own work** — don't make commits on my behalf. Prepare diffs and let me commit.
- **Don't trust the AI-generated extracts under `firmware/docs/datasheet_extracts/` blindly.** They've had load-bearing errors (flashlight topology in 20.17 said low-side, actual schematic was source-follower). When the schematic and the extract disagree, the **schematic .kicad_sch files are canonical**. The PDFs in `docs/datasheets/` are also canonical.
- The whole iv7.1 PCB has known bugs (5 critical found in stage 3 alone — see report). Verify against schematic and datasheet, not the docs/extracts.
- It's iv7.1, not v7.2. Anywhere you see "v7.2" referenced in old memory, that's a stale name.

## Pointers

- Stage 3 report: `hardware/Reflow_info/Stage_3_Build_Report.md` — **start here**
- Stage 2 report: `hardware/Reflow_info/Stage_2_Build_Report.md` — prior context
- Master pinout: `hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md`
- Current smoke sketch: `firmware/test/3_smoke_test_mk1_hotair/3_smoke_test_mk1_hotair.ino`
- SDMMC pump debug sketch: `firmware/test/sd_pump_test_mk1/sd_pump_test_mk1.ino`

Read the report, then ask me whether I'm ready to wire the GPIO0 / socket-pin-2 bodge for SPI CS, or whether I want you to draft the SPI sketch first so I have it ready before doing the bodge.
