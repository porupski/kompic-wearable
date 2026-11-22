---
Date opened: 2026-09-27
Date closed: 2026-09-29
Status: CLOSED (Mk1b antenna is a mounting-topology bottleneck, not a chip
issue; software levers exhausted; further gain requires Mk2 antenna redesign)
---

# Stage 36 -- Mk1b GPS signal / GPS_PING debug

Board: Mk1b iv8.0. Firmware baseline: 0.4.91 (Stage 35 close).

Full context prompt is captured in `Stage_36_Mk1b_GPS_Signal_Debug_PROMPT.md`.
This doc is the working log; the PROMPT sibling stays as the fresh-session
kick-off.

Two problems to disentangle (in order):
1. `GPS_PING` (UBX-MON-VER poll) never gets a response, even though every
   other UBX path works (`ack_ack=1`, `mon_rf=611+`, `nav_timeutc=0`,
   `mon_ver=0`).
2. SNR peaks 17-21 dBHz in 2 s bursts; AGC ~43 %, noise/ms ~65, ant OK.
   Below the ~25 dBHz floor needed for a fix.

## §1 GPS_PING -- root cause pinpointed before coding

Suspect A of the prompt lists three hypotheses inherited from the
2026-09-23 Gemini session (Stage 31 §11 appendum):

1. Poll frame checksum wrong.
2. Dispatcher not routing MON-VER back to our handler.
3. Response length guard in `handle_ubx_mon_ver()` too strict.

Cross-referencing the two logs already on disk narrows the answer to a
fourth thing that all three hypotheses are downstream of:

- `docs/build_info/Mk1b_build_reports/GPS_M10S_Fixing.md:80-82` captures
  the ESP32-C3 bench rig sending the same 8-byte MON-VER poll to the
  same chip and receiving `UBX cls=0x0A id=0x04 len=190 OK` with
  `SW='ROM SPG 5.10 (7b202e)' HW='000A0000'`. So the chip's MON-VER
  response for this particular MAX-M10S firmware is **190 bytes**
  (30 SW + 10 HW + 5 * 30 extensions).
- `firmware/esp-idf/components/max_m10s/max_m10s.c:65` defines
  `UBX_MAX_PAYLOAD = 128` and line 702 rejects any frame whose
  advertised length exceeds it, at the `UBX_LEN_HI` state:

  ```c
  case UBX_LEN_HI:
      s_ubx.len |= ((uint16_t)b << 8); ubx_ck_add(b);
      if (s_ubx.len > UBX_MAX_PAYLOAD) { ubx_reset(); }
  ```

  The reset happens before the CK bytes arrive, before the handler
  runs, and before `s_ubx_ctr.total` is incremented. From bench
  telemetry this looks identical to "chip never answered".

That's why MON-RF (28 B), ACK-ACK (10 B), and NAV-TIMEUTC (28 B) all
work, but MON-VER (190 B) uniquely never counts. Hypothesis 3 was in
the right neighborhood; the actual guard sits in the *parser*, not in
`handle_ubx_mon_ver`.

Fix: raise `UBX_MAX_PAYLOAD` to 256 (next reasonable power of two,
covers 190 B response plus headroom for any firmware that ships more
extensions). Also add a WARN log on the oversize-reset branch so a
future regression of the same kind is visible on the bench instead
of silent.

Deliberately leaving `handle_ubx_mon_ver`'s `if (len < 40) return;`
alone. 40 B is the u-blox minimum (30 SW + 10 HW) -- anything shorter
is malformed. Hypothesis 3 assumed we might see a shorter-than-40
response; the C3 log says no.

Hypotheses 1 and 2 are refuted by the same evidence:
- Checksum `0E 34` on the wire is confirmed correct by the C3 bench
  (which uses the byte-identical 8-byte poll and gets a response).
- Dispatcher branch at `max_m10s.c:725-728` correctly routes
  `cls == 0x0A && id == 0x04` to `handle_ubx_mon_ver`.

## §2 SNR / RF path -- deferred

Suspect B of the prompt is genuine but blocked on bench moves that
only Ivan can do (outdoor test, active-antenna swap). Codified in
"What to try, in order" step 2-4: don't chase self-noise until the
outdoor test and antenna-swap check are done. Noise/ms 64-65 is
already clean -- shielding is the wrong lever.

No code change in this stage for Suspect B. Wait for outdoor bench
result.

## §3 Plan for this stage

**Batch 1 (this flash)** -- single-file change to `max_m10s.c`:
1. Bump `UBX_MAX_PAYLOAD` from 128 to 256.
2. Add a `ESP_LOGW` at the oversize-reset branch so any future frame
   that overflows is visible.

Ivan flashes + runs `GPS_PING`. Expected result: within ~10 ms
`mon_ver` counter increments, CLI prints the `HW Version` +
`SW Version` + extension lines.

If GPS_PING succeeds, the "Suspect A" story is closed; the loop moves
to Suspect B (outdoor bench) on Ivan's schedule.

