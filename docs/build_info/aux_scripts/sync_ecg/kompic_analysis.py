"""
kompic_analysis.py -- Kompic PPG+BCG signal analysis toolkit.
v1.0 | 2026-08-26

The Kompic firmware records combined PPG and BCG samples to
`/sd/data/ppgbcg/s<boot>_r<seq>.csv`. Each recording contains two interleaved
streams. The `src` column tags each row as `ppg` (MAX30101 raw green) or `bcg`
(LSM6DSV16X accel-Z, HPF+LPF filtered on-device).

The toolkit loads one CSV, splits the two streams, and runs a common analysis
pipeline on each:

- Estimate the sample rate from the actual timestamps in `t_local_us`.
- Bandpass the signal to the physiological band for that modality.
- Detect stable regions with a rolling MAD gate. The gate removes movement
  artefacts and disconnect noise.
- Detect beats with an adaptive-threshold peak detector (Pan-Tompkins-lite).
- Compute BPM statistics, RR intervals, noise floor, and SNR.
- Compare the on-device `beat` markers against the offline detector.

Entry points:

    KompicRecording.from_csv(path)      Load one file.
    KompicRecording.analyze_ppg()       Return a PpgResult.
    KompicRecording.analyze_bcg()       Return a BcgResult.
    analyze_folder(folder)              Return a dict {path: results}.
    summarize(results)                  Return a DataFrame, one row per file.

Plot helpers live in `kompic_plots.py`. The plots do not import here so this
module is import-safe on headless machines.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd
from scipy import signal as sps

# ---------------------------------------------------------------------------
# Constants -- physiological bands per modality
# ---------------------------------------------------------------------------

PPG_BAND      = (0.5, 5.0)   # Hz -- covers 30..300 BPM plus one harmonic.
BCG_BAND      = (1.0, 15.0)  # Hz -- BCG upstroke energy sits here.
STAGE15_STEP  = 5000         # us -- Stage 15 target tick period (200 Hz).

# Stable-region gate. The same values work for both modalities because the
# gate normalises against the recording's own median.
STABLE_WIN_S     = 2.0
STABLE_MAD_MULT  = 6.0
STABLE_MIN_LEN_S = 3.0

# Peak detector. Values match the ECG toolkit for consistency; the refractory
# window is the only value the caller may want to change per modality.
INT_WIN_S    = 0.150
REFRACT_S    = 0.300   # 200 BPM ceiling.
INIT_LEARN_S = 2.0
THR_FRAC     = 0.35
NOISE_FRAC   = 0.15


# ---------------------------------------------------------------------------
# Loader
# ---------------------------------------------------------------------------

@dataclass
class Stream:
    """One extracted stream (PPG or BCG) from a Kompic CSV."""
    src: str                  # "ppg" or "bcg"
    fs: float                 # estimated sample rate (Hz)
    t: np.ndarray             # seconds since first sample of this stream
    t_local_us: np.ndarray    # firmware clock, us since session start
    t_pc_us: np.ndarray       # PC-aligned clock, us
    raw: np.ndarray           # raw value (green counts or accel-Z in g)
    filt: np.ndarray          # filtered value from firmware (BCG only; NaN for PPG)
    beat: np.ndarray          # 0/1 beat markers from firmware
    bpm: np.ndarray           # instantaneous BPM from firmware

    def duration_s(self) -> float:
        return float(self.t[-1] - self.t[0]) if self.t.size else 0.0


@dataclass
class KompicRecording:
    """A full Kompic PPG+BCG CSV, split into two Streams."""
    path: Path
    header: dict
    ppg: Optional[Stream]
    bcg: Optional[Stream]

    @classmethod
    def from_csv(cls, path) -> "KompicRecording":
        path = Path(path)
        header = _read_header(path)
        df = pd.read_csv(path, comment="#")
        df = df.dropna(subset=["t_local_us", "src", "raw"]).reset_index(drop=True)

        ppg = _extract_stream(df, "ppg")
        bcg = _extract_stream(df, "bcg")
        return cls(path=path, header=header, ppg=ppg, bcg=bcg)

    def sync_offset_us(self) -> Optional[int]:
        """Return the pc_sync offset from the CSV header, or None if unsynced."""
        applied = self.header.get("pc_sync_applied")
        if applied != "1":
            return None
        try:
            return int(self.header.get("pc_sync_offset_us", ""))
        except ValueError:
            return None

    def analyze_ppg(self) -> Optional["PpgResult"]:
        if self.ppg is None:
            return None
        return analyze_stream(self.ppg, PPG_BAND, PpgResult)

    def analyze_bcg(self) -> Optional["BcgResult"]:
        if self.bcg is None:
            return None
        return analyze_stream(self.bcg, BCG_BAND, BcgResult)


def _read_header(path: Path) -> dict:
    """Read `# key=value` and `# key: value` lines from the top of the file."""
    header: dict = {}
    with open(path) as f:
        for line in f:
            if not line.startswith("#"):
                break
            body = line.lstrip("#").strip()
            m = re.match(r"([^=:\s]+)\s*[:=]\s*(.+)", body)
            if m:
                header[m.group(1).strip()] = m.group(2).strip()
    return header


def _extract_stream(df: pd.DataFrame, src: str) -> Optional[Stream]:
    """Extract rows for one modality and estimate the sample rate."""
    sub = df[df["src"] == src].reset_index(drop=True)
    if len(sub) < 20:
        return None
    t_us = sub["t_local_us"].astype(np.int64).to_numpy()
    t_s  = (t_us - t_us[0]) / 1_000_000.0

    dt = np.diff(t_s)
    if dt.size == 0:
        return None
    fs = 1.0 / float(np.median(dt[dt > 0])) if np.any(dt > 0) else float("nan")

    filt = sub.get("filt")
    filt_arr = (filt.astype(float).to_numpy()
                if filt is not None and filt.notna().any()
                else np.full(len(sub), np.nan))

    return Stream(
        src=src,
        fs=fs,
        t=t_s,
        t_local_us=t_us,
        t_pc_us=sub["t_pc_us"].astype(np.int64).to_numpy(),
        raw=sub["raw"].astype(float).to_numpy(),
        filt=filt_arr,
        beat=sub["beat"].astype(int).to_numpy(),
        bpm=sub.get("bpm",
                    pd.Series([0.0] * len(sub))).astype(float).to_numpy(),
    )


# ---------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------

@dataclass
class _AnalysisResultBase:
    """Common analysis result. Both PPG and BCG use the same fields."""
    stream: Stream
    signal_bp: np.ndarray        # bandpassed input, DC removed
    polarity: int                # +1 or -1 -- peaks point up in signal_bp * polarity
    stable_mask: np.ndarray
    stable_segments: list        # list of (i_start, i_end)
    peaks: np.ndarray            # sample indices in the bandpassed frame
    peaks_stable: np.ndarray
    rr_ms: np.ndarray
    bpm_mean: float
    bpm_median: float
    sdnn_ms: float
    rmssd_ms: float
    noise_rms: float             # in raw signal units
    signal_rms: float
    peak_amp_median: float
    snr_db: float
    stable_frac: float
    fw_beats_total: int          # count of beat==1 rows from firmware
    fw_beats_stable: int         # subset of firmware beats inside stable regions

    def summary_dict(self) -> dict:
        return {
            "file": self.stream.src,
            "fs_hz": round(self.stream.fs, 2),
            "duration_s": round(self.stream.duration_s(), 1),
            "polarity": self.polarity,
            "stable_frac": round(self.stable_frac, 3),
            "n_peaks_total": int(len(self.peaks)),
            "n_peaks_stable": int(len(self.peaks_stable)),
            "n_fw_beats": self.fw_beats_total,
            "n_fw_beats_stable": self.fw_beats_stable,
            "bpm_median": (round(self.bpm_median, 1)
                           if np.isfinite(self.bpm_median) else float("nan")),
            "bpm_mean": (round(self.bpm_mean, 1)
                         if np.isfinite(self.bpm_mean) else float("nan")),
            "sdnn_ms": round(self.sdnn_ms, 1),
            "rmssd_ms": round(self.rmssd_ms, 1),
            "noise_rms": round(self.noise_rms, 3),
            "peak_amp": round(self.peak_amp_median, 3),
            "snr_db": round(self.snr_db, 1),
        }


@dataclass
class PpgResult(_AnalysisResultBase):
    pass


@dataclass
class BcgResult(_AnalysisResultBase):
    pass


# ---------------------------------------------------------------------------
# Filtering primitives
# ---------------------------------------------------------------------------

def _butter_bandpass(low: float, high: float, fs: float, order: int = 3):
    ny = 0.5 * fs
    return sps.butter(order, [low / ny, min(high, ny * 0.99) / ny], btype="band")


def bandpass(x: np.ndarray, fs: float, band, order: int = 3) -> np.ndarray:
    b, a = _butter_bandpass(band[0], band[1], fs, order=order)
    return sps.filtfilt(b, a, x, method="pad")


# ---------------------------------------------------------------------------
# Stable regions -- same idea as the ECG toolkit. See ecg_analysis.py for the
# original commentary. The values match by design.
# ---------------------------------------------------------------------------

def _rolling_mad(x: np.ndarray, w: int) -> np.ndarray:
    if w < 3:
        w = 3
    if w % 2 == 0:
        w += 1
    xs = pd.Series(x)
    med = xs.rolling(w, center=True, min_periods=1).median()
    mad = (xs - med).abs().rolling(w, center=True, min_periods=1).median()
    return mad.to_numpy()


def find_stable_regions(x_bp: np.ndarray, fs: float,
                        win_s: float = STABLE_WIN_S,
                        mad_mult: float = STABLE_MAD_MULT,
                        min_len_s: float = STABLE_MIN_LEN_S):
    """Return (mask, segments). Segments are (i_start, i_end) tuples.

    A sample is stable when the local MAD sits inside a plausible range and
    the local peak-to-peak does not run away. The gate rejects manhandling and
    disconnect noise but keeps physiological signal.
    """
    w = int(round(win_s * fs))
    mad = _rolling_mad(x_bp, w)
    global_mad = float(np.median(mad[mad > 0])) if np.any(mad > 0) else 1.0
    stable_mad = (mad > global_mad * 0.15) & (mad < global_mad * mad_mult)

    p2p = pd.Series(x_bp).rolling(w, center=True, min_periods=1).max().to_numpy() \
        - pd.Series(x_bp).rolling(w, center=True, min_periods=1).min().to_numpy()
    global_p2p = float(np.median(p2p))
    stable_p2p = p2p < 3.0 * global_p2p

    stable = stable_mad & stable_p2p
    segs = _mask_to_segments(stable)
    min_len = int(round(min_len_s * fs))
    segs = [(a, b) for (a, b) in segs if b - a >= min_len]

    mask_clean = np.zeros_like(stable, dtype=bool)
    for a, b in segs:
        mask_clean[a:b] = True
    return mask_clean, segs


def _mask_to_segments(mask: np.ndarray) -> list:
    if len(mask) == 0:
        return []
    d = np.diff(mask.astype(np.int8))
    starts = list(np.where(d == 1)[0] + 1)
    ends   = list(np.where(d == -1)[0] + 1)
    if mask[0]:  starts.insert(0, 0)
    if mask[-1]: ends.append(len(mask))
    return list(zip(starts, ends))


# ---------------------------------------------------------------------------
# Peak detector -- Pan-Tompkins-lite on the bandpassed signal.
# ---------------------------------------------------------------------------

def detect_polarity(x_bp: np.ndarray) -> int:
    hi = np.percentile(x_bp, 99.5)
    lo = np.percentile(x_bp, 0.5)
    return -1 if abs(lo) > abs(hi) else +1


def _pt_transform(x_bp: np.ndarray, fs: float):
    d = np.zeros_like(x_bp)
    d[2:-2] = (2 * x_bp[4:] + x_bp[3:-1] - x_bp[1:-3] - 2 * x_bp[:-4]) / 8.0 * fs
    sq = d * d
    n = max(3, int(round(INT_WIN_S * fs)))
    kernel = np.ones(n) / n
    inte = np.convolve(sq, kernel, mode="same")
    return inte


def find_peaks_adaptive(x_bp: np.ndarray, fs: float,
                        stable_mask: Optional[np.ndarray] = None) -> np.ndarray:
    integ = _pt_transform(x_bp, fs)
    n_learn = max(int(round(INIT_LEARN_S * fs)), 20)
    if stable_mask is not None and stable_mask.any():
        idx_stable = np.where(stable_mask)[0]
        init = integ[idx_stable[:n_learn]] if len(idx_stable) > n_learn else integ[:n_learn]
    else:
        init = integ[:n_learn]

    spki = float(np.max(init)) if init.size else 1.0
    npki = float(np.mean(init))
    thr  = npki + THR_FRAC * (spki - npki)

    refract = int(round(REFRACT_S * fs))
    peaks = []
    last  = -refract
    all_peaks, _ = sps.find_peaks(integ, distance=refract, height=None)
    for i in all_peaks:
        if i - last < refract:
            continue
        if integ[i] >= thr:
            peaks.append(i)
            spki = 0.125 * integ[i] + 0.875 * spki
            last = i
        else:
            npki = 0.125 * integ[i] + 0.875 * npki
        thr = npki + THR_FRAC * (spki - npki)

    peaks = np.array(peaks, dtype=int)

    # Snap each integrator peak back to the nearest local extremum in x_bp.
    if peaks.size:
        w = int(round(0.10 * fs))
        snapped = []
        for i in peaks:
            a, b = max(0, i - w), min(len(x_bp), i + w + 1)
            j = a + int(np.argmax(np.abs(x_bp[a:b])))
            snapped.append(j)
        peaks = np.array(snapped, dtype=int)
    return peaks


# ---------------------------------------------------------------------------
# Noise floor + SNR
# ---------------------------------------------------------------------------

def noise_floor_between_peaks(x_bp: np.ndarray, fs: float, peaks: np.ndarray,
                              stable_mask: Optional[np.ndarray] = None) -> float:
    if len(peaks) < 2:
        return float("nan")
    w = int(round(0.075 * fs))
    chunks = []
    for a, b in zip(peaks[:-1], peaks[1:]):
        m = (a + b) // 2
        lo, hi = max(0, m - w), min(len(x_bp), m + w)
        if stable_mask is not None and not stable_mask[lo:hi].all():
            continue
        chunk = x_bp[lo:hi]
        if chunk.size:
            chunks.append(chunk)
    if not chunks:
        return float("nan")
    arr = np.concatenate(chunks)
    return float(np.sqrt(np.mean(arr * arr)))


# ---------------------------------------------------------------------------
# Common pipeline -- called by KompicRecording.analyze_ppg / analyze_bcg.
# ---------------------------------------------------------------------------

def analyze_stream(stream: Stream, band, result_cls) -> _AnalysisResultBase:
    x = stream.raw.astype(float)
    x = x - float(np.mean(x))
    x_bp = bandpass(x, stream.fs, band=band)
    polarity = detect_polarity(x_bp)
    x_signed = x_bp * polarity

    stable_mask, stable_segments = find_stable_regions(x_signed, stream.fs)
    peaks = find_peaks_adaptive(x_signed, stream.fs, stable_mask=stable_mask)

    if peaks.size and stable_mask.size:
        peaks_stable = peaks[stable_mask[peaks]]
    else:
        peaks_stable = np.array([], dtype=int)

    # RR intervals from stable peaks inside the same segment only.
    rr_ms_list = []
    for a, b in stable_segments:
        seg_pk = peaks_stable[(peaks_stable >= a) & (peaks_stable < b)]
        if len(seg_pk) >= 2:
            rr = np.diff(seg_pk) / stream.fs * 1000.0
            rr_ms_list.append(rr)
    rr_ms = np.concatenate(rr_ms_list) if rr_ms_list else np.array([])

    if rr_ms.size:
        rr_phys = rr_ms[(rr_ms > 350) & (rr_ms < 2000)]
    else:
        rr_phys = rr_ms

    if rr_phys.size >= 2:
        bpm = 60000.0 / rr_phys
        bpm_mean = float(np.mean(bpm))
        bpm_median = float(np.median(bpm))
        sdnn = float(np.std(rr_phys, ddof=1))
        rmssd = float(np.sqrt(np.mean(np.diff(rr_phys) ** 2)))
    else:
        bpm_mean = bpm_median = float("nan")
        sdnn = rmssd = 0.0

    noise_rms = noise_floor_between_peaks(x_signed, stream.fs, peaks_stable, stable_mask)

    if stable_mask.any():
        signal_rms = float(np.sqrt(np.mean(x_signed[stable_mask] ** 2)))
    else:
        signal_rms = float(np.sqrt(np.mean(x_signed ** 2)))

    if peaks_stable.size:
        w = int(round(0.100 * stream.fs))
        amps = []
        for i in peaks_stable:
            lo, hi = max(0, i - 4 * w), min(len(x_signed), i + 4 * w + 1)
            baseline = float(np.median(x_signed[lo:hi]))
            amps.append(x_signed[i] - baseline)
        peak_amp = float(np.median(amps))
    else:
        peak_amp = float("nan")

    if np.isfinite(peak_amp) and np.isfinite(noise_rms) and noise_rms > 0 and peak_amp > 0:
        snr_db = 20.0 * float(np.log10(peak_amp / noise_rms))
    else:
        snr_db = float("nan")

    fw_beats_total  = int(np.sum(stream.beat == 1))
    fw_beats_stable = int(np.sum((stream.beat == 1) & stable_mask))
    stable_frac = float(stable_mask.mean()) if stable_mask.size else 0.0

    return result_cls(
        stream=stream,
        signal_bp=x_signed,
        polarity=polarity,
        stable_mask=stable_mask,
        stable_segments=stable_segments,
        peaks=peaks,
        peaks_stable=peaks_stable,
        rr_ms=rr_ms,
        bpm_mean=bpm_mean,
        bpm_median=bpm_median,
        sdnn_ms=sdnn,
        rmssd_ms=rmssd,
        noise_rms=noise_rms,
        signal_rms=signal_rms,
        peak_amp_median=peak_amp,
        snr_db=snr_db,
        stable_frac=stable_frac,
        fw_beats_total=fw_beats_total,
        fw_beats_stable=fw_beats_stable,
    )


# ---------------------------------------------------------------------------
# Batch helpers
# ---------------------------------------------------------------------------

def analyze_folder(folder, min_rows: int = 400) -> dict:
    folder = Path(folder)
    out = {}
    for p in sorted(folder.glob("s*_r*.csv")):
        try:
            rec = KompicRecording.from_csv(p)
        except Exception as e:
            print(f"{p.name}: LOAD FAIL -- {e}")
            continue
        if (rec.ppg is None or len(rec.ppg.raw) < min_rows) and \
           (rec.bcg is None or len(rec.bcg.raw) < min_rows):
            print(f"{p.name}: too short -- skipping")
            continue
        out[p] = {"ppg": rec.analyze_ppg(), "bcg": rec.analyze_bcg()}
    return out


def summarize(results: dict) -> pd.DataFrame:
    rows = []
    for path, r in results.items():
        for modality in ("ppg", "bcg"):
            res = r.get(modality)
            if res is None:
                continue
            row = res.summary_dict()
            row["file"] = path.name
            row["modality"] = modality
            rows.append(row)
    df = pd.DataFrame(rows)
    if len(df):
        df = df.sort_values(["file", "modality"]).reset_index(drop=True)
    return df
