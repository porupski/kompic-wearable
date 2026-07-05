# SDMMC 1-bit Rescue Plan — Kompic Mk I (iv7.1)

> ## ⚠ SUPERSEDED 2026-07-05
>
> **Iv7.1 SD bus is functional as-designed. This rescue plan is unnecessary.**
>
> On 2026-07-05, testing with a 32 GB SDHC (msdos/FAT32) and a 128 MB SDSC (msdos/FAT16) card produced **65 / 65 PASSes at 400 kHz, 1 MHz, 4 MHz, AND 20 MHz** with the original iv7.1 wiring — no 1 µF cap at the socket, no GND bodge, no CLK series-R. The bus tolerates 3 cm stub wires on CLK/CMD/DAT0 without regression.
>
> Root cause of all the failures documented below: **a dying 64 GB Kodak AliExpress SDXC card**, which was also formatted exFAT (unsupported by arduino-esp32 SD_MMC by default → `mount_to_vfs 0xffffffff`). The deterministic 643 ms timeout at `send_scr` was the card's own internal firmware timing out at the same step every attempt, not a bus time-constant.
>
> EXP-0 through EXP-5 below are all **not needed** for iv7.1. This document is preserved for archaeology and as a lesson in over-attributing to the bus what belongs to the component under test.
>
> See `Stage_4_Build_Report.md` for the corrected final state and the diagnostic autopsy. Auto-memory: `feedback_suspect_cheap_sd_cards.md`.

---

**Created:** 2026-06-29
**Last updated:** 2026-06-29 (post-EXP-3 desk run; failure mode reclassified)
**Status:** EXP-3 completed at the desk. Failure mode is now reclassified as a *deterministic single-line electrical fault*, not the borderline-signal-integrity story carried over from Stage 3. Priority experiments reordered accordingly — see § 3.
**Goal:** Make SDMMC 1-bit init complete reliably on the iv7.1 PCB *without* sacrificing GPIO0 / DRV_EN to an SPI-mode CS pin. SPI mode remains the fallback if everything in this plan fails.

---

## 1. Why this plan exists

Stage 3 closed with SDMMC 1-bit not enumerating across every passive variable that can be swept from the bench (clock 0.4/1/4 MHz, pull-up 10k/5.1k/3k, drive cap 2/3, three cards). The `Next_Session_Prompt.md` framed the next step as a hard pivot to SPI mode. But SPI requires a CS pin, and the only realistic GPIO to free is DRV_EN — losing it kills firmware-controlled DRV2605L power-down, which is on the critical path for low-power modes later.

So before committing to SPI, this plan exhausts the higher-confidence SDMMC failure modes that were *not* tested in stage 3. Each item is a discrete experiment with a clear pass/fail signature and a decision criterion.

## 2a. UPDATE 2026-06-29 — failure mode is a regression, not a marginal bus

The pump test was reflashed at the desk with `gpio_get_drive_capability()` instrumented at three checkpoints (`setup-end`, `pre-begin`, `post-begin`). 41 attempts captured. **Every single attempt:**

- `CLK=3 CMD=3 D0=3` at all three checkpoints — drive cap survives the library call. EXP-3 ruled in: **drive strength is not the bottleneck.**
- Fails at `sdmmc_init_sd_scr: send_scr (1) returned 0x107`.
- Total elapsed: **exactly 643 ms**, attempt after attempt.
- Clock target: 400 kHz / 1 MHz / 4 MHz rotated round-robin. **All three clocks fail identically with the same timing.**
- Pass count: **0 / 41.**

This is a *much* harder fail signature than Stage 3 reported. Stage 3 §4.1 row #4 noted "occasional mount_to_vfs" — meaning init *sometimes* completed and the failures rolled forward to the FAT layer. Now init fails deterministically at ACMD51 (the first DAT0 data-block command), at the same elapsed time, on every clock. **This is a regression from Stage 3.**

Implications:

- Borderline RC signal-integrity stories don't fit. A bus that's *almost* working would show occasional passes, varying failure points, clock-sensitive behavior. We see none of that.
- A clock-independent, time-deterministic fail at the first command that reads from DAT0 points at **DAT0 being electrically non-functional** between the SD socket and the ESP. Either an open (cold joint, hairline crack), a short to a rail, or a card that simply isn't asserting DAT0 even when commanded.
- EXP-1 (1 µF at socket) and EXP-2 (GND bodge) are still good hygiene for the bus, but they will **not** fix a broken/disconnected DAT0. They address droop and return-path issues — both of which would manifest as intermittent failures, not the absolute-determinism wall we now see.

