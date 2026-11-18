"""Report figures: the two random forests and the trained harvest forest.

Refits the harvest regression exactly as the dashboard does (src/harvest.py in
the dashboard repo: RandomForestRegressor, 200 trees, min_samples_leaf=2,
random_state=0, scikit-learn 1.9.1 as on the Pi) on the clean window behind the
poster's result: the 10 day-to-day growth pairs from 8 to 16 Oct. The training
table and the daily conditions were exported on the Pi from the full record
(export_rf_table.py, breaks for the windowsill 3-25 Sep and the plain-water
bring-ups 6-8 Oct).

    python rf_figures.py [DASHBOARD_REPO]

writes, next to this file:
    rf_pipeline.png/.pdf   both forests: inputs, bootstrap, trees, aggregation
    rf_trees.png/.pdf      three of the 200 trained trees, drawn node by node
    rf_forest.png/.pdf     the whole forest: structure, importances, tree votes
"""

from __future__ import annotations

import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
DASHBOARD = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parents[2] / "smart-garden-iot-dashboard"
sys.path.insert(0, str(DASHBOARD / "src"))

import report_style as rs  # noqa: E402
import harvest  # noqa: E402  (the dashboard's own module: same features and forecast)

from sklearn.ensemble import RandomForestRegressor  # noqa: E402

rs.apply(9)
CLEAN_END = pd.Timestamp("2026-10-16")      # last day with a clean, flash-lit photo
POTS = ["basil", "lettuce"]
SHORT = {"log_area": "Plant size", "is_lettuce": "Crop", "f_temp": "Temp. fit",
         "temp_c": "Temperature", "light_hours": "Light hours",
         "tds_in_band": "Nutrient in range", "ph_in_band": "pH in range"}


def save(fig, name: str) -> None:
    for ext in ("png", "pdf"):
        fig.savefig(HERE / f"{name}.{ext}", dpi=300, bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)


# ---------------------------------------------------------------- data + model
sizes = pd.read_csv(HERE / "rf_sizes.csv", parse_dates=["day"])
sizes = sizes[sizes["day"] <= CLEAN_END]
conditions = {}
for crop in POTS:
    daily = pd.read_csv(HERE / f"rf_daily_{crop}.csv", index_col=0, parse_dates=True)
    conditions[crop] = daily[daily.index <= CLEAN_END]
table = harvest.training_table(sizes, conditions, POTS)
fill = table[harvest.FEATURES].median()
x = table[harvest.FEATURES].fillna(fill)
y = table["rgr"].to_numpy()
model = RandomForestRegressor(n_estimators=harvest.TREES, min_samples_leaf=2, random_state=0,
                              n_jobs=1).fit(x, y)
result = harvest.forecast(sizes, conditions, POTS, CLEAN_END.to_pydatetime())
print(f"{len(table)} pairs; importances:",
      {SHORT[k]: round(v, 3) for k, v in result["importance"].items()})
for pot, r in result["pots"].items():
    print(pot, r["crop"], round(r["area_cm2"], 2), "growth now %.1f %%/day" % r["rgr_pct"],
          "date", r["date"], "earliest", r["earliest"], "latest", r["latest"])


def pct_day(rgr: float) -> float:
    return 100 * (np.exp(rgr) - 1)


# ------------------------------------------------------------- 1. pipeline
def arrow(ax, a, b):
    ax.add_patch(FancyArrowPatch(a, b, arrowstyle="-|>", mutation_scale=9, lw=0.9, color="0.25"))


def mini_tree(ax, cx, top, scale, color):
    """A small generic tree icon: root, two children, four leaves."""
    kids = [(cx - scale, top - scale), (cx + scale, top - scale)]
    for px, py in kids:
        ax.plot([cx, px], [top, py], color="0.4", lw=0.6, zorder=1)
        for dx in (-0.45, 0.45):
            ax.plot([px, px + dx * scale], [py, py - scale * 0.9], color="0.4", lw=0.6, zorder=1)
            ax.scatter(px + dx * scale, py - scale * 0.9, s=6, color=rs.GREEN, zorder=2)
    for px, py in [(cx, top), *kids]:
        ax.scatter(px, py, s=8, color=color, zorder=2)


