"""Poster figure: why the plants are behind the calendar.

One bar per crop for the days from sowing (3 Sep) to 19 Oct: the measured days
in the tank split into growth (each day counted by how well its temperature and
light suited the crop) and what was lost to warm air and light outages; then the
days that could not be measured. Numbers from scripts/trial_development.py on
the Pi's full record (dashboard repo).

    python why_growth_figure.py TRIAL_DEV.csv  ->  why_growth.png
"""
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

HERE = Path(__file__).resolve().parent
d = pd.read_csv(sys.argv[1], parse_dates=["day"])
rows = []
for crop in ("basil", "lettuce"):
    g = d[d.crop == crop]
    m = g[g.status == "measured"]
    rows.append({
        "crop": crop.title(),
        "Growth in the tank": m.rate.sum(),
        "Lost: air too warm": m.lost_heat.sum() + m.lost_cold.sum(),
        "Lost: light outages": m.lost_light.sum(),
        "Not measured: windowsill and rebuilds": (g.status == "break").sum(),
        "Not measured: no data": (g.status == "no data").sum(),
    })
t = pd.DataFrame(rows).set_index("crop")
colours = ["#2e9d4a", "#e4572e", "#f2b632", "#9aa5b1", "#d5dbe1"]

fig, ax = plt.subplots(figsize=(8, 2.9), dpi=250)
left = pd.Series(0.0, index=t.index)
for column, colour in zip(t.columns, colours):
    ax.barh(t.index, t[column], left=left, color=colour, edgecolor="white", height=0.55, label=column)
    for crop, value in t[column].items():
        if value >= 1.5:
            ax.text(left[crop] + value / 2, crop, f"{value:.0f}" if value >= 5 else f"{value:.1f}",
                    ha="center", va="center", fontsize=11, weight="bold",
                    color="white" if colour in ("#2e9d4a", "#e4572e", "#9aa5b1") else "black")
    left += t[column]
ax.axvline(42, color="black", lw=1.5, ls="--")
ax.text(41.7, -0.45, "calendar harvest (day 42)", fontsize=9, va="center", ha="right")
ax.set_xlim(0, 48)
ax.set_ylim(1.5, -0.75)
ax.set_xlabel("days since sowing, 3 Sep to 19 Oct")
ax.tick_params(axis="y", labelsize=12)
ax.spines[["top", "right"]].set_visible(False)
ax.legend(ncol=2, fontsize=8.5, frameon=False, loc="upper center", bbox_to_anchor=(0.45, -0.32))
ax.set_title("Why the plants are behind: how each day since sowing counted", fontsize=12, loc="left")
fig.tight_layout()
fig.savefig(HERE / "why_growth.png", bbox_inches="tight", pad_inches=0.05)
print(t.round(1).to_string())