What likely changed between Stage 3 and now:
- Mechanical knock on the SD socket loosened a spring-tab solder joint.
- A solder ball / whisker from recent rework (LDO swap, +1V8 cut at C29) landed near the SD lines.
- The card currently inserted may differ from the three cards tested in Stage 3.

## 2b. Analysis pointing to what's actually failing

SD traces on iv7.1 are short: CLK = 6 mm, CMD/DAT0 ≈ 15 mm (longest pad-to-pad). Lumped trace capacitance ~1.5 pF per line. Total bus capacitance per line, including endpoints, is therefore ~20–25 pF — not enough by itself to explain the RC-shaped edges seen at the bench. That moves the suspect list off "trace capacitance" and onto:

| Suspect | Why | Confidence |
|---|---|:-:|
| **VDD droop at the SD socket during data-block surges** | 100 nF decoupling is populated but on the *opposite* face of the PCB (within 2–3 mm). For a card that pulls 50–100 mA transients during ACMD51 data-block reads, that's effectively no local decoupling. Droop on 3V2 mis-samples the card's internal logic → fails at the first data block. | High |
| **Ground return path discontinuity** | SD socket sits under the ESP module. Return current has to find its way through the plane via fence. Any cutout / slot / moat between them inflates loop area and produces L·di/dt ringing that looks like slow rise on the scope. | Medium |
| **GPIO drive capability not sticking through `SD_MMC.begin()`** | Library calls `sdmmc_host_init_slot()` inside `begin()`, which reconfigures the GPIO matrix and **may reset FUN_DRV back to CAP_2 (~20 mA)** before the first SDMMC transaction. Sketch's pre-begin `gpio_set_drive_capability(CAP_3)` is wiped; post-begin call is too late — init has already failed. At CAP_2 into ~25 pF the slew rate drops enough to make CLK look "checkmark"-shaped at 4 MHz. | Medium |
| **CLK transmission-line stub / via-stack capacitance** | One via per pin through GND and 3V2 planes adds ~0.5 pF/via — small per pin, but cumulative if the via stack reaches inner-layer routing. | Low |

Explicitly **ruled out** (evidence in Stage 3 §4.1, recent confirmations):
- Long trace capacitance — traces are 6/15 mm.
- ESD/TVS diodes on the bus — none populated.
- Test points on the bus — none.
- Shared caps with other rails — SD lines are clean.
- DAT1/DAT2 floating — 10 kΩ pull-ups already populated.
- Cards — three cards spanning SDSC/SDHC/SDXC, all healthy on PC reader, all fail identically here.
- ESP32-S3 silicon — bit-bang on the same GPIOs produces clean rail-to-rail squares.

## 3. Bench experiment plan (priority order — REVISED 2026-06-29)

The original plan ordered EXP-1 → EXP-2 → EXP-3 (drive cap) on the assumption of borderline signal integrity. The EXP-3 desk run inverted that hypothesis: drive cap is fine, but the failure is now perfectly deterministic. **EXP-0 (meter checks) is inserted at the top** because a single-line electrical fault is now the most probable cause and a 5-minute multimeter session can definitively confirm or rule it out before any soldering happens.

Run these in this order. Each step has a defined pass criterion that lets you stop early if it works, or a defined failure tell that says "move on."

### EXP-0: Meter triage on DAT0 (do this FIRST when back at the bench)

**Why this is now first:** the post-EXP-3 fail signature looks exactly like DAT0 being open between the socket and the ESP. Three quick meter checks resolve this before any rework. Total time ≈ 5 minutes, no soldering.

**Check 0.1 — continuity, card removed:** probe SD socket pin 7 (DAT0) to GPIO40 pad on the ESP module with the multimeter in low-Ω mode. Expect <1 Ω. **If open or >10 Ω → cold joint at one end.** Most likely failure mode: socket pin 7 solder pad cracked from mechanical handling.

