"""
Kompic Mk I — Reflow Profile Plot
Derived from: Reflow & Assembly Reference iv7.0
Binding constraints: MAX-M10S, WS2812B-2020, FC31M2 crystal
"""

import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# ── Toggle switches ────────────────────────────────────────────────────────────
SHOW_ELBOW_VERTICALS    = True
SHOW_ZONE_FILLS         = True
SHOW_REFERENCE_LINES    = True
SHOW_REFERENCE_LABELS   = True
SHOW_PHASE_LABELS       = True
SHOW_SUBTITLE           = True

# ── Palette — print/PDF (white pages) ─────────────────────────────────────────
BG          = "white"
BORDER      = "#dddddd"
TEXT_MUT    = "#444444"
CURVE       = "#c87f2a"

# Zone fill colors
FILL_SOAK   = "#f5ddb0"   # pastel orange — soak zone
FILL_PEAK   = "#f5b0b0"   # pastel red saturated — peak trapezoid
FILL_RAMP2  = "#f5c8c8"   # pastel red light — 2nd ramp + descent
FILL_COOL   = "#b8d8f0"   # pastel blue — post-217 cooling

# Reference lines
COL_217     = "#2a7a52"
COL_245     = "#b03030"

# ── Profile keypoints ──────────────────────────────────────────────────────────
t = [  0,   75,  180,  218,  248,  263,  278,  358]
T = [ 25,  150,  200,  217,  245,  245,  217,   25]

# ── Elbow verticals: one per phase transition ──────────────────────────────────
# t=75 end-of-ramp1, t=180 end-of-soak, t=218 cross-up (start ramp2),
# t=248 start-peak, t=263 end-peak, t=278 cross-down
elbow_xs = [75, 180, 218, 248, 263, 278]
elbow_T  = dict(zip(t, T))

# ── Figure ─────────────────────────────────────────────────────────────────────
fig, ax = plt.subplots(figsize=(8, 5))
fig.patch.set_facecolor(BG)
ax.set_facecolor(BG)
ax.set_xlim(-8, 390)
ax.set_ylim(0, 290)

# ── Background grid ────────────────────────────────────────────────────────────
ax.grid(axis="y", which="major", color=BORDER, linewidth=0.5, zorder=0)
ax.grid(axis="x", which="major", color=BORDER, linewidth=0.5, zorder=0)

# ── Zone fills ────────────────────────────────────────────────────────────────
if SHOW_ZONE_FILLS:
    ta = np.array(t)
    Ta = np.array(T)

    def zone_fill(x0, x1, color, alpha=0.45, zorder=1):
        mask = (ta >= x0) & (ta <= x1)
        # include boundary points via interpolation
        xs = np.linspace(x0, x1, 300)
        ys = np.interp(xs, ta, Ta)
        ax.fill_between(xs, 0, ys, color=color, alpha=alpha, zorder=zorder)

    # Soak zone: t=75 → 180
    zone_fill(75, 180, FILL_SOAK, alpha=0.5)

    # 2nd ramp (light red): t=180 → 248
    zone_fill(180, 248, FILL_RAMP2, alpha=0.4)

    # Peak (more saturated red): t=248 → 263
    zone_fill(248, 263, FILL_PEAK, alpha=0.55)

    # Descent to 217 (light red): t=263 → 278
    zone_fill(263, 278, FILL_RAMP2, alpha=0.4)

    # Cooling (blue): t=278 → 358
    zone_fill(278, 358, FILL_COOL, alpha=0.4)

# ── Vertical elbow guides — bottom to profile line ─────────────────────────────
if SHOW_ELBOW_VERTICALS:
    for ex in elbow_xs:
        ey = np.interp(ex, t, T)
        ax.plot([ex, ex], [0, ey], color="#bbbbbb", linewidth=0.8,
                linestyle=(0, (4, 3)), zorder=2)

