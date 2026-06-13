# PHASE 3 BATCH — SENSORS 2 (GPS + ENV + HR + SKIN TEMP)
**Assigned:** 10 June 2026
**Firmware version:** iv7.1.f0.0
**Master instructions:** docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md (required reading)

---

## MODULES TO PORT (in order)

### Module 1: MAX-M10S (GNSS Receiver)

**Hardware spec from v7.2:**
- Bus: UART1 (GPIO17 TX→GPS, GPIO18 RX←GPS)
- Protocol: NMEA + UBX binary (both on same UART)
- 1PPS TimePulse: GPIO46 (edge ISR, RTC discipline later Phase 2+)
- Baud: typically 38400 or 9600 (check datasheet default)

**Code audit:**
Old code: TU10F (reuse NMEA parsing, timegm() UTC seed, RTC sync pattern). New chip is MAX-M10S (u-blox; same NMEA protocol, adds UBX-NAV-TIMEUTC for time-before-fix). Core NMEA logic reusable; add UBX parser for atomic time sync.

**Implementation scope:**
- UART1 driver (rx_buffer, line discipline)
- NMEA parser: GGA (position), RMC (date+position+time), VTK (speed) — reuse old logic
- UBX-NAV-TIMEUTC parser (new): raw UTC time without waiting for position fix
- Boot RTC seed: prefer UBX-TIMEUTC if available immediately; fallback to NMEA+timegm()
- 1PPS ISR on GPIO46: edge count for RTC discipline (Phase 2+; Phase 1 just count)
- 200 ms polling task (UART drain + parse)
- Profiling: boot init, parse latency, 1PPS ISR overhead

**Test harness:**
- Can we initialize UART1?
- Can we parse NMEA GGA/RMC sentences (simulate with hardcoded strings)?
- Can we parse UBX-NAV-TIMEUTC (simulate)?
- Does 1PPS ISR fire (GPIO46) and count edges?
- Measure: UART drain time, parse latency, ISR overhead

**Deliverables:**
- `/docs/porting/MAX_M10S_2026-06-10_iv7.1.f0.0.md`
- `/components/max_m10s/max_m10s.{c,h}` (UART driver, NMEA parser, UBX parser, 1PPS ISR)
- `/components/max_m10s/gps_tile.{c,h}` (position, altitude, speed, time display; UBX-TIMEUTC updates RTC atomically)
- `/components/max_m10s/CMakeLists.txt`
- `/test/test_max_m10s.c` (standalone, UART init + NMEA/UBX parse + 1PPS ISR emulation)

---

### Module 2: BME688 (Environmental Sensor)

**Hardware spec from v7.2:**
- Bus: I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
- Address: 0x76
- No interrupt pin wired
- Heater: on-chip, firmware-controlled sequences (tens of ms blocking on I2C)

**Code audit:**
Old code: BME280 (Bosch sensor family, same address 0x76). BME688 is the next gen: adds gas sensor (VOC measurement), heater control, forced+sleep modes. Bosch BME68x library available; port to it.

**Implementation scope:**
- Replace raw BME280 code with Bosch BME68x library integration
- Register map: pressure, temperature, humidity, gas resistance
- Forced mode + gas heater sequence: configure heater profile → trigger measurement → wait on-chip → read data
- Heater blocking (tens of ms) happens in `task_env` at low priority (prio 2) so higher-priority devices (touch, button) aren't starved
- Read-before-write pattern: preserve Core-1 UI fields when broker updates
- 2 s polling task (sensor takes time to stabilize after heater)
- Profiling: boot init, per-measurement I2C overhead (heater sequence cost), heap usage (BME68x lib state)

**Test harness:**
- Can we initialize I2C + BME688?
- Can we read T/P/H (forced mode)?
- Can we enable gas heater and trigger a measurement?
- Does the heater block for expected time (measure ISR latency on other cores)?
- Measure: boot init, per-measurement cost, heater blocking time

**Deliverables:**
- `/docs/porting/BME688_2026-06-10_iv7.1.f0.0.md`
- `/components/bme688/bme688_drv.{c,h}` (Bosch BME68x lib wrapper, heater state machine)
- `/components/bme688/env_tile.{c,h}` (T/P/H/gas display, altitude via fusion)
- `/components/bme688/CMakeLists.txt` (add Bosch BME68x managed component)
- `/test/test_bme688.c` (standalone, I2C init + T/P/H read + gas heater sequence + heater blocking measurement)

---

### Module 3: MAX30101 (HR / PPG Sensor)

