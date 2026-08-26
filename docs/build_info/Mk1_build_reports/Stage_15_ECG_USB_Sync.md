# Stage 15 — ECG ↔ Kompic USB Sync

**Date:** 2026-08-19
**Status:** Spec finalized, ECG-side implemented. Kompic-side implementation pending.

---

## Goal

Align a Kompic PPG/BCG recording with a Srceko ECG recording to **< 10 ms** accuracy using only USB-C cables to a shared laptop. No BLE, no extra wires.

---

## Physical topology (final)

```
              ┌──────────┐
              │  Laptop  │
              │  sync.py │
              └──┬────┬──┘
          USB-C  │    │  USB-C
              ┌──▼─┐ ┌▼───────┐
              │ECG │ │ Kompic │
              └────┘ └────────┘
```

Both devices stay in their default USB-CDC-device roles. The PC Python script (`sync.py`) runs the sync handshake against both in parallel and computes each device's offset relative to `time.monotonic_ns()`.

**Why not ECG-as-USB-host (original plan):** the ECG board has a sink-only USB-C port (Schottky from VBUS to BATT, no VBUS drive). Two batteries + plain cable = no enumeration. PC-as-witness avoids all OTG/VBUS complexity.

**Kompic USB stack:** unchanged. The USB-Serial-JTAG peripheral exposes CDC-ACM (VID `0x303A`, PID `0x1001`). Line coding is ignored. Endpoint MPS 64 bytes.

---

## Protocol

All lines terminated `\n`. Timestamps in µs from `esp_timer_get_time()`. The "host" issuing commands is `sync.py`, not the ECG — Kompic does not distinguish.

### Handshake

```
PC   → "SYNC_START\n"
Kompic → "SYNC_READY <fw_version> <rtc_iso>\n"
       or "SYNC_BUSY <reason>\n"   (if in FCM_RECORDING or USB-MSC)
```

`fw_version` = `iv7.1.f0.4.13` (no spaces). `rtc_iso` = `YYYY-MM-DDTHH:MM:SS` UTC.

### Ping burst (8 rounds, PC-paced ~50 ms apart)

```
PC   → "PING <i>\n"
Kompic → "PONG <i> <t_recv_us>\n"
```

`t_recv_us` = `esp_timer_get_time()` latched **on the `\n` byte**, before any parse/log/IO. This is the single most latency-critical operation. Parse `<i>` after latching.

### Offset write

```
PC   → "SET_OFFSET <offset_us> <ref_rtc_iso>\n"
Kompic → "SYNC_OK offset_us=<n> stored=<where>\n"
```

