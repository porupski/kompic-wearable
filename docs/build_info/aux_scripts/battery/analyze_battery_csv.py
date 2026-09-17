#!/usr/bin/env python3
"""
analyze_battery_csv.py  -- Batch-analyse a folder of batt_XXXX.csv files.

Usage:
    python analyze_battery_csv.py  <folder>  [--current-ma 106] [--no-plot]
                                             [--delete-short] [--out-dir DIR]
                                             [--profile-fw FW]

Classification: by `fw=` in the CSV header comment (written by fc_battery_test).
Sessions with no fw= tag fall into an "unknown" bucket. boot_seq is NOT used
for grouping because it resets on NVS wipe -- Stage 18 firmware ended up
with lower boot numbers than Stage 11 firmware after a device reflash.

Filtering:
  Discharge phase  > 30 min  to count as a valid discharge session
  Charging  phase  > 20 min  to count as a valid charging session
  Files qualifying for neither are listed as "short" and can be deleted.

Outputs (written to --out-dir, default = script's own directory):
  battery_profile.txt  --  loaded-voltage vs SoC% table (human-readable)
  battery_profile.h    --  same, as C uint16_t array for ESP-IDF firmware
  battery_analysis.png --  1x2 Vbat overview (discharge + charge, coloured by fw)

The profile is built from --profile-fw's discharge sessions (default: latest
fw= seen). Pass e.g. `--profile-fw 0.4.24` to lock in a specific baseline.

Notes:
  batt_mv and batt_pct are BQ25619 register values -- not real measurements.
  vbat_adc_mv is from a 5k1-5k1 divider on GPIO9, ~+/-1% accuracy.
  Battery profile voltages are under ~106 mA load, not open-circuit (OCV).
"""

from __future__ import annotations

import argparse
import math
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from statistics import mean, median
from typing import Optional

# ── Session filtering constants ──────────────────────────────────────────────
DISCHARGE_MIN_MIN = 30       # minimum discharge duration (minutes) to qualify
CHARGE_MIN_MIN    = 20       # minimum charging duration (minutes) to qualify
VBAT_LO, VBAT_HI = 2800, 4500   # mV sanity clip for vbat_adc_mv

# The BQ25619 sets charging=0 as soon as it enters CV termination -- even if
# USB is still plugged in and the battery is being held near ~4.13 V. That
# "topped-off plateau" is NOT real discharge, but CHRG=1→0 alone can't tell
# the two apart (no separate VUSB-detect signal is logged). We flag the real
# discharge start as the first sustained vbat drop below PLATEAU_MV.
PLATEAU_MV        = 4060     # first sustained vbat < this = real discharge start
PLATEAU_STREAK    = 3        # need this many consecutive samples below

UNKNOWN_FW = "unknown"


# ── Tiny type helpers ─────────────────────────────────────────────────────────

def _f(x) -> Optional[float]:
    try:    return float(x)
    except: return None  # noqa: E722


def _i(x) -> Optional[int]:
    try:    return int(x)
    except: return None  # noqa: E722


def _ms2str(ms: int) -> str:
    s = ms // 1000
    h, r = divmod(s, 3600)
    m, s = divmod(r, 60)
    return f"{h}h {m:02d}m" if h else f"{m}m {s:02d}s"


def _dur_ms(rows: list[dict]) -> int:
    if len(rows) < 2:
        return 0
    t0 = _i(rows[0].get("t_ms")) or 0
    t1 = _i(rows[-1].get("t_ms")) or 0
    return max(0, t1 - t0)


def _vbat_field(header: list[str]) -> Optional[str]:
    """Which column holds the battery voltage for this file?
    Mk1b (datetime CSVs) logs MAX17048 vcell_mv; iv7.1 (batt_XXXX) used a
    GPIO ADC divider labelled vbat_adc_mv. Return the first one present.
    """
    for name in ("vcell_mv", "vbat_adc_mv"):
        if name in header:
            return name
    return None


def _vbat_samples(rows: list[dict], header: list[str]) -> list[int]:
    field = _vbat_field(header)
    if field is None:
        return []
    return [v for r in rows
            if (v := _i(r.get(field))) is not None and VBAT_LO <= v <= VBAT_HI]


def _temp_samples(rows: list[dict]) -> list[float]:
    return [t for r in rows
            if (t := _f(r.get("soc_temp_c"))) is not None and t > -100]


