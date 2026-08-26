# Pre-Fab Checklist — Kompic PCBs

**Purpose:** a mandatory pass before every Gerber export. Every entry here exists because a past bug got past DRC and cost a full board respin. If DRC catches it, it doesn't belong here.

**How to use:** open this file, walk every item, mark it done in the current revision's Mk*_fix_list.md. Do NOT export Gerbers until every item is confirmed. Add a new item every time a fab escape happens.

Legend: check the box only after you have physically confirmed the item on your KiCad screen or bench.

---

## §1 — Footprint orientation (physical parts vs KiCad footprints)

Founding case: **iv7.1 AMOLED FPC receptacle mirror.** Panel FPC pin 13 (GND) landed on PCB pin 1 because the receptacle footprint was drawn mirrored. DRC did not — cannot — catch this. Only physical lay-across catches it.

- [ ] Every **FPC connector** on the board: print the KiCad footprint at 1:1, physically lay the FPC across it contacts-down, verify pin 1 aligns with FPC pin 1. Repeat for every FPC.
- [ ] Every **USB-C receptacle**: physically lay the part on the footprint, verify A/B row orientation and mounting shell alignment.
- [ ] Every **SD card socket**: verify card-insertion direction matches the enclosure cut-out plan.
- [ ] Every **header / 2×N connector**: verify pin 1 is where the mating cable expects it.

## §2 — Polar IC pin-1 markers

- [ ] Every QFN, LGA, DFN, WSON, SOT, and BGA has a **pin-1 silkscreen dot** on the top layer, placed within the fab window (survives silk registration tolerance).
- [ ] Every polar IC has its **pin 1 orientation** verified against the datasheet Pin Configuration figure — chamfer, dot, or corner mark on the KiCad footprint matches the physical part.
- [ ] LGA/DFN parts have **0.4-0.5 mm perimeter pad extension outward** so hot-air rework can catch pads from outside the package footprint.

## §3 — BOM ↔ Schematic ↔ Footprint consistency

- [ ] Every schematic symbol has a valid **LCSC ID** in the parts list AND in the symbol's `LCSC` field.
- [ ] For any part where the schematic symbol carries a note ("**batch is dead, USE X**", "**library symbol NOT fixed**", etc.), the LCSC ID in the BOM references the **actual part being ordered**, not the placeholder.
- [ ] BOM is split into `JLC-assembles-0402-passives` sheet vs `Ivan-hand-solders-ICs` sheet. No mixed-tier lines.
- [ ] Every "conditional" or "swap" note is either resolved or explicitly deferred with rev-target.

## §4 — Test pads and debuggability

- [ ] **USB pigtail fallback** — four test pads (VBUS, GND, D+, D-) near the USB-C receptacle, so USB access survives a dead receptacle. (Added post-iv7.1 USB-C fatigue.)
- [ ] **Display connector** — pin-1 silk + test pads on every named net at the FPC.
- [ ] Every interrupt line has a via or pad reachable from the top for scope probing.
- [ ] **UART0 (GPIO43 TXD / GPIO44 RXD)** — test pads for esptool fallback flashing if USB path is compromised.

## §5 — Symbol / library gotchas ledger

Past library-side bugs that must be re-checked each revision because they will regenerate if libraries are refreshed from source:

- [ ] **XC6206 LDO** — schematic pin labels Vin (pin 3) vs Vout (pin 2) are the historically-swapped ones. Verify the actual net connections match your intent, not the label. iv7.1 lost a MAX30101 to this.
- [ ] **ESDALCL5-1BM2** — healthy parts read OC in DMM diode mode. Note this in the BOM so a future you doesn't scrap a working part.
- [ ] **BSS138W (SOT-23)** — the batch shipped in the iv7.1 cycle is dead-leaky. If BOM still references `C890266`, verify the actual FET on hand is not from that batch, or swap to BC847C low-side NPN.

## §6 — Ground / high-speed routing

- [ ] Continuous **GND under SDMMC bus** with stitching vias (if space permits).
- [ ] Continuous GND under **QSPI display bus** — 40 MHz needs a clean return path.
- [ ] Continuous GND under **USB D+/D-** — differential integrity.
- [ ] **PCF85063 RTC** — 1 mm GND moat on the top layer under the crystal (EMI shielding).
- [ ] **Case-to-GND bond pad** — exposed copper pad where the metal enclosure will contact.

## §7 — Voltage-domain sanity

- [ ] Every sensor's **VDDIO** matches the I²C bus voltage (LSM/BME/LIS/mic — verified after iv7.1 back-feed bug).
- [ ] Every sensor's **VDD** is within its datasheet spec including LDO tolerance. Specifically check: LIS3MDL VDD min is 1.9 V — MUST be on 3V3, not 1V8 (XC6206P182 tolerance can drop to 1.76 V).
- [ ] Any pin driving into the I²C pull-up domain (CS, address-select, alert lines) has its pull-up going to VDDIO, not to VDD.

## §8 — Strapping-pin discipline (ESP32-S3)

- [ ] GPIO0 idles HIGH at boot (not driven LOW by any peripheral).
- [ ] GPIO3 (JTAG source select) has a defined state at reset (no floating boot).
- [ ] GPIO45, GPIO46 (VDD_SPI, boot-msg) are LOW at reset — no external pull-ups.
- [ ] GPIO43/44 (UART0) — any peripheral tied here tolerates the boot-log burst without misbehaving.
- [ ] EN pin has the 10 k + 1 µF RC brown-out network populated.

## §9 — 3D model + mechanical

- [ ] Open the 3D viewer. Look for missing / broken 3D models on any placed component.
- [ ] Verify **clearance** around every tall component (crystal, connectors, encoder shaft, USB-C).
- [ ] Verify **case cutouts** align with USB-C, SD, button, display window, mic hole, BME688 vent.
- [ ] For any part with a heat-sink pad (TDFN EP, QFN EP): verified tied to the correct net (usually GND) AND has stitching vias into the ground plane.

## §10 — Assembly plan

- [ ] Reflow order documented (bottom-side USB-C last, MAX-M10S last for antenna).
- [ ] Parts with hidden thermal pads (BQ25619, TPS62840, MAX17048, TMP117, PCF85063, MAX30101) have documented rework strategy — hot air or iron, and pre-heat temperature.
- [ ] Any part that will not survive a second reflow (MAX-M10S, WS2812B) is placed and reflowed in the final pass only.

---

## Rev-by-rev escape ledger

Every fab escape adds an entry. This is how the checklist grows.

| Rev | Escape | Section added |
|---|---|---|
| iv7.1 → Mk1b | XC6206 Vin/Vout library-symbol swap killed MAX30101 | §5 |
| iv7.1 → Mk1b | BSS138W batch die-leak | §5 |
| iv7.1 → Mk1b | LSM/BME/mic VDDIO on 1V8 back-fed rail via ESD clamps | §7 |
| iv7.1 → Mk1b | AMOLED FPC receptacle footprint mirrored (**founding case**) | §1 |
| iv7.1 → Mk1b | Flashlight FET wired source-follower | (schematic-side, not fab) |
| iv7.1 → Mk1b | BQ25619 R12 (5k1 to +3V3) trapped ship mode | (schematic-side, not fab) |
| iv7.1 → Mk1b | USB-C receptacle mechanical fatigue after ~50 plug cycles | §4 (pigtail fallback) |