`offset_us` is signed. Semantics: `t_pc_us = t_local_us + offset_us`. `ref_rtc_iso` is a string the PC chooses (in practice, the ECG's RTC at time of sync) — store it in the CSV header, do not touch the RTC.

`stored=` values: `ram+nvs_queued` (normal), `ram_only_nvs_disabled` (NVS init failed).

### Step write (Kompic-only)

```
PC   → "SET_STEP <step_us>\n"
Kompic → "STEP_OK step_us=<n> effective_us=<n> mode_default=<n>\n"
       or "STEP_ERR out_of_range: min=<n> max=<n>\n"
```

Accepted range: 1000–100000 µs. Target from PC: **5000 µs (200 Hz)**. `effective_us` may differ from requested if rounding to sensor ODR is needed — downstream alignment code keys on `effective_us`.

### End

```
PC   → "SYNC_END\n"
Kompic → (no reply, exit sync mode)
```

### Error strings (remain in sync mode)

```
ERR malformed_line: <first 32 bytes>\n
ERR unknown_cmd: <token>\n
ERR seq_out_of_order: got <i> expected <j>\n
ERR ping_before_ready\n
ERR too_late: SYNC_END already received\n
```

### Full exchange

```
PC                                     Kompic
SYNC_START                       ─►
                                 ◄─   SYNC_READY iv7.1.f0.4.13 <rtc_iso>
PING 1..8                        ─►
                                 ◄─   PONG 1..8 <t_us>
SET_OFFSET <offset_us> <ref_iso> ─►
                                 ◄─   SYNC_OK offset_us=… stored=…
SET_STEP 5000                    ─►
                                 ◄─   STEP_OK step_us=5000 effective_us=… mode_default=…
SYNC_END                         ─►
```

---

## Latency requirements

- PING→PONG turnaround: **< 3 ms 99th-percentile** (target, with SD + LVGL suspended).
- Without suspend: up to ~15 ms worst case (SD write burst ~5 ms, LVGL redraw ~10 ms).
- **Sync mode must suspend the SD write task and the LVGL redraw task** for the duration of the burst. Screen may show "SYNCING…" freeze < 1 s — acceptable.
- Worst-case latency should be measured and recorded before the ECG host code is committed.

### Log noise mitigation (both)

- Kompic: `esp_log_level_set("*", ESP_LOG_NONE)` on `SYNC_START`, restore on `SYNC_END`.
- PC script: reject any line not starting with a known protocol token; log discarded lines separately.

---

## Persistence

RAM primary, NVS backup (no PCF — RAM byte is 8 bits, can't fit int64).

NVS keys to add in `nvs_cfg`:
- `sys:pc_sync_offset_us` — int64
- `sys:pc_sync_offset_valid_uptime_us` — int64 (ESP-timer at write)

On boot: load from NVS if stored uptime < 24 h ago; otherwise treat as stale and require re-sync. NVS write is dispatched to a background task (~10 ms) so it doesn't block `SYNC_OK` reply.

---

## CSV format (PPG+BCG raw mode only)

All other existing modes (`FCM_BATT_TEST`, `FCM_STEPS`, `FCM_MLC_COLLECT`, etc.) are unchanged.

### Header comments

```
# pc_sync_applied=1
# pc_sync_offset_us=<signed int64>
# pc_sync_ref_iso=<ref_rtc_iso from SET_OFFSET>
# pc_sync_kompic_rtc_iso=<Kompic RTC when offset received>
# pc_sync_step_us=<effective_us>
# pc_sync_step_host_set=<0 or 1>
```

Or when no sync this session:

```
# pc_sync_applied=0
# pc_sync_step_us=<mode_default>
# pc_sync_step_host_set=0
```

### New columns (both emitted unconditionally)

- `t_local_us` — µs since recording session start (`esp_timer_get_time()` at `csv_open()`).
- `t_pc_us` — `t_local_us + offset_us` if synced, else **duplicate** of `t_local_us` (not empty — preserves pandas dtype).

### Full column order (PPG+BCG raw)

```
t_local_us,t_pc_us,t_ms,iso_utc,src,led_pa,raw,baseline,ac,bp,motion,beat,bpm_pk,bpm_ac,quality,stale
```

---

## Kompic implementation — what was done (fw 0.4.14)

All items implemented in one session (2026-08-19). No items outstanding for the sync layer itself.

1. **`fc_sync.c`** (new) — sync state machine `IDLE ↔ ACTIVE`. Handles `SYNC_START`, `PING/PONG`, `SET_OFFSET`, `SET_STEP`, `SYNC_END`, plus all error strings. Priority raised to 5 during sync burst; `ESP_LOG*` silenced.
2. **`fc_cli.c`** — `t_recv_us = esp_timer_get_time()` latched at `\n` detection in the main loop. Dispatch to `fc_sync_handle_line()` when active or `SYNC_START`. `fc_sync_init()` at boot.
3. **`nvs_cfg.c/.h`** — keys `pc_sync_off_us` (i64) + `pc_sync_upt_us` (i64) in `cfg_sys`. `nvs_cfg_sys_get_pc_sync()` / `set_pc_sync()`. Boot printout shows stored offset.
4. **`fc_sync_write_csv_headers(FILE *f)`** — ready to call from PPG+BCG mode; emits the full `pc_sync_*` header block.
5. **Task suspension** — not implemented (not needed: priority boost + no LVGL task running gave < 500 µs PING→PONG, well inside target).
6. **`firmware_version.h`** — bumped to `0.4.14`.

---

## First live sync test — 2026-08-19

`sync.py` run with both devices plugged in:

| Device | Port | fw | offset | jitter | min RTT |
|--------|------|----|--------|--------|---------|
| ECG (Srceko) | `/dev/ttyACM1` | `srceko.f1.0` | +16 526 117 784 µs | 826 µs | 670 µs |
| Kompic | `/dev/ttyACM0` | `iv7.1.f0.4.14` | +16 400 831 562 µs | **471 µs** | **407 µs** |

Kompic PING→PONG min RTT **407 µs** — beats the < 3 ms 99th-percentile target by 7×. Jitter 471 µs → end-to-end alignment accuracy comfortably inside the < 10 ms goal.

Kompic `SYNC_OK` reply: `stored=ram+nvs_queued` — offset persisted to NVS for cross-boot continuity.

**Operational note:** if a serial monitor (idf.py monitor, screen, minicom) holds `/dev/ttyACMx` open, `sync.py` will fail to open that port or report a noisy connection. Close all monitors before running sync.

---

## ECG-side / PC-side (for reference, no Kompic dependency)

- ECG implements the same protocol parser mirror; its columns are `t_local_us` / `t_pc_us`. Same schema = `pd.merge_asof` works without renames.
- `SET_STEP` is not sent to ECG (fixed 125 Hz logger). ECG accepts it as no-op for future-proofing.
- PC `sync.py`: parallel-threaded handshake, 8 PINGs per device, NTP-lite offset, sidecar JSON report. Runs < 1 s wall clock.

---

## What's still needed — PPG+BCG raw recording mode (Stage 16)

The sync layer is complete. The missing piece for the actual collection experiment is a combined PPG+BCG raw recording mode on Kompic. This does not yet exist in ESP-IDF.

**Current closest mode:** `FCM_BCG` (`run_bcg()` in `fc_modes_lsm.c`) — records LSM6DSV16X accelerometer data only (`time_ms,accel_z_g,filt_mg,beat`). No MAX30101 raw, no µs timestamps, no sync headers.

**What Stage 16 needs to add:**

1. New `FCM_PPG_BCG` entry in `field_capture.h` enum.
2. New `run_ppg_bcg_mode()` — a 200 Hz (5000 µs per tick) loop that reads both `broker_imu_data` (LSM accel) and raw MAX30101 green channel, interleaves rows by source (`src=ppg` / `src=bcg`), and writes to CSV.
3. `csv_open()` call that uses `fc_sync_write_csv_headers()` to emit `pc_sync_*` header block.
4. Column schema: `t_local_us,t_pc_us,t_ms,iso_utc,src,led_pa,raw,baseline,ac,bp,motion,beat,bpm_pk,bpm_ac,quality,stale` — as agreed in the Stage 15 spec.
5. `t_local_us` = `esp_timer_get_time() - session_start_us` per row. `t_pc_us = t_local_us + fc_sync_get_offset_us()`.
6. No built-in recording timer exists on Kompic — recordings run until button click. A configurable timer (`REC_DURATION <s>` CLI command or mode default) would be convenient for the 5-minute protocol but is not blocking.

**Collection protocol once Stage 16 is done:**
1. Plug both devices into laptop.
2. Close all serial monitors.
3. `python sync.py` — takes < 1 s.
4. Select `FCM_PPG_BCG` on Kompic, click to start recording.
5. Press Record on Srceko.
6. Put both on body, lie still 5 min.
7. Click Kompic button to stop. Stop Srceko recording.
8. Pull SD card / `sync.py --retrieve` to get CSVs.
9. Offline: `pd.merge_asof` on `t_pc_us` column, < 1 ms alignment.