If GPS_PING still fails after this change, the next lead is a
temporary hex-dump of every UBX byte received in a 1 s window after
the poll -- not touching until the payload-buffer fix is proven.

## §4 Bench log

**2026-09-29 close bench, fw 0.4.95** -- epoxy removed, retested,
verdict landed.

Numbers (mean nonzero CN0 per file, over the same 8×8 mm chip antenna):
- pre-epoxy indoor 13 min: 16.4 dBHz  peak 30
- pre-epoxy outdoor 5 min:  15.9 dBHz  peak 29
- post-epoxy indoor 3.4 min: 19.4 dBHz  peak 27 (+3 dB mean)
- post-epoxy outdoor 11 min: 16.1 dBHz  peak 30 (~unchanged)

Epoxy removal helped ~3 dB indoors, ~none outdoors. Peak CN0 still
capped at 30 dBHz, sustained top-4 ≥22 dBHz still only 6 s max.
Never a fix in ~30 minutes total outdoor time. That is the antenna
ceiling for this install, not the epoxy.

**Root cause is the antenna install topology, not the chip:**
1. **BAT Wireless BWGNSCNX8-8W2 spec: 1 dBi gain typ, RHCP, 50 Ω,
   VSWR<3** -- but that's on the vendor's ideal ~70×70 mm ground plane
   test fixture. Real installs on small PCBs lose 3-6 dB out of the
   box.
2. **Mounted OFF the PCB** via ~15 mm of 50 Ω coax (sig to center pin,
   gnd to the ceramic backplate). The PCB ground plane isn't acting
   as counterpoise -- the antenna is trying to radiate against its
   own 8×8 mm backplate + the coax shield. Chip patch antennas need
   ≥30 mm × 30 mm of continuous ground copper directly under them.
   Estimated loss: another 3-6 dB.
3. **Antenna oriented perpendicular to PCB** (ceramic surface
   vertical). RHCP boresight is normal to the ceramic face -- so the
   main beam is pointing horizontal (at walls / ground), not at the
   sky where the sats are. That's a further 3-10 dB depending on how
   far off-axis sats fall.
4. **1.5 cm coax stub with no matching network at the feed** -- 50 Ω
   assumption is off, mismatch loss unquantifiable without a VNA.

Sum-of-losses puts real-world CN0 6-15 dB below the datasheet number.
That matches the observed data: we're 8-15 dB below the ~35 dBHz
needed for reliable fixes.

Answer to "how does a shitty china watch get a fix?": those don't
use chip patch antennas. They use PIFA / F-antennas / chassis
antennas -- a metal strip driven from a feed point on the PCB with
the case metal + PCB ground copper serving as the radiator. That
approach:
- doesn't need a ground-plane keep-out (the ground IS the radiator)
- tolerates small volumes and irregular case shapes
- typical gain 0-1 dBi, but no boresight orientation problem
- easily proven with a stamped metal contact + spring-finger from
  bezel to a PCB pad

Standard in smartphones and smartwatches -- Apple Watch uses the
crown/bezel as part of the GPS antenna. This is what Mk2 should
target instead of a chip ceramic.

**Batch 4 polish (fw 0.4.95):** the false-sync UBX WARN was firing
on boot when NMEA byte patterns happened to spell `B5 62` followed
by garbage. Gated it: only WARN when `cls ≤ 0x28` and `len ≤ 4096`
(real oversize territory). Genuine oversize frames still log.

---

**2026-09-27 outdoor bench, fw 0.4.93** -- all-GNSS enable landed and
paid off. Peak `sats_with_snr = 11`, brief `max CN0 = 30 dBHz`,
`top4_avg = 25 dBHz`. That's brushing the decode floor -- not a fix
yet, but the odds went from "impossible" to "barely possible on a good
minute." Still bursty because weak sats drift in and out of tracking.

Two follow-up problems from Ivan's outdoor run:
- **SNR tile goes dark ~2 s per cycle** -- weak sats drop for one GSV
  cycle and the 2500 ms aging window drops them from the summary.
- **He needs GPS data logged to SD** so he can walk outdoors without
  the laptop and post-hoc-analyse. `GPS_SNR` on the CLI only shows a
  live snapshot; typing it burns bench time.

**Batch 3 (fw 0.4.94)** -- data-collection + smoothing pass:
1. `max_m10s.c/h`: SNR window becomes runtime-tunable
   (`max_m10s_get/set_snr_window_ms`), default bumped from 2500 -> 6000 ms.
   Range 500..30000. Hot -- no reboot.
2. `nvs_cfg.c/h`: new `gps_log` u8 flag (default 0).
3. `firmware/esp-idf/components/field_capture/fc_gps_log.c` (new):
   `task_gps_log_fn`, 1 Hz cadence, writes
   `/sd/data/gps/gps_<datetime>.csv`. Columns capture the full triage
   picture: SNR summary + MON-RF (noise/AGC/ant) + fix state + UBX
   per-class counters + 1PPS count. Open-append-close per row
   (per [[feedback_sd_write_durable]]).
