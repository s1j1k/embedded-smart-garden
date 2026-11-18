"""Poster figure: how the camera finds and measures the plants (15 Oct 2026 photo).

Top: the pre-trained detector (Grounding DINO, prompt "net pot. plant.") on the
photo: net pots, plants, and the basil's reflection, which it also calls a plant.
Bottom: the measurement: leaf mask, one box per pot with the reflection cut off,
sizes in cm from the pot-rim scale.

    python plant_detection_figure.py PHOTO DETECTIONS.json  ->  plant_detection.png

DETECTIONS.json is what detect_one.py wrote on the Pi for the same photo.
Needs OpenCV, matplotlib and the dashboard's src/ on the path.
"""
import json
import sys
from pathlib import Path

import cv2
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "smart-garden-iot-dashboard" / "src"))
import growth_detection as gd  # noqa: E402

HERE = Path(__file__).resolve().parent
POT, PLANT, REFLECTION, MEASURE = "#2f86c9", "#2e9d4a", "#c0392b", "#e67e22"
photo, det_path = sys.argv[1], sys.argv[2]
bgr = cv2.imread(photo)
rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
det = json.loads(Path(det_path).read_text())
height, width = bgr.shape[:2]
y0, y1 = int(0.43 * height), int(0.87 * height)     # the band with pots and plants


def iou(a, b):
    ix = max(0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union else 0


def label(ax, x, y, text, colour, dark=False):
    ax.text(x, y, text, color="black" if dark else "white", fontsize=11, weight="bold",
            va="bottom", bbox=dict(fc=colour, ec="none", pad=1.5))


fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8, 5.6), dpi=250)
for ax in (ax1, ax2):
    ax.axis("off")

# --- top: the AI detector ---
ax1.imshow(rgb[y0:y1])
kept = []
for d in sorted(det["detections"], key=lambda d: -d["score"]):
    box, name = d["box"], (d["label"] or "").strip()
    if not name or (box[2] - box[0]) > 0.6 * width or any(iou(box, k) > 0.4 for k in kept):
        continue
    kept.append(box)
    is_pot = "pot" in name
    is_reflection = not is_pot and box[1] > 0.70 * height
    colour = POT if is_pot else (REFLECTION if is_reflection else PLANT)
    ax1.add_patch(Rectangle((box[0], box[1] - y0), box[2] - box[0], box[3] - box[1],
                            fill=False, lw=2.5, ec=colour))
    text = "net pot" if is_pot else ("its reflection, also called 'plant'" if is_reflection else "plant")
    ty = box[3] - y0 + 4 if is_pot else box[1] - y0 - 4
    if is_pot:
        ax1.text(box[0] + 6, box[3] - y0 - 6, f"{text} {d['score']:.2f}", color="white",
                 fontsize=11, weight="bold", va="bottom", bbox=dict(fc=colour, ec="none", pad=1.5))
    else:
        label(ax1, box[0] + 6, ty if not is_reflection else box[3] - y0 - 6 - 40,
              f"{text} {d['score']:.2f}", colour)
ax1.set_title("1. A pre-trained AI model finds the pots and plants", fontsize=13, loc="left")

# --- bottom: our measurement ---
plants = gd.find_plants(bgr)
mask = gd.leaf_mask(bgr)
over = rgb.copy()
over[mask > 0] = (0.4 * over[mask > 0] + 0.6 * np.array([60, 230, 90])).astype("uint8")
ax2.imshow(over[y0:y1])
rims = gd.find_rims(bgr)
for rim in rims:
    x, y, w, h = rim["box"]
    ax2.add_patch(Rectangle((x, y - y0), w, h, fill=False, lw=2, ec=POT, ls="--"))
if len(rims) == 2:
    cx = [r["box"][0] + r["box"][2] / 2 for r in rims]
    yy = max(r["box"][1] + r["box"][3] for r in rims) - y0 + 35
    ax2.annotate("", xy=(cx[0], yy), xytext=(cx[1], yy),
                 arrowprops=dict(arrowstyle="<->", color=POT, lw=2.5))
    ax2.text((cx[0] + cx[1]) / 2, yy + 8, "207 mm between pot centres: the scale", color=POT,
             fontsize=11, ha="center", va="top", weight="bold",
             bbox=dict(fc="white", ec="none", alpha=0.85, pad=1.5))
names = {1: "basil", 2: "lettuce"}
for p in plants:
    x, y, w, h = p["box"]
    ax2.add_patch(Rectangle((x, y - y0), w, h, fill=False, lw=2.5, ec=MEASURE))
    if p.get("mirror_y") is not None:
        ax2.plot([x - 20, x + w + 20], [p["mirror_y"] - y0] * 2, color=REFLECTION, lw=2.5, ls=(0, (4, 3)))
    if p.get("area_cm2") is not None:
        label(ax2, x, y - y0 - 4,
              f"{names.get(p['number'], 'pot')}: {p['area_cm2']:.1f} cm², {p['height_cm']:.1f} cm",
              "#f8d7a8", dark=True)
ax2.text(20, (y1 - y0) - 20, "red dashes: reflection cut", color=REFLECTION, fontsize=10,
         weight="bold", va="bottom", bbox=dict(fc="white", ec="none", alpha=0.85, pad=1.5))
ax2.set_title("2. Leaves measured in centimetres, reflections removed", fontsize=13, loc="left")

fig.tight_layout(h_pad=1.2)
fig.savefig(HERE / "plant_detection.png", bbox_inches="tight", pad_inches=0.05)
print("plants:", [(p["number"], p.get("area_cm2"), p.get("height_cm"), p.get("mirror_y")) for p in plants])