def _vbat_or_nan(r: dict, header: list[str]) -> float:
    field = _vbat_field(header)
    if field is None:
        return math.nan
    v = _i(r.get(field))
    return float(v) if (v is not None and VBAT_LO <= v <= VBAT_HI) else math.nan


def _fw_sort_key(fw: str) -> tuple:
    """Semver-ish sort so 0.4.13 < 0.4.24 < unknown."""
    if fw == UNKNOWN_FW:
        return (999, 999, 999)
    try:
        parts = [int(p) for p in fw.split(".")]
        while len(parts) < 3:
            parts.append(0)
        return tuple(parts[:3])
    except ValueError:
        return (999, 999, 999)


# ── CSV reader ────────────────────────────────────────────────────────────────

def _read_csv(path: Path) -> tuple[dict, list[str], list[dict]]:
    """Return (meta, header_fields, data_rows). '#' lines are comments."""
    meta: dict = {}
    header: Optional[list[str]] = None
    rows: list[dict] = []

    with path.open("r", newline="") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            if not line:
                continue
            if line.startswith("#"):
                if "fw=" in line and "boot=" in line:
                    for tok in line.lstrip("# ").split():
                        if "=" in tok:
                            k, _, v = tok.partition("=")
                            meta[k] = v
                continue
            if header is None:
                header = [c.strip() for c in line.split(",")]
                continue
            vals = [v.strip() for v in line.split(",")]
            if len(vals) == len(header):
                rows.append(dict(zip(header, vals)))

    return meta, header or [], rows


# ── Phase splitter ────────────────────────────────────────────────────────────

def _split_phases(rows: list[dict]) -> tuple[list[dict], list[dict]]:
    """Split at the first charging 1→0 transition.
    Returns (charge_rows, discharge_rows).
    If no transition, classifies by dominant flag.
    """
    for i in range(1, len(rows)):
        if (_i(rows[i-1].get("charging")) == 1 and
                _i(rows[i].get("charging")) == 0):
            return rows[:i], rows[i:]
    ones = sum(1 for r in rows if _i(r.get("charging")) == 1)
    return (rows, []) if ones > len(rows) // 2 else ([], rows)


def _find_real_disch_start(disch_rows: list[dict], header: list[str]) -> int:
    """
    Return index within disch_rows where the on-USB plateau ends and real
    battery discharge begins. Marker: first Vbat < PLATEAU_MV with
    PLATEAU_STREAK consecutive samples below. Returns 0 if no plateau
    (voltage already below threshold, or Vbat unavailable).
    """
    field = _vbat_field(header)
    if field is None or not disch_rows:
        return 0

    streak = 0
    streak_start = 0
    for i, r in enumerate(disch_rows):
        v = _i(r.get(field))
        if v is None or not (VBAT_LO <= v <= VBAT_HI):
            continue
        if v < PLATEAU_MV:
            if streak == 0:
                streak_start = i
            streak += 1
            if streak >= PLATEAU_STREAK:
                return streak_start
        else:
            streak = 0
    return 0


# ── Session dataclass ─────────────────────────────────────────────────────────

@dataclass
class Session:
    path: Path
    boot: int
    fw: str                     # from CSV `fw=` metadata, or "unknown"
    meta: dict
    header: list[str]
    chg_rows: list[dict]
    disch_rows: list[dict]      # trimmed: starts at real discharge, plateau removed
    chg_dur_ms: int
    disch_dur_ms: int           # trimmed discharge duration
    plateau_ms: int             # duration of on-USB plateau removed from disch_rows
    is_chg: bool                # charge phase qualifies (>= CHARGE_MIN_MIN)
    is_disch: bool              # trimmed discharge qualifies (>= DISCHARGE_MIN_MIN)


# ── Folder scanner ────────────────────────────────────────────────────────────

def _list_csvs(folder: Path) -> list[Path]:
    """Collect both naming schemes:
      batt_XXXX.csv                    -- iv7.1 legacy (boot-seq index)
      kompic_YYYY-MM-DD_HH-MM-SS_batt.csv -- Mk1b datetime scheme
    Pre-RTC-sync files (kompic_2000-*) are ignored per Ivan's convention
    (bench testing before RTC was set).
    """
    seen: set[Path] = set()
    out: list[Path] = []
    for pattern in ("batt_*.csv", "kompic_*_batt.csv"):
        for p in folder.glob(pattern):
            if p in seen:
                continue
            if p.name.startswith("kompic_2000-"):
                continue
            seen.add(p)
            out.append(p)
    return sorted(out, key=lambda p: p.name)


