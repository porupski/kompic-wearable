# Kompic̄ Mk I — Datasheet & Component Manifest
**Date:** 2026-06-11  
**Purpose:** Single source of truth for all datasheets, technical references, and component traceability (LCSC codes).

This is a **committed file** — part of the repo. The accompanying `fetch_datasheets.py` script downloads PDFs on demand and caches them locally (in `.gitignore`d `docs/datasheets/`).

---

## MCU & Modules

| Component | Vendor | Part Number | LCSC Code | Datasheet | Notes |
|---|---|---|---|---|---|
| **MCU** | Espressif | ESP32-S3-WROOM-1U-N16R8 | C3013946 | [ESP32-S3 Datasheet](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf) | 16 MB flash, 8 MB octal PSRAM, -1U external antenna |
| **MCU Ref** | Espressif | — | — | [ESP32-S3 Technical Reference](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf) | Register-level reference |
| **MCU Module** | Espressif | — | — | [ESP32-S3-WROOM-1U Datasheet](https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf) | Module pinout & form factor |

## Display & Touch

| Component | Vendor | Part Number | LCSC Code | Datasheet | Notes |
|---|---|---|---|---|---|
| **Display IC** | Chipone | CO5300AF-51 | — | [CO5300 Datasheet](https://admin.osptek.com/uploads/CO_5300_Datasheet_V0_00_20230328_07edb82936.pdf) | AMOLED driver, QSPI, COLMOD 0x77 (RGB888) mandatory |
| **Display Panel** | — | PY206-W38-V2 (2.06" AMOLED) | — | *Chinese vendor, no public datasheet; see v7.2 PCB doc for command set* | 410×502 resolution, column offset 22 |
| **Touch IC** | Chipone / Hynitron | CST9217 | — | *No public datasheet found; LCSC: search "CST9217"* | I2C @0x5A, INT on GPIO6, ISR-driven |

## Sensors — I2C Bus 1 (GPIO1/2, 400 kHz)

| Component | Vendor | Part Number | LCSC Code | Datasheet | I2C Addr | Notes |
|---|---|---|---|---|---|---|
| **Light** | Vishay | VEML6030 | — | [VEML6030 Datasheet](https://www.vishay.com/docs/84366/veml6030.pdf) | 0x10 | Ambient light, auto-ranging |
| **Magnetometer** | ST | LIS3MDLTR | — | [LIS3MDLTR Datasheet](https://www.st.com/resource/en/datasheet/lis3mdl.pdf) | 0x1C | 3-axis mag, ±4 gauss (note: LRA magnet ~1G offset) |
| **GPS** | u-blox | MAX-M10S | — | [MAX-M10S Datasheet](https://content.u-blox.com/sites/default/files/MAX-M10S_DataSheet_UBX-20035208.pdf) | 0x42 (I2C) | UART on GPIO17/18; 1PPS on GPIO46 |
| **Skin Temp** | TI | TMP117AIDRVR | — | [TMP117 Datasheet](https://www.ti.com/lit/ds/symlink/tmp117.pdf) | 0x48 | Precision temperature (daughter board) |
| **RTC** | NXP | PCF85063A | — | [PCF85063A Datasheet](https://www.nxp.com.cn/docs/en/data-sheet/PCF85063A.pdf) | 0x51 | Real-time clock, 1 Hz backup |
| **Heart Rate** | Analog Devices | MAX30101 | — | [MAX30101 Datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX30101.pdf) | 0x57 | PPG + SpO2, INT on GPIO7 (daughter board) |
| **Touch Controller** | Chipone | CST9217 | — | (See Display & Touch section) | 0x5A | Capacitive touch, INT on GPIO6 |
| **IMU** | ST | LSM6DSV16X | — | [LSM6DSV16X Datasheet](https://www.st.com/resource/en/datasheet/lsm6dsv16x.pdf) | 0x6B | 6-axis accel + gyro, INT1 on GPIO8 (raise-to-wake) |
| **Environment** | Bosch | BME688 | — | [BME688 Datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme688-ds000.pdf) | 0x76 | Temperature, humidity, pressure, gas (heater) |
| **Environment Ref** | Bosch | — | — | [BME688 Application Note AN001](https://www.bosch-sensortec.com/media/boschsensortec/downloads/application_notes_1/bst-bme688-an001.pdf) | — | Heater control, gas measurement mode |

## Sensors — I2C Bus 2 (GPIO4/5, 400 kHz)

| Component | Vendor | Part Number | LCSC Code | Datasheet | I2C Addr | Notes |
|---|---|---|---|---|---|---|
| **Haptic Driver** | TI | DRV2605LDGSR | — | [DRV2605L Datasheet](https://www.ti.com/lit/ds/symlink/drv2605l.pdf) | 0x5A | Piezo / LRA driver, on bus 2 (not bus 1) |
| **Charger** | TI | BQ25619RTWR | C2864534 | [BQ25619 Datasheet](https://www.ti.com/lit/ds/symlink/bq25619.pdf) | 0x6A | 1.5A charger + 1A boost (PMID), fuel gauge, button gate |

## Power Management

| Component | Vendor | Part Number | LCSC Code | Datasheet | Purpose | Notes |
|---|---|---|---|---|---|---|
| **3.3V Buck** | TI | TPS62840DLCR | C2071859 | [TPS62840 Datasheet](https://www.ti.com/lit/ds/symlink/tps62840.pdf) | 750 mA → 3.2V rail | Output set to 3.2V (100 kΩ divider), PFM/PWM via JP15 |
| **1.8V LDO** | Torex | XC6206P182MR | — | [XC6206 Datasheet](https://product.torexsemi.com/system/files/series/xc6206.pdf) | 200 mA → 1.8V rail | Fixed 1.8V output |
| **RTC Backup Cell** | Panasonic | ML621 | — | [ML621 Datasheet](https://mediap.industry.panasonic.eu/assets/imported/industrial.panasonic.com/cdbs/www-data/pdf/AAF4000/ast-ind-174496.pdf) | RTC timekeeping during power-off | 5.5 mAh, rechargeable, ~4 year hold |

## RF & Antennas

| Component | Vendor | Part Number | LCSC Code | Datasheet | Purpose | Notes |
|---|---|---|---|---|---|---|
| **BLE Antenna** | Johanson | 2450AT18A100E | — | [2450AT18A Datasheet](https://www.johansontechnology.com/docs/3827/Antenna-2450AT18A0100001E-Rev4.0.pdf) | 2.4 GHz BLE | Chip antenna, top-left corner, 14×8mm keep-out |
| **GPS Antenna** | — | Ceramic patch | — | *Reference: v7.2 PCB doc §GPS antenna mounting* | 1.575 GHz L1 | Perpendicular edge mount, through epoxy window |

## Haptics & Actuators

| Component | Vendor | Part Number | LCSC Code | Datasheet | Purpose | Notes |
|---|---|---|---|---|---|---|
| **LRA Motor** | AAC / TDK | ELV1411A | — | [ELV1411A Datasheet](https://nfpshop.com/wp-content/uploads/2025/08/linear-vibration-motor-model-NFP-ELV1411A.pdf) | Haptic actuator | Contains small permanent magnet; ~6–7mm from magnetometer |
| **Status LED** | — | Generic white 0603 | C601698 | [Flashlight LED LCSC](https://www.lcsc.com/datasheet/C601698.pdf) | Indicator pixel | Single RGB WS2812, GPIO42 |
| **Flashlight LED** | — | Generic white 0603 | C601698 | (See above) | Torch output | LEDC PWM via GPIO41 |

## USB & Charging

| Component | Vendor | Part Number | LCSC Code | Datasheet | Purpose | Notes |
|---|---|---|---|---|---|---|
| **USB-C Connector** | JUSHUO | JS16T | C49118447 | [JS16T Datasheet](https://www.lcsc.com/datasheet/C49118447.pdf) | Power + data | Rubber O-ring seal; CC1/CC2 = 5.1k to GND |
| **ESD Diode Array** | TI | TPD2E001 | — | [TPD2E001 Datasheet](https://www.ti.com/lit/ds/symlink/tpd2e001.pdf) | USB D+/D- protection | Series array on GPIO19/20 lines |

## I/O & Expansion

| Component | Vendor | Part Number | LCSC Code | Datasheet | Purpose | Notes |
|---|---|---|---|---|---|---|
| **SD Card Reader** | — | Generic SDMMC | C393941 | [SDMMC Socket LCSC](https://www.lcsc.com/datasheet/C393941.pdf) | Mass storage | SDMMC on GPIO38/39/40; mount on demand |
| **Crown Encoder** | — | Rotary tactile | C116648 | [Encoder LCSC](https://www.lcsc.com/datasheet/C116648.pdf) | Navigation input | PCNT-driven, GPIO21 (wake), GPIO43 (boot-log safe) |
| **Power Button** | — | Tactile switch | C115361 | [Push Button LCSC](https://www.lcsc.com/datasheet/C115361.pdf) | Power control | Dual-wired to BQ_BUTTON (GPIO16) + BQ25619 QON |
| **Pogo Pins (Qvar)** | — | Spring contacts | C7429439 | [Pogo Pin LCSC](https://www.lcsc.com/datasheet/C7429439.pdf) | ECG electrodes | Two contacts: skin (sleeve) + crown (groove) |

## Audio

| Component | Vendor | Part Number | LCSC Code | Datasheet | Purpose | Notes |
|---|---|---|---|---|---|---|
| **PDM Microphone** | — | Generic PDM | C965555 | [PDM Mic LCSC](https://www.lcsc.com/datasheet/C965555.pdf) | Audio input | I2S PDM on GPIO47/48; PTFE sticker window |

## Passive Components (0402, single-supply variants)

### Capacitors — General (0402, 100nF, 50V, X7R)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **100nF** | Samsung | CL05B104KO5NNNC | C1525 | 20+ | Bulk decoupling, logic domains |
| **100nF** | FH | 0402CG101J500NT | C1546 | 2 | Signal layer bypassing |
| **1pF** | FH | 0402CG1R0C500NT | C1550 | 1 | BLE antenna match (series) |

### Capacitors — Bulk (0402, 10µF, 16V, X7R)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **10µF** | Samsung | CL05A106MQ5NUNC | C15525 | 8 | Power supply filtering |

### Capacitors — Tantalum (0402, 4.7µF, 16V)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **4.7µF** | Samsung | CL05A475MP5NRNC | C23733 | 4 | Switching rail (TPS62840), boost output |

### Capacitors — High-Value (0402, 10µF, 50V, X7R)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **10µF** | Samsung | CL05A105KA5NQNC | C52923 | 5 | TPS62840 input, audio coupling (future) |

### Resistors — Pull-ups (0402, 5.1kΩ, 5%, 1/16W)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **5.1kΩ** | Uni-Royal | 0402WGF5101TCE | C25905 | 16 | I2C bus pull-ups (both buses), CS pull-ups, interrupt pull-ups |

### Resistors — Signal Series (0402, 470Ω, 5%, 1/16W)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **470Ω** | Uni-Royal | 0402WGF470JTCE | C25118 | 4 | WS2812 signal series, flashlight LED series (2×) |
| **470Ω** | Uni-Royal | 0402WGF4700TCE | C25117 | 2 | Qvar electrode RC filters |

### Resistors — Pull-downs / Gates (0402, 10kΩ, 5%, 1/16W)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **10kΩ** | Uni-Royal | 0402WGF1002TCE | C25744 | 15 | Gate pull-downs, enable pull-ups, GPS reset, BQ TS fixed, SD pull-ups, RTC backup charge |

### Resistors — Special (0402, 100kΩ & 1kΩ, 5%, 1/16W)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **100kΩ** | Uni-Royal | 0402WGF1003TCE | C25741 | 1 | TPS62840 VSET (→3.2V preset) |

### Inductors (0402)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Rating | Notes |
|---|---|---|---|---|---|---|
| **27nH** | muRata | LQW15AN27NJ00D | C113112 | 10 | 280 mA | BLE antenna match (shunt to GND) |
| **2.7nH** | muRata | LQW15AN2N7C00D | C98058 | 20 | 850 mA | BLE antenna match (series to antenna) |
| **3.9nH** | muRata | LQG15HS3N9B02D | C5452416 | 20 | 750 mA | BLE antenna match (series to antenna) |
| **2.2µH** | — | — | — | 1 | TPS62840 boost | TPS62840 main inductor (1008 case, not 0402) |

### Capacitors — RF Tuning (0402, 50V)

| Value | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **47pF** | TDK | CGA2B2C0G1H470JT0Y0F | C432937 | 50 | C0G dielectric, BLE antenna match |
| **10nF** | YAGEO | CC0402KRX7R9BB103 | C60133 | — | Standard decoupling (bulk of capacitors) |

### Thermistor (0402)

| Component | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Value | Notes |
|---|---|---|---|---|---|---|
| **NTC** | muRata | NCP15XH103F03RC | C77131 | 1 | 10kΩ @ 25°C | BQ25619 battery thermistor (TS divider), over LiPo |

## Discrete Semiconductors

| Component | Manufacturer | Part Number | LCSC Code | Qty (in v1) | Notes |
|---|---|---|---|---|---|
| **Schottky Diode** | JSCJ | 1N4148WS | C2128 | 3 | GPS + RTC backup isolation, ESD protection |
| **Flashlight Gate MOSFET** | ON Semi | BSS138W | — | 1 | N-channel, LEDC gate for GPIO41 flashlight |
| **ESD Diode Array** | ST | ESDALCL5-1BM2 | — | 1 | (See TPD2E001 above) | — |

## Crystals & Oscillators

| Component | Manufacturer | Part Number | LCSC Code | Frequency | Notes |
|---|---|---|---|---|---|
| **32 kHz RTC Crystal** | — | Generic 32768 Hz | C2842180 | 32.768 kHz | PCF85063 RTC reference, ±10 ppm typical |

---

## Usage Notes

- **LCSC codes** are the primary traceability anchor for all components. Search `https://www.lcsc.com/product-detail/{CODE}.html` to verify vendor, price, stock, and alternate sources.
- **Datasheets without public URLs** (CST9217, CO5300 panel, M10S GPS timing API docs) may require contacting vendors or checking cached versions in the repo's `docs/datasheets/` after manual download.
- **0402 passives** — wattage and voltage derived from part numbers per IPC standards (e.g., `0402WGF5101TCE` = 0402, 1/16W, ±5%, 5.1 kΩ).
- **Script automation** — `scripts/fetch_datasheets.py` (author it next) downloads from the URLs in this manifest and verifies checksums. Run manually; never in CI.

---

## To-Do

- [ ] Locate official CO5300 datasheet or vendor contact info
- [ ] Find CST9217 public datasheet or confirm Chipone/Hynitron part source
- [ ] Verify M10S UBX-TIMEUTC message format (may require u-blox support login)
- [ ] Collect SLM titanium / 3D printing tolerances for case design review
- [ ] Document BSEC / BME688 gas algorithms license (firmware v2+)

---

*Last updated: 2026-06-11 · Maintained by: Ivan Porupski*