def pipeline() -> None:
    fig, ax = plt.subplots(figsize=(7.4, 4.6))
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1.1)
    ax.axis("off")
    ax.grid(False)
    rows = [
        (0.55, rs.BLUE, r"Harvest date: regression (harvest.py)",
         ["1 row per pot per pair of days:", "10 pairs, 8 to 16 Oct", "",
          r"Inputs (7): plant size $\ln A$,", "crop, temperature fit $f_T$,",
          "temperature, light hours,", "nutrient and pH in range", "",
          r"Target: growth $\ln(A_{d+1}/A_d)$"],
         ["200 trees", "Each on a bootstrap", "sample of the rows", "",
          "All 7 inputs tried", "at every split", "At least 2 rows", "per leaf"],
         ["Each tree predicts", "tomorrow's growth", "", "Median of the 200:", "the forecast", "",
          "20th and 80th", "percentiles: the range"],
         ["Grow day by day to", "the harvest size", r"(basil 90 cm$^2$,", r"lettuce 100 cm$^2$)", "",
          "No date while the", r"median growth is $\leq 0$"]),
        (0.04, rs.ORANGE, r"Growth stage: classifier (plant$\_$growth.py)",
         ["1 row per hour of photos", "", "Inputs (7): leaf area, height,", "width, leaf count,",
          "growth per day, water", "and nutrient uptake", "",
          "Label: the stage the", "controller ran"],
         ["100 trees", "Each on a bootstrap", "sample of the rows", "",
          r"$\sqrt{7}\approx 2$ inputs tried", "at each split", "At least 3 rows per",
          "leaf; stages weighted", "equally"],
         ["Each tree votes", "for a stage", "", "Majority: the", "suggested stage", "",
          "Share of votes:", "the confidence"],
         ["Scored one held-out", "day at a time, against", "the age schedule", "",
          "Advisory: the user", "applies the stage", "",
          "Not trained yet: the", "trial ran one stage"]),
    ]
    cols = [(0.0, 0.25), (0.275, 0.22), (0.52, 0.215), (0.76, 0.24)]
    heads = ["Training data", "Forest", "Aggregate", "Output"]
    h = 0.41
    for (cx, cw), head in zip(cols, heads):
        ax.text(cx + cw / 2, 1.1, head, ha="center", va="top", fontsize=9, color="0.3",
                style="italic")
    for y0, color, title, *cells in rows:
        ax.text(0.0, y0 + h + 0.012, title, fontsize=9.5, color=color, va="bottom")
        for i, ((cx, cw), lines) in enumerate(zip(cols, cells)):
            ax.add_patch(FancyBboxPatch((cx, y0), cw, h, boxstyle="round,pad=0.006,rounding_size=0.012",
                                        fc=color + "14", ec=color, lw=0.8))
            ax.text(cx + 0.012, y0 + h - 0.018, "\n".join(lines), ha="left", va="top",
                    fontsize=7.3, linespacing=1.3)
            if i == 1:
                for k, tx in enumerate((cx + 0.16, cx + 0.195)):
                    mini_tree(ax, tx, y0 + 0.07 - 0.015 * k, 0.014, color)
            if i < 3:
                arrow(ax, (cx + cw + 0.004, y0 + h / 2), (cols[i + 1][0] - 0.004, y0 + h / 2))
    save(fig, "rf_pipeline")


# ------------------------------------------------------------- 2. three trees
def describe_split(feature: str, threshold: float) -> str:
    if feature == "log_area":
        return rf"Plant size $\leq$ {np.exp(threshold):.1f} cm$^2$"
    if feature == "is_lettuce":
        return "Crop: basil"
    if feature in ("tds_in_band", "ph_in_band"):
        return rf"{SHORT[feature]} $\leq$ {100 * threshold:.0f} % of day"
    if feature == "temp_c":
        return rf"Temperature $\leq$ {threshold:.1f} $^\circ$C"
    if feature == "light_hours":
        return rf"Light $\leq$ {threshold:.1f} h"
    if feature == "f_temp":
        return rf"Temp. fit $f_T \leq$ {threshold:.2f}"
    return f"{feature} <= {threshold:.2f}"


