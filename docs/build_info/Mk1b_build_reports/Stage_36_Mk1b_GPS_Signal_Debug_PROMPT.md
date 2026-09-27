# Stage 36 -- Mk1b GPS signal / GPS_PING debug (fresh-session prompt)

**Date opened:** 2026-09-27
**Board:** Mk1b iv8.0.
**Firmware baseline:** 0.4.91 (Stage 35 close: i2c_master migration
complete, sensor surface trimmed).
**Status:** OPEN, queued.

Paste the "**Prompt for a fresh Claude instance**" section below into a
new session's first message.

---

## Symptoms (from Stage 35 close bench, fw 0.4.91)

- Chip is talking two-way. Boot log shows `MAX-M10S init OK
  (dynmodel=PED, MON-RF on)`, and background counters accumulate:
  `total=612  mon_ver=0  mon_rf=611  nav_timeutc=0  ack_ack=1
  ack_nak=0`.
- MON-RF snapshot is stable: `antStatus=OK(2)  antPower=ON(1)
  noise/ms=64-65  AGC count=3534 (~43%)`. Antenna not shorted, not
  open, chip is powering it, RF frontend AGC is mid-range.
- SNR is bad and bursty. `GPS_SNR` reports `sats_with_snr=0
  max CN0=0` most of the time; when signal does appear it peaks
  at **17-21 dBHz** and only for ~2 s at a time before dropping
  back to noise for another ~2 s. Never enough continuous signal
  for a fix.
- `GPS_PING` (UBX-MON-VER poll) consistently reports "NO
  UBX-MON-VER response in 1000 ms" -- even though every other UBX
  path works (`ack_ack` increments, MON-RF frames pour in). Only
  MON-VER never comes back. `mon_ver=0` in the UBX counter dump.

## Hardware context

- Chip: u-blox MAX-M10S GNSS module.
- Antenna: chip antenna (small ceramic), passive.
- Antenna is covered by ~1 mm of clear epoxy on the assembled Mk1b.
  Visible through the epoxy. Ivan is skeptical this is the problem
  but can't rule it out.
- UART is verified working: `uart_wait_tx_done()` fix landed in
  Stage 31 appendum, MON-RF proves TX+RX are healthy end-to-end.
- VIO_SEL floats (3V3 mode), SAFEBOOT_N floats (per
  `project_max_m10s_wiring`).
- 1PPS on GPIO46, wired but not exercised by current firmware.

## Two suspects, one investigation

### Suspect A -- GPS_PING is broken, not the chip

Every UBX message class works EXCEPT MON-VER. Three hypotheses
(inherited from the 2026-09-23 Gemini session, filed in
Stage 31 §11 GPS appendum -- read that first):

1. **Poll frame checksum wrong** -- the 8-byte MON-VER poll is
   `B5 62 0A 04 00 00 0E 34`. Verify the two Fletcher checksum
   bytes are actually `0x0E 0x34` on the wire, not
   pre-computed against the wrong header.
2. **Dispatcher not routing MON-VER back to our handler** --
   check `max_m10s_feed_ubx_byte()` -- confirm the branch for
   class `0x0A` id `0x04` calls `handle_ubx_mon_ver()`. Watch
   with a DEBUG log at the top of the branch to see if it's
   even being entered when a response arrives.
3. **Response length guard too strict** -- `handle_ubx_mon_ver()`
   likely rejects payloads shorter than 40 bytes. u-blox MAX-M10S
   returns a MON-VER of exactly 40 (SW+HW versions) or 40+30*N
   (with N extension strings). If our chip's firmware ships with
   fewer extensions, we might drop a valid response. Relax to
   `len >= 10`.

Do all three before concluding the chip's MON-VER is broken.
The pinpoint tool would be a logic-analyzer capture of the
UART lines during a `GPS_PING`. Failing that: temp-add a
verbose-hex dump of every UBX byte received in a 1 s window
after the poll goes out.

### Suspect B -- Antenna / RF path is starving the fix

Even if PING gets fixed, the signal-strength story is bad on its
own merit. Numbers to interpret:

- **17-21 dBHz** max CN0: fix requires ~25 dBHz sustained on
  4+ satellites for cold start, ~30+ dBHz for reliable tracking.
  We are below that floor. GPS chips will NEVER get a fix from
  17-21 dBHz signals -- they can't decode the ephemeris.
- **Bursty on/off**: 2 s on, 2 s off pattern suggests either
  (a) antenna tuned wrong (satellite drifts in/out of resonance
  peak as it moves through the sky), (b) severe multi-path
  from nearby metal, or (c) self-noise from another peripheral
  (worst offender is usually the display QSPI clock or
  switched-mode power supply harmonics).
- **AGC count 3534 (~43%)**: the AGC control word is 0..8191.
  Mid-range means the frontend is neither cranked to max gain
  (nothing to receive) nor pinned at min gain (RF overload).
  It's what "quiet RF environment with some noise" looks like.
  For reference: strong open-sky signal usually pushes AGC
  DOWN (~10-25%); heavy jamming pushes it UP (~80%+). 43%
  says the chip is not being blinded, but signal is weak.
