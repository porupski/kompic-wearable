---
Date opened: 2026-10-04
Date closed:
Status: ACTIVE (v1) -- governs all new CSV writers; existing writers
                      migrate opportunistically per
                      [[feedback_recording_datetime_opportunistic]].
---

# Kompic CSV standard (v1)

The firmware emits many CSV logs (batt, blackbox, env, mot, qvar,
ppg/bcg, bcg, steps, mlc_train, flight). Historically each writer
grew ad-hoc headers and filename shapes. This doc codifies the
convention; writers migrate as they're touched.

First writer born under this standard: `fc_flight.c` (Stage 38).

---

## 1. Filename

```
<device>_YYYY-MM-DD_HH-MM-SS_<mode>.csv
```

Example: `kompic_2026-10-10_14-32-05_flight.csv`.

If RTC not yet synced at record start, use `kompic_boot<SEQ>_<mode>_
<N>.csv` and keep a sidecar `.sync` log mapping boot_seq -> wall
clock once sync lands.

Lives under `/sd/data/<mode>/`.

---

## 2. Header block

Every file opens with a `#`-prefixed metadata block, THEN the column
header row (no `#`).

```
# device_hw = iv8.0
# device_fw = 0.4.98
# device_id = <chip_mac_short>
# record_mode = flight
# record_start_utc = 2026-10-10T14:32:05Z
# record_start_tz_offset_h = +2
# rtc_valid_at_start = 1
# sample_rate_hz = 120
# notes =
# columns = ts_ms, ax_mg, ay_mg, az_mg, ...
# units   = ms, mg, mg, mg, ...
ts_ms,ax_mg,ay_mg,az_mg,...
```

Rules:
  - `#` prefix = standard CSV-with-metadata; Pandas / numpy /
    spreadsheet tools skip comment lines cleanly.
  - `device_fw` tracks `KOMPIC_FW_VERSION`; post-fact filtering of
    "all logs on fw X" is trivial.
  - `columns` and `units` metadata lines mirror the actual column
    header; redundancy by design (metadata survives if the header
    row is edited by a tool).
  - `rtc_valid_at_start` is `1` if RTC was synced before the first
    row, `0` otherwise. If `0`, `ts_ms` is "ms since boot" not "ms
    since RTC epoch".
  - `record_start_utc` and `record_start_tz_offset_h` describe the
    moment of the first row, not the file-open syscall.

---

## 3. Column conventions

  - **Timestamp**: first column is ALWAYS `ts_ms`. Monotonic within
    a file. Reset on new file (rotation does NOT continue the
    counter).
  - **Raw sensor LSBs**: integer, use `_raw` suffix if the unit
    isn't obvious.
  - **Engineering units**: 3 decimal places (m/s^2, degC, hPa). 6
    for GPS lat/lon. Scientific notation OK for gas resistance
    (`gas_ohm`).
  - **Enum / state**: lowercase short strings, `|`-delimited bitmap
    if multi-valued.
  - **Boolean**: `0` / `1`.
  - **Row delimiter**: `\n` (LF). Never CRLF.
  - **Field delimiter**: `,`. Never space-padded.
  - **Missing / stale**: either blank (empty between commas) OR
    carry last-known value plus a `_fresh` boolean column. Pick ONE
    strategy per file and stick with it in that file's §2.2 header
    `notes` field.

---

## 4. Durability

Per [[feedback_sd_write_durable]]: `fflush()` alone leaves 0-byte
files on power cut. Three allowed cadences:

  - **Per-row** `fflush` + `fsync` -- safest, slowest. Use when
    sample rate <= 10 Hz.
  - **Per-N-row** `fflush` + `fsync` every N rows (worst case loss
    = N / sample_rate_hz). Use for 10 Hz < rate <= 200 Hz. Default
    N chosen so worst-case loss <= 100 ms.
  - **PSRAM ring + periodic flush** -- Tier-4 of the port
    (`Stage_38_Mk1b_Flight_Recording_PROMPT.md` §1.4); not available
    today.

Files MUST be closed with `fclose()` on mode exit; never rely on
process exit or SD remount to flush.

---

## 5. Rotation

Start a new file when any of:
  - 1 hour of continuous recording elapsed.
  - `record_mode` changes.
  - RTC jumps (sync landed mid-record, or user changed clock).
  - Target SD free space drops below 50 MB (hard stop with warning).

Each new file re-emits the §2 header block.

---

## 6. Example header (flight mode, with GPS not-yet-fixed)

```
# device_hw = iv8.0
# device_fw = 0.4.98
# device_id = a84b2d9f
# record_mode = flight
# record_start_utc = 2026-10-10T14:32:05Z
# record_start_tz_offset_h = +2
# rtc_valid_at_start = 1
# sample_rate_hz = 120
# notes = sparse channels carry last-known value + _fresh flag
# columns = ts_ms, ax_mg, ay_mg, az_mg, gx_mdps, gy_mdps, gz_mdps, mx_mG, my_mG, mz_mG, temp_c, p_hpa, h_pct, gas_ohm, mag_fresh, env_fresh, gps_lat_deg, gps_lon_deg, gps_alt_m, gps_fix
# units   = ms, mg, mg, mg, mdps, mdps, mdps, mG, mG, mG, degC, hPa, pct, ohm, bool, bool, deg, deg, m, enum
ts_ms,ax_mg,ay_mg,az_mg,gx_mdps,gy_mdps,gz_mdps,mx_mG,my_mG,mz_mG,temp_c,p_hpa,h_pct,gas_ohm,mag_fresh,env_fresh,gps_lat_deg,gps_lon_deg,gps_alt_m,gps_fix
0,12,-9,1003,45,-12,8,187,-412,89,21.342,1013.221,43.112,24531,1,1,,,,0
8,15,-11,1001,47,-10,9,187,-412,89,21.342,1013.221,43.112,24531,0,0,,,,0
...
```

---

## 7. Migration status (writers)

Per [[feedback_recording_datetime_opportunistic]], existing writers
upgrade one at a time when their module is touched. Mark here as
each lands.

| Writer         | Datetime filename | §2 header | Durability     |
|----------------|:-----------------:|:---------:|----------------|
| `batt_`        | yes (Stage 23)    | legacy    | fclose/row     |
| `blackbox_`    | yes (Stage 23)    | legacy    | fclose/row     |
| `env_`         | boot_seq          | legacy    | fclose/session |
| `mot_`         | boot_seq          | legacy    | fclose/session |
| `qvar_`        | boot_seq          | legacy    | fclose/session |
| `ppgbcg_`      | boot_seq          | legacy    | fclose/session |
| `bcg_`         | boot_seq          | legacy    | fclose/session |
| `steps_`       | boot_seq          | legacy    | fclose/session |
| `mlc_train_`   | boot_seq          | legacy    | fclose/session |
| `flight_`      | **yes**           | **v1**    | **fsync/10 rows** |
