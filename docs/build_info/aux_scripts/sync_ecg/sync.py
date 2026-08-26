#!/usr/bin/env python3
"""
sync.py — PC-as-witness time-alignment for Srceko ECG + Kompic Wearable.

Runs the NTP-lite handshake defined in
`docs/build_info/Stage_15_ECG_USB_Sync_SPEC_REPLY.md` against both devices
in parallel, computes each device's offset relative to the PC's monotonic
clock, and pushes SET_OFFSET (and Kompic-only SET_STEP) commands.

Usage:
    python3 sync.py                        # auto-detect, run once
    python3 sync.py --dry                  # discover only, no state change
    python3 sync.py --ecg /dev/ttyACM0 --kompic /dev/ttyACM1
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.stderr.write("pyserial not installed. `pip install pyserial`\n")
    sys.exit(1)


# ------------------------------------------------------------------- config
ESPRESSIF_VID   = 0x303A
PING_COUNT      = 8
PING_INTERVAL_S = 0.05
READ_TIMEOUT_S  = 0.5
KOMPIC_STEP_US  = 5000
PROTOCOL_TOKENS = (
    "SYNC_READY", "PONG", "SYNC_OK", "STEP_OK",
    "SYNC_BUSY", "STEP_ERR", "ERR",
)


# ------------------------------------------------------------------- types
@dataclass
class PingRound:
    i: int
    t_send_pc_ns: int
    t_recv_dev_us: int
    t_recv_pc_ns: int

    @property
    def rtt_ns(self) -> int:
        return self.t_recv_pc_ns - self.t_send_pc_ns


@dataclass
class SyncResult:
    port: str
    role: str                        # "ecg" or "kompic"
    fw_version: str = ""
    rtc_iso: str    = ""
    offset_us: int  = 0
    jitter_us: int  = 0
    min_rtt_us: int = 0
    set_offset_reply: str = ""
    set_step_reply: str   = ""
    error: Optional[str]  = None
    rounds: list[PingRound] = field(default_factory=list)


# ------------------------------------------------------------------- io
def send_line(port: serial.Serial, line: str) -> int:
    data = (line + "\n").encode("ascii")
    port.write(data)
    port.flush()
    return time.monotonic_ns()


def read_line(port: serial.Serial, timeout_s: float = READ_TIMEOUT_S) -> tuple[str, int]:
    """Read one \\n-terminated protocol line. Diagnostic noise is skipped."""
    deadline = time.monotonic() + timeout_s
    port.timeout = max(0.01, timeout_s)
    while time.monotonic() < deadline:
        raw = port.readline()
        t_ns = time.monotonic_ns()
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        if not line:
            continue
        if any(line.startswith(tok) for tok in PROTOCOL_TOKENS):
            return line, t_ns
        sys.stderr.write(f"    [noise on {port.port}] {line!r}\n")
    return "", time.monotonic_ns()


# ------------------------------------------------------------------- protocol
def sync_worker(
    result: SyncResult,
    ready_event: threading.Event,
    ready_dict: dict[str, str],
    lock: threading.Lock,
    ref_iso_holder: dict[str, str],
) -> None:
    """Run the full handshake on one device.

    Phase A: SYNC_START, capture SYNC_READY, publish rtc_iso to ready_dict,
             set ready_event, then WAIT on ref_iso_holder["value"] being set
             (main thread fills this after both workers have published).
    Phase B: PING burst.
    Phase C: SET_OFFSET (+ SET_STEP if Kompic) + SYNC_END.
    """
    port: Optional[serial.Serial] = None
    try:
        port = serial.Serial(result.port, baudrate=115200, timeout=READ_TIMEOUT_S)
        time.sleep(0.05)
        port.reset_input_buffer()
        port.reset_output_buffer()

        # ---- Phase A: SYNC_START → SYNC_READY ----
        send_line(port, "SYNC_START")
        line, _ = read_line(port, timeout_s=1.0)
        if not line.startswith("SYNC_READY"):
            result.error = f"no SYNC_READY, got: {line!r}"
            return
        parts = line.split(maxsplit=2)
        if len(parts) >= 2: result.fw_version = parts[1]
        if len(parts) >= 3: result.rtc_iso    = parts[2]

        with lock:
            ready_dict[result.role] = result.rtc_iso
        ready_event.set()

        # Wait for the main thread to publish ref_iso.
        for _ in range(200):   # ~2 s cap
            with lock:
                if "value" in ref_iso_holder: break
            time.sleep(0.01)
        with lock:
            ref_iso = ref_iso_holder.get("value", "-")

        # ---- Phase B: PING burst ----
        for i in range(1, PING_COUNT + 1):
            t_send = send_line(port, f"PING {i}")
            line, t_recv = read_line(port)
            if not line.startswith("PONG"):
                result.error = f"no PONG for i={i}, got: {line!r}"
                return
            _, seq_s, t_dev_s = line.split(maxsplit=2)
            if int(seq_s) != i:
                result.error = f"seq mismatch: expected {i}, got {seq_s}"
                return
            result.rounds.append(PingRound(
                i=i,
                t_send_pc_ns=t_send,
                t_recv_dev_us=int(t_dev_s.strip()),
                t_recv_pc_ns=t_recv,
            ))
            time.sleep(PING_INTERVAL_S)

        # NTP-lite: min-RTT round wins.
        # per-round offset: pc_us_at_dev_recv - dev_us_at_dev_recv
        # where pc_us_at_dev_recv ≈ (t_send_pc + t_recv_pc)/2
        pairs = []
        for r in result.rounds:
            mid_pc_ns = (r.t_send_pc_ns + r.t_recv_pc_ns) // 2
            off_ns    = mid_pc_ns - r.t_recv_dev_us * 1000
            pairs.append((r.rtt_ns, off_ns))
        pairs.sort()
        min_rtt_ns, best_off_ns = pairs[0]
        result.offset_us  = best_off_ns // 1000
        result.min_rtt_us = min_rtt_ns // 1000
        offsets_sorted = sorted(off for _, off in pairs)
        p10 = offsets_sorted[len(offsets_sorted) // 10]
        p90 = offsets_sorted[-1 - (len(offsets_sorted) // 10)]
        result.jitter_us = (p90 - p10) // 1000

        # ---- Phase C: SET_OFFSET (+ SET_STEP if Kompic) + SYNC_END ----
        send_line(port, f"SET_OFFSET {result.offset_us} {ref_iso}")
        line, _ = read_line(port, timeout_s=1.0)
        if not line.startswith("SYNC_OK"):
            result.error = f"no SYNC_OK, got: {line!r}"
            return
        result.set_offset_reply = line

        if result.role == "kompic":
            send_line(port, f"SET_STEP {KOMPIC_STEP_US}")
            line, _ = read_line(port, timeout_s=1.0)
            result.set_step_reply = line
            if not (line.startswith("STEP_OK") or line.startswith("STEP_ERR")):
                result.error = f"no STEP_* reply, got: {line!r}"
                return

        send_line(port, "SYNC_END")
        # no reply expected

    except serial.SerialException as e:
        result.error = f"serial: {e}"
    except Exception as e:
        result.error = f"unexpected: {e}"
    finally:
        if port is not None:
            port.close()


# ------------------------------------------------------------------- discovery
def discover(role_hint: str, cli_override: Optional[str]) -> Optional[str]:
    if cli_override:
        return cli_override
    prefix = "srceko" if role_hint == "ecg" else "iv7"
    for p in serial.tools.list_ports.comports():
        if p.vid != ESPRESSIF_VID:
            continue
        try:
            with serial.Serial(p.device, 115200, timeout=0.3) as s:
                time.sleep(0.05)
                s.reset_input_buffer()
                s.write(b"SYNC_START\n")
                s.flush()
                deadline = time.monotonic() + 0.5
                while time.monotonic() < deadline:
                    line = s.readline().decode("ascii", errors="replace").strip()
                    if line.startswith("SYNC_READY") and prefix in line:
                        s.write(b"SYNC_END\n")
                        return p.device
        except serial.SerialException:
            continue
    return None


# ------------------------------------------------------------------- driver
def run(args: argparse.Namespace) -> int:
    print("Discovering devices...")
    ecg    = discover("ecg",    args.ecg)
    kompic = discover("kompic", args.kompic)
    print(f"  ECG:    {ecg or 'not found'}")
    print(f"  Kompic: {kompic or 'not found'}")
    if args.dry:
        return 0
    if not (ecg or kompic):
        print("Nothing to sync.", file=sys.stderr)
        return 1

    results: dict[str, SyncResult] = {}
    if ecg:    results["ecg"]    = SyncResult(port=ecg,    role="ecg")
    if kompic: results["kompic"] = SyncResult(port=kompic, role="kompic")

    ready_event    = threading.Event()
    ready_dict: dict[str, str] = {}
    ref_iso_holder: dict[str, str] = {}
    lock = threading.Lock()

    threads = [
        threading.Thread(target=sync_worker,
                         args=(r, ready_event, ready_dict, lock, ref_iso_holder),
                         name=f"sync-{r.role}")
        for r in results.values()
    ]
    for t in threads: t.start()

    # Wait until at least one worker has published its rtc_iso, then decide.
    # Give both up to 2s to finish Phase A.
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        with lock:
            n = len(ready_dict)
        if n >= len(results):
            break
        time.sleep(0.02)

    with lock:
        # Prefer ECG's RTC as the reference; fall back to Kompic's; else "-".
        ref_iso = ready_dict.get("ecg") or ready_dict.get("kompic") or "-"
        ref_iso_holder["value"] = ref_iso
    print(f"ref_iso = {ref_iso}")

    for t in threads: t.join(timeout=5.0)

    for role, r in results.items():
        if r.error:
            print(f"[{role}] ERROR: {r.error}", file=sys.stderr)
        else:
            print(f"[{role}] {r.port}  fw={r.fw_version}  "
                  f"offset={r.offset_us:+d} µs  "
                  f"jitter≈{r.jitter_us} µs  min_rtt={r.min_rtt_us} µs")
            print(f"    SET_OFFSET reply: {r.set_offset_reply}")
            if r.set_step_reply:
                print(f"    SET_STEP   reply: {r.set_step_reply}")

    # Sidecar JSON — pandas alignment code can read this if CSV headers get
    # corrupted or if you want the raw ping rounds.
    out = Path("sync_report.json")
    with out.open("w") as f:
        json.dump({
            "ref_iso": ref_iso,
            "ping_count": PING_COUNT,
            "kompic_step_us": KOMPIC_STEP_US,
            "devices": {role: {
                **{k: v for k, v in asdict(r).items() if k != "rounds"},
                "rounds": [asdict(pr) for pr in r.rounds],
            } for role, r in results.items()},
        }, f, indent=2)
    print(f"Sidecar report → {out.resolve()}")

    return 0 if all(r.error is None for r in results.values()) else 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ecg",    help="serial port for ECG (auto-detected if omitted)")
    ap.add_argument("--kompic", help="serial port for Kompic (auto-detected if omitted)")
    ap.add_argument("--dry",    action="store_true",
                    help="Discover devices and exit without touching state")
    return run(ap.parse_args())


if __name__ == "__main__":
    sys.exit(main())
