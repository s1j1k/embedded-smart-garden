"""Report figure style: LaTeX fonts on MATLAB-style axes.

Import and call ``apply()`` before any figure is created. Every report figure
uses it so the plots match the LaTeX body text:

* Computer Modern serif (cmr10 and the ``cm`` math set, shipped with
  matplotlib, so no TeX install is needed);
* MATLAB axes: a full box, ticks inward on all four sides, a light dotted grid;
* MATLAB's default colour order (R2014b+).
"""

from __future__ import annotations

import matplotlib as mpl
from cycler import cycler

# MATLAB default colour order (lines(7)).
BLUE = "#0072BD"
ORANGE = "#D95319"
YELLOW = "#EDB120"
PURPLE = "#7E2F8E"
GREEN = "#77AC30"
CYAN = "#4DBEEE"
RED = "#A2142F"
COLORS = [BLUE, ORANGE, YELLOW, PURPLE, GREEN, CYAN, RED]


def apply(font_size: float = 10.0) -> None:
    mpl.rcParams.update({
        "font.family": "serif",
        "font.serif": ["cmr10", "CMU Serif", "Computer Modern Roman", "DejaVu Serif"],
        "mathtext.fontset": "cm",
        "axes.formatter.use_mathtext": True,  # proper minus sign with cmr10
        "axes.unicode_minus": True,
        "font.size": font_size,
        "axes.titlesize": font_size,
        "axes.labelsize": font_size,
        "xtick.labelsize": font_size - 1,
        "ytick.labelsize": font_size - 1,
        "legend.fontsize": font_size - 1,
        "axes.prop_cycle": cycler(color=COLORS),
        "axes.linewidth": 0.6,
        "axes.edgecolor": "black",
        "axes.spines.top": True,
        "axes.spines.right": True,
        "axes.grid": True,
        "grid.color": "#b0b0b0",
        "grid.linestyle": ":",
        "grid.linewidth": 0.5,
        "grid.alpha": 1.0,
        "xtick.direction": "in",
        "ytick.direction": "in",
        "xtick.top": True,
        "ytick.right": True,
        "xtick.major.size": 3.5,
        "ytick.major.size": 3.5,
        "xtick.minor.visible": False,
        "lines.linewidth": 1.2,
        "legend.frameon": True,
        "legend.edgecolor": "black",
        "legend.fancybox": False,
        "legend.framealpha": 1.0,
        "figure.facecolor": "white",
        "savefig.facecolor": "white",
        "savefig.bbox": "tight",
        "savefig.dpi": 300,
    })