4. `boot_tasks.c`: `task_gpslog` entry (Core 0, pri 1, 4KB) mirrors
   `task_bbox`. Self-exits if flag=0 -- no cost when disabled.
5. `fc_cli.c`: two new verbs.
   - `GPS_LOG [ON|OFF]` -- NVS-persist + reboot-to-apply (same
     ergonomics as BLACKBOX).
   - `GPS_SNR_WIN [<ms>]` -- hot; no reboot. Debug knob.
6. `fc_common.c`: `try_mkdir("/sd/data/gps")` in `ensure_sd`.

Deliberately did NOT touch the debug NMEA tile that shows
"C.,,,V..........N..V*37" every second. That's the RMC sentence with
`V`=void (no fix) and `N`=no data -- when the chip has no fix the
sentence content is legitimately identical every second, so the tile
looks frozen but is actually being written correctly. Would need a
"live" indicator (age counter, rx-since-boot count) to prove it. Not
worth spend right now.

Also deliberately no code answer for the on-tile bar burst: the 6 s
window makes it look less awful without lying too hard. If Ivan wants
smoother, he can push it to 10-15 s via `GPS_SNR_WIN 15000` and see
if the tradeoff feels right.

---

**2026-09-27 outdoor bench, fw 0.4.92** -- outdoor SNR is *worse* than
indoor-at-window. Numbers: outside `max CN0 = 16-21 dBHz` on 0-2 sats,
inside at window `max CN0 = 25-28 dBHz` on 3-7 sats. `noise/ms = 67`
and `AGC = 3534 (~43%)` unchanged between locations. That rules out
"marginal reception outdoors" -- the frontend is receiving essentially
the same energy everywhere, so the ~1 mm epoxy cover on the chip
antenna is almost certainly the dominant loss. Suspect B story is:
antenna is severely detuned, not just marginal. Ivan doesn't want to
remove the epoxy at the moment (no tools handy + water-ingress risk).

**Batch 2 (fw 0.4.93)** -- software-only lever before physical work:
enable every GNSS the M10 supports. MON-VER extensions confirm the
chip has `GPS;GLO;GAL;BDS` + `SBAS;QZSS`; if defaults are the usual
GPS+GAL+BDS, turning on GLO+QZSS+SBAS gives us 1.5-2x more sats
above the ~25 dBHz decode floor. Zero antenna cost.

Changes:
1. `max_m10s.c`: 14x `CFG-SIGNAL-*_ENA` VALSETs (system + L1 signal
   for each of GPS/GLO/GAL/BDS/QZSS/SBAS). Layer=RAM; re-applied
   every boot from `max_m10s_init`.
2. `max_m10s.h` / `max_m10s.c`: new `max_m10s_enable_all_gnss()`.
3. `fc_cli.c`: new `GPS_GNSS_ALL` verb -- resends the 14 frames
   live, reports `ACK+N NAK+N` in a 500 ms window.

Expected outcome: at boot log shows `all GNSS on`; MON-RF/SNR run
should surface more `sats_with_snr`. Cold-start ephemeris still
requires 25+ dBHz sustained on 4 sats, so a first-time fix isn't
guaranteed -- but the odds improve.

If SNR still doesn't move outdoors, the story really is antenna
attenuation and the only remaining levers are physical (peel epoxy,
active antenna, or Mk2 patch respin).

**No manual gain / AGC boost is possible** -- AGC is a closed-loop
analog frontend behavior; forcing higher gain amplifies noise 1:1
with signal, SNR stays flat. That's why the M10 doesn't expose a
"gain override" key.

---

**2026-09-27, fw 0.4.92** -- GPS_PING fixed. First flash after the
`UBX_MAX_PAYLOAD` bump returned:

```
[GPS] PING -> UBX-MON-VER Response Received!
      HW Version : 000A0000
      SW Version : ROM SPG 5.10 (7b202e)
      Ext [0]    : FWVER=SPG 5.10
      Ext [1]    : PROTVER=34.10
      Ext [2]    : MOD=MAX-M10S
      Ext [3]    : GPS;GLO;GAL;BDS
      Ext [4]    : SBAS;QZSS
      Status     : Communication & Parsing Verified OK
```

MON-RF still healthy (`ant=OK, pwr=ON, noise=68, AGC ~43%`). SNR
still 0 indoors -- expected until Suspect B is worked. Suspect A
closed.

## §5 Follow-ups / next steps

- SNR / antenna workstream (Suspect B). Sequence per the PROMPT
  "What to try, in order" §2-3.
- If MON-VER now works, consider polling it at boot (once) so
  `max_m10s_get_version_info()` returns real data on first CLI
  request instead of empty until PING.
- Note for Mk2: bake a proper GPS antenna footprint into the
  PCB (larger ceramic patch + ground plane sized to datasheet).
  Filed under `docs/build_info/Mk2_intel/` when Mk2 fab wait
  starts (per `[[reference_mk2_intel_folder]]`).
