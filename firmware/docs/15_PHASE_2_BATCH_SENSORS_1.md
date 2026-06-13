# PHASE 2 BATCH — SENSORS 1 (IMU + COMPASS + LIGHT)
**Assigned:** 10 June 2026
**Firmware version:** iv7.1.f0.0
**Master instructions:** docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md (required reading)

---

## MODULES TO PORT (in order)

### Module 1: LSM6DSV16X (Inertial Measurement Unit)

**Hardware spec from v7.2:**
- Bus: I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
- Address: 0x6B
- INT1 pin: GPIO8 (RTC wake: raise-to-wake)
- INT2 pin: not on GPIO (ignore)
- WHO_AM_I: 0x70 (device ID verification)

**Code audit:**
Old code: QMI8658 (6-axis IMU, read-before-write pattern, complementary filter in tile, imu_tile reusable). New chip is LSM6DSV16X (ST 6-axis, register-compatible family, same interrupt model). Register map differs; tile logic reusable.

**Implementation scope:**
- Register map from LSM6DSV16X datasheet (accel/gyro control, data registers, interrupt config)
- WHO_AM_I verification (0x70)
- INT1 ISR: raise-to-wake gesture (tilt detection, wakeup-on-motion)
- 20 ms polling task (or INT1-driven, TBD based on datasheet)
- Read-before-write pattern: read broker slot first, preserve Core-1 fields, write updates
- Profiling: boot init, per-read I2C cost, ISR latency

**Test harness:**
- Can we initialize I2C + LSM6DSV16X (WHO_AM_I 0x70)?
- Can we read accel data (X/Y/Z, 16-bit)?
- Can we read gyro data (X/Y/Z, 16-bit)?
- Does INT1 fire (GPIO8) on motion/tilt?
- Measure: I2C transaction times, ISR latency, boot-to-ready

**Deliverables:**
- `/docs/porting/LSM6DSV16X_2026-06-10_iv7.1.f0.0.md`
- `/components/lsm6dsv16x/lsm6dsv16x.{c,h}` (register map, WHO_AM_I, INT1 config, data read)
- `/components/lsm6dsv16x/imu_tile.{c,h}` (complementary filter from old code, reused; tile resized for 410×502)
- `/components/lsm6dsv16x/CMakeLists.txt`
- `/test/test_lsm6dsv16x.c` (standalone, I2C init + WHO_AM_I verify + accel/gyro read + INT1 test)

---

### Module 2: LIS3MDLTR (3-Axis Magnetometer)

**Hardware spec from v7.2:**
- Bus: I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
- Address: 0x1C
- INT / DRDY pins: not on GPIO (ignore)
- WHO_AM_I: 0x3D (device ID verification)
- Note: LRA magnet is ~6–7mm away (daughter board below); expect small DC offset (~1 gauss). Hard-iron calibration subtracts it.

**Code audit:**
Old code: QMC5883P (dead; different address 0x2C, different register map). LIS3MDLTR is new from datasheet. No code to reuse except compass tile calibration pattern (hard-iron min/max sweep).

**Implementation scope:**
- Register map from LIS3MDLTR datasheet (control, data, interrupt config)
- WHO_AM_I verification (0x3D)
- 100 ms polling task (non-blocking reads)
- Hard-iron calibration: min/max sweep + offset storage in NVS (reuse old compass_tile logic)
- LRA magnet offset measurement during first-boot calibration (new)
- Read-before-write pattern (same as LSM6DSV16X)
- Profiling: boot init, per-read I2C cost, calibration overhead

**Test harness:**
- Can we initialize I2C + LIS3MDLTR (WHO_AM_I 0x3D)?
- Can we read heading (X/Y/Z, 16-bit)?
- Can we measure LRA magnet DC offset (place magnet nearby, observe field)?
- Measure: I2C transaction times, boot-to-ready