**Check 0.2 — DAT0 idle voltage, card inserted, firmware idle (or running):** probe DAT0 net at any reachable point (socket pin 7 or GPIO40 pad). Expect ~3.2 V (external 10 k pull-up to 3V2). **If 0 V → shorted to GND. If 3.2 V and unresponsive during init → line is held high, not floating.**

**Check 0.3 — try a different card.** Use a card from Stage 3 that previously got at least *occasional* mount_to_vfs passes. If symptom is identical → bus side. If symptom changes → card side.

**Pass criterion:**
- Check 0.1 confirms continuity AND
- Check 0.2 shows healthy idle pull-up AND
- Check 0.3 shows symptom invariant across cards
→ DAT0 line is electrically clean. The bus-quality experiments (EXP-1 / EXP-2) become relevant again. Continue.

**Failure tell + immediate fix:**
- Check 0.1 fails (continuity open) → **reflow the bad joint with iron + flux.** Re-run pump test. If passes now: regression resolved, original signal-integrity story stands (EXP-1/EXP-2 still worth doing for robustness).
- Check 0.2 shows DAT0 at 0 V → solder bridge near the SD lines from recent rework. Inspect under magnification, wick the bridge.
- Check 0.3 shows a different card behaves differently → card-side; not a board problem. Stop chasing the bus.

### EXP-1: Local 1 µF cap directly on SD socket VDD ↔ GND pins

**Why this is first:** addresses the highest-confidence suspect (VDD droop), cheapest to do, lowest risk. The existing 100 nF is 2–3 mm away on the *opposite face* of the PCB — same plane via stack, but the inductance of crossing planes for a fast transient kills the decoupling.

**How:**
- 1 µF X7R 0402 or 0603, soldered **directly bridging the VDD pin and the nearest GND pin** on the SD socket — bridge it across the pads with the cap body. No traces, no via, the cap *is* the connection.
- If you have 100 nF 0402s to spare, stack one on top of (or right next to) the 1 µF — high-frequency PSRR helper.

**Pass criterion:**
- Pump test with default settings (5.1 kΩ pull-ups, CAP_3 drive, 1/4/0.4 MHz sweep) → at least 1 PASS in 20 attempts where it never passed before.
- **Bonus probe:** scope CH1 on 3V2 *at the socket VDD pin*, trigger off CMD falling edge or LED ramp rising edge, run an init attempt. Look at the moment ACMD51 fires — if there's >100 mV droop synchronous with the data-block transfer, this experiment is exactly the right one. If droop is <50 mV, this isn't your problem.

**Stop criterion if fail:** if pump test still 0/20 with this cap fitted, move to EXP-2.

### EXP-2: GND bodge wire — SD socket GND pin → ESP module GND pin

**Why second:** addresses the second-highest-confidence suspect (return path). Cheap, fast, reversible.

**How:**
- Thin enameled magnet wire (or 0.2 mm Kynar). One end to SD socket GND contact pad (any of the metal-shell solder tabs is fine if the actual GND pin is hard to reach). Other end to the nearest ESP32-S3-WROOM-1U GND pad on the module periphery.
- **Make the route as short and direct as possible** — go *over* components if needed, don't snake around. We're trying to shortcut the plane discontinuity, not add another long path.

**Pass criterion:**
- Same pump test → PASS rate measurably improves. Even sporadic PASSes (1–5 in 20) are a strong signal that return path is in the failure chain.

**Stop criterion if fail:** if pump test 0/20 again with both EXP-1 and EXP-2 in place, move to EXP-3.

### EXP-3: Verify CAP_3 actually sticks through `begin()` (firmware-side, can run before going to bench)

**STATUS: DONE 2026-06-29. Result: drive cap survives the library call. Not the bottleneck. EXP-4 (IDF-direct mount) is no longer triggered by this path.**

**Original procedure (kept for reference):** purely software, no bench required. The current sketch (2026-06-29) prints `gpio_get_drive_capability()` readback at three checkpoints: `setup-end`, `pre-begin`, `post-begin`.

**Result:** all 41 attempts printed `CLK=3 CMD=3 D0=3` at all three checkpoints. CAP_3 (~40 mA) is active on the very first transaction. The library does *not* reset FUN_DRV. Drive strength is conclusively not the contributor to the current failure mode.

