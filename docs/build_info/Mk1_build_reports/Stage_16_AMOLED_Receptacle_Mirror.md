# Stage 16 — AMOLED Receptacle Mirrored (iv7.1 display bring-up blocked, Mk2 required)

**Date:** 2026-08-22
**Board:** iv7.1
**Firmware baseline:** f0.0 (unchanged — sketch was flashed but never met a working panel)
**Status:** CLOSED for iv7.1 — display subsystem cannot be brought up on this board rev. Deferred to Mk2.

---

## 1. What happened

Received the CO5300 2.06" AMOLED FPC + panel. Hand-soldered the OK-14F024-04
24-pin receptacle to the iv7.1 board. Flashed
`firmware/arduino/15_amoled_touch_test/15_amoled_touch_test.ino` — the same
QSPI + CST9217 driver that had been verified on the standalone breakout,
with pins re-aligned to the iv7.1 master pinout (v20). Plugged the panel
in. Nothing happened. Panel dark, no touch ACK, no observable current
change on the 3V3 rail.

Nothing else on the board misbehaved — no rail collapse, no dead sensors on
bus 1. The panel simply did not come up.

Post-mortem on the physical connector uncovered the reason: **the
receptacle footprint on iv7.1 is mirrored**. Panel FPC pin 13 (GND on the
panel side) lands on PCB position 1 (QSPI_RESET / GPIO3 net). The full
24-pin map is reversed vs. what the schematic expects. This is a
footprint-side error, not a solder-side error.

Only reason the rail survived: the panel is a passive load until its
internal PMIC boots, and with RST held wrong plus multiple 3V3 pins
landing on GNDs (and vice versa), the panel simply refused to power up
before any short current could build. Lucky miss.

---

## 2. Why the obvious "fixes" don't work

| Option | Verdict | Reason |
|---|---|---|
| Rotate the FPC 180° | ✗ | FPC is single-sided (contacts on one face only). Rotation doesn't move the contact face. |
| Rotate the receptacle 180° | ✗ | Same issue on the other end — rotating the receptacle just re-aligns to a different-but-still-wrong mapping. The footprint is mirrored, not rotated. |
| Swap for opposite contact-face variant of same receptacle (top-contact ↔ bottom-contact) | ✗ | Would work IF the mirrored footprint were on the bottom side of the PCB. It's on the top. The variant swap only helps when the mirror axis matches the board side. Here it doesn't. |
| FPC gender-flipper adapter | ✗ | Would need to cross 24 lanes on a folded flex; QSPI at 40 MHz is unhappy over any hand rework longer than a few mm. Ruled out. |
| Deadbug / kynar rework of all 14 named nets | ✗ | 14 nets including QSPI at 40 MHz. Too fragile, too finicky, too much benchtime for a one-board fix. Ruled out. |

The receptacle would only be usable if it were on the **bottom** side of the
PCB — mirroring the footprint AND moving it to the opposite copper layer
cancels out. That's a board respin, not a rework.

---

## 3. Direction — speedrun Mk2

Only path forward: fix the footprint in the next board rev and pull that
rev in. Everything else about iv7.1 is working (LSM, HR pipeline, ECG sync,
flashlight, SD, RTC, PM, BQ ship mode, sensor suite), so Mk2 is not
gated on new firmware — it's a hardware fix pass.

### Must-fix list for Mk2 (display-related)

1. **Receptacle footprint** — verify pin 1 corresponds to the panel FPC's
   pin 1 with the FPC inserted contacts-down (matching the panel's actual
   contact orientation). Cross-check by physically laying the FPC across
   the KiCad footprint before committing.
2. **Contact-face variant confirmation** — re-check the ordered part
   (OK-14F024-04) in the datasheet's top/bottom contact table. If a
   -T / -B suffix distinguishes them, pin the exact suffix in the BOM.
3. **Silk-print pin 1 indicator** — mark pin 1 on the silkscreen next to
   the receptacle so this class of error is catchable pre-reflow.
4. **Test-pad breakouts** — expose the 14 named nets (QSPI × 6,
   TP-SDA/SCL/INT/RST, DISP_RST, TE, +3V3, GND) as via-in-copper test
   pads next to the receptacle. Lets us diff-probe the pinout on next
   assembly before mating the panel.

Non-display items already queued for Mk2 in `Stage_8_Re-Blueprinting.md`
carry over unchanged.

### Bring-up plan once Mk2 arrives

The sketch `15_amoled_touch_test.ino` is preserved as-is — it was correct
except for the mirrored physical connection. First flash on Mk2 will be
that sketch, verbatim, to confirm the fix.

---

## 4. What we keep from iv7.1

- Master pinout v20 — every GPIO assignment matches what Mk2 needs.
  No pin changes required in firmware; the mistake was purely on the
  receptacle footprint.
- Firmware iv7.2.f0.0 baseline — the pending ESP-IDF port work continues
  on iv7.1 (all sensor subsystems that DO work). Display integration was
  going to be the last major subsystem anyway.
- Panel + FPC — undamaged, will mate to Mk2.

---

## 5. Lessons

1. **Physically lay the FPC across the KiCad footprint before commit.**
   Rendering the schematic and PCB is not the same as checking that the
   physical part orients correctly. This is the second footprint-orientation
   surprise on Kompic (cf. earlier BSS138W pinout scare); worth a written
   pre-fab checklist for FPC/QFN parts.
2. **Test pads next to any connector we've never physically populated
   before.** Would have caught this in 30 seconds with a DMM instead of
   after a full receptacle solder cycle.
3. **Mirrored footprints are unrecoverable in-place.** File this alongside
   "no second hot-plate cycle" as an iv7-series class of unfixable error
   modes — the moment they're identified, the answer is a respin, not a
   rework.

---

## 6. Status

- [x] Sketch preserved: `firmware/arduino/15_amoled_touch_test/15_amoled_touch_test.ino`
- [x] Panel + FPC set aside safely — will be used with Mk2
- [ ] Add "footprint physical-orientation check" step to Mk2 pre-fab checklist
- [ ] Add pin-1 silkscreen indicator + net test-pads to Mk2 display section
- [ ] Re-audit ordered receptacle part number against datasheet variant table