def layout(tree, node=0, depth=0, x=[0]):
    """In-order x positions (leaves spaced evenly), depth as y."""
    left, right = tree.children_left[node], tree.children_right[node]
    if left == -1:
        pos = {node: (x[0], depth)}
        x[0] += 1
        return pos
    pos = layout(tree, left, depth + 1, x)
    pos.update(layout(tree, right, depth + 1, x))
    pos[node] = ((pos[left][0] + pos[right][0]) / 2, depth)
    return pos


def draw_tree(ax, estimator, title):
    tree = estimator.tree_
    pos = layout(tree, x=[0])
    names = harvest.FEATURES
    width = max(p[0] for p in pos.values()) + 1
    depth = max(p[1] for p in pos.values())
    vmax = 60
    cmap = plt.get_cmap("RdYlGn")
    for node, (px, py) in pos.items():
        left, right = tree.children_left[node], tree.children_right[node]
        for child, label in ((left, "yes"), (right, "no")):
            if child == -1:
                continue
            cx, cy = pos[child]
            ax.plot([px, cx], [-py, -cy], color="0.45", lw=0.7, zorder=1)
            ax.text((px + cx) / 2 + (-0.06 if label == "yes" else 0.06), -(py + cy) / 2, label,
                    fontsize=6.3, color="0.35", ha="right" if label == "yes" else "left",
                    va="center")
    for node, (px, py) in pos.items():
        n = int(tree.weighted_n_node_samples[node])
        value = pct_day(float(tree.value[node].ravel()[0]))
        if tree.children_left[node] == -1:
            colour = cmap(0.5 + np.clip(value, -vmax, vmax) / (2 * vmax))
            text = f"{value:+.0f} %/day\n{n} rows"
            ax.text(px, -py, text, ha="center", va="center", fontsize=6.6, zorder=3,
                    bbox=dict(boxstyle="round,pad=0.3", fc=colour, ec="0.3", lw=0.5))
        else:
            text = describe_split(names[tree.feature[node]], tree.threshold[node])
            ax.text(px, -py, f"{text}\n{n} rows, mean {value:+.0f} %/day", ha="center",
                    va="center", fontsize=6.6, zorder=3,
                    bbox=dict(boxstyle="round,pad=0.3", fc="white", ec=rs.BLUE, lw=0.7))
    ax.set_xlim(-0.8, width - 0.2)
    ax.set_ylim(-depth - 0.5, 0.5)
    ax.axis("off")
    ax.set_title(title, fontsize=8.5)


def trees() -> None:
    roots = [harvest.FEATURES[e.tree_.feature[0]] for e in model.estimators_]
    picked, seen = [], set()
    for i, root in enumerate(roots):          # the first tree with each of the 3 commonest roots
        if root not in seen and root in pd.Series(roots).value_counts().index[:3]:
            picked.append(i)
            seen.add(root)
        if len(picked) == 3:
            break
    fig, axes = plt.subplots(2, 2, figsize=(7.4, 5.6))
    axes = axes.ravel()
    for ax, i in zip(axes, picked):
        e = model.estimators_[i]
        draw_tree(ax, e, f"Tree {i + 1} of {len(model.estimators_)} (depth {e.get_depth()}, "
                         f"{e.get_n_leaves()} leaves)")
    ax = axes[3]
    ax.axis("off")
    ax.text(0.04, 0.85, "\n".join([
        f"... and {len(model.estimators_) - 3} more trees, each grown on its own",
        "bootstrap sample of the 10 rows.",
        "",
        "A split sends a row left (yes) or right (no).",
        "Each leaf holds the mean growth of the rows",
        "that reach it; rows counts bootstrap draws.",
        "",
        "For a new day, every tree follows its splits",
        "to one leaf. The forest's forecast is the",
        "median of the 200 leaf values, and the 20th",
        "and 80th percentiles give its range.",
    ]), transform=ax.transAxes, va="top", fontsize=8, linespacing=1.4)
    save(fig, "rf_trees")