- **noise/ms 64-65**: u-blox integer noise metric. Anything
  under ~150 is a clean environment. Our value is clean.
  So no local interference -- the antenna just isn't picking
  up the sky.

Bottom line: with clean noise + mid AGC + no CN0, the story is
**"antenna cannot see the sky well enough"**. Chip is fine.
Epoxy covering + small chip antenna + indoor testing all
compound.

## What to try, in order

1. **Fix GPS_PING first** (Suspect A). Without a working
   MON-VER poll we can't ask the chip for its firmware version
   or run other pokes. Cheap wins there. Follow the three leads
   in Stage 31 §11 appendum.
2. **Bench-test outdoors** with the current build. Take the
   watch outside with clear sky for 5-10 minutes. If CN0
   climbs past 30 dBHz on any satellite, the epoxy + tiny
   antenna are just marginal -- workable but poor. If CN0
   stays 17-21 dBHz outdoors, the antenna installation is
   broken (impedance-mismatched, detuned by epoxy, ground
   plane wrong, or the antenna itself is bad).
3. **If outdoors doesn't help:**
   - Try a temporary flying-wire active antenna (u-blox ANN-MB
     is the standard bench antenna). Rules out chip-antenna-
     specific problems. If SNR jumps to 40+ dBHz with an
     active antenna, the on-board chip antenna is the culprit.
   - Measure the antenna's return loss / VSWR at 1.575 GHz
     if you have a VNA. Detuning by epoxy is measurable.
   - Consider a Mk2 respin with a proper GPS antenna
     footprint (larger ceramic patch, ground plane sized
     per antenna datasheet).
4. **Do NOT chase self-noise until 2 and 3 have been done.**
   Noise/ms is already low; adding shielding is expensive and
   unlikely to help.

## Files to read on the way in

- `firmware/esp-idf/components/max_m10s/max_m10s.c` (driver
  proper, feed_ubx_byte / handle_ubx_mon_ver / ubx_send_*)
- `firmware/esp-idf/components/max_m10s/max_m10s.h` (public API,
  including UBX class/id defines)
- `firmware/esp-idf/components/field_capture/fc_cli.c` (GPS_PING,
  GPS_SNR, GPS_MONRF, GPS_UBX_STATS, GPS_DYNMODEL verbs)
- `docs/build_info/Mk1b_build_reports/Stage_31_Mk1b_LVGL_Foundations_Rebuild.md`
  §11 "2026-09-23" appendum (GPS UART TX close + the three
  GPS_PING debug leads)
- `docs/build_info/Mk1b_build_reports/GPS_M10S_Fixing.md` (full
  diagnostic trail from the pin-swap saga)
- `docs/build_info/reference_files/datasheets/` -- check for
  MAX-M10S Integration Manual + Interface Description PDFs.
  If missing, u-blox document numbers are UBX-20035208 (Data
  Sheet), UBX-20053088 (Integration Manual), UBX-19035940
  (Interface Description).

## Auto-memory to consult

- `project_max_m10s_wiring` -- VIO_SEL / SAFEBOOT_N conventions.
- `project_mk1b_arrived` -- board rev context.
- `project_photo_gps_plan_b` -- fallback external-GPS plan if
  Mk1b GPS never delivers a fix in a reasonable timeframe.

---

## Prompt for a fresh Claude instance

Copy-paste the block below verbatim into a new session's first message.

```
Continue GPS debug on Mk1b iv8.0, fw 0.4.91 baseline.

Read the full context here:
  docs/build_info/Mk1b_build_reports/Stage_36_Mk1b_GPS_Signal_Debug_PROMPT.md

Two problems to disentangle in order:
  1) GPS_PING (UBX-MON-VER poll) never gets a response, even though
     every other UBX path works (mon_rf=611+, ack_ack=1, mon_ver=0).
     Three hypotheses inherited from the 2026-09-23 Gemini session
     already filed in Stage 31 §11 appendum -- read that first.
     Fix GPS_PING before any deeper GPS work.
  2) SNR peaks at 17-21 dBHz and bursts on/off in 2 s cycles;
     AGC ~43%, noise/ms ~65, ant OK. Below the ~25 dBHz floor
     needed for a fix. Interpretation + suggested bench sequence
     is in §"Suspect B" of the Stage 36 prompt.

Follow the "What to try, in order" list. Don't chase self-noise
until the outdoor bench + swap-antenna checks are done -- noise
metrics say the RF environment is quiet.

Boot logs, tile behavior, and the exact CN0/AGC/noise numbers
observed on the Stage 35 close bench are in §"Symptoms" of the
Stage 36 prompt file.

Open Stage_36_Mk1b_GPS_Signal_Debug.md as the working stage doc
(use the frontmatter convention per feedback_md_frontmatter_convention).
```
