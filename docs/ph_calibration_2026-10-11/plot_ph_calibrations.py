"""The pH probe's two-point calibrations on one plot: board voltage against buffer pH.

    python plot_ph_calibrations.py  ->  ph_calibrations.png / .pdf

Points are the buffer captures (board volts, after the 10k/20k divider gain of 1.5).
Buffer pH is the sachet value at the buffer temperature where it was recorded.
The slope in mV per pH shows the response falling from 16 Sep to 10 Oct; the
11 Oct line is the new probe on signal board 1, checked in a 6.86 buffer.
"""
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import report_style  # noqa: E402

report_style.apply()

# (label, colour, marker, [(volts, buffer pH)])
CALS = [
    ("16 Sep, probe 1, signal board 2", report_style.BLUE, "o",
     [(1.819812, 4.00), (2.15684, 9.18)]),
    ("10 Oct, probe 1, signal board 1", report_style.ORANGE, "s",
     [(1.6069, 4.00), (1.7273, 9.203)]),
    ("11 Oct, probe 2, signal board 1", report_style.GREEN, "^",
     [(1.797626, 4.00), (1.962312, 9.200)]),
]
CHECK = (1.8838, 6.87)  # 6.86 buffer at 22.6 C, read as pH 6.72 on the 11 Oct line

fig, ax = plt.subplots(figsize=(5.6, 3.6))
v = np.linspace(1.55, 2.25, 50)
for label, color, marker, pts in CALS:
    (va, pa), (vb, pb) = pts
    slope = (pb - pa) / (vb - va)          # pH per V
    ax.plot(v, pa + slope * (v - va), color=color, lw=1.0, ls="--", alpha=0.7)
    ax.plot([va, vb], [pa, pb], marker=marker, ls="none", color=color, ms=6,
            label=f"{label}: {1000 / slope:.0f} mV/pH")
ax.plot(*CHECK, marker="x", ls="none", color="black", ms=8, mew=1.5,
        label="11 Oct check, 6.86 buffer (read 6.72)")
ax.axhspan(0, 14, color="none")
ax.set(xlim=(1.55, 2.25), ylim=(0, 14), xlabel="Board voltage (V)", ylabel="Buffer pH")
ax.legend(loc="upper left", fontsize=8)
for ext in ("png", "pdf"):
    fig.savefig(Path(__file__).with_name(f"ph_calibrations.{ext}"))
print("saved")