# ------------------------------------------------------------- 3. the forest
def forest() -> None:
    est = model.estimators_
    depths = np.array([e.get_depth() for e in est])
    leaves = np.array([e.get_n_leaves() for e in est])
    roots = pd.Series([SHORT[harvest.FEATURES[e.tree_.feature[0]]] for e in est]).value_counts()
    per_tree = pd.DataFrame([e.feature_importances_ for e in est], columns=harvest.FEATURES)
    order = per_tree.mean().sort_values().index

    fig = plt.figure(figsize=(7.4, 5.4))
    grid = fig.add_gridspec(2, 3, height_ratios=[1, 1.15], hspace=0.55, wspace=0.85)

    ax = fig.add_subplot(grid[0, 0])
    bins = np.arange(leaves.min() - 0.5, leaves.max() + 1.5)
    ax.hist(leaves, bins=bins, color=rs.BLUE, ec="black", lw=0.4)
    ax.set_xlabel("Leaves per tree")
    ax.set_ylabel("Trees")
    ax.set_xticks(np.arange(leaves.min(), leaves.max() + 1))
    ax.set_title(f"(a) Tree size (depth {depths.min()} to {depths.max()})", fontsize=9)

    ax = fig.add_subplot(grid[0, 1:])
    ax.barh(roots.index[::-1], roots.to_numpy()[::-1], color=rs.BLUE, ec="black", lw=0.4)
    ax.set_xlabel(f"Trees (of {len(est)}) whose first split uses it")
    ax.set_title("(b) First split of each tree", fontsize=9)
    ax.grid(axis="y", visible=False)

    ax = fig.add_subplot(grid[1, :2])
    used = pd.Series({c: np.mean([c in {harvest.FEATURES[f] for f in e.tree_.feature if f >= 0}
                                  for e in est]) for c in harvest.FEATURES})
    mean = per_tree.mean()
    yy = np.arange(len(order))
    ax.barh(yy + 0.2, 100 * mean[order], height=0.38, color=rs.BLUE, ec="black", lw=0.4,
            label="Importance (forest)")
    ax.barh(yy - 0.2, 100 * used[order], height=0.38, color=rs.YELLOW, ec="black", lw=0.4,
            label="Trees that split on it")
    for k, c in enumerate(order):
        ax.text(100 * mean[c] + 1, k + 0.2, f"{100 * mean[c]:.0f} %", va="center", fontsize=7)
    ax.set_yticks(yy, [SHORT[c] for c in order])
    ax.set_xlim(0, 70)
    ax.set_xlabel("%")
    ax.set_title("(c) Input importance and use", fontsize=9)
    ax.grid(axis="y", visible=False)
    ax.legend(loc="lower right", fontsize=7)

    ax = fig.add_subplot(grid[1, 2])
    colours = {"basil": rs.GREEN, "lettuce": rs.PURPLE}
    for pot, r in result["pots"].items():
        recent = harvest._recent(conditions[r["crop"]])
        votes, _ = harvest._rates(model, fill, r["area_cm2"], r["crop"], recent)
        votes = pct_day(votes)
        ax.hist(votes, bins=np.arange(-45, 65, 7.5), histtype="stepfilled", alpha=0.35,
                color=colours[r["crop"]], ec=colours[r["crop"]], lw=0.8,
                label=r["crop"].title())
        for q, ls in ((50, "-"), (20, ":"), (80, ":")):
            ax.axvline(np.percentile(votes, q), color=colours[r["crop"]], ls=ls, lw=1)
    ax.axvline(0, color="black", lw=0.6)
    ax.set_xlabel("Predicted growth (% per day)")
    ax.set_ylabel("Trees")
    ax.set_title("(d) Tree votes, 16 Oct", fontsize=9)
    ax.set_ylim(0, ax.get_ylim()[1] * 1.35)
    ax.legend(fontsize=7, loc="upper right", ncol=2, handlelength=1, columnspacing=0.8)
    save(fig, "rf_forest")


if __name__ == "__main__":
    pipeline()
    trees()
    forest()
