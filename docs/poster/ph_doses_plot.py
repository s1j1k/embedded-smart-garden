"""Poster figure: pH-up doses decided by the controller in the 7 Oct pH-loop run (1.9.12)."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parents[1]))
import report_style  # noqa: E402  LaTeX fonts, MATLAB axes

TEAL, CORAL = "#0b5d7a", "#e4572e"
report_style.apply()
doses = [(0.0, 3.16), (20.1, 2.02)]   # min after the step (21:12:41), mL delivered
fig, ax = plt.subplots(figsize=(9, 3.2))
ax.vlines([m for m, _ in doses], 0, [v for _, v in doses], color=CORAL, lw=5)
ax.scatter([m for m, _ in doses], [v for _, v in doses], color=CORAL, s=70, zorder=3, label="pH-up")
for m, v in doses:
    ax.annotate(f"{v:.1f} mL", (m, v), xytext=(6, 2), textcoords="offset points", fontsize=11, color=CORAL)
ax.axvline(37.7, color="k", lw=1.4)
ax.annotate("back in range, no dose", (37.7, 3.3), xytext=(-6, 0), textcoords="offset points",
            ha="right", fontsize=11)
ax.set_xlim(-2, 41)
ax.set_ylim(0, 3.6)
ax.set_ylabel("dose (mL)")
ax.set_xlabel("minutes after the step (first pH-up dose)")
ax.set_title("pH-up doses decided by the controller", loc="left", color=TEAL, fontweight="bold")
ax.legend(frameon=False, fontsize=10, loc="upper center")
fig.tight_layout()
for ext in ("png", "pdf"):
    fig.savefig(f"ph_doses_1.9.12.{ext}", dpi=220)
