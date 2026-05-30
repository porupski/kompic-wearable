# Kompic̄ &nbsp;·&nbsp; Mk I

**Compact Open Multisensor Platform & Apparatus**

*/ˈkompitɕ/* — Croatian for "little computer" (and the word you'd only use for an actual friend, not a stranger).

A fully open-source, offline-first, ML-capable wrist-mounted sensor platform. The hardware collects. The firmware infers. You own everything.

> **Internal hardware version:** `iv7.0` — tracked in the docs to keep the two PCBs matched across revisions.

Page:
https://porupski.github.io/kompic-wearable/

---

## What it is

Kompic̄ is a wrist-mounted sensor platform built around an **ESP32-S3** (dual-core, 8 MB PSRAM, 16 MB flash) with a sensor suite denser than any open-source watch, and enough on-device compute to run real neural networks on that data — sleep staging, sound classification, sensor fusion — all locally, all offline, all inspectable.

It is **not** a companion in the AI sense and **not** a phone accessory. It runs standalone. BLE is a capability, not a requirement. There is no cloud, no subscription, no account.

### Philosophy

- **Open source** — hardware (KiCad), firmware, ML models, and case design. Everything inspectable, modifiable, forkable.
- **Offline first** — works fully standalone. No phone, no cloud, no subscription.
- **Raw data access** — PPG waveforms, ECG traces, IMU streams, mic audio, environmental readings. Not just processed metrics.
- **ML-native** — the compute surplus exists specifically to run on-device inference.

---

## Hardware at a glance

| Domain | Detail |
|--------|--------|
| MCU | ESP32-S3 dual-core Xtensa LX7 @ 240 MHz, 8 MB PSRAM, 16 MB flash |
| Storage | MicroSD (128 GB+) |
| Connectivity | BLE 5.x |
| Display | 2.06" rectangular AMOLED, 45.6 × 37.3 mm glass, QSPI |
| PCBs | Two 0.8 mm 4-layer boards: main + skin-facing daughter |
| Case | Two-piece SLM titanium (Ti-6Al-4V), ~48 × 40 × 12 mm |
| Power | ~380 mAh LiPo, USB-C charging |

### Sensor suite

Optical HR/SpO2 (MAX30101) · 6-axis IMU with on-chip ML core (LSM6DSV16X) · single-lead ECG (LSM6DSV16X Qvar) · skin temperature (TMP117) · magnetometer (LIS3MDLTR) · GPS (u-blox M10S) · environmental T/P/H/VOC (BME688) · ambient light (VEML6030) · MEMS microphone (MSM261DGT003) · LRA haptics (ELV1411A + DRV2605L).

---

## Repository layout

```
kompic-wearable/
├── hardware/    KiCad schematics, PCB layout, production files   [CERN-OHL-S v2]
├── firmware/    ESP32-S3 dual-core firmware, ML models           [GPLv3]
├── case/        Onshape / 3D-printable case models               [CC BY-SA 4.0]
└── docs/        Documentation site (GitHub Pages) + images       [CC BY-SA 4.0]
```

---

## Project status

Active development. Schematic capture complete; PCB routing underway; first fabrication (Mk I, `iv7.0`) in preparation.

| Area | Status |
|------|--------|
| Circuit + PCB | In progress (routing) |
| Fabrication + assembly | Planned |
| 3D models + case | Planned |
| Firmware | Planned |
| Application & data analysis | Planned |

Documentation: **https://porupski.github.io/kompic-wearable/**

---

## License

Kompic̄ is released under three copyleft licenses, one per kind of material. All three keep the project open and require attribution — anyone may use, modify, build on, and even sell derivatives, **provided they keep their version equally open and credit the original author.**

| Part of the project | License | What it means in one line |
|---------------------|---------|---------------------------|
| **Hardware** (`/hardware`) | [CERN-OHL-S v2](https://ohwr.org/cern_ohl_s_v2.txt) | Make/modify/sell boards freely; distributed modifications must publish their full design files under the same license. |
| **Firmware** (`/firmware`) | [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html) | Run/modify/redistribute freely; distributed modifications must publish full source under GPLv3. |
| **Docs & 3D / case** (`/docs`, `/case`) | [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) | Reuse/remix freely with credit; shared derivatives stay under CC BY-SA. |

GitHub displays **GPL-3.0** as the repository's headline license; the per-folder breakdown above is authoritative. See [`LICENSING.md`](LICENSING.md) for details.

**Copyright © 2026 Ivan Porupski.** Each contributor retains copyright on their own contributions; the whole project remains under the copyleft licenses above.

There is no separate commercial license and no contributor license agreement (CLA). Contributions are accepted under the same licenses as the files they touch (inbound = outbound).

---

## Contributing

Contributions are welcome. By submitting a contribution you agree it is licensed under the same license that already covers the file(s) you are changing (see table above). Keep copyright and license notices intact.

---

## Disclaimer

Kompic̄ is **not a medical device** and is not intended to diagnose, treat, or prevent any condition. All work with these electronics and lithium batteries is done at your own risk. The author and contributors are not liable for any direct, indirect, incidental, or consequential damages.