def scan_folder(folder: Path) -> tuple[list[Session], list[Path]]:
    """
    Scan folder for battery CSVs (both legacy batt_XXXX and Mk1b
    kompic_<datetime>_batt naming). Returns (sessions, short_paths) where
    short_paths are empty files or files that qualify for neither charge
    nor discharge analysis.
    """
    sessions: list[Session] = []
    short_paths: list[Path] = []

    for p in _list_csvs(folder):
        # Legacy files carry boot in the stem (batt_0025 -> 25).
        # Mk1b files carry boot inside the # comment (boot=135); read
        # meta first to grab it.
        stem_parts = p.stem.split("_")
        boot_from_stem = _i(stem_parts[-1]) if p.stem.startswith("batt_") else None

        meta, header, rows = _read_csv(p)
        if boot_from_stem is not None:
            boot = boot_from_stem
        else:
            boot = _i(meta.get("boot")) or 0

        fw = meta.get("fw", UNKNOWN_FW) or UNKNOWN_FW

        if not rows:
            short_paths.append(p)
            continue

        chg_rows, disch_rows_raw = _split_phases(rows)
        chg_ms = _dur_ms(chg_rows)

        # Trim on-USB plateau off the front of the discharge phase.
        trim_idx = _find_real_disch_start(disch_rows_raw, header)
        if trim_idx > 0:
            plateau_ms = _dur_ms(disch_rows_raw[: trim_idx + 1])
            disch_rows = disch_rows_raw[trim_idx:]
        else:
            plateau_ms = 0
            disch_rows = disch_rows_raw

        disch_ms = _dur_ms(disch_rows)
        is_chg   = chg_ms   >= CHARGE_MIN_MIN    * 60_000
        is_disch = disch_ms >= DISCHARGE_MIN_MIN * 60_000

        s = Session(
            path=p, boot=boot, fw=fw, meta=meta, header=header,
            chg_rows=chg_rows, disch_rows=disch_rows,
            chg_dur_ms=chg_ms, disch_dur_ms=disch_ms,
            plateau_ms=plateau_ms,
            is_chg=is_chg, is_disch=is_disch,
        )
        sessions.append(s)

        if not is_chg and not is_disch:
            short_paths.append(p)

    return sessions, short_paths


# ── Per-session metrics ───────────────────────────────────────────────────────

def _disch_metrics(s: Session, current_ma: float) -> dict:
    vb = _vbat_samples(s.disch_rows, s.header)
    tp = _temp_samples(s.disch_rows)
    dur_h = s.disch_dur_ms / 3_600_000
    return dict(
        boot      = s.boot,
        fw        = s.fw,
        dur_min   = s.disch_dur_ms / 60_000,
        vbat_start= vb[0]              if vb           else None,
        vbat_end  = vb[-1]             if vb           else None,
        delta_v   = vb[0] - vb[-1]    if len(vb) >= 2 else None,
        temp_mean = mean(tp)           if tp           else None,
        temp_med  = median(tp)         if tp           else None,
        mah       = current_ma * dur_h,
    )


def _chg_metrics(s: Session) -> dict:
    vb = _vbat_samples(s.chg_rows, s.header)
    tp = _temp_samples(s.chg_rows)
    return dict(
        boot      = s.boot,
        fw        = s.fw,
        dur_min   = s.chg_dur_ms / 60_000,
        vbat_start= vb[0]              if vb           else None,
        vbat_end  = vb[-1]             if vb           else None,
        delta_v   = vb[-1] - vb[0]    if len(vb) >= 2 else None,
        temp_mean = mean(tp)           if tp           else None,
        temp_med  = median(tp)         if tp           else None,
    )


# ── Group stats printer ───────────────────────────────────────────────────────

def _stat(label: str, vals: list, unit: str = "", fmt: str = ".1f"):
    vs = [v for v in vals if v is not None]
    if not vs:
        print(f"    {label:<28} n/a")
        return
    print(f"    {label:<28} mean={mean(vs):{fmt}}{unit}  "
          f"median={median(vs):{fmt}}{unit}  "
          f"min={min(vs):{fmt}}{unit}  max={max(vs):{fmt}}{unit}  n={len(vs)}")


