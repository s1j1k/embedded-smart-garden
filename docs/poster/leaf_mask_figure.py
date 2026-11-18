"""Report figure: how the leaf (green) mask is found, on the 15 Oct 2026 13:01 photo.

(a) the photo under the white flash; (b) the excess-green mask the first version used
(2g - r - b on brightness-normalised colour, Otsu threshold); (c) the CIE L*a*b* a*
channel (green to magenta); (d) the leaf mask the dashboard uses now, a* <= -6.

    python leaf_mask_figure.py PHOTO  ->  leaf_mask.png / leaf_mask.pdf

Run with the dashboard's venv (OpenCV, matplotlib); imports its src/growth_detection.
"""
import sys
from pathlib import Path

import cv2
import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "smart-garden-iot-dashboard" / "src"))
import growth_detection as gd  # noqa: E402

HERE = Path(__file__).resolve().parent
EXG_MIN = 0.06  # the first version's floor on excess green (dashboard commit 0e59c75)


def excess_green_mask(bgr):
    """The first leaf mask: normalised excess-green index with an Otsu threshold."""
    pixels = bgr.astype(np.float32)
    total = pixels.sum(axis=2) + 1e-6
    blue, green, red = (pixels[..., i] / total for i in range(3))
    excess = np.clip((2 * green - red - blue) * 255, 0, 255).astype(np.uint8)
    otsu, _ = cv2.threshold(excess, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    mask = (excess > max(otsu, EXG_MIN * 255)) & (total / 3 > gd.MIN_BRIGHTNESS) & (green > blue)
    mask = mask.astype(np.uint8) * 255
    kernel = np.ones((3, 3), np.uint8)
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
    return cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)


def overlay(rgb, mask, colour=(46, 157, 74)):
    """Mask pixels in solid colour over a dimmed photo."""
    out = (rgb.astype(np.float32) * 0.35).astype(np.uint8)
    out[mask > 0] = colour
    return out


photo = sys.argv[1]
bgr = cv2.imread(photo)
rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
a_star = cv2.cvtColor(bgr, cv2.COLOR_BGR2LAB)[..., 1].astype(np.int16) - 128
exg = excess_green_mask(bgr)
lab_mask = gd.leaf_mask(bgr)

fig, axes = plt.subplots(2, 2, figsize=(10, 6.2), dpi=200)
panels = [
    (rgb, "(a) Photo under the white flash", None),
    (overlay(rgb, exg), f"(b) Excess-green mask (first version): {int((exg > 0).sum()):,} px", None),
    (a_star, "(c) CIE L*a*b* a* channel: leaves green, light and lid magenta", "PiYG_r"),
    (overlay(rgb, lab_mask), f"(d) Leaf mask, a* $\\leq$ {gd.GREEN_A_MAX}: {int((lab_mask > 0).sum()):,} px",
     None),
]
for ax, (img, title, cmap) in zip(axes.flat, panels):
    if cmap:
        im = ax.imshow(img, cmap=cmap, vmin=-40, vmax=40)
        fig.colorbar(im, ax=ax, fraction=0.03, pad=0.01, label="a*")
    else:
        ax.imshow(img)
    ax.set_title(title, fontsize=9, loc="left")
    ax.axis("off")
fig.tight_layout()
for ext in ("png", "pdf"):
    fig.savefig(HERE / f"leaf_mask.{ext}", bbox_inches="tight")
print("exg px", int((exg > 0).sum()), "a* px", int((lab_mask > 0).sum()))
