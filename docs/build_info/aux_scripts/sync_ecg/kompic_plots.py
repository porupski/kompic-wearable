"""
kompic_plots.py -- matplotlib helpers for Kompic PPG+BCG analysis results.
v1.0 | 2026-08-26

The helpers work on a `_AnalysisResultBase` from `kompic_analysis`. Each helper
takes an optional `ax`; the caller may pass a shared axes for layout control.

The two modalities plot with different colour palettes so a shared figure keeps
them visually separate:

- PPG uses green (matches the LED wavelength).
- BCG uses steel blue.

Firmware `beat` markers plot as translucent triangles below the trace so the
comparison against the offline detector stays visible without overlap.
"""

from __future__ import annotations

import numpy as np
import matplotlib.pyplot as plt

from kompic_analysis import _AnalysisResultBase, PpgResult, BcgResult

# ---------------------------------------------------------------------------
# Colour palettes -- one per modality.
# ---------------------------------------------------------------------------

_PALETTE = {
    "ppg": {
        "signal": "#2CA02C",
        "peak":   "#D62728",
        "fw":     "#88CC88",
        "stable": "#B4E7B4",
    },
    "bcg": {
        "signal": "#1F77B4",
        "peak":   "#D62728",
        "fw":     "#9BB3D9",
        "stable": "#C6D8ED",
    },
}


def _pal(res: _AnalysisResultBase) -> dict:
    return _PALETTE[res.stream.src]


# ---------------------------------------------------------------------------
# Single-trace overview
# ---------------------------------------------------------------------------

def plot_overview(res: _AnalysisResultBase, ax=None, title: str | None = None):
    """Full-recording view. Filtered signal, stable regions shaded, peaks marked.

    Firmware beats appear as small down-triangles at the bottom of the axes.
    Offline detector peaks appear as red down-triangles on the trace.
    """
    if ax is None:
        _, ax = plt.subplots(figsize=(14, 3.2))
    pal = _pal(res)
    t   = res.stream.t
    x   = res.signal_bp

    ax.plot(t, x, lw=0.5, color=pal["signal"])

    for a, b in res.stable_segments:
        end = min(b - 1, len(t) - 1)
        ax.axvspan(t[a], t[end], color=pal["stable"], alpha=0.30, lw=0)

    if res.peaks_stable.size:
        ax.plot(t[res.peaks_stable], x[res.peaks_stable], "v",
                color=pal["peak"], ms=6,
                label=f"offline peaks stable (n={len(res.peaks_stable)})")

    discarded = np.setdiff1d(res.peaks, res.peaks_stable, assume_unique=False)
    if discarded.size:
        ax.plot(t[discarded], x[discarded], "x", color="#888888", ms=6,
                label=f"offline peaks discarded (n={discarded.size})")

    # Firmware beat markers -- placed at the bottom of the axes so they do not
    # overlap the trace. Only stable-region beats are marked; the rest are
    # visible in the number under `n_fw_beats` in the summary table.
    fw = np.where(res.stream.beat == 1)[0]
    if fw.size:
        y_bottom = np.full(fw.size, x.min() - 0.1 * (x.max() - x.min()))
        ax.plot(t[fw], y_bottom, "v", color=pal["fw"], ms=4, alpha=0.7,
                label=f"firmware beats (n={fw.size})")

    ax.set_xlabel("t (s)")
    ax.set_ylabel(f"{res.stream.src.upper()} (bandpassed, oriented)")
    ax.set_title(title or f"{res.stream.src.upper()} overview")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(alpha=0.2)
    return ax


# ---------------------------------------------------------------------------
# Zoom into one stable segment
# ---------------------------------------------------------------------------

def plot_stable_close(res: _AnalysisResultBase, seg_idx: int = 0,
                      max_s: float = 12.0, ax=None):
    if ax is None:
        _, ax = plt.subplots(figsize=(14, 3.2))
    pal = _pal(res)

    if not res.stable_segments:
        ax.text(0.5, 0.5, "no stable segments", ha="center",
                transform=ax.transAxes)
        return ax

    a, b = res.stable_segments[seg_idx]
    fs   = res.stream.fs
    b    = min(b, a + int(max_s * fs))
    t    = res.stream.t[a:b]
    x    = res.signal_bp[a:b]

    ax.plot(t, x, lw=0.6, color=pal["signal"])
    pk = res.peaks_stable[(res.peaks_stable >= a) & (res.peaks_stable < b)]
    if pk.size:
        ax.plot(res.stream.t[pk], res.signal_bp[pk], "v",
                color=pal["peak"], ms=6)

    if np.isfinite(res.noise_rms):
        ax.axhspan(-res.noise_rms, res.noise_rms,
                   color="#AAAAAA", alpha=0.25, lw=0,
                   label=f"noise RMS +/-{res.noise_rms:.3f}")

    ax.set_xlabel("t (s)")
    ax.set_ylabel(res.stream.src.upper())
    ax.set_title(f"{res.stream.src.upper()} stable segment {seg_idx}")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(alpha=0.2)
    return ax


# ---------------------------------------------------------------------------
# RR histogram
# ---------------------------------------------------------------------------

def plot_rr_histogram(res: _AnalysisResultBase, ax=None):
    if ax is None:
        _, ax = plt.subplots(figsize=(6, 3.2))
    pal = _pal(res)

    if res.rr_ms.size == 0:
        ax.text(0.5, 0.5, "no RR intervals", ha="center",
                transform=ax.transAxes)
        return ax

    rr_phys = res.rr_ms[(res.rr_ms > 350) & (res.rr_ms < 2000)]
    ax.hist(rr_phys, bins=30, color=pal["signal"], alpha=0.8)

    med = float(np.median(rr_phys))
    ax.axvline(med, color=pal["peak"], ls="--",
               label=f"median {med:.0f} ms -> {60000 / med:.1f} BPM")
    ax.set_xlabel("RR interval (ms)")
    ax.set_ylabel("count")
    ax.set_title(f"{res.stream.src.upper()} RR distribution")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.2)
    return ax


# ---------------------------------------------------------------------------
# One-shot triple layout
# ---------------------------------------------------------------------------

def plot_stream_full(res: _AnalysisResultBase):
    """Overview, first stable close-up, and RR histogram in one figure."""
    fig, axs = plt.subplots(3, 1, figsize=(14, 8),
                            gridspec_kw={"height_ratios": [2, 2, 1.4]})
    plot_overview(res, ax=axs[0])
    plot_stable_close(res, seg_idx=0, max_s=10.0, ax=axs[1])
    plot_rr_histogram(res, ax=axs[2])
    fig.tight_layout()
    return fig


# ---------------------------------------------------------------------------
# PPG + BCG side-by-side overview
# ---------------------------------------------------------------------------

def plot_combined_overview(ppg: PpgResult | None, bcg: BcgResult | None):
    """Stack PPG and BCG overviews so the analyst compares the two modalities.

    Pass None for either stream to skip that panel.
    """
    n = int(ppg is not None) + int(bcg is not None)
    if n == 0:
        raise ValueError("no results to plot")
    fig, axs = plt.subplots(n, 1, figsize=(14, 3.2 * n), squeeze=False)
    row = 0
    if ppg is not None:
        plot_overview(ppg, ax=axs[row][0], title="PPG (MAX30101 raw green)")
        row += 1
    if bcg is not None:
        plot_overview(bcg, ax=axs[row][0], title="BCG (LSM6DSV16X accel Z)")
    fig.tight_layout()
    return fig
