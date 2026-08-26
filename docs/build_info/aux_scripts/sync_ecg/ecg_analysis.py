"""
ecg_analysis.py — Srceko-ECG signal analysis toolkit
v1.0 | 2026-08-18

Loads srceko CSV recordings and extracts, per file:
  - Fs (from actual timestamps, not header)
  - Bandpass-filtered ECG (0.5–40 Hz) + 50 Hz notch
  - Polarity (auto: signed skew of QRS-band-filtered signal)
  - Stable regions (rolling MAD gate — kills manhandling / disconnect noise)
  - Noise floor (RMS of stable regions between peaks)
  - R-peaks (Pan-Tompkins-lite: BP → derivative → square → moving-window integrate,
              adaptive threshold, 300 ms refractory)
  - BPM statistics + RR intervals + SNR

Entry points:
  Recording.from_csv(path)            → load one file
  Recording.analyze()                 → returns AnalysisResult
  analyze_folder(folder)              → dict {path: AnalysisResult}
  summarize(results)                  → DataFrame, one row per file

Plots are in ecg_plots.py so this module is import-safe on headless machines.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd
from scipy import signal as sps

FS_NOMINAL = 125.0        # Hz — 8 ms time_step
QRS_BAND    = (5.0, 15.0) # Hz — Pan-Tompkins bandpass
ECG_BAND    = (0.5, 40.0) # Hz — clinical ECG band
MAINS_HZ    = 50.0        # notch (change to 60 if you cross the pond)

# Stable-region parameters
STABLE_WIN_S     = 2.0    # rolling window for MAD estimate
STABLE_MAD_MULT  = 6.0    # sample must be within this × median MAD to be "stable"
STABLE_MIN_LEN_S = 3.0    # discard stable segments shorter than this

# Peak detection
INT_WIN_S    = 0.150      # moving-window integration length (Pan-Tompkins classic)
REFRACT_S    = 0.300      # min time between R-peaks (≤ 200 BPM)
INIT_LEARN_S = 2.0        # initial threshold learning window
THR_FRAC     = 0.35       # threshold = THR_FRAC * running peak height (SPKI)
NOISE_FRAC   = 0.15       # noise level is updated as running peak in non-QRS regions


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class Recording:
    """One loaded CSV. Timestamps in seconds since first sample."""
    path: Path
    fs: float
    t: np.ndarray
    ecg_mv: np.ndarray
    temp_c: np.ndarray
    vbat_mv: np.ndarray
    header: dict = field(default_factory=dict)

    @classmethod
    def from_csv(cls, path) -> "Recording":
        path = Path(path)
        header: dict = {}
        with open(path) as f:
            for line in f:
                if not line.startswith("#"):
                    break
                m = re.match(r"#\s*([^:]+):\s*(.+)", line)
                if m:
                    header[m.group(1).strip()] = m.group(2).strip()

        df = pd.read_csv(path, comment="#")
        df = df.dropna().reset_index(drop=True)

        # Time column: either "datetime" (RTC) or "time_ms" (boot)
        if "datetime" in df.columns:
            dt = pd.to_datetime(df["datetime"])
            t = (dt - dt.iloc[0]).dt.total_seconds().to_numpy()
        elif "time_ms" in df.columns:
            t = (df["time_ms"].astype(float).to_numpy()
                 - float(df["time_ms"].iloc[0])) / 1000.0
        else:
            raise ValueError(f"{path}: no datetime/time_ms column")

        # Estimate Fs from median inter-sample interval (robust to gaps)
        dt = np.diff(t)
        if len(dt) == 0:
            raise ValueError(f"{path}: only {len(df)} rows")
        fs = 1.0 / float(np.median(dt))

        return cls(
            path=path, fs=fs, t=t,
            ecg_mv=df["ecg_mv"].astype(float).to_numpy(),
            temp_c=df["temp_c"].astype(float).to_numpy(),
            vbat_mv=df["vbat_mv"].astype(float).to_numpy(),
            header=header,
        )

    def duration_s(self) -> float:
        return float(self.t[-1] - self.t[0]) if len(self.t) else 0.0

    def analyze(self, mains_hz: float = MAINS_HZ) -> "AnalysisResult":
        return analyze_recording(self, mains_hz=mains_hz)


@dataclass
class AnalysisResult:
    rec: Recording
    ecg_filt: np.ndarray      # bandpassed + notched ECG (mV, DC removed)
    polarity: int             # +1 (peaks up) or -1 (inverted)
    stable_mask: np.ndarray   # bool, per sample — True in stable segments
    stable_segments: list     # list of (i_start, i_end) into filt
    peaks: np.ndarray         # sample indices of detected R-peaks (in filt frame, orientation-corrected)
    peaks_stable: np.ndarray  # subset of peaks that fall inside a stable segment
    rr_ms: np.ndarray         # RR intervals (ms) between adjacent stable peaks
    bpm_mean: float
    bpm_median: float
    sdnn_ms: float            # std-dev of NN intervals
    rmssd_ms: float           # sqrt(mean of squared successive RR differences)
    noise_rms_mv: float       # noise floor (RMS of filtered signal in inter-beat intervals of stable regions)
    signal_rms_mv: float      # RMS of QRS-band signal on stable regions
    peak_amp_median_mv: float # median R-peak height above local baseline
    snr_db: float             # 20*log10(peak_amp / noise_rms)
    stable_frac: float        # fraction of recording that is stable

    def summary_dict(self) -> dict:
        return {
            "file": self.rec.path.name,
            "fs_hz": round(self.rec.fs, 2),
            "duration_s": round(self.rec.duration_s(), 1),
            "polarity": self.polarity,
            "stable_frac": round(self.stable_frac, 3),
            "n_peaks_total": int(len(self.peaks)),
            "n_peaks_stable": int(len(self.peaks_stable)),
            "bpm_median": round(self.bpm_median, 1) if np.isfinite(self.bpm_median) else float("nan"),
            "bpm_mean": round(self.bpm_mean, 1) if np.isfinite(self.bpm_mean) else float("nan"),
            "sdnn_ms": round(self.sdnn_ms, 1),
            "rmssd_ms": round(self.rmssd_ms, 1),
            "noise_rms_mv": round(self.noise_rms_mv, 1),
            "peak_amp_mv": round(self.peak_amp_median_mv, 1),
            "snr_db": round(self.snr_db, 1),
        }


# ---------------------------------------------------------------------------
# Filtering
# ---------------------------------------------------------------------------

def _butter_bandpass(low: float, high: float, fs: float, order: int = 3):
    ny = 0.5 * fs
    return sps.butter(order, [low / ny, min(high, ny * 0.99) / ny], btype="band")


def _iir_notch(f0: float, fs: float, q: float = 30.0):
    return sps.iirnotch(f0 / (fs / 2.0), q)


def bandpass(x: np.ndarray, fs: float, band=ECG_BAND, order: int = 3) -> np.ndarray:
    b, a = _butter_bandpass(band[0], band[1], fs, order=order)
    return sps.filtfilt(b, a, x, method="pad")


def notch(x: np.ndarray, fs: float, f0: float = MAINS_HZ) -> np.ndarray:
    if f0 <= 0 or f0 >= fs / 2:
        return x
    b, a = _iir_notch(f0, fs)
    return sps.filtfilt(b, a, x, method="pad")


# ---------------------------------------------------------------------------
# Polarity detection
# ---------------------------------------------------------------------------

def detect_polarity(x_qrs: np.ndarray) -> int:
    """
    QRS-band-filtered signal. If R-peaks are upward, the tallest excursions
    are positive; if inverted, negative. Compare 99.5th percentile vs |0.5th|.
    """
    hi = np.percentile(x_qrs, 99.5)
    lo = np.percentile(x_qrs, 0.5)
    return -1 if abs(lo) > abs(hi) else +1


# ---------------------------------------------------------------------------
# Stable-region detection
# ---------------------------------------------------------------------------

def _rolling_mad(x: np.ndarray, w: int) -> np.ndarray:
    """Rolling median absolute deviation from rolling median."""
    if w < 3:
        w = 3
    if w % 2 == 0:
        w += 1
    x_pd = pd.Series(x)
    med = x_pd.rolling(w, center=True, min_periods=1).median()
    mad = (x_pd - med).abs().rolling(w, center=True, min_periods=1).median()
    return mad.to_numpy()


def find_stable_regions(x_bp: np.ndarray, fs: float,
                        win_s: float = STABLE_WIN_S,
                        mad_mult: float = STABLE_MAD_MULT,
                        min_len_s: float = STABLE_MIN_LEN_S):
    """
    Detects segments where the bandpassed signal behaves like ECG rather than
    disconnect noise or manhandling. Idea: MAD is stable on physiological ECG;
    balloons on artefacts. Sample is "stable" if MAD within [0.25×, 4×] of the
    global median MAD AND its short-window peak-to-peak is not runaway.
    """
    w = int(round(win_s * fs))
    mad = _rolling_mad(x_bp, w)
    global_mad = np.median(mad[mad > 0]) if np.any(mad > 0) else 1.0
    stable_mad = (mad > global_mad * 0.15) & (mad < global_mad * mad_mult)

    # Rolling p2p: reject samples where local range is >> the recording median
    p2p = pd.Series(x_bp).rolling(w, center=True, min_periods=1).max().to_numpy() \
        - pd.Series(x_bp).rolling(w, center=True, min_periods=1).min().to_numpy()
    global_p2p = float(np.median(p2p))
    stable_p2p = p2p < 3.0 * global_p2p

    stable = stable_mad & stable_p2p

    # Merge into contiguous segments and drop short ones
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
    ends = list(np.where(d == -1)[0] + 1)
    if mask[0]:
        starts.insert(0, 0)
    if mask[-1]:
        ends.append(len(mask))
    return list(zip(starts, ends))


# ---------------------------------------------------------------------------
# Peak detection — Pan-Tompkins-lite
# ---------------------------------------------------------------------------

def _pt_transform(x_bp: np.ndarray, fs: float):
    """Bandpass → derivative → square → moving-window integrate."""
    # Bandpass to QRS band
    b, a = _butter_bandpass(QRS_BAND[0], QRS_BAND[1], fs, order=3)
    x_bp2 = sps.filtfilt(b, a, x_bp, method="pad")
    # Derivative — 5-point (Pan-Tompkins style, scaled by fs/8)
    d = np.zeros_like(x_bp2)
    d[2:-2] = (2 * x_bp2[4:] + x_bp2[3:-1] - x_bp2[1:-3] - 2 * x_bp2[:-4]) / 8.0 * fs
    # Square
    sq = d * d
    # Moving-window integrate
    n = max(3, int(round(INT_WIN_S * fs)))
    kernel = np.ones(n) / n
    inte = np.convolve(sq, kernel, mode="same")
    return x_bp2, inte


def find_r_peaks(x_bp: np.ndarray, fs: float, stable_mask: Optional[np.ndarray] = None):
    """
    Adaptive-threshold R-peak detector on integrated QRS signal.

    - Initial threshold: mean of first INIT_LEARN_S seconds of the integrator
    - Signal peak (SPKI) and noise peak (NPKI) run adaptively (Pan-Tompkins style)
    - Refractory: REFRACT_S
    """
    x_qrs, integ = _pt_transform(x_bp, fs)

    # Learning window
    n_learn = int(round(INIT_LEARN_S * fs))
    n_learn = max(n_learn, 20)
    if stable_mask is not None:
        # Prefer to learn on stable data if available
        idx_stable = np.where(stable_mask)[0]
        if len(idx_stable) > n_learn:
            init = integ[idx_stable[:n_learn]]
        else:
            init = integ[:n_learn]
    else:
        init = integ[:n_learn]

    spki = float(np.max(init)) if init.size else 1.0
    npki = float(np.mean(init))
    thr  = npki + THR_FRAC * (spki - npki)

    refract = int(round(REFRACT_S * fs))
    peaks = []
    last = -refract

    # Find local maxima above threshold in the integrator
    # Use scipy.find_peaks for robustness, then walk with adaptive threshold
    all_peaks, _ = sps.find_peaks(integ, distance=refract, height=None)
    for i in all_peaks:
        if i - last < refract:
            continue
        if integ[i] >= thr:
            # Adopt as R-peak
            peaks.append(i)
            spki = 0.125 * integ[i] + 0.875 * spki
            last = i
        else:
            npki = 0.125 * integ[i] + 0.875 * npki
        thr = npki + THR_FRAC * (spki - npki)

    peaks = np.array(peaks, dtype=int)

    # Snap each integrator-peak back to the local max of x_qrs (± 100 ms window)
    if peaks.size:
        w = int(round(0.10 * fs))
        snapped = []
        for i in peaks:
            a, b = max(0, i - w), min(len(x_qrs), i + w + 1)
            j = a + int(np.argmax(np.abs(x_qrs[a:b])))
            snapped.append(j)
        peaks = np.array(snapped, dtype=int)

    return peaks, integ


# ---------------------------------------------------------------------------
# Noise floor + SNR
# ---------------------------------------------------------------------------

def noise_floor_between_peaks(x_bp: np.ndarray, fs: float, peaks: np.ndarray,
                              stable_mask: Optional[np.ndarray] = None) -> float:
    """
    RMS of x_bp taken from windows centred halfway between adjacent R-peaks,
    150 ms wide, restricted to stable regions.
    """
    if len(peaks) < 2:
        return float("nan")
    w = int(round(0.075 * fs))  # ±75 ms → 150 ms window
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
# Top-level pipeline
# ---------------------------------------------------------------------------

def analyze_recording(rec: Recording, mains_hz: float = MAINS_HZ) -> AnalysisResult:
    x = rec.ecg_mv - float(np.mean(rec.ecg_mv))     # de-mean
    x = notch(x, rec.fs, mains_hz)
    x = bandpass(x, rec.fs, band=ECG_BAND)

    x_qrs = bandpass(x, rec.fs, band=QRS_BAND)
    polarity = detect_polarity(x_qrs)
    x_signed = x * polarity                          # peaks point up in x_signed

    stable_mask, stable_segments = find_stable_regions(x_signed, rec.fs)

    peaks, _ = find_r_peaks(x_signed, rec.fs, stable_mask=stable_mask)

    if peaks.size:
        peaks_stable = peaks[stable_mask[peaks]] if stable_mask.size else peaks
    else:
        peaks_stable = np.array([], dtype=int)

    # RR intervals — only from consecutive stable peaks in the SAME segment
    rr_ms_list = []
    peak_times_s = peaks_stable / rec.fs
    for a, b in stable_segments:
        seg_pk = peaks_stable[(peaks_stable >= a) & (peaks_stable < b)]
        if len(seg_pk) >= 2:
            rr = np.diff(seg_pk) / rec.fs * 1000.0
            rr_ms_list.append(rr)
    rr_ms = np.concatenate(rr_ms_list) if rr_ms_list else np.array([])

    # Filter physiological range (350-2000 ms = 30-171 BPM)
    if rr_ms.size:
        rr_phys = rr_ms[(rr_ms > 350) & (rr_ms < 2000)]
    else:
        rr_phys = rr_ms

    if rr_phys.size >= 2:
        bpm = 60000.0 / rr_phys
        bpm_mean, bpm_median = float(np.mean(bpm)), float(np.median(bpm))
        sdnn = float(np.std(rr_phys, ddof=1))
        rmssd = float(np.sqrt(np.mean(np.diff(rr_phys) ** 2))) if rr_phys.size >= 2 else 0.0
    else:
        bpm_mean = bpm_median = float("nan")
        sdnn = rmssd = 0.0

    noise_rms = noise_floor_between_peaks(x_signed, rec.fs, peaks_stable, stable_mask)

    # Signal RMS on stable regions
    if stable_mask.any():
        signal_rms = float(np.sqrt(np.mean(x_signed[stable_mask] ** 2)))
    else:
        signal_rms = float(np.sqrt(np.mean(x_signed ** 2)))

    # Peak amplitude — median height of R-peak above surrounding baseline
    if peaks_stable.size:
        w = int(round(0.100 * rec.fs))
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

    stable_frac = float(stable_mask.mean()) if stable_mask.size else 0.0

    return AnalysisResult(
        rec=rec,
        ecg_filt=x_signed,
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
        noise_rms_mv=noise_rms,
        signal_rms_mv=signal_rms,
        peak_amp_median_mv=peak_amp,
        snr_db=snr_db,
        stable_frac=stable_frac,
    )


def analyze_folder(folder, min_rows: int = 200) -> dict:
    folder = Path(folder)
    out = {}
    for p in sorted(folder.glob("LOG_*.CSV")):
        try:
            rec = Recording.from_csv(p)
        except Exception as e:
            print(f"{p.name}: LOAD FAIL — {e}")
            continue
        if len(rec.ecg_mv) < min_rows:
            print(f"{p.name}: too short ({len(rec.ecg_mv)} rows) — skipping")
            continue
        try:
            out[p] = rec.analyze()
        except Exception as e:
            print(f"{p.name}: ANALYZE FAIL — {e}")
    return out


def summarize(results: dict) -> pd.DataFrame:
    rows = [r.summary_dict() for r in results.values()]
    df = pd.DataFrame(rows)
    return df.sort_values("file").reset_index(drop=True)
