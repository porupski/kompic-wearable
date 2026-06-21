# Stage 3 Build Report — Kompic Mk I (iv7.1)

## CRITICAL BUG SUMMARY (running list — keep at top of report)

| # | Bug | Status | Section |
|:-:|---|---|---|
| 1 | Flashlight LED in N-FET source-follower topology — LED can't conduct | **RESOLVED on bench**; schematic fix needed for v8 | §4.2 |
| 2 | SD card init never completes — `send_scr` / `send_op_cond` / `mount_to_vfs` timeouts across every clock / pull-up / card combination tested | **UNRESOLVED** after exhaustive bench investigation. Likely needs SPI-mode fallback (no free GPIOs without freeing DRV_EN) or PCB respin with proper SDMMC pull-ups and trace layout. See §4.1 for the full debug history. | §4.1 |
| 3 | XC6206 LDO Vin/Vout swapped in library symbol — +1V8 rail at 2.3–2.6 V | **RESOLVED on bench** by rotating LDO + lead-bend (no traces cut); fresh chip regulates 1.8 V cleanly | §4.8 |
| 4 | LSM6DSV16X Vdd_IO and BME688 Vddio routed to +1V8 while I2C runs at +3V3 — chip ESD diodes back-fed the +1V8 rail to 2.3–2.8 V, prevented LDO regulation, contributed to MAX30101 over-voltage death | **RESOLVED on bench**: cut +1V8 branch near ref C29 (cut over the "9"), jumpered the cut-off side to +3V3. LSM/LIS/BME/mic all now on +3V3 supply. +1V8 rail now only feeds MAX (to replace) and M10S GPS | §4.9 |
| 5 | MAX30101 damaged by Bug #3+#4 over-voltage | UNRESOLVED — replace chip after verifying +1V8 = 1.8 V cleanly | §4.8 / §4.9 |
| 6 | BQ25619 /QON pull-up (R12, 5k1) routed to +3V3 instead of an always-alive rail — ship mode unconditionally self-wakes after ~1 s because 3V3 collapses to 0 V and R12 becomes a hard pull-down on /QON, tripping `t_SHIPMODE` (0.9–1.3 s, datasheet p. 25) | **RESOLVED on bench** by lifting R12. BQ's internal 200 kΩ to V_BAT (datasheet pin 12) is sufficient on its own — matches TI EVM topology. For v7.2 schematic: mark R12 as DNP | §8 |
| 7 | Flashlight LED leaks visible light when GPIO drives gate LOW — MOSFET I_DSS or LED leakage through R25+drain path; even sub-µA passes enough current to glow visibly in the dark | UNRESOLVED — likely needs a small drain-to-VDD bleed resistor on the LED cathode or a different low-leakage FET. Cosmetic, not blocking | §8 |

---


**Date:** 2026-06-20
**Board:** First Kompic Mk I prototype, PCB iv7.1
**Builder:** Ivan
**Stage:** 3 of 5 (post-stage-2 hot-air additions + daughter PCB)
**Prerequisite:** Stage 2 (USB-C JS16T, VEML6030, LIS3MDLTR, LSM6DSV16X, BME688,
MSM261DGT003 PDM mic, WS2812B-2020) was reflowed and verified PASS with
`firmware/test/2_smoke_test_mk1_hotair/`. Both I2C buses scan clean, all
chip-ID probes return correct values, mic samples vary, WS2812 walks RGB.

Stage 3 finishes board population: SD socket, DRV2605L haptic driver, ALPS
EC05E rotary encoder, and the daughter PCB (MAX30101 PPG + TMP117 temp).
After Stage 3 the watch has its complete sensor suite minus the battery and
the ECG / Qvar electrodes.

---

## 1. Population added this stage

### Main board (hot-air, top + underside)

| Designator | Part | Bus / pin | Address / signal |
|---|---|---|---|
| (SD socket) | microSD push-push TF | SDMMC 1-bit | CLK=GPIO38, CMD=GPIO39, DAT0=GPIO40 |
| U14 | DRV2605L (LRA haptic driver) | I2C **bus 2** (GPIO4/5) | 0x5A; EN = GPIO0 |
| (LRA motor) | ELV1411A coin LRA | DRV2605L OUT± | 2 V_RMS rated, 150 ± 5 Hz, 150 mA max |
| (Encoder) | ALPS EC05E1220401 | quadrature A/B | A = GPIO21, B = GPIO43, 12 PPR / 12 detents |

> **Physical bus assignment (PCB is canonical; KiCad hierarchy disagrees and
> should be updated for v8):**
> - **Bus 1 (east, sensors):** all daughter-PCB sensors + every Stage 2 sensor.
> - **Bus 2 (west, power):** BQ25619 (top of main board) + DRV2605L (underside
>   of main board). Just those two — nothing else.

### Daughter PCB (separate hot-plate reflow, then connector-attached)

| Designator | Part | Bus / pin | Address / signal |
|---|---|---|---|
| U13 | MAX30101 (PPG / pulse oximeter) | I2C **bus 1** (via daughter connector) | 0x57; INT = GPIO7 |
| U15 | TMP117 (precision temperature) | I2C **bus 1** (via daughter connector) | 0x48 or 0x49 (ADD0 strap) |
| (Electrodes) | Qvar / ECG pads | LSM6DSV16X Qvar input | electrical only — not yet tested |
| (ESD diodes) | ESDALCL5-1BM2 × 2 | across Qvar pads | bidirectional 5 V TVS, SOD882 |

Power rails affected: 3V3 (MAX, TMP, encoder pull-ups, SD socket VDD via the
3V2 LDO), 5V (MAX VLED via PMID).

---

## 2. Pre-power checks

Before plugging anything in:

1. **Daughter-connector continuity.** Probe each pin on the daughter PCB
   against its counterpart on the main board (3V3, 5V, GND, SDA_bus2,
   SCL_bus2, MAX_INT, TMP_ALRT, Qvar1/2). A single weak crimp on the FPC
   ribbon will give you the exact "always open" symptom we hit on the ESD
   diodes.
2. **SD-socket pull-ups.** Probe each of CLK / CMD / DAT0 to 3V2 with the
   meter in resistance mode (no card present). Each should read ~10 kΩ.
   **CLK had no pull-up populated on the iv7.1 PCB** — see §4.1. Hack one in
   before powering up.
