# ESP32-S3-WROOM-1U-N16R8 — GPIO Pinout (Final)
**Smartwatch PCB | Session 9 | All 32 GPIOs Assigned**

---

## Master Pinout Table

| GPIO | KiCad Label | Signal | Function | IOMUX | RTC Wake | Boot Behavior |
|------|-------------|--------|----------|-------|----------|---------------|
| 0 | IO0 | DRV_EN | Haptic driver enable | — | ✓ | Strapping, pull-up → HIGH → haptic on at boot |
| 1 | IO1 | SDA | I2C bus 1 data | — | ✓ | Clean |
| 2 | IO2 | SCL | I2C bus 1 clock | — | ✓ | Clean |
| 3 | IO3 | QSPI-RESET | Display hard reset | — | — | Strapping, no pull → ext pull-up → not reset |
| 4 | IO4 | SDA_bus2 | I2C bus 2 data | — | ✓ | Clean |
| 5 | IO5 | SCL_bus2 | I2C bus 2 clock | — | ✓ | Clean |
| 6 | IO6 | TP-INT_3V3 | Touch interrupt (CST9217) | — | ✓ | Clean |
| 7 | IO7 | TimePulse | GPS 1PPS input | — | ✓ | Clean |
| 8 | IO8 | LSM_INT1 | IMU wake-on-motion | — | ✓ | Clean |
| 9 | IO9 | QSPI-I2 | Display D2 (HD) | FSPIHD | ✓ | Clean |
| 10 | IO10 | MAX_INT | MAX30101 FIFO interrupt | — | ✓ | Clean |
| 11 | IO11 | QSPI-IO | Display D0 (MOSI) | FSPID | ✓ | Clean |
| 12 | IO12 | QSPI-CLK | Display clock | FSPICLK | ✓ | Clean |
| 13 | IO13 | QSPI-I1 | Display D1 (MISO) | FSPIQ | ✓ | Clean |
| 14 | IO14 | QSPI-I3 | Display D3 (WP) | FSPIWP | ✓ | Clean |
| 15 | IO15 | RTC_INT | RTC alarm wake (PCF85063) | — | ✓ | Clean |
| 16 | IO16 | BQ_BUTTON | Tactile button input | — | ✓ | Clean |
| 17 | IO17 | M10-TX | GPS UART TX | U1TXD | ✓ | Clean |
| 18 | IO18 | M10-RX | GPS UART RX | U1RXD | ✓ | Clean |
| 19 | IO19 | D- | USB D- | USB | — | Reserved (fixed) |
| 20 | IO20 | D+ | USB D+ | USB | — | Reserved (fixed) |
| 21 | IO21 | QSPI_TE | Tearing effect input | — | ✓ | Clean |
| 35 | IO35 | — | PSRAM | — | — | Unavailable |
| 36 | IO36 | — | PSRAM | — | — | Unavailable |
| 37 | IO37 | — | PSRAM | — | — | Unavailable |
| 38 | IO38 | SD_CLK | SD card clock | — | — | Clean |
| 39 | IO39 | SD_CMD | SD card command | — | — | Clean |
| 40 | IO40 | SD_DAT0 | SD card data | — | — | Clean |
| 41 | IO41 | GPIO_FLASHLIGHT | Flashlight LEDC PWM | — | — | Clean (MTDI, no pull) |
| 42 | IO42 | Sig_LED_Din | WS2812B RMT output | — | — | Clean (MTMS) |
| 43 | TXD0 | BQ_INT | BQ25619 charger interrupt | — | — | Boot log output ~3ms, ext pull-up |
| 44 | RXD0 | TP-RST_3V3 | Touch reset (CST9217) | — | — | Boot log RX input, ext pull-up |
| 45 | IO45 | OLED_EN | Display power enable | — | — | Strapping, pull-down → LOW → display off |
| 46 | IO46 | QSPI_CS | Display chip select | — | — | Strapping, pull-down → LOW → CS asserted, harmless |
| 47 | IO47 | Mic_CLK | PDM mic clock output | — | — | Clean (3.3V domain on R8) |
| 48 | IO48 | Mic_Dout | PDM mic data input | — | — | Clean (3.3V domain on R8) |

---

## By Module Side (KiCad Schematic Wiring Reference)

### Left Side (top to bottom)