def print_group_stats(title: str, metrics: list[dict], kind: str):
    bar = "─" * 68
    print(f"\n{bar}")
    print(f"  {title}  ({len(metrics)} sessions)")
    print(bar)
    if not metrics:
        print("    (no qualifying sessions)")
        return
    _stat("duration (min)",       [m["dur_min"]    for m in metrics], " min")
    _stat("vbat start (mV)",      [m["vbat_start"] for m in metrics], " mV",  ".0f")
    _stat("vbat end (mV)",        [m["vbat_end"]   for m in metrics], " mV",  ".0f")
    _stat("delta vbat (mV)",      [m["delta_v"]    for m in metrics], " mV",  ".0f")
    _stat("SoC temp mean (°C)",   [m["temp_mean"]  for m in metrics], " °C")
    _stat("SoC temp median (°C)", [m["temp_med"]   for m in metrics], " °C")
    if kind == "discharge":
        _stat("delivered (mAh)",  [m["mah"]        for m in metrics], " mAh")


def _group_by_fw(sessions: list[Session]) -> "dict[str, list[Session]]":
    """Group by fw= string, return dict sorted by semver ascending."""
    groups: dict[str, list[Session]] = defaultdict(list)
    for s in sessions:
        groups[s.fw].append(s)
    return dict(sorted(groups.items(), key=lambda kv: _fw_sort_key(kv[0])))


# ── Battery profile builder ───────────────────────────────────────────────────

def _build_profile(sessions: list[Session]) -> list[Optional[float]]:
    """
    Build a 101-element loaded-voltage-vs-SoC profile from discharge sessions.
    SoC is linearly estimated from elapsed discharge time (0% at cutoff, 100% at start).
    Returns profile[soc_pct] = median Vbat_mV across all contributing samples.
    """
    bins: dict[int, list[float]] = defaultdict(list)

    for s in sessions:
        rows = s.disch_rows
        if len(rows) < 10 or s.disch_dur_ms == 0:
            continue
        field = _vbat_field(s.header)
        if field is None:
            continue
        t0 = _i(rows[0].get("t_ms")) or 0
        for r in rows:
            v = _i(r.get(field))
            if v is None or not (VBAT_LO <= v <= VBAT_HI):
                continue
            t = _i(r.get("t_ms")) or 0
            soc = int(round(100.0 * (1.0 - (t - t0) / s.disch_dur_ms)))
            bins[max(0, min(100, soc))].append(float(v))

    profile: list[Optional[float]] = [None] * 101
    for pct, vs in bins.items():
        if vs:
            profile[pct] = median(vs)

    # Linear interpolation for empty bins
    idxs = [i for i, v in enumerate(profile) if v is not None]
    if idxs:
        for i in range(len(profile)):
            if profile[i] is not None:
                continue
            lo = max((x for x in idxs if x <= i), default=None)
            hi = min((x for x in idxs if x >= i), default=None)
            if lo is not None and hi is not None and lo != hi:
                t = (i - lo) / (hi - lo)
                profile[i] = profile[lo] * (1 - t) + profile[hi] * t  # type: ignore[operator]
            elif lo is not None:
                profile[i] = profile[lo]
            elif hi is not None:
                profile[i] = profile[hi]

    # 5-point moving-average smooth (skip edges)
    src = list(profile)
    for i in range(2, 99):
        window = [src[j] for j in range(i - 2, i + 3) if src[j] is not None]
        if window:
            profile[i] = mean(window)

    return profile