3. **Encoder pull-ups.** Same check on A (GPIO21) and B (GPIO43).
4. **DRV_EN (GPIO0) idle state.** With power off, GPIO0 floats; with USB-C
   plugged in but firmware not booted yet, GPIO0 is held HIGH by the ESP32
   strap pull-up — that's correct, no action.
5. **Visual under magnification:** SOD882 ESD diodes are 1.0 × 0.6 mm — easy
   to tombstone or skew. Verify the cathode bar on each part.

---

## 3. First boot

Sketch: `firmware/test/3_smoke_test_mk1_hotair/3_smoke_test_mk1_hotair.ino`

Carries forward every Stage 2 check so regressions are visible, then adds:

- **DRV2605L identity + auto-calibration** at boot. Writes ELV1411A profile
  (RATED_VOLTAGE, OD_CLAMP, CTRL1, FEEDBACK, AUTO_CAL_TIME), enters MODE=7,
  fires GO, polls until self-clear, prints PASS / FAIL / TIMEOUT plus
  A_CAL_COMP / A_CAL_BEMF for the cluster.
- **Encoder ISR** on GPIO21 rising edge with 8 ms software debounce
  (6 ms mechanical bounce + 2 ms margin); ISR attached *after* boot summary
  so the ~3 ms U0TXD blip on GPIO43 is over.
- **MAX30101 probe + minimal HR-mode config**, FIFO pointers polled in
  dwell to verify the AFE is actually sampling.
- **TMP117 dual-address probe** (0x48 then 0x49), DEVICE_ID verified, temp
  read in dwell.
- **SD card boot moved to LAST init step** so all other peripherals settle
  before SDMMC negotiation. Clock dropped to 400 kHz (SD spec
  identification clock) after 1 MHz still failed signal integrity.

---

## 4. Issues encountered

### 4.1 SD card never enumerates — exhaustive bench investigation, UNRESOLVED

**Final state (2026-06-20):** SD card never completes init across *any*
combination of pull-up value, clock target, drive strength, or card. The
symptom set is consistent with **marginal SDMMC bus signal integrity that
no single passive change is enough to cross over.** Logged here in detail
so the next session can pick up either from the SPI-mode fallback or from
a v8 PCB respin without re-running every experiment.

---

#### Initial defect: CLK pull-up not populated on iv7.1 PCB

Voltage measurements at first boot (no card inserted):

| Line | Pull-up | Idle voltage |
|---|---|---|
| CMD (GPIO39) | 10 kΩ populated | 3.2 V ✓ |
| DAT0 (GPIO40) | 10 kΩ populated | 3.2 V ✓ |
| **CLK (GPIO38)** | **not populated** | **3.6 V (floating, meter coupling)** |
| DAT2 / DAT3 | 10 kΩ each | 3.2 V ✓ |

CLK pull-up footprint inspected and found empty on the PCB. BOM intent was
10 kΩ to 3V2 (same as CMD/DAT0). **Bench fix:** soldered a 10 kΩ 0402 from
the CLK net to a 3V2 via. After hack, CLK idles at 3.2 V with no card.

This fixed the immediate "no pull-up at all" defect but did not fix init.

---

#### Failure-point progression across every variable swept

Each row = one configuration tested with the `sd_pump_test_mk1.ino` retry
loop (or the smoke sketch). All at SDMMC 1-bit mode.

| # | Clock | Pull-ups (CMD/DAT0/CLK) | GPIO drive | Failure | Pass count |
|:-:|---|---|---|---|:-:|
| 1 | 40 MHz default | 10k / 10k / **none** | CAP_2 default | `send_csd` 0x108 INVALID_RESPONSE | 0 |
| 2 | 1 MHz | 10k / 10k / 10k hack | CAP_2 | `send_scr` 0x107 TIMEOUT | 0 |
| 3 | 400 kHz | 10k / 10k / 10k hack | CAP_2 | `send_op_cond` 0x107 TIMEOUT | 0 |
| 4 | 400 kHz | 10k / 10k / 10k hack | **CAP_3 (~40 mA)** | `send_scr` 0x107 + occasional `mount_to_vfs` | 0 / many |
| 5 | sweep 0.4/1/4 MHz | 10k / 10k / 10k | CAP_3 | `send_scr` 0x107 every attempt, every clock | 0 / 72 |
| 6 | sweep 0.4/1/4 MHz | **5.1k / 5.1k / 10k** | CAP_3 | `send_scr` 0x107 every attempt | 0 / many |
| 7 | sweep 0.4/1/4 MHz | **3 kΩ / 3 kΩ / 10k** | CAP_3 | `send_scr` 0x107 every attempt | 0 / many |

#### Cards tested

| Card | Capacity | Standard | PC-reader sanity | Result on board |
|---|:-:|---|---|---|
| Kodak | 64 GB | SDXC | Read/write OK on PC | Same fail pattern |
| Unbranded | 32 GB | SDHC | OK | Same fail pattern |
| Unbranded | 128 MB | SDSC | OK | Same fail pattern |

**Card variable eliminated** — three cards spanning the entire SD capacity
range, all functional on a PC reader, all fail identically on this board.

#### Scope observations along the way

- GPIO38 / 39 / 40 bit-banged directly (no SDMMC peripheral) → produce
  **clean rail-to-rail square waves**. Confirms the ESP32-S3 GPIOs are
  healthy and the chip itself isn't damaged. (Sketch in
  `firmware/test/sd_pump_test_mk1/sd_pump_test_mk1.ino` was used as the
  bit-bang scaffold during this test.)
- With GPIO_DRIVE_CAP_3 + 10 kΩ pull-ups → CLK shape is "checkmark"
  (sharp fall, slow RC rise). CMD and DAT0 are "smooshed square /
  spiky" — bits visible but never settling cleanly to rail.
- Drive bump 10 kΩ → 5.1 kΩ → 3 kΩ → progressively flatter, less RC-y,
  more "castle"-shaped square wave. **Signal quality is improving with
  each pull-up reduction** but does not cross over into successful
  init at any value down to 3 kΩ.
- At 400 kHz, signal looks cleaner than at 4 MHz (as expected from the
  RC math — longer bit period gives the line time to settle). But
  init still fails at all three clocks tested.

#### What was tried and didn't help

