"""
ecg_plots.py — matplotlib helpers for AnalysisResult
v1.0 | 2026-08-18
"""

from __future__ import annotations
import numpy as np
import matplotlib.pyplot as plt

from ecg_analysis import AnalysisResult


def plot_overview(res: AnalysisResult, ax=None, title: str | None = None):
    """Full recording, filtered ECG, stable regions shaded, peaks marked."""
    if ax is None:
        _, ax = plt.subplots(figsize=(14, 3.2))
    t = res.rec.t
    x = res.ecg_filt
    ax.plot(t, x, lw=0.5, color="#1F77B4")
    # Shade stable segments
    for a, b in res.stable_segments:
        ax.axvspan(t[a], t[min(b - 1, len(t) - 1)], color="#88CC88", alpha=0.20, lw=0)
    # Peaks (stable vs discarded)
    if len(res.peaks_stable):
        ax.plot(t[res.peaks_stable], x[res.peaks_stable], "v",
                color="#D62728", ms=6, label=f"stable peaks (n={len(res.peaks_stable)})")
    discarded = np.setdiff1d(res.peaks, res.peaks_stable, assume_unique=False)
    if len(discarded):
        ax.plot(t[discarded], x[discarded], "x",
                color="#888888", ms=6, label=f"discarded (n={len(discarded)})")
    ax.set_xlabel("t (s)")
    ax.set_ylabel("ECG (mV, filtered, oriented)")
    ax.set_title(title or res.rec.path.name)
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(alpha=0.2)
    return ax


def plot_stable_close(res: AnalysisResult, seg_idx: int = 0,
                      max_s: float = 12.0, ax=None):
    """Zoom into one stable segment for close inspection."""
    if ax is None:
        _, ax = plt.subplots(figsize=(14, 3.2))
    if not res.stable_segments:
        ax.text(0.5, 0.5, "no stable segments", ha="center", transform=ax.transAxes)
        return ax
    a, b = res.stable_segments[seg_idx]
    fs = res.rec.fs
    b = min(b, a + int(max_s * fs))
    t = res.rec.t[a:b]
    x = res.ecg_filt[a:b]
    ax.plot(t, x, lw=0.6, color="#1F77B4")
    pk = res.peaks_stable[(res.peaks_stable >= a) & (res.peaks_stable < b)]
    if len(pk):
        ax.plot(res.rec.t[pk], res.ecg_filt[pk], "v", color="#D62728", ms=6)
    # Noise-floor band
    if np.isfinite(res.noise_rms_mv):
        ax.axhspan(-res.noise_rms_mv, res.noise_rms_mv,
                   color="#AAAAAA", alpha=0.25, lw=0,
                   label=f"noise RMS ±{res.noise_rms_mv:.0f} mV")
    ax.set_xlabel("t (s)")
    ax.set_ylabel("ECG (mV)")
    ax.set_title(f"{res.rec.path.name} — stable segment {seg_idx}")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(alpha=0.2)
    return ax


def plot_rr_histogram(res: AnalysisResult, ax=None):
    if ax is None:
        _, ax = plt.subplots(figsize=(6, 3.2))
    if res.rr_ms.size == 0:
        ax.text(0.5, 0.5, "no RR intervals", ha="center", transform=ax.transAxes)
        return ax
    rr_phys = res.rr_ms[(res.rr_ms > 350) & (res.rr_ms < 2000)]
    ax.hist(rr_phys, bins=30, color="#1F77B4", alpha=0.8)
    ax.axvline(np.median(rr_phys), color="#D62728", ls="--",
               label=f"median {np.median(rr_phys):.0f} ms → {60000/np.median(rr_phys):.1f} BPM")
    ax.set_xlabel("RR interval (ms)")
    ax.set_ylabel("count")
    ax.set_title(f"{res.rec.path.name} — RR distribution")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.2)
    return ax


def plot_recording_full(res: AnalysisResult):
    """Convenience: overview + first stable close-up + RR hist."""
    fig, axs = plt.subplots(3, 1, figsize=(14, 8),
                            gridspec_kw={"height_ratios": [2, 2, 1.4]})
    plot_overview(res, ax=axs[0])
    plot_stable_close(res, seg_idx=0, max_s=10.0, ax=axs[1])
    plot_rr_histogram(res, ax=axs[2])
    fig.tight_layout()
    return fig