def write_battery_profile(sessions: list[Session], fw_tag: str,
                          current_ma: float, out_dir: Path):
    if not sessions:
        print(f"  (no qualifying discharge sessions for fw={fw_tag} -- skipping profile)")
        return

    profile = _build_profile(sessions)
    n = len(sessions)

    # Human-readable table
    txt_path = out_dir / "battery_profile.txt"
    with txt_path.open("w") as f:
        f.write("# Kompic Mk I battery loaded-voltage profile\n")
        f.write(f"# Source: {n} discharge run(s), fw={fw_tag}, {current_ma:.0f} mA assumed\n")
        f.write("# Voltages are UNDER LOAD, not open-circuit (OCV)\n")
        f.write("# SoC_pct, vbat_mv\n")
        for pct in range(100, -1, -1):
            v = profile[pct]
            f.write(f"{pct:3d}, {round(v) if v is not None else 'N/A'}\n")
    print(f"  battery_profile.txt  -> {txt_path}")

    # C header for firmware
    h_path = out_dir / "battery_profile.h"
    with h_path.open("w") as f:
        f.write("#pragma once\n")
        f.write(f"// Kompic Mk I battery loaded-voltage profile.\n")
        f.write(f"// {n} discharge run(s) at fw={fw_tag}, {current_ma:.0f} mA assumed, 10 s sample interval.\n")
        f.write("// Voltages are under load -- correct upward if estimating at rest.\n")
        f.write("// batt_ocv_mv[soc_pct] = mV  (index 0 = empty/UVLO, index 100 = full)\n")
        f.write("#define BATT_PROFILE_POINTS 101\n\n")
        f.write("static const uint16_t batt_ocv_mv[BATT_PROFILE_POINTS] = {\n")
        for pct in range(0, 101):
            v = profile[pct]
            mv = int(round(v)) if v is not None else 0
            comma = "," if pct < 100 else " "
            f.write(f"    {mv:5d}{comma}  // {pct:3d}%\n")
        f.write("};\n\n")
        f.write("// Linear-interpolated SoC estimate from measured Vbat.\n")
        f.write("// Returns 0-100. Rounds toward empty (conservative).\n")
        f.write("static inline uint8_t batt_soc_from_mv(uint16_t mv) {\n")
        f.write("    if (mv <= batt_ocv_mv[0])   return 0;\n")
        f.write("    if (mv >= batt_ocv_mv[100]) return 100;\n")
        f.write("    for (uint8_t i = 1; i <= 100; i++) {\n")
        f.write("        if (mv < batt_ocv_mv[i]) return (uint8_t)(i - 1);\n")
        f.write("    }\n")
        f.write("    return 100;\n")
        f.write("}\n")
    print(f"  battery_profile.h    -> {h_path}")


# ── Plot (1×2, coloured by fw) ────────────────────────────────────────────────

_COLORS = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd",
           "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf"]