| Lever | Tried | Result |
|---|---|---|
| Clock slowdown | 40 MHz → 4 MHz → 1 MHz → 400 kHz | Failure point regressed earlier; never passed |
| GPIO drive strength | CAP_2 (default) → CAP_3 (max ~40 mA) | Bus signals visibly sharper, init progressed from CMD9 to ACMD51, never finished |
| Pull-up value | 10 k → 5.1 k → 3 k on CMD, DAT0 | Visibly sharper edges, no change in init success |
| Card variation | 3 cards, 128 MB / 32 GB / 64 GB | Identical failure |
| Bit-bang test | direct GPIO toggle bypassing SDMMC peripheral | Clean squares — confirms GPIO health |

#### Where it stops

Failures cluster at **`ACMD51` (SEND_SCR)** — the first command that
reads a data block from the card on DAT0. Init progresses through every
other step (CMD0, CMD8, ACMD41, CMD2, CMD3, CMD9, CMD7) cleanly. The
data-block transfer on DAT0 is what doesn't survive the bus signal
quality.

Occasional `mount_to_vfs` failures (with the same retry loop, different
attempts) show that *sometimes* init does complete and gets to the FAT
layer — proving that the bus isn't deterministically broken, just
unreliable. Borderline signal integrity, ~half of bits crossing VIH in
time, CRC roulette per attempt.

#### Why we're stuck

Three knobs determine whether a bit makes it: **pull-up R**, **bus
capacitance**, **card drive strength**. We've taken pull-up R from
10 kΩ all the way down to 3 kΩ and it's still not enough. Bus
capacitance is fixed by PCB layout (can't change without respin).
Card drive strength is fixed by the card's silicon (can't change at
all). We are at the floor of what passive components can fix.

#### Plan B (pending next session): SPI mode

The SD card supports a fallback **SPI mode** that is dramatically more
tolerant of signal integrity issues — the host drives every edge, MISO
is the only direction the card sends, and bit timing is host-pace not
synchronous-bus-pace. The arduino-esp32 `<SD.h>` library uses SPI.

**GPIO constraint:** SPI needs a CS pin in addition to SCK/MOSI/MISO.
The iv7.1 PCB has no free GPIOs in the current sketch — every pin is
allocated. The realistic options for freeing a CS pin:
- **Repurpose DRV_EN (GPIO0).** DRV2605L stays enabled by tying its
  EN pin to 3V3 via a 0 Ω jumper (or just bridging the pad), freeing
  GPIO0 entirely. Trade-off: lose firmware-controlled DRV power
  shutdown for low-power modes. Acceptable for v0 / smoke test.
- **Repurpose one of the I²C INT lines** (RTC_INT GPIO15, LSM_INT1
  GPIO8, etc.) — but these are required for wake-from-sleep, less
  willing to give up.

SPI pin mapping using existing SDMMC traces (no rework, just remap):

| SD socket pin | iv7.1 net | SDMMC role | SPI role |
|:-:|---|---|---|
| 5 | CLK / GPIO38 | CLK | **SCK** |
| 3 | CMD / GPIO39 | CMD | **MOSI** |
| 7 | DAT0 / GPIO40 | DAT0 | **MISO** |
| — | GPIO0 (freed from DRV_EN) | — | **CS** — needs a single bodge wire from GPIO0 to socket pin 2 (DAT3/CS) |

Sketch is straightforward — Arduino `SD.begin(CS, SPI)` with the SPI bus
pinned to those GPIOs. ~30 lines of code adapted from the SDMMC pump
test. Performance hit: SPI tops out around 25 MHz, easily 4× slower
than 1-bit SDMMC, but for logging (kB/s class) it's fine.

#### For v8 PCB respin

- Populate R_pullup on CLK from day one.
- Default all three SDMMC pull-ups to 4.7 kΩ, not 10 kΩ.
- Reroute SDMMC traces for minimum length and capacitance — keep CMD
  and DAT0 short, keep CLK away from parallel runs (crosstalk).
- Route DAT3 to the ESP32 (currently floats / shared with CD test pad)
  so the same hardware can do either SDMMC 4-bit or SPI without
  bodge wires.
- Consider whether the v8 SD socket has fewer mechanical solder issues
  — the iv7.1 push-push socket's spring contacts may also contribute
  some capacitance / contact noise (not measured).

### 4.2 Flashlight FET in source-follower topology — *RESOLVED on bench*

**Symptom:** LED never lights at any duty cycle. Replaced the BSS138W twice
on the suspicion that the FET was blown — same dead behaviour both times.

**Reading the schematic 5_Lights.kicad_sch directly** (not the AI-generated
20.17 extract, which is wrong about this — see §6 caveat):

```
+5 V → R25 (47 Ω) → R26 (47 Ω) → U12 Drain
                                 U12 Source → LED2 anode → LED2 cathode → GND
                                 U12 Gate ← R32 (47 Ω) ← GPIO41
                                          ↳ R24 (10 kΩ) → GND
```

That's an **N-channel source follower**, not a low-side switch. With
GPIO41 = 3.2 V, the FET's source pin pinches at V_gate − V_GS(th) ≈
1.5–2.0 V — well below the LED's Vf ≈ 3.0 V. The LED can never forward-
conduct in this topology, regardless of FET health.

**Bench measurements that confirm it:**

- With LED removed and the multimeter across the LED pads in current mode:
  4 mA. That's the FET operating weakly-on in its subthreshold region —
  consistent with source-follower clamp behaviour, *not* a fault.
- Diode-mode test, red on GND, black on the LED-source pad: OC both
  directions. This is **expected** for a healthy source-follower N-FET in
  this circuit because the body diode (anode = source, cathode = drain)
  can never be forward-biased through external probes from this geometry.
- Working reference on another circuit (low-side topology): red on GND,
  black on LED-source = 0.9 V; reverse = 0.6 V. Healthy readings, but
  *only* meaningful because that reference is in the *correct* topology.

So the previous FETs were never blown — they were just wired into a circuit
that can't drive the load.

**Proposed bench fix (still to perform):**

```
Cut:    U12-source-pad → LED2-anode-pad
Cut:    LED2-cathode-pad → GND
Jumper: U12-source-pad   → GND
Jumper: LED2-cathode-pad → U12-drain-pad
Restock: R26 (47 Ω 0402) — was lost during prior probing
```

End state, a textbook low-side switch:

```
+5 V → R25 → R26 → LED2_anode → LED2_cathode → U12_Drain → U12_Source → GND
```

