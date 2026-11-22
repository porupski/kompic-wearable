"""
Loader for Kompic flight CSVs (CSV format v1,
docs/build_info/Mk2_intel/CSV_Standard.md).

Usage:
    from flight_csv import load
    df, meta = load("path/to/kompic_YYYY-MM-DD_HH-MM-SS_flight.csv")
    print(meta["device_fw"], meta["sample_rate_hz"])
    df.plot(x="t_s", y="az_mg")

The loader reads the `# key = value` header block into a dict, then
reads the body with pandas. `ts_ms` is reused as-is (ms since RTC
epoch) and augmented with `t_s` (seconds since first row) for easier
plotting. Sparse columns (mag/env/gps) stay as-written -- callers
decide whether to interpolate using the `_fresh` flags.
"""

from __future__ import annotations

import io
from pathlib import Path

import pandas as pd


def _parse_header(lines: list[str]) -> dict[str, str]:
    """Parse `# key = value` lines into a dict. Non-matching lines ignored."""
    meta: dict[str, str] = {}
    for raw in lines:
        if not raw.startswith("#"):
            continue
        body = raw.lstrip("#").strip()
        if "=" not in body:
            continue
        key, _, val = body.partition("=")
        meta[key.strip()] = val.strip()
    return meta


def load(path: str | Path) -> tuple[pd.DataFrame, dict[str, str]]:
    """Load a flight CSV. Returns (dataframe, header_metadata).

    Blank sparse-channel fields (e.g. GPS before fix) become NaN.
    """
    p = Path(path)
    text = p.read_text()

    header_lines: list[str] = []
    body_start = 0
    for i, line in enumerate(text.splitlines(keepends=True)):
        if line.startswith("#"):
            header_lines.append(line)
            continue
        # First non-comment line is the column header row.
        body_start = sum(len(l) for l in text.splitlines(keepends=True)[:i])
        break

    meta = _parse_header(header_lines)

    body = text[body_start:]
    df = pd.read_csv(io.StringIO(body))

    # Convenience: seconds-since-first-row.
    if "ts_ms" in df.columns and len(df):
        df["t_s"] = (df["ts_ms"] - df["ts_ms"].iloc[0]) / 1000.0

    return df, meta


# Canonical channel grouping -- lets the notebook iterate groups
# without hard-coding column names in every cell.
CHANNEL_GROUPS = {
    "accel (mg)":    ["ax_mg", "ay_mg", "az_mg"],
    "gyro (mdps)":   ["gx_mdps", "gy_mdps", "gz_mdps"],
    "mag (mG)":      ["mx_mG", "my_mG", "mz_mG"],
    "env":           ["temp_c", "p_hpa", "h_pct", "gas_ohm"],
    "gps":           ["gps_lat_deg", "gps_lon_deg", "gps_alt_m"],
}