def plot_overview(sessions: list[Session], out_path: Path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("  (matplotlib not installed -- skipping plot)")
        return

    # Assign one colour per fw so lines from the same firmware share hue.
    fws = sorted({s.fw for s in sessions}, key=_fw_sort_key)
    fw_color = {fw: _COLORS[i % len(_COLORS)] for i, fw in enumerate(fws)}

    fig, axes = plt.subplots(1, 2, figsize=(14, 5.5))
    fig.suptitle("Kompic Mk I  Battery runs  (grouped by fw=)",
                 fontsize=13, fontweight="bold")

    panels = [
        (axes[0], "disch_rows", "is_disch", "Discharge"),
        (axes[1], "chg_rows",   "is_chg",   "Charging"),
    ]

    for ax, rows_attr, qual_attr, title in panels:
        ax.set_title(title, fontsize=10)
        ax.set_xlabel("time (h)", fontsize=8)
        ax.set_ylabel("Vbat (mV)", fontsize=8)
        ax.tick_params(labelsize=7)
        ax.grid(True, alpha=0.3)
        ax.axhline(4200, color="#aaaaaa", lw=0.8, ls="--", label="4200 mV (full)")
        ax.axhline(3000, color="#aaaaaa", lw=0.8, ls=":",  label="3000 mV (cut)")

        seen_fw: set[str] = set()
        plotted = 0
        for s in sessions:
            if not getattr(s, qual_attr):
                continue
            rows = getattr(s, rows_attr)
            if len(rows) < 2:
                continue
            t0  = _i(rows[0].get("t_ms")) or 0
            t_h = [(((_i(r.get("t_ms")) or 0) - t0) / 3_600_000) for r in rows]
            vb  = [_vbat_or_nan(r, s.header) for r in rows]
            label = f"fw {s.fw}" if s.fw not in seen_fw else None
            seen_fw.add(s.fw)
            ax.plot(t_h, vb,
                    color=fw_color[s.fw], lw=1.1, alpha=0.85,
                    label=label)
            plotted += 1

        if plotted == 0:
            ax.text(0.5, 0.5, "no qualifying data",
                    ha="center", va="center", transform=ax.transAxes,
                    color="gray", fontsize=9)
        else:
            ax.legend(fontsize=7, loc="best")

    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    print(f"  plot                 -> {out_path}")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("folder", type=Path,
                    help="directory containing batt_XXXX.csv files")
    ap.add_argument("--current-ma", type=float, default=106.0,
                    help="assumed constant discharge current in mA (default 106)")
    ap.add_argument("--no-plot", action="store_true",
                    help="skip the PNG overview plot")
    ap.add_argument("--delete-short", action="store_true",
                    help="delete CSV files that qualify for neither charge nor discharge "
                         "analysis (default: list only)")
    ap.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent,
                    help="where to write derived artefacts (profile + plot). "
                         "Default: script's own directory.")
    ap.add_argument("--profile-fw", type=str, default=None,
                    help="firmware version to build the battery profile from "
                         "(e.g. 0.4.24). Default: highest fw= seen in the data.")
    args = ap.parse_args()

    if not args.folder.is_dir():
        print(f"error: {args.folder} is not a directory", file=sys.stderr)
        sys.exit(1)

    args.out_dir.mkdir(parents=True, exist_ok=True)

    sessions, short_paths = scan_folder(args.folder)

    # ── Per-file summary table ────────────────────────────────────────────
    W = 19
    bar = "─" * 92
    print(f"\n{bar}")
    print(f"  Folder:  {args.folder}")
    print(f"  Out-dir: {args.out_dir}")
    print(f"  Discharge duration = time after on-USB plateau (vbat sustained < "
          f"{PLATEAU_MV} mV)")
    print(f"{bar}")
    print(f"  {'file':<{W}} {'fw':<9} {'charge':<11} {'plateau':<10} {'discharge':<12} status")
    print(f"  {'─'*W} {'─'*9} {'─'*11} {'─'*10} {'─'*12} {'─'*18}")

    for s in sessions:
        chg_str  = _ms2str(s.chg_dur_ms)      if s.chg_dur_ms     else "—"
        plat_str = _ms2str(s.plateau_ms)      if s.plateau_ms     else "—"
        disch_str = _ms2str(s.disch_dur_ms)   if s.disch_dur_ms   else "—"
        if   s.is_disch and s.is_chg: status = "✓ disch + chg"
        elif s.is_disch:              status = "✓ discharge"
        elif s.is_chg:                status = "✓ charge only"
        else:                         status = "✗ too short"
        print(f"  {s.path.name:<{W}} {s.fw:<9} {chg_str:<11} {plat_str:<10} {disch_str:<12} {status}")

    # ── Short-file report / deletion ──────────────────────────────────────
    if short_paths:
        print(f"\n  Short / empty files ({len(short_paths)}):")
        for p in short_paths:
            print(f"    {p.name}")
        if args.delete_short:
            for p in short_paths:
                p.unlink()
                print(f"    [deleted] {p.name}")
        else:
            print("  (pass --delete-short to remove them)")

    # ── Group stats by firmware ───────────────────────────────────────────
    fw_groups = _group_by_fw(sessions)

    for fw, grp in fw_groups.items():
        disch = [s for s in grp if s.is_disch]
        chg   = [s for s in grp if s.is_chg]
        if not disch and not chg:
            continue
        print_group_stats(
            f"fw={fw}  — DISCHARGE",
            [_disch_metrics(s, args.current_ma) for s in disch],
            "discharge",
        )
        print_group_stats(
            f"fw={fw}  — CHARGING",
            [_chg_metrics(s) for s in chg],
            "charge",
        )

    # ── Battery profile ───────────────────────────────────────────────────
    fw_candidates = [fw for fw in fw_groups.keys() if fw != UNKNOWN_FW]
    if args.profile_fw:
        target_fw = args.profile_fw
    elif fw_candidates:
        target_fw = sorted(fw_candidates, key=_fw_sort_key)[-1]
    else:
        target_fw = UNKNOWN_FW

    profile_sessions = [s for s in fw_groups.get(target_fw, []) if s.is_disch]

    print(f"\n{'─'*68}")
    print(f"  Battery profile  (fw={target_fw} discharge sessions, "
          f"n={len(profile_sessions)})")
    write_battery_profile(profile_sessions, target_fw,
                          args.current_ma, args.out_dir)

    # ── Plot ──────────────────────────────────────────────────────────────
    if not args.no_plot:
        plot_overview(sessions, out_path=args.out_dir / "battery_analysis.png")

    print()


if __name__ == "__main__":
    main()