Two cuts, two wire jumpers, one 0402 resistor put back. After the bodge
the existing BSS138W (any of the ones already on the bench, none were
killed) should drive the LED at the design 18 mA on full duty.

**Results to collect after the bodge:**

- Measured LED current at GPIO41 HIGH steady state — expect ~18 mA.
- Subjective brightness at 50 % / 75 % / 100 % LEDC duty.
- FET drain voltage with FET on — expect ~0.2 V (Rds(on) × 18 mA).

**Bench outcome (2026-06-20):** rework done — LED moved to the FET drain
side, FET source jumpered to GND. LED lights cleanly and brightly. **R26
was lost during prior probing**, so only R25 (47 Ω) is in series instead
of the design 2 × 47 Ω. At full duty this would push ~36 mA — above the
LED's 30 mA abs max. Mitigation in firmware (`3_smoke_test_mk1_hotair.ino`,
2026-06-20 edit): `LED_MAX_DUTY = 64` (25 %), applied to both the boot
brightness sweep and the loop's sine breathe. Average ≈ 9 mA, peak ≈
18 mA — well inside the LED rating and stays cool. **Restock R26** before
removing the duty cap.

**For v8:** redraw the FET symbol in `5_Lights.kicad_sch` so source is on
the GND rail; rerun ERC. This is the canonical low-side topology and
should never have left the schematic in source-follower form. Also: add
an assembly-fixture R26 stencil note so the part isn't lost during rework.

### 4.3 ESD diodes "always open" on daughter PCB — *not actually a fault*

**Symptom:** Five ESDALCL5-1BM2 diodes swapped onto the daughter PCB
Qvar ESD positions; multimeter diode-mode test reads OC in both directions
on every one of them. Reference part on a different board "shows a diode
drop."

**Root cause:** the ESDALCL5-1BM2 is a **bidirectional 5 V TVS** (two
back-to-back zeners between I/O1 and I/O2). A standard DMM diode-mode test
applies ~2–3 V — well below either zener's breakdown (5 V min one
direction, 11 V min the other, per `docs/datasheets/ESDALCL5-1BM2.pdf`
Table 2). Neither junction conducts → meter correctly reads OC in both
directions. **That's the expected reading on a healthy part.**

Leakage at 3 V is 10 nA typ (datasheet p. 2) — invisible without a
nanoamp-capable supply.

The reference part that "shows a diode" on another board is the *suspect*
one — a healthy ESDALCL5-1BM2 should not conduct at DMM test voltages.
That reading likely came from a chip-internal ESD diode on the same net,
not from the TVS itself.

**Status:** all five diodes lost to needless desoldering. No further parts
were on hand at the time of writing. **Restock and stop swapping based on
this test.** Proper validation needs a ≥10 V controlled source with a
current limit, a curve tracer, or a real ESD-gun event captured on scope.

**For v8:** add a note to the BOM line for the ESDALCL5-1BM2 saying
"healthy reads OC in DMM diode mode."

### 4.4 DRV2605L auto-cal trips OC_DETECT

**Symptom:** Auto-cal fails with DIAG_RESULT=1 *and* OC_DETECT latched
(STATUS=0xE9). Library effects 1/4/10/47 (clicks, double-click, pulse)
also trip OC_DETECT during dwell-loop playback. Effects 14/16 (Strong
Buzz, 750 ms Alert — continuous near-resonance) do not.

**Root cause:** the chip's open-loop probe pulses during auto-cal drive
up to OD_CLAMP into the motor's DC resistance (≈ 10–13 Ω). With
OD_CLAMP = 0x8D (3.0 V peak), I_peak = ~250 mA into DCR — at or above the
chip's OC threshold. Wire-swap on the LRA was tried — predictably no
effect since LRAs are non-polarised AC loads.

**Mitigation in current sketch (2026-06-20 edit):** dropped
`RATED_VOLTAGE` 0x61 → 0x49 (1.5 V_RMS) and `OD_CLAMP` 0x8D → 0x60
(~2.0 V peak). Cal result to be re-checked on next flash. If cal still
fails, drop further (RATED → 0x30, OD_CLAMP → 0x40).

**Results to collect:**

- Auto-cal PASS / FAIL with new values.
- A_CAL_COMP and A_CAL_BEMF values once cal passes — log across boards.
- OC_DETECT incidence on library effects 1, 4, 10, 47 after cal passes.
- Subjective rattle strength once cal is clean (user wants "rattle a lot",
  target steady-state 50–100 mA RMS).

### 4.5 First-boot bus mis-assignment for MAX30101 / TMP117 — *now fixed in sketch*

**Symptom:** Bus scan showed `Bus1: 0x10 0x1C 0x48 0x51 0x6B 0x76` (TMP117
at 0x48 on bus 1) and `Bus2: 0x5A 0x6A` (just DRV + BQ). The sketch
was probing MAX (0x57) and TMP (0x48/0x49) on bus 2 — both NO ACK.

**Root cause:** Daughter PCB is wired entirely to **bus 1** through the
connector, not bus 2. The KiCad hierarchy puts MAX/TMP/DRV under a single
`6_PCB2_Skin_Sensors_LRA.kicad_sch` sub-sheet, which gave the misleading
impression that everything on that sheet shares a bus. In reality only the
DRV's I2C pins on that sheet wire to `SDA_bus2`/`SCL_bus2`; the MAX/TMP
SDA/SCL nets go through different connector pins to bus 1. v8 should
split the schematic so each sub-page reflects one physical bus.

**Fix in sketch:** `MAX30101_ADDR` and `TMP117_ADDR_*` probes now use
`Wire` (bus 1) instead of `Wire1`. Banner and summary lines updated.
TMP117 at 0x48 should ACK on next boot; MAX at 0x57 should also appear
on the Bus 1 scan output.

**Status:** if MAX still NO-ACKs on bus 1 after the sketch fix, the
daughter-PCB connector pin for MAX's SDA/SCL is bad. Probe continuity
from the chip's SDA pin to the connector contact, then from the
connector to the main board's GPIO1.

### 4.6 Boot panic: stack canary on `ipc1` after SD `begin()` fails

**Symptom:** After SD_MMC.begin() returns 0x107 (TIMEOUT on
`sdmmc_init_ocr: send_op_cond`), the summary section starts printing
fine, then mid-line on the encoder summary entry:

```
  ALPS Enc   (A=21 B=43)           :Guru Meditation Error: Core  1 panic'ed
  (Unhandled debug exception).
  Debug exception reason: Stack canary watchpoint triggered (ipc1)
```

**Root cause:** `ipc1` is the inter-processor-communication task on core 1
that ESP-IDF uses to schedule small jobs across cores. arduino-esp32
allocates it ~1 kB of stack. The SDMMC driver, when init fails partway
through, queues deferred cleanup callbacks that run on this IPC task —
and those callbacks blow past the canary asynchronously. Because they
fire *after* `begin()` returned, the crash lands mid-printf of an
unrelated line, which is what you see. The summary printout, ISR
attach, etc. are bystanders.

This is a known class of issue with arduino-esp32 SD_MMC + a card that
can't enumerate. It's not configurable from a sketch (the IPC task stack
size is fixed at compile-time in IDF's sdkconfig).

**Mitigation in current sketch:**

1. **SD init moved to the very last statement of `setup()`** — after the
   summary print, the encoder ISR attach, and the "[BTN] Press GPIO16"
   prompts. A panic from SDMMC cleanup now lands when there's nothing
   left to lose; `loop()` reaches readiness from cached state regardless.
2. **`SD_MMC.end()` called explicitly after a failed `begin()`** to force
   the cleanup work to happen synchronously inside the calling task,
   reducing (but not guaranteeing the elimination of) the deferred IPC
   activity.

**If the panic still happens** after the next flash, the next step is to
disable SD entirely until hardware is sorted: comment out the
`SD_MMC.begin()` call. The card mount is provably not working yet at
the bench-current state of the hardware — keep the code path live only
when we expect it to succeed.

**For v8 / IDF tuning:** if Phase 6 wants to keep this SD code path live
with bad cards in the field, IDF needs `CONFIG_ESP_IPC_TASK_STACK_SIZE`
bumped from 1024 to ≥ 2048. Not a sketch-side fix.

### 4.8 CRITICAL — XC6206 LDO Vin / Vout swapped in library symbol — *RESOLVED on bench*

**Symptom:** Daughter PCB attached for the first time at the start of
Stage 3. After the bus fix in §4.5, TMP117 ACKs and reads sane (29.7 °C),
but **MAX30101 NO-ACKs on bus 1** despite being adjacent to TMP. If the
bus itself were down, TMP wouldn't work either — so the failure is
upstream of MAX's I2C, not on the bus.

Probing MAX's VDD pin: **2.2 V (with replug variation up to 2.6 V)**.
Probing the +1V8 rail at a test point: **2.3–2.6 V** depending on the
moment of measurement.

The MAX30101 VDD absolute maximum rating is **2.0 V** (datasheet §
Absolute Maximum Ratings). The chip has been continuously over-driven
since first power-up. **Almost certainly fried.**

**Root cause:** the KiCad library symbol
`Kompic_Mk1:XC6206P182MR` in `2_Power_Managment.kicad_sch` has its pin
names swapped. Datasheet pinout for XC6206P182MR (SOT-23-3, marked side
up, body shown from above):

```
                        ┌──────┐
        (datasheet)     │      │   (datasheet)
   Vss / GND  pin 1 ────┤      ├──── pin 3   Vin
                        │      │
                        │      ├──── pin 2   Vout
                        └──────┘
```

The library symbol assigns:
- Pin 1 → "GND" ✓ correct
- Pin 2 → "Vin"  **(wrong — pin 2 is Vout)**
- Pin 3 → "Vout" **(wrong — pin 3 is Vin)**

So the PCB net `+3V3` is wired to pin 2 — which is the chip's actual
Vout pin — and the net `+1V8` is wired to pin 3 — the chip's actual Vin
pin. The chip is **driven backwards**: 3.3 V is being pushed into the
output side, with the "input" net hanging off the LDO's actual Vin pin
with no real supply behind it. What "+1V8" measures (2.3–2.6 V) is the
chip's internal protection diodes leaking from the over-driven Vout
back toward Vin, partially feeding the downstream loads — that's why
parts that survive overvoltage (TMP117, PDM mic) still work.

**Affected loads:**

| Load | Spec V_max | Saw | Outcome |
|---|---|---|---|
| MAX30101 VDD | 2.0 V | 2.2 – 2.6 V | Likely dead |
| TMP117 VDD | 5.5 V | 2.3 – 2.6 V | Survives, working |
| MSM261DGT003 mic VDD | ≥ 3.6 V | 2.3 – 2.6 V | Survives, working |
| PCF85063A RTC VDD | 5.5 V (if on +1V8 rail) | 2.3 – 2.6 V | Survives, working |

**Bench fix performed (2026-06-20):**

Cleaner than the originally-proposed trace cut: **rotated the LDO 180°
and bent the leads** to physically swap which pad each lead lands on.
Fresh chip, no traces cut. The chip's Vss/Vin/Vout pins now sit on the
correct PCB pads.

Verification:
- Diode test from Vout → Vin showed normal 0.36 V (slightly low; could
  be Schottky-style body diode on this Torex part, or low-current
  reading. Not a flag.)
- With the LDO output pin lifted (foot in air), the +1V8 net **still
  read 2.3 V** — proving the LDO was no longer the cause. That isolated
  the residual fault to the downstream loads — see §4.9.

Once §4.9 was also resolved, the LDO settles at clean 1.8 V output.
**Do not replace MAX30101 until +1V8 is verified at 1.8 V steady-state.**

**Schematic fix for v8:** edit the KiCad library symbol so the pin-name
↔ pin-number mapping matches the datasheet:
- Pin 1 = GND (no change)
- Pin 2 = **Vout** (was "Vin")
- Pin 3 = **Vin**  (was "Vout")

Re-run ERC. Re-verify against datasheet diagram before fab.
Add a sanity-check note to the build instructions: "measure +1V8 rail
at TP before populating any 1.8 V-only chip (MAX30101)."

**Lessons:** library symbols built from third-party packs (or hand-drawn
from a fading memory of a datasheet) should always be checked against
the actual chip pinout in the datasheet, especially for power devices.
ERC won't catch a pin-name swap because the connectivity is still
internally consistent — it just doesn't match the chip.

### 4.9 CRITICAL — Mixed-voltage VDDIO routing on LSM6DSV16X and BME688

**Symptom (after §4.8 LDO rework was complete and verified):** +1V8 rail
still measured 2.3–2.8 V depending on what was connected. Lifting the
fresh LDO's output pin off the board (foot in the air) **still gave
2.3–2.8 V on the +1V8 net** — definitive proof the LDO wasn't driving
the rail; something downstream was injecting current.

**Root cause:** mixed-voltage VDDIO wiring. Per the datasheets
(`lsm6dsv16x.pdf` Table 2 p. 11, `lis3mdl.pdf` Table 7 p. 8,
`bst-bme688-ds000.pdf`):

| Chip | Pin | Function | iv7.1 routed to | Should be |
|---|:-:|---|---|---|
| **LSM6DSV16X** | 5 | **Vdd_IO** | **+1V8** ✗ | +3V3 |
| LSM6DSV16X | 8 | Vdd | +3V3 ✓ | (either) |
| LIS3MDLTR | 5 | Vdd | +1V8 | (either) |
| LIS3MDLTR | 6 | Vdd_IO | +3V3 ✓ | +3V3 ✓ |
| **BME688** | – | **Vddio** | **+1V8** ✗ | +3V3 |
| BME688 | – | Vdd | +3V3 ✓ | (either) |
| MSM261DGT003 mic | – | VDD (single) | +1V8 | +3V3 |

The chip's digital I/O ESD clamps go to **VDDIO, not VDD**. With SDA/SCL
pulled to +3V3 and VDDIO sitting at +1V8 nominal, the clamp diode from
each I/O pin to VDDIO is constantly forward-biased — current pumps from
+3V3 through the chip's pads into the +1V8 net.

Three chips × multiple I/O pins each = enough injected current that the
LDO (which is source-only, no sink capability) was unable to pull the
rail back down to 1.8 V. Equilibrium settled at ~`3V3 − one ESD-diode-Vf`
≈ 2.3–2.8 V depending on what loads were drawing current to bleed it off.

**The same overvoltage that killed MAX30101 (Bug #3) was as much from
this back-feed as from the LDO swap.** Even after fixing the LDO, the
rail couldn't go below ~2.3 V because of the ESD-clamp current sources.

LIS3MDLTR is *correctly* wired (Vdd_IO already on +3V3). LSM6DSV16X
and BME688 are not.

**Bench fix:** physically cut the +1V8 branch where it splits to feed
the mis-wired pins on LSM/BME/mic, leaving the MAX30101 and M10S GPS
branches untouched on +1V8.

Specifically: **cut over reference `C29`, across the "9" of the
silkscreen**. That severs the +1V8 trunk feeding LSM Vdd_IO, BME Vddio,
mic VDD, and LIS Vdd. Then hot-wire (jumper) the cut-off side to a
+3V3 testpoint.

End state:
- +1V8 net: MAX30101 VDD + M10S GPS VCC only.
- +3V3 net: LSM (both Vdd and Vdd_IO), LIS (both Vdd and Vdd_IO), BME
  (both Vdd and Vddio), mic VDD — all four chips now have all their
  supply pins at +3V3.

**Voltage-tolerance audit (verified before cutting):**

| Chip | Spec max VDD (now on 3V3) | OK? |
|---|---|---|
| LSM6DSV16X Vdd_IO | 3.6 V op (Table 4 p. 14) | ✓ |
| LIS3MDLTR Vdd | 4.8 V abs max (Table 7 p. 8) | ✓ |
| BME688 Vddio | 4.25 V abs max | ✓ |
| MSM261DGT003 VDD | 3.6 V (per 20.19 extract) | ✓ |

**Bench outcome:** LDO now regulates +1V8 to a clean 1.8 V with the
cut-off branch jumpered to +3V3. All four chips operate normally on
the +3V3 supply. Sensor probes (LSM/LIS/BME/mic) continue to PASS in
the smoke test.

**Trap that bit during the fix:** first attempt at the cut + jumper
left **the +1V8 rail rising to 3V3** when 3V3 was applied to the
rerouted branch — a sneak path remained between the two nets through
a cap pad, via, or another tap-point on the +1V8 polygon that wasn't
included in the first cut. With the second physical cut at the right
location (over C29's "9") the isolation became complete and the rail
behaved.

**For v8:** in `4_Air_Sensors.kicad_sch` and `3_Space_Time_Sensors.kicad_sch`,
audit every chip with a separate VDDIO/Vdd_IO pin. The wiring rule:
**VDDIO matches the I2C/SPI bus voltage; VDD is free to be lower.**
ERC won't catch a VDDIO-on-wrong-rail error because the connectivity
is internally consistent — this is a design-review-only check.

### 4.7 LIS3MDL saturated on Y and Z

**Observation only — not a Stage 3 regression.** LIS reads Y=−32768 /
Z=+32767 (rail-to-rail saturation) consistently in the dwell loop. The
sensor is in ±4 G mode (1300 µT full scale). A magnet near the bench
(speaker, ferromagnetic enclosure, mains transformer) is the simplest
explanation. Verify by moving the board to open space and re-reading.

If saturation persists in clean space, the LIS is mis-installed (wrong
orientation? mis-soldered pad?) — investigate before treating it as a
sensor problem.

---

## 5. Session-end state (2026-06-20)

### Working and verified on the bench

| Subsystem | Status | Notes |
|---|---|---|
| ESP32-S3 + USB-C + power-on | ✓ PASS | Stable, ~110 mA idle |
| BQ25619 (charger) | ✓ PASS | I²C bus 2 0x6A, TS_IGNORE + WD silenced |
| PCF85063A (RTC) | ✓ PASS | I²C bus 1 0x51, ticking |
| VEML6030 (ALS) | ✓ PASS | I²C bus 1 0x10 |
| LIS3MDLTR (mag) | ✓ PASS (electrical) | Y/Z saturate near bench magnets — see §4.7 |
| LSM6DSV16X (IMU) | ✓ PASS | All 6 axes + temp, after §4.9 rail fix |
| BME688 (env) | ✓ PASS | T/P/H reading sanely, after §4.9 rail fix |
| WS2812B RGB | ✓ PASS | Drives correctly |
| MSM261DGT003 PDM mic | ✓ PASS | Samples varying, after §4.9 move to +3V3 |
| TMP117 (temp) | ✓ PASS | I²C bus 1 0x48, reads ~30 °C |
| DRV2605L (haptic) | ✓ PASS (electrical) | Auto-cal still FAILs OC_DETECT — see §4.4. Library effects play but tune is off. |
| Flashlight LED | ✓ PASS (post-bodge §4.2) | Bright at 25 % duty cap; R26 lost, restock before lifting cap |
| ALPS EC05E encoder | ⚪ DRIVEN (untested polarity) | ISR works; CW/CCW direction not yet bench-verified |
| XC6206 LDO → +1V8 rail | ✓ PASS | 1.80 V steady, after §4.8 + §4.9 |

### Unresolved / broken

| Subsystem | Status | Next step |
|---|---|---|
| **SD card** | ✗ FAIL (init never completes — see §4.1) | SPI mode rework (frees from SDMMC bus signal-integrity issue). Requires freeing one GPIO for CS (likely DRV_EN tied off to 3V3) + one bodge from GPIO0 to socket pin 2. |
| **MAX30101 PPG** | ✗ FAIL (likely fried) | Replace chip. Safe to populate now that §4.8 + §4.9 are fixed and +1V8 measures clean 1.8 V. |
| **Qvar / ECG electrodes** | ⚪ Not yet exercised | No firmware path yet; defer to Phase 6 ECG driver port. |
| **GPS (MAX-M10S)** | ⚪ Not yet exercised | Hardware present, no firmware path yet. Defer. |
| **Battery + boost / charge arbitration** | ⚪ No battery on board | Once attached, exercise BQ25619 BST_CONFIG and re-verify flashlight (which depends on PMID 5 V). |

### Bench-rework debt that needs to ride into v8

These are the bodges and lost parts that need re-fab or restock:

- **R26 (flashlight series resistor):** lost during prior probing. Restock 47 Ω 0402.
- **LED topology bodge:** LED moved from FET source to drain side via cut+jumper. Schematic / PCB redraw needed.
- **CLK pull-up bodge:** 10 kΩ hand-soldered onto an empty footprint. Populate properly in v8.
- **+1V8 rail bodge:** cut at C29, jumpered to +3V3 for LSM/BME/mic. Schematic library symbol for XC6206 also has Vin/Vout swapped — fix both.
- **VDDIO routing:** LSM6DSV16X Vdd_IO and BME688 Vddio should land on the bus rail (+3V3), not the analog rail (+1V8). Re-route in v8.
- **SD bus pull-ups:** populate 4.7 kΩ (not 10 kΩ) on all three. Consider 2.2 kΩ if HS mode is wanted. *And* route DAT3 to ESP so 4-bit / SPI-CS are both possible without bodges.

### Critical bug count (final)

5 critical bugs found in the iv7.1 PCB. 4 resolved on the bench. **1 unresolved (SD card)** — requires either software re-architect (SPI mode) or PCB respin.

---

## 6. Caveats / lessons

1. **AI-generated datasheet extracts under `firmware/docs/datasheet_extracts/`
   have been wrong in load-bearing ways.** The 20.17 (flashlight) extract
   claimed a low-side switch topology — the actual `5_Lights.kicad_sch`
   has a source follower. **Always read the schematic file directly when
   the circuit behaviour and the extract disagree.** Update the extracts
   when discrepancies are confirmed; treat them as derived not canonical.
2. **Mechanical-bounce parts** (encoder, button) need software debounce on
   top of PCNT glitch filter — the filter only catches RF/EMI glitches.
3. **Source-follower with logic-level GPIO** is a common subtle schematic
   error — the chip still ACKs / probes fine, the FET still passes the
   diode test, but the load never lights. ERC won't catch it; visual
   inspection of the schematic by someone who's seen it before will.
4. **Bidirectional TVS diodes can't be tested with a normal DMM.** Document
   this in the BOM, especially for parts the assembler will be tempted to
   probe during bring-up.

---

## 7. References

- Stage-3 smoke sketch: `firmware/test/3_smoke_test_mk1_hotair/3_smoke_test_mk1_hotair.ino`
- SD-pump debug sketch: `firmware/test/sd_pump_test_mk1/sd_pump_test_mk1.ino`
- Master pinout: `hardware/Kompic_Mk1/0_Kompic_Pinout_MASTER_v20_iv7.1.md`
- Schematic — lights / flashlight: `hardware/Kompic_Mk1/5_Lights.kicad_sch`
- Schematic — power management (XC6206 LDO): `hardware/Kompic_Mk1/2_Power_Managment.kicad_sch`
- Schematic — daughter PCB: `hardware/Kompic_Mk1/6_PCB2_Skin_Sensors_LRA.kicad_sch`
- DRV2605L datasheet (TI): `docs/datasheets/drv2605l.pdf`
- ESDALCL5-1BM2 datasheet (ST): `docs/datasheets/ESDALCL5-1BM2.pdf`
- LIS3MDL datasheet (ST): `docs/datasheets/lis3mdl.pdf`
- LSM6DSV16X datasheet (ST): `docs/datasheets/lsm6dsv16x.pdf`
- BME688 datasheet (Bosch): `docs/datasheets/bst-bme688-ds000.pdf`
- Stage 2 report: `hardware/Reflow_info/Stage_2_Build_Report.md`

---

*Stage 3 — bench session ended 2026-06-20. 4 of 5 critical bugs resolved;
SD card carried forward as Plan-B (SPI mode) for the next session.*

---

## 8. Demo bring-up follow-up — 2026-06-21

**Goal:** turn the stage-3 board into a survivable field-test demo on a 400 mAh
LiPo: button + encoder + DRV haptic + WS2812 + flashlight, with BQ ship mode
for storage. Drop sensors and SD/mic from the runtime loop (boot-time scan
only) to cut idle drain.

Sketch: `firmware/test/4_demo_mk1/4_demo_mk1.ino`.

### 8.1 Bug 6 — R12 traps ship mode (the single biggest finding)

**Symptom.** Double-click triggers `BATFET_DIS=1` write, BATFET drops, ESP
loses power → board comes back alive ~1 s later. Repeats forever. Ship mode
unusable on battery.

**Investigation path.**
1. First hypothesis: the button (= /QON) was still being held LOW at the
   moment of the BATFET_DIS write, tripping `t_QON_RST` (8–12 s) full system
   reset. Firmware was rewritten to fire the ship-mode action on the *release*
   edge of the second click and to spin-wait for /QON stable HIGH before the
   write. Result: the zombie window shrank from ~10 s to ~1 s.
2. The new 1 s timing matched `t_SHIPMODE` (0.9–1.3 s, datasheet p. 25) — the
   documented "wake from ship mode by /QON LOW" gesture. So something was
   pulling /QON LOW for 1 s *after* BATFET dropped, even with the user not
   touching the button.
3. Followed the trace: **R12 (5k1) pulls /QON to +3V3.** When BATFET drops in
   ship mode, +3V3 collapses (XC6206 input gone). R12 instantly becomes a hard
   pull-DOWN from /QON to the dead +3V3 rail. Divider math against the BQ's
   internal 200 kΩ to V_BAT: V_QON ≈ 3.7 V × 5k1 / (5k1 + 200k) ≈ 92 mV →
   solidly LOW. BQ trips `t_SHIPMODE` after 0.9–1.3 s → BATFET re-enables →
   +3V3 comes back → R12 pulls HIGH again (looks like a clean release) → ESP
   boots. The chip is doing exactly what its datasheet says.

**Fix.** Lift R12. Single 0402 resistor, top-side, directly above SW1 at
schematic position (163.83, 123.19). Hot air ~280 °C, 5–10 s with tweezers.
No jumper / no trace cut needed — the BQ's own internal 200 kΩ pull-up to
V_BAT (datasheet pin 12) is sufficient on its own. This is the topology
shown on every TI BQ25619 reference design / EVM: tactile button to GND,
internal pull does the rest.

**Confirmed working after lift.** Double-click → 2 s red RGB countdown →
BATFET drops → board stays off. >1 s press on the button wakes cleanly.
Quiescent current in ship mode is dominated by I_SHIP_BAT (7 µA typ per the
datasheet extract §Power consumption).

**v7.2 action:** Mark R12 as DNP on the schematic, leave the footprint as a
depopulated placeholder. Do not re-route to V_BAT or SYS_Power — the internal
200 kΩ to V_BAT is the manufacturer-intended pull.

### 8.2 Other firmware findings this session

- **`led_hard_off` / lazy `ledcAttach` pattern was a mistake.** Attempted as a
  fix for what looked like `ledcWrite(pin, 0)` residual on GPIO41. Detaching
  LEDC and using `digitalWrite(LOW)` confused the GPIO matrix routing on
  arduino-esp32 v3.x — the next `ledcAttach` didn't cleanly re-bind, leaving
  the pin stuck where `digitalWrite` left it. The LED then ignored every
  subsequent duty change. **Lesson:** stick with the 2_smoke pattern —
  `ledcAttach` once at setup, `ledcWrite(0)` for off. If residual brightness
  remains it's a hardware issue (see Bug 7), not a LEDC quirk.

- **Ship-mode register write needs `BATFET_RST_WVBUS=1` when USB is plugged
  in.** Datasheet p. 25, "Adapter present" path. Without it, `BATFET_DIS=1`
  alone has no visible effect on USB power. Also clearing `BATFET_RST_EN`
  (REG07 bit 2) prevents the 8–12 s QON-hold full-system-reset path so an
  accidental long press during normal use can't power-cycle the board.

- **DRV2605L auto-cal is necessary in practice.** Open-loop drive with the
  ELV1411A profile values (`RATED=0x49`, `OD_CLAMP=0x60`, `CTRL1=0x9C`,
  `FEEDBACK=0xB6`) produces only a barely-perceptible buzz even at "Strong
  Click 100 %". Running auto-cal each boot (or once, then storing the result
  in NVS) is required to actually feel the haptic. Per-3_smoke cal
  succeeded in <1.2 s; demo now performs the same cal at boot and falls
  back to open-loop if cal returns DIAG_RESULT=1.

### 8.3 Bug 7 — flashlight leaks visible light when "off"

User-described circuit: 5V → R25 (47 Ω) → LED → MOSFET (D-S) → GND; gate has
100 kΩ to GND + 47 Ω to GPIO. When GPIO is driven LOW, V_GS is solidly 0 V
and the MOSFET is nominally off — but the LED still glows visibly in a dark
room. Most likely cause is I_DSS leakage (logic-level small-signal FETs are
often spec'd 1–25 µA at V_DS = 5 V), which is enough to forward-bias a
modern white LED into the sub-mA range where it still emits perceptible
light. Not fixable in firmware. **Possible mitigations for v7.2:**
- Lower-leakage MOSFET (DMG3414U or similar with <100 nA I_DSS).
- Add a small (1–10 kΩ) bleed resistor across the LED (cathode-to-anode), so
  any sub-µA leakage drops across the bleed instead of forward-biasing the
  LED. Adds a tiny extra load when LED is ON (a few µA worst case at full
  duty).
- Or: switch the high side instead of the low side, with a PNP/PMOS — gate
  off pulls D and S both to 5V, no path through LED.

### 8.4 Stage 3 final demo state (2026-06-21)

| Function | Status |
|---|---|
| BQ25619 charge management | ✓ (charge / done detected via REG08; status printed each poll) |
| BQ25619 ship mode (double-click → off; >1 s press → wake) | ✓ after R12 lifted |
| DRV2605L click on every encoder detent | ✓ after auto-cal at boot |
| ALPS EC05E encoder (24 levels, smooth brightness ramp) | ✓ after a loose connector fix on the encoder header |
| Flashlight on/off via button single-click | ✓ (with residual leak per Bug 7) |
| WS2812 always-on color roam (~30 s/cycle) | ✓ |
| 2 s red RGB countdown before ship-mode entry | ✓ |
| Boot-time I²C scan + per-device identity report | ✓ (VEML, LIS, LSM, BME, RTC, BQ, DRV) |
| Non-essential sensors parked in lowest power state | ✓ (VEML shutdown, LIS power-down, LSM ODRs cleared, BME sleep) |
| CPU dropped to 80 MHz to cut idle draw | ✓ |
| SD card (SDMMC 1-bit) | UNRESOLVED — carried over to next session |
| MAX30101 | UNRESOLVED — chip replacement needed, daughter PCB off bench |
| Daughterboard (MAX + TMP + QVAR + GPS) | not fitted for this demo |

**Verdict:** the stage-3 main PCB is now a working — and now actually usable
— wrist-worn flashlight + RGB + haptic encoder + USB charging. Sensors are
all alive but parked. SD and MAX are the only remaining bench tasks; the
daughterboard and GPS bring-up are the next stage.