**Deliverables:**
- `/docs/porting/LIS3MDLTR_2026-06-10_iv7.1.f0.0.md`
- `/components/lis3mdl/lis3mdl.{c,h}` (register map, WHO_AM_I, data read, LRA offset measurement)
- `/components/lis3mdl/compass_tile.{c,h}` (hard-iron calibration UI from old code; add LRA offset step)
- `/components/lis3mdl/CMakeLists.txt`
- `/test/test_lis3mdl.c` (standalone, I2C init + WHO_AM_I verify + heading read + LRA offset measurement)

---

### Module 3: VEML6030 (Ambient Light Sensor)

**Hardware spec from v7.2:**
- Bus: I2C bus 1 (GPIO1 SDA, GPIO2 SCL, 400 kHz)
- Address: 0x10
- INT pin: not on GPIO (INT register available but not wired to an ISR)
- No hardware enable; sensor always draws leakage current when powered

**Code audit:**
Old code: BH1750 (simpler sensor, different command set, different lux range). VEML6030 is more capable (wider dynamic range, per-gain auto-ranging, interrupt available). Tile + EMA + auto-brightness logic reusable; low-level driver rewritten.

**Implementation scope:**
- Register map from VEML6030 datasheet (config, command, lux data, interrupt threshold)
- 500 ms polling task (non-blocking reads)
- Auto-ranging: if raw ADC near saturation/underflow, switch gain and re-read
- Auto-brightness: EMA filter on lux → percentage map → write to panel register 0x51
- Lux-to-percentage curve: recompute for VEML6030's wider range (BH1750 curve won't work)
- Optional: use log(lux) for perceptual brightness (address audit report defect D-UM about EMA undershoot)
- Profiling: boot init, per-read I2C cost, auto-brightness update overhead

**Test harness:**
- Can we initialize I2C + VEML6030?
- Can we read lux value?
- Can we detect auto-range transitions (low/normal/high gain)?
- Can we modulate panel brightness (write to CO5300 reg 0x51)?
- Measure: I2C transaction times, auto-brightness update latency

**Deliverables:**
- `/docs/porting/VEML6030_2026-06-10_iv7.1.f0.0.md`
- `/components/veml6030/veml6030.{c,h}` (register map, auto-ranging, lux read)
- `/components/veml6030/light_tile.{c,h}` (EMA filter, lux-to-percentage, auto-brightness update, panel brightness write)
- `/components/veml6030/CMakeLists.txt`
- `/test/test_veml6030.c` (standalone, I2C init + lux read + auto-range test + brightness modulation)

---

## OUTPUT

You will produce **3 dated .md files** in `/docs/porting/`:
- LSM6DSV16X_2026-06-10_iv7.1.f0.0.md
- LIS3MDLTR_2026-06-10_iv7.1.f0.0.md
- VEML6030_2026-06-10_iv7.1.f0.0.md

Plus driver skeletons in `/components/` and test harnesses in `/test/` for each.

**Profiling template must be filled:**
- Boot-time cost (I2C init + device init)
- Per-operation cost (data read, interrupt latency, auto-ranging)
- Memory (task stack, driver state, NVS calibration)
- Current draw estimate (TBD; measure on real hardware)

**Defects logged** as `[DEFECT-NN]` in each .md.

**Commit strategy:** one commit per module (3 total), or one commit at the end. Your choice.

---

## DEPENDENCY NOTES

- LSM6DSV16X has no external dependency (boot independently)
- LIS3MDLTR references LSM6DSV16X in compass_tile for coordinated calibration (optional Phase 2.5)
- VEML6030 calls `co5300_set_brightness()` from the display driver (Phase 0) — ensure that API exists before Phase 2

---

## REMEMBER

- Reference v7.2 §GPIO ASSIGNMENT (GPIO8 INT1), §I2C BUS ASSIGNMENT (bus 1)
- LSM6DSV16X and LIS3MDLTR are ST devices (datasheets on ST site)
- VEML6030 is Vishay (datasheet on Vishay site)
- Profiling is real: boot times, per-read costs, calibration overhead
- Auto-brightness lux curve is important — test manually with different light levels
- Test harnesses are standalone and reproducible
- Live in the .md. All work is visible

Go.