### EXP-4: IDF-direct mount, with drive cap forced between init_slot and card_init

**Why fourth:** only worth doing if EXP-3 shows drive cap is being reset by the library. Requires bypassing `SD_MMC.begin()` and calling the IDF functions directly so we can pin the drive bits between `sdmmc_host_init_slot()` (which configures GPIO) and `sdmmc_card_init()` (which fires the first transactions).

**How (sketch-side, ~30 lines):**
```c
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

sdmmc_host_t host = SDMMC_HOST_DEFAULT();
host.flags = SDMMC_HOST_FLAG_1BIT;
host.slot  = SDMMC_HOST_SLOT_1;
host.max_freq_khz = clk_khz;

sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
slot.width = 1;
slot.clk = (gpio_num_t)PIN_SD_CLK;
slot.cmd = (gpio_num_t)PIN_SD_CMD;
slot.d0  = (gpio_num_t)PIN_SD_D0;
slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;

ESP_ERROR_CHECK(sdmmc_host_init());
ESP_ERROR_CHECK(sdmmc_host_init_slot(host.slot, &slot));

// >>> Drive cap forced here, after slot init, before card init. <<<
gpio_set_drive_capability((gpio_num_t)PIN_SD_CLK, GPIO_DRIVE_CAP_3);
gpio_set_drive_capability((gpio_num_t)PIN_SD_CMD, GPIO_DRIVE_CAP_3);
gpio_set_drive_capability((gpio_num_t)PIN_SD_D0,  GPIO_DRIVE_CAP_3);

sdmmc_card_t card;
esp_err_t err = sdmmc_card_init(&host, &card);
// ... then esp_vfs_fat_sdmmc_mount() if you want FAT, or just leave it
//     mounted at the host level for raw block tests.
```

**Pass criterion:** card enumerates and reports correct size / type at least once.

**Stop criterion if fail:** SDMMC is structurally broken on this PCB and won't be saved by drive-strength tuning. Move to EXP-5.

### EXP-5 (last resort): Lift socket, fly-wire SD card with 23 Ω series on CLK

**Why last:** heavy rework, irreversible-ish, only worth it after 1–4 are exhausted. Adds a 22–47 Ω series R on CLK between the ESP and the card to damp ringing — counterintuitive but the right answer for an LC-resonant bus. If 2× 47 Ω in parallel is what you have, that's 23.5 Ω, ideal range.

**How:**
- Hot-air the SD socket off the board.
- Solder the SD card via short wires (CLK, CMD, DAT0, VDD, GND) directly to the iv7.1 pads.
- Insert 2× 47 Ω 0402 in parallel inline on CLK only, as close to the ESP side as possible.
- Re-run pump test.

**Pass criterion:** pump test starts passing reliably (>50 % of attempts).

**If still fail:** the bus genuinely can't be saved on iv7.1 hardware. Hard pivot to SPI — that's when DRV_EN gets sacrificed.

## 4. Probe points to capture during each experiment

When at the scope, capture these for the working / failing comparison archive (single-channel scope, the iv7.1 standard):

| Probe | Where | Trigger | Look for |
|---|---|---|---|
| 3V2 | SD socket VDD pin (under cap, not at the IC end of the rail) | LED ramp rising edge | Droop magnitude during ACMD51 data block. >100 mV = EXP-1 is right. |
| CLK | GPIO38 pad at the ESP side | CMD falling edge ~1.6 V | Rise time, overshoot, undershoot. Healthy = <10 ns rise, <0.5 V overshoot. RC-shaped = drive strength or hidden cap. |
| CMD | GPIO39 at the socket end | CMD falling edge | Tri-state behavior during direction change between host→card and card→host. Should rise to >2.5 V within bit period. |
| DAT0 | GPIO40 at the socket end | start of ACMD51 (manual scope arm) | Push-pull or open-drain shape during the data block. Slow rise during the data block = card driving open-drain (spec violation by a cheap card) or external pull-up dominating. |

## 5. Decision tree (REVISED 2026-06-29 — EXP-0 inserted at top after the post-EXP-3 fail reclassification)

