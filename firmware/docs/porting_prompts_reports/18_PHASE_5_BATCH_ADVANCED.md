# PHASE 5 BATCH — STORAGE + VOICE + ECG (ADVANCED, v2-LEANING)
**Assigned:** 10 June 2026
**Firmware version:** iv7.1.f0.0
**Master instructions:** docs/10_KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md (required reading)

---

## MODULES TO PORT (in order)

### Module 1: SD Card (SDMMC)

**Hardware spec from v7.2:**
- Bus: SDMMC (GPIO38 CLK, GPIO39 CMD, GPIO40 DAT0)
- Protocol: SD 1-bit mode (no 4-bit)
- Mount point: `/sd` (FatFS)

**Code audit:**
New module, no old code.

**Implementation scope:**
- SDMMC host driver: configure for 1-bit mode (GPIO40 only, no DAT1–DAT3)
- FatFS mount: on-demand (mount only when logging needed, unmount to save power)
- File operations: open/write/close, simple rotation (new file per session)
- Logging format: CSV or binary (minimal framing, timestamps from RTC)
- Profiling: mount time, write latency, current draw

**Test harness:**
- Can we initialize SDMMC?
- Can we mount SD card?
- Can we create/write/close a file?
- Can we read it back?
- Measure: mount time, write speed, unmount time

**Deliverables:**
- `/docs/porting/SDCard_2026-06-10_iv7.1.f0.0.md`
- `/components/sdcard/sdcard.{c,h}` (SDMMC host, FatFS wrapper, file ops)
- `/components/sdcard/CMakeLists.txt`
- `/test/test_sdcard.c` (standalone, SDMMC init + mount + file write/read)

---

### Module 2: PDM Microphone (I2S)

**Hardware spec from v7.2:**
- Bus: I2S0 PDM (GPIO47 CLK, GPIO48 DAT)
- Sample rate: typically 16 kHz (confirm datasheet)
- Resolution: 16-bit
- No preprocessing in firmware v1 (capture only; VAD/Speex Phase 2+)

**Code audit:**
New module, no old code.

**Implementation scope:**
- I2S PDM mode: clock GPIO47, data GPIO48
- Ring buffer: circular DMA buffer in PSRAM, double-buffered
- 20 ms frame rate (typical for speech; 320 samples @ 16 kHz)
- On-demand capture: trigger record, fill buffer, trigger stop
- Optional: save to SD card (depends on SDCard from Phase 5.1)
- Profiling: DMA latency, buffer management overhead, current draw

**Test harness:**
- Can we initialize I2S PDM on GPIO47/48?
- Can we capture audio samples (16-bit)?
- Can we save to SD (if SDCard available)?
- Measure: DMA latency, sample rate accuracy

**Deliverables:**
- `/docs/porting/PDM_Mic_2026-06-10_iv7.1.f0.0.md`
- `/components/mic_pdm/mic_pdm.{c,h}` (I2S PDM driver, ring buffer, capture API)
- `/components/mic_pdm/CMakeLists.txt`
- `/test/test_mic_pdm.c` (standalone, I2S init + capture + optional SD save)

---

### Module 3: Qvar ECG (LSM6DSV16X Qvar Block)

**Hardware spec from v7.2:**
- Bus: I2C (part of LSM6DSV16X address 0x6B)
- Electrodes: Qvar1 (skin-facing pogo pin), Qvar2 (crown groove)
- Protocol: special Qvar read register (datasheet TBD)
- No hardware interrupt wired

**Code audit:**
New module, no old code. Depends on LSM6DSV16X (Phase 2).

**Implementation scope:**
- Qvar is a capacitive sensing block inside LSM6DSV16X
- Register reads: raw impedance + filtered data
- On-demand capture: trigger 1 s ECG record, save raw to ring buffer
- Optional: display ECG waveform strip (UI tile, Phase 2+)
- Profiling: I2C overhead, impedance stability

**Test harness:**
- Can we read Qvar registers (via LSM6DSV16X I2C)?
- Can we detect electrode contact (high impedance = no contact)?
- Can we capture a 1 s ECG trace?
- Measure: register read latency

**Deliverables:**
- `/docs/porting/QvarECG_2026-06-10_iv7.1.f0.0.md`
- `/components/qvar_ecg/qvar_ecg.{c,h}` (Qvar read, impedance check, ECG capture)
- `/components/qvar_ecg/ecg_tile.{c,h}` (optional UI, waveform display stub)
- `/components/qvar_ecg/CMakeLists.txt`
- `/test/test_qvar_ecg.c` (standalone, LSM6DSV16X init + Qvar read)

---

## OUTPUT

You will produce **3 dated .md files** in `/docs/porting/`:
- SDCard_2026-06-10_iv7.1.f0.0.md
- PDM_Mic_2026-06-10_iv7.1.f0.0.md
- QvarECG_2026-06-10_iv7.1.f0.0.md

Plus driver skeletons in `/components/` and test harnesses in `/test/` for each.

**Profiling template must be filled:**
- Boot-time cost
- Per-operation cost (mount, write, capture)
- Memory (ring buffer size, driver state)
- Current draw (especially during active recording)

**Defects logged** as `[DEFECT-NN]` in each .md.

**Commit strategy:** one commit per module (3 total), or one commit at the end. Your choice.

---

## DEPENDENCY NOTES

- SDCard is standalone (no external dependency)
- PDM Mic can optionally write to SDCard (requires SDCard from Phase 5.1)
- Qvar ECG requires LSM6DSV16X from Phase 2 (I2C bus 1)
- All three modules are optional for v1 (Phase 5 is v2-leaning)

---

## REMEMBER

- Reference v7.2 §GPIO ASSIGNMENT (GPIO47/48 PDM, GPIO38/39/40 SD), v7.2 mentions Qvar in LSM6DSV16X context
- SD card: 1-bit mode only (GPIO40 DAT0, no 4-bit)
- PDM Mic: 16 kHz typical, 16-bit samples (confirm datasheet)
- Qvar ECG: needs LSM6DSV16X datasheet for register definitions
- Profiling: write speed is critical (don't block UI), capture latency matters for real-time feedback
- Test harnesses are standalone and reproducible
- Live in the .md. All work is visible

Go.
