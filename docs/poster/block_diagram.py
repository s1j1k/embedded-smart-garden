"""Poster figure: the two staged SISO loops (firmware 1.9.12).

    python block_diagram.py  ->  block_diagram.png / .pdf / .svg

Nutrient -> TDS and pH-up/down -> pH are separate single-input single-output
loops sharing one reservoir. A sequencer enables one at a time: the nutrient loop
until TDS is inside its pass band, then the pH loop (the nutrient shifts pH, so pH
is corrected last). The coupling (nutrient lowers pH; TDS sets the pH gain) is
shown dashed.
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

TEAL, GREEN, CORAL, GREY, INK, PURPLE = "#0b5d7a", "#3a9d5d", "#e4572e", "#8c959f", "#1f2328", "#7a5cc4"
FILL = {"ctrl": "#e3f0f6", "plant": "#e6f4ea", "sense": "#fdece6", "ref": "#f3f4f6", "seq": "#efe9fb"}

fig, ax = plt.subplots(figsize=(14, 7.4))
ax.set_xlim(0, 14)
ax.set_ylim(0, 7.4)
ax.axis("off")


def box(x, y, w, h, title, sub, kind, edge=None):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.04,rounding_size=0.16",
                                fc=FILL[kind], ec=edge or (TEAL if kind == "ctrl" else GREY), lw=1.6))
    ax.text(x + w / 2, y + h * 0.68, title, ha="center", va="center", fontsize=12,
            fontweight="bold", color=INK)
    ax.text(x + w / 2, y + h * 0.30, sub, ha="center", va="center", fontsize=9.3, color=INK,
            linespacing=1.25)
    return (x, y, w, h)


def arrow(p, q, color=INK, text=None, off=(0, 0.1), style="-|>", ls="-", fs=9.3):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle=style, mutation_scale=15, lw=1.6, color=color,
                                 linestyle=ls, shrinkA=0, shrinkB=0))
    if text:
        ax.text((p[0] + q[0]) / 2 + off[0], (p[1] + q[1]) / 2 + off[1], text, ha="center",
                va="bottom", fontsize=fs, color=color)


def summer(x, y):
    ax.add_patch(plt.Circle((x, y), 0.2, fc="white", ec=INK, lw=1.6))
    ax.text(x, y, "Σ", ha="center", va="center", fontsize=12)
    ax.text(x - 0.38, y - 0.3, "+", fontsize=11)
    ax.text(x + 0.1, y - 0.48, "−", fontsize=13)


H = 1.15
rows = {
    # name: (y, reference text, law text, actuator title, actuator text, colour)
    "TDS": (4.15, "TDS 550–850 ppm", "u = Kp·e\nKp = 0.8 / K_N(V)",
            "Nutrient pump", "calibrated mL", GREEN),
    "pH": (1.95, "pH 5.5–6.5", "u = Kp·e\nKp = 0.8 / K_pH(V, TDS)",
           "pH-up / pH-down", "calibrated mL", CORAL),
}
tank = box(11.35, 1.95, 2.45, 3.35, "Reservoir", "≈ 8 L, aerated\nmixing τ ≈ 5–9 min\n\nnutrient also\nlowers pH", "plant")
feeds = {}
for name, (y, ref_txt, law_txt, act_t, act_s, col) in rows.items():
    cy = y + H / 2
    ref = box(0.1, y, 1.75, H, f"{name} pass band", ref_txt, "ref")
    sx = 2.35
    summer(sx, cy)
    dead = box(2.85, y, 1.85, H, "Dead band", "dose only outside\nthe pass band", "ctrl")
    law = box(5.05, y, 2.55, H, f"P dose law ({name})", law_txt, "ctrl")
    act = box(8.25, y, 2.15, H, act_t, act_s, "plant")
    arrow((ref[0] + ref[2], cy), (sx - 0.2, cy))
    arrow((sx + 0.2, cy), (dead[0], cy), text="e")
    arrow((dead[0] + dead[2], cy), (law[0], cy))
    arrow((law[0] + law[2], cy), (act[0], cy), text="u (mL)" if name == "pH" else None)
    arrow((act[0] + act[2], cy), (tank[0], cy))
    feeds[name] = (sx, cy, law)

# sequencer: enables one loop at a time
seq = box(4.75, 5.6, 3.15, 1.05, "Sequencer", "1 · nutrient loop until TDS in band\n2 · then pH loop", "seq",
          edge=PURPLE)
# enable 1 -> nutrient law (straight down)
lawT = feeds["TDS"][2]
arrow((lawT[0] + 0.5, seq[1]), (lawT[0] + 0.5, lawT[1] + lawT[3]), color=PURPLE, ls="--",
      text="1 enable", off=(-0.5, -0.1), fs=9)
# enable 2 -> pH law (down the gap right of the nutrient law, then in from the right)
lawP = feeds["pH"][2]
gx = 7.92
arrow((seq[0] + seq[2], seq[1] + 0.3), (gx, seq[1] + 0.3), color=PURPLE, ls="--", style="-")
arrow((gx, seq[1] + 0.3), (gx, lawP[1] + lawP[3] - 0.15), color=PURPLE, ls="--", style="-")
arrow((gx, lawP[1] + lawP[3] - 0.15), (lawP[0] + lawP[2], lawP[1] + lawP[3] - 0.15), color=PURPLE,
      ls="--")
ax.text(gx + 0.08, 3.55, "2 enable", fontsize=9, color=PURPLE, ha="left")

# feedback: reservoir -> probes + filter -> each summer
fb = box(5.05, 0.2, 4.0, 1.05, "Probes + low-pass filter", "pH, TDS every 10 s · Butterworth 2 mHz\nsettle detection after each dose",
         "sense")
tx = tank[0] + tank[2] / 2
fy = fb[1] + fb[3] / 2
arrow((tx, tank[1]), (tx, fy), style="-")
arrow((tx, fy), (fb[0] + fb[2], fy), text="y")
arrow((fb[0], fy), (1.55, fy), style="-")
# to the pH summer
sx, cy, _ = feeds["pH"]
arrow((1.55, fy), (1.55, 1.6), style="-")
arrow((1.55, 1.6), (sx, 1.6), style="-")
arrow((sx, 1.6), (sx, cy - 0.2), text="ŷ_pH", off=(0.3, -0.25))
# to the TDS summer (route left of the boxes)
sxT, cyT, _ = feeds["TDS"]
arrow((1.55, fy), (0.05, fy), style="-")
arrow((0.05, fy), (0.05, 3.75), style="-")
arrow((0.05, 3.75), (sxT, 3.75), style="-")
arrow((sxT, 3.75), (sxT, cyT - 0.2), text="ŷ_TDS", off=(0.33, -0.2))
# gain scheduling: measured TDS sets the pH gain
lawpH = feeds["pH"][2]
arrow((sxT + 0.9, 3.75), (lawpH[0] + 1.0, lawpH[1] + lawpH[3]), color=GREY, ls=":")
ax.text(sxT + 1.0, 3.5, "measured TDS\nsets Kp (pH)", fontsize=8.8, color=GREY, ha="left", va="top")

# disturbances
arrow((tx, 6.6), (tx, tank[1] + tank[3]), color=CORAL)
ax.text(tx + 0.1, 6.2, "disturbances:\nplant uptake,\nevaporation, top-up", ha="left", va="center",
        fontsize=9, color=CORAL)
ax.text(0.1, 7.15, "Two SISO loops, staged — each dose → wait ≥ 20 min (dead time) → re-measure",
        ha="left", fontsize=10.5, color=TEAL, fontweight="bold")

fig.tight_layout()
for ext in ("png", "pdf", "svg"):
    fig.savefig(f"block_diagram.{ext}", dpi=220, transparent=(ext != "pdf"))
print("ok")