```
EXP-0 (meter checks on DAT0) → continuity + healthy idle + card-invariant?
  ├─ NO, Check 0.1 fails → reflow joint → re-run pump → PASS?
  │     ├─ YES → done. Regression resolved. Run EXP-1/EXP-2 for robustness.
  │     └─ NO  → EXP-1 →
  ├─ NO, Check 0.2 shows DAT0 = 0 V → fix solder bridge → re-run pump → PASS?
  │     ├─ YES → done.
  │     └─ NO  → EXP-1 →
  ├─ NO, Check 0.3 different card behaves differently → card was the cause →
  │     pick a working card; revisit the original story only if all cards fail.
  └─ YES → DAT0 line is electrically clean → continue:
        EXP-1 (1 µF at socket) → PASS?
          ├─ YES → done. Update Stage 3 report § 4.1. Move on to MAX30101 / MAX17048.
          └─ NO  → EXP-2 (GND bodge wire) → PASS?
                    ├─ YES → done. Return-path discontinuity is the cause,
                    │        document for v8 layout review.
                    └─ NO  → EXP-3 is already done (drive cap not the issue) →
                              EXP-4 won't help (it relied on EXP-3 failing) →
                              skip to EXP-5 (lift socket, fly-wire, 22 Ω on CLK)
                              → PASS?
                                ├─ YES → done. iv7.1 SD requires the off-board
                                │        fly-wire fix; v8 needs full layout rework.
                                └─ NO  → accept SPI fallback, free GPIO0 from
                                         DRV_EN, write the SPI sketch.
```

## 6. Carry-over for v8 PCB regardless of outcome

Even if a bench fix resolves iv7.1, v8 needs all of these to avoid the same fight:

- **VDD decoupling at the SD socket:** 1 µF X7R + 100 nF, both on the *same face* of the PCB as the socket, vias-to-plane *adjacent* to the cap, not 2–3 mm away on the opposite face.
- **Default pull-ups 4.7 kΩ on CLK, CMD, DAT0** (not 10 kΩ). DAT1/2/3 can keep 10 k weak pull-ups for noise immunity.
- **CLK pull-up footprint populated by BOM, not bodge.** (iv7.1 had the footprint empty — caught and bench-fixed in Stage 3.)
- **Ground plane continuous between ESP module and SD socket.** No moats, no slots, no plane cutouts. Add a stitching via fence along both sides of the SDMMC bus traces to keep the return path tight.
- **CLK series-R footprint** (currently DNP, 0 Ω jumpered) populated with 22 Ω. Belt-and-suspenders for signal integrity at 20+ MHz target.
- **Route DAT3 to a real GPIO** *or* commit explicitly to never needing it. If kept for SPI-mode fallback option, route to a pin with no strapping conflicts.
- **Consider an alternate socket** — push-push contacts have variable spring impedance. Push-pull (slider) sockets exist with better contact reliability but cost more board space.

## 7. Firmware references

- Current pump test: `firmware/test/sd_pump_test_mk1/sd_pump_test_mk1.ino`
  - As of 2026-06-29: instrumented with `gpio_get_drive_capability()` readback at `setup-end`, `pre-begin`, `post-begin` checkpoints (EXP-3).
- Stage 3 smoke test (uses SD_MMC.begin only): `firmware/test/3_smoke_test_mk1_hotair/3_smoke_test_mk1_hotair.ino`
- arduino-esp32 SD_MMC library reference: `~/.arduino15/packages/esp32/hardware/esp32/3.2.0/libraries/SD_MMC/src/SD_MMC.cpp:226–246`

## 8. Update log

| Date | Experiment | Result | Notes |
|---|---|---|---|
| 2026-06-29 | Plan written, pump test instrumented (EXP-3 ready in firmware) | — | Bench experiments EXP-1 / EXP-2 pending next bench session. EXP-3 can be run any time at the desk. |
| 2026-06-29 | EXP-3 ran (sd_pump_test_mk1 at desk, battery on) | **Drive cap sticks at CAP_3 across all three checkpoints. 0 / 41 passes. Every attempt fails at `send_scr (1) returned 0x107` after exactly 643 ms, regardless of clock target (400 / 1000 / 4000 kHz).** Drive strength conclusively ruled out as contributor. | Failure mode reclassified: this is a **regression from Stage 3** (which had occasional passes). Single-line electrical fault on DAT0 is now top suspect. EXP-0 inserted as the first thing to do at the bench. |
