# Licensing

Kompic̄ is a multi-part project (hardware design, firmware, and creative/documentation content). Because no single open-source license fits all three kinds of material, each part is licensed separately. This is a normal, widely-used pattern.

## Summary

| Folder | Material | License | Full text |
|--------|----------|---------|-----------|
| `/hardware` | KiCad schematics, PCB layout, gerbers, BOM | **CERN-OHL-S v2** | [`/hardware/LICENSE`](hardware/LICENSE) |
| `/firmware` | ESP32-S3 source code, ML models | **GPLv3** | [`/firmware/LICENSE`](firmware/LICENSE) |
| `/case` | 3D-printable / CAD case models | **CC BY-SA 4.0** | [`/case/LICENSE`](case/LICENSE) |
| `/docs` | Documentation site, text, images, renders | **CC BY-SA 4.0** | [`/docs/LICENSE`](docs/LICENSE) |
| repository root | headline license for GitHub | **GPLv3** | [`/LICENSE`](LICENSE) |

GitHub shows **GPL-3.0** as the project's primary license (it reads the root `LICENSE` file). The per-folder table above is authoritative for each part.

## What these licenses have in common

All three are **copyleft** ("share-alike") licenses. They let anyone use, study, modify, build on — and sell — the work, on one condition: if you distribute a modified version, it must remain just as open as you received it, and you must keep the original author's notices. Copyleft uses copyright to keep the project open downstream; it never forbids selling, only closing.

## Per-license plain English

### CERN-OHL-S v2 — hardware
The CERN Open Hardware License, Strongly-reciprocal, written for physical hardware design files. Anyone may study, make, modify, manufacture, and sell hardware based on the design. If they distribute a modified design (or sell boards made from it), they must release their modified design files — the actual editable source, e.g. the KiCad project — under CERN-OHL-S v2 as well.

### GPLv3 — firmware
The GNU General Public License v3. Anyone may run, study, modify, and redistribute the firmware. If they distribute a modified version (for example, ship a device running their fork), they must publish the full corresponding source under GPLv3, keep the notices, and state their changes. Includes an explicit patent grant and anti-lockdown ("tivoization") provisions.

### CC BY-SA 4.0 — docs, images, 3D / case
Creative Commons Attribution-ShareAlike, intended for creative/content works. Reuse and remix are free, with two conditions: **attribution** (credit the original author) and **share-alike** (shared derivatives must stay under CC BY-SA 4.0).

## Copyright

Copyright © 2026 Ivan Porupski. Each contributor retains copyright on their own contributions; the project as a whole remains under the copyleft licenses above. There is no CLA and no separate commercial license — contributions are accepted inbound under the same license as the files they modify (inbound = outbound).

---

*This document is a plain-language summary for convenience and is not legal advice. The full license texts linked above are the binding terms.*