**Hardware spec from v7.2:**
- Bus: I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
- Address: 0x57
- INT pin: GPIO7 (RTC wake: PPG FIFO during sleep HR)
- LEDs: red + IR + greenish (3 channels, vs. MAX30102's 2)
- FIFO: 32-sample hardware buffer

**Code audit:**
Old code: MAX30102 (register-compatible family, same I2C handshake, FIFO read pattern reusable). New chip MAX30101 adds a 3rd LED (greenish). Register map nearly identical; datasheet confirms compatibility.

**Implementation scope:**
- Register map: LED config (3 channels), FIFO control, data read
- INT7 ISR: FIFO threshold (or data ready), wake source for background HR during sleep
- INT-driven task: read FIFO on interrupt (low latency for HR accuracy)
- HR algorithm: stub only (Phase 3, real algorithm Phase 4+)
- LED current control: scope for power optimization
- Profiling: boot init, ISR latency, FIFO read cost, LED current consumption

**Test harness:**
- Can we initialize I2C + MAX30101?
- Can we configure LED currents (red, IR, green)?
- Can we read FIFO samples (32-sample burst)?
- Does INT7 fire on FIFO threshold?
- Measure: boot init, ISR latency, FIFO read time

**Deliverables:**
- `/docs/porting/MAX30101_2026-06-10_iv7.1.f0.0.md`
- `/components/max30101/max30101.{c,h}` (register map, LED config, FIFO read, INT7 ISR)
- `/components/max30101/health_tile.{c,h}` (HR stub, SpO2 stub, integrates TMP117 skin temp display)
- `/components/max30101/CMakeLists.txt`
- `/test/test_max30101.c` (standalone, I2C init + LED config + FIFO read + INT7 emulation)

---

### Module 4: TMP117 (Skin Temperature Sensor)

**Hardware spec from v7.2:**
- Bus: I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
- Address: 0x48
- TMP_ALRT pin: not on GPIO (ignore)
- Resolution: 16-bit temperature (0.0078°C/LSB)

**Code audit:**
New chip, no old code. Simple I2C temperature sensor.

**Implementation scope:**
- Register map: configuration, temperature data
- 5 s polling task (skin temp changes slowly)
- Temperature read + broker update (read-before-write pattern)
- No tile (subtile in health_tile, read from broker)
- Profiling: boot init, per-read I2C cost

**Test harness:**
- Can we initialize I2C + TMP117?
- Can we read temperature (16-bit, convert to °C)?
- Measure: boot init, per-read cost

**Deliverables:**
- `/docs/porting/TMP117_2026-06-10_iv7.1.f0.0.md`
- `/components/tmp117/tmp117.{c,h}` (register map, temperature read)
- `/components/tmp117/CMakeLists.txt`
- `/test/test_tmp117.c` (standalone, I2C init + temperature read)

---

## OUTPUT

You will produce **4 dated .md files** in `/docs/porting/`:
- MAX_M10S_2026-06-10_iv7.1.f0.0.md
- BME688_2026-06-10_iv7.1.f0.0.md
- MAX30101_2026-06-10_iv7.1.f0.0.md
- TMP117_2026-06-10_iv7.1.f0.0.md

Plus driver skeletons in `/components/` and test harnesses in `/test/` for each.

**Profiling template must be filled:**
- Boot-time cost
- Per-operation cost (especially heater blocking for BME688, ISR latency for MAX30101)
- Memory
- Current draw estimate (TBD; LED current is user-configurable)

**Defects logged** as `[DEFECT-NN]` in each .md.

**Commit strategy:** one commit per module (4 total), or one commit at the end. Your choice.

---

## DEPENDENCY NOTES

- MAX-M10S updates PCF85063 RTC atomically on UBX-TIMEUTC parse (requires PCF85063 API from Phase 0)
- BME688 heater sequences block I2C bus 1 for tens of ms; designed to run at low priority (prio 2) so touch/button aren't starved
- MAX30101 INT7 is a wake source; design for background HR during sleep (Phase 2+)
- TMP117 is read-only; no configuration needed at runtime
- health_tile will integrate MAX30101 + TMP117 + (later) SpO2 algorithm

---

## REMEMBER

- Reference v7.2 §GPIO ASSIGNMENT (GPIO46 1PPS, GPIO7 MAX_INT), §I2C BUS ASSIGNMENT (bus 1)
- MAX-M10S datasheet is key for UART config and UBX frame format
- Bosch BME68x library is a managed component (add to idf_component.yml)
- Profiling: heater blocking time is critical (affects touch latency under load)
- Test harnesses are standalone and reproducible
- Live in the .md. All work is visible

Go.