# ── Reference dashed lines ─────────────────────────────────────────────────────
if SHOW_REFERENCE_LINES:
    ax.axhline(217, color=COL_217, linewidth=1.0, linestyle=(0, (5, 4)), zorder=3, alpha=0.9)
    ax.axhline(245, color=COL_245, linewidth=1.0, linestyle=(0, (5, 4)), zorder=3, alpha=0.9)

# ── Profile curve ──────────────────────────────────────────────────────────────
ax.plot(t, T, color=CURVE, linewidth=3.6, solid_capstyle="round",
        solid_joinstyle="round", zorder=4)

# ── Reference labels ──────────────────────────────────────────────────────────
if SHOW_REFERENCE_LABELS:
    ax.text(382, 220, "217°C", va="bottom", ha="right", fontsize=8.5,
            color=COL_217, fontweight="bold")
    ax.text(382, 248, "245°C", va="bottom", ha="right", fontsize=8.5,
            color=COL_245, fontweight="bold")

# ── Phase labels ───────────────────────────────────────────────────────────────
if SHOW_PHASE_LABELS:
    lkw = dict(fontsize=8.5, color="#555555", fontweight="bold",
               bbox=dict(boxstyle="round,pad=0.2", fc="white", ec="none", alpha=0.85))

    ax.text(37, 125, "Ramp\n1–3 °C/s", ha="center", va="bottom", **lkw)

    ax.text(127, 125, "Soak zone\n150–200 °C · 60–120 s", ha="center", va="bottom", **lkw)

    ax.text(190, 230, "2–3 °C/s", ha="left", va="bottom", **lkw)

    ax.text(255, 250, "Peak · 10–20 s", ha="center", va="bottom", fontsize=9,
            color="#b03030", fontweight="bold",
            bbox=dict(boxstyle="round,pad=0.3", fc="white", ec="none", alpha=0.9))

    ax.text(190, 190, "45–60 s above 217 °C", ha="left", va="bottom",
            fontsize=8.5, color="#1a5fa8", style="italic", fontweight="bold",
            bbox=dict(boxstyle="round,pad=0.2", fc="white", ec="none", alpha=0.85))

    ax.text(350, 155, "≤ 4 °C/s", ha="right", va="center", **lkw)

    ax.text(363, 35, "< 50 °C\nsafe to handle", ha="left", va="bottom",
            fontsize=8.5, color="#555555", fontweight="bold")

# ── Axes ───────────────────────────────────────────────────────────────────────
ax.set_xlabel("Time (s)", fontsize=9.5, color=TEXT_MUT, labelpad=6, fontweight="bold")
ax.set_ylabel("Temperature (°C)", fontsize=9.5, color=TEXT_MUT, labelpad=6, fontweight="bold")

ax.xaxis.set_major_locator(ticker.MultipleLocator(60))
ax.xaxis.set_minor_locator(ticker.MultipleLocator(30))
ax.yaxis.set_major_locator(ticker.MultipleLocator(50))
ax.yaxis.set_minor_locator(ticker.MultipleLocator(25))

ax.tick_params(axis="both", which="major", labelsize=8.5, color="#aaaaaa",
               labelcolor=TEXT_MUT, length=6, width=1.0)
ax.tick_params(axis="both", which="minor", color="#cccccc", length=3, width=0.7)

for label in ax.get_xticklabels() + ax.get_yticklabels():
    label.set_fontweight("bold")

for spine in ax.spines.values():
    spine.set_edgecolor(BORDER)
    spine.set_linewidth(0.8)

# ── Title + subtitle ───────────────────────────────────────────────────────────
ax.set_title("Kompic Mk I — Lead-free reflow profile",
             fontsize=11, color="#c87f2a", pad=18, loc="left", fontweight="bold")

if SHOW_SUBTITLE:
    ax.text(0, 1.02,
            "Constraints: MAX-M10S · WS2812B-2020 · FC31M2 crystal",
            transform=ax.transAxes, fontsize=7.5, color="#999999",
            va="bottom", fontweight="bold")

plt.tight_layout()

out = "hardware/Reflow_info/reflow_profile.png"
plt.savefig(out, dpi=180, bbox_inches="tight", facecolor=BG)
print(f"Saved → {out}")