| KiCad Pin | GPIO | Signal |
|-----------|------|--------|
| GND | — | GND |
| 3V3 | — | +3V3 |
| EN | — | Enable (RC network) |
| IO4 | 4 | SDA_bus2 |
| IO5 | 5 | SCL_bus2 |
| IO6 | 6 | TP-INT_3V3 |
| IO7 | 7 | TimePulse |
| IO15 | 15 | RTC_INT |
| IO16 | 16 | BQ_BUTTON |
| IO17 | 17 | M10-TX |
| IO18 | 18 | M10-RX |
| IO8 | 8 | LSM_INT1 |
| IO19 | 19 | D- (USB) |
| IO20 | 20 | D+ (USB) |

### Bottom Side (left to right)

| KiCad Pin | GPIO | Signal |
|-----------|------|--------|
| IO3 | 3 | QSPI-RESET |
| IO46 | 46 | QSPI_CS |
| IO9 | 9 | QSPI-I2 (D2) |
| IO10 | 10 | MAX_INT |
| IO11 | 11 | QSPI-IO (D0) |
| IO12 | 12 | QSPI-CLK |
| IO13 | 13 | QSPI-I1 (D1) |
| IO14 | 14 | QSPI-I3 (D3) |
| IO21 | 21 | QSPI_TE |
| IO47 | 47 | Mic_CLK |
| IO48 | 48 | Mic_Dout |
| IO45 | 45 | OLED_EN |

### Right Side (top to bottom)

| KiCad Pin | GPIO | Signal |
|-----------|------|--------|
| GND | — | GND |
| GND | — | GND |
| IO1 | 1 | SDA |
| IO2 | 2 | SCL |
| TXD0 | 43 | BQ_INT |
| RXD0 | 44 | TP-RST_3V3 |
| IO42 | 42 | Sig_LED_Din |
| IO41 | 41 | GPIO_FLASHLIGHT |
| IO40 | 40 | SD_DAT0 |
| IO39 | 39 | SD_CMD |
| IO38 | 38 | SD_CLK |
| IO37 | 37 | PSRAM (unavailable) |
| IO36 | 36 | PSRAM (unavailable) |
| IO35 | 35 | PSRAM (unavailable) |
| IO0 | 0 | DRV_EN |

---

## IOMUX Assignments

| Peripheral | GPIOs | Benefit |
|------------|-------|---------|
| Display QSPI (SPI2/FSPI) | 9, 11, 12, 13, 14 | Direct IOMUX — bypasses GPIO matrix for high-speed QSPI |
| GPS UART (UART1) | 17, 18 | Direct IOMUX — clean UART path |

---

## Sleep Wake Sources (RTC GPIO)

| GPIO | Signal | Wake Trigger |
|------|--------|--------------|
| 6 | TP-INT_3V3 | Tap to wake (touch) |
| 8 | LSM_INT1 | Raise to wake (accelerometer) |
| 10 | MAX_INT | PPG FIFO during sleep monitoring |
| 15 | RTC_INT | Scheduled alarm wake |
| 16 | BQ_BUTTON | Button press wake |

---

## Unassigned Signals (No GPIO — Test Pad / Solder Jumper Only)

| Signal | Disposition |
|--------|-------------|
| SD_Cd | Poll or ignore |
| M10S_RST | 10kΩ pull-up keeps GPS running, no GPIO reset |
| LSM_INT2 | Firmware polls |
| LIS_INT | Firmware polls |
| LIS_DRDY | Firmware polls |
| TMP_ALRT | Firmware polls |
| VEML_INT | Firmware polls |
| BQ_STAT | Poll via I2C or ignore |

---

## Critical Warnings

⚠️ **GPIO45 (OLED_EN): NEVER add an external pull-up.** Must be LOW at reset for PSRAM 3.3V voltage selection. Internal pull-down ensures this. Display stays off until firmware drives HIGH — this is correct and desired behavior.

⚠️ **GPIO46 (QSPI_CS): LOW at boot** = display CS asserted, but no QSPI clock running so no data corruption. Harmless.

⚠️ **GPIO0 (DRV_EN): HIGH at boot** from internal pull-up. DRV2605 enables at boot, runs autocalibration. Firmware takes control afterward. If unwanted, swap solder jumper JP12 default to 3.3V (always on) and remove GPIO connection.

⚠️ **GPIO43 (BQ_INT): Boot log serial output for ~3ms.** BQ25619 INT is open-drain with 10kΩ pull-up. Boot log drives pin as output briefly — charger IC not affected (it's watching, not driving). Firmware reconfigures as input after boot.
