#!/usr/bin/env python3
"""Report figures for the autonomous vegetative run from 30 Sep 2026.

Same axes style and series colours as plot_characterization_figures.py.
Run scripts/build_autonomous_run.py first.

    python scripts/plot_autonomous_run.py
"""

from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.lines import Line2D

from plot_characterization_figures import (
    COL_BAND,
    COL_DOWN,
    COL_EMA,
    COL_FILT,
    COL_MED,
    COL_RAW,
    COL_UP,
    apply_style,
    style_ax,
)

ROOT = Path(__file__).resolve().parents[1]
RUN = ROOT / "docs" / "autonomous_run_2026-09-30"
OUT_PDF = RUN / "autonomous_run_figures.pdf"
OUT_PNG = [
    RUN / "autonomous_run_overview.png",
    RUN / "autonomous_run_tds_filter.png",
    RUN / "autonomous_run_ph_filter.png",
    RUN / "autonomous_run_dose_response.png",
]
TZ = "Australia/Melbourne"

TDS_DEAD = (550.0, 850.0)
TDS_TARGET = (600.0, 800.0)
PH_DEAD = (5.9, 6.1)
PH_SETPOINT = 6.0
COL_NUTR = (0.55, 0.35, 0.10)
COL_OVERRUN = (0.80, 0.15, 0.15)
GAP_H = 180.0 / 3600.0


def load():
    s = pd.read_csv(RUN / "sensor_readings.csv", low_memory=False)
    s = s[s["row_source"] == "live"].copy()
    s["t"] = pd.to_datetime(s["time_utc"], utc=True)
    s["h"] = s["hours_since_plain_water"]
    ev = pd.read_csv(RUN / "actuator_events.csv")
    ev["t"] = pd.to_datetime(ev["time_utc"], utc=True)
    ctrl = pd.read_csv(RUN / "control_events.csv")
    ms = pd.read_csv(RUN / "milestones.csv").set_index("milestone")
    return s, ev, ctrl, ms


def line(ax, h, y, **kw):
    """Plot y, breaking the line across logging gaps."""
    h = np.asarray(h, dtype=float)
    y = np.asarray(y, dtype=float)
    brk = np.flatnonzero(np.diff(h) > GAP_H) + 1
    h = np.insert(h, brk, np.nan)
    y = np.insert(y, brk, np.nan)
    return ax.plot(h, y, **kw)


def band(ax, lo, hi, label="deadband"):
    ax.axhspan(lo, hi, facecolor=(0.75, 0.75, 0.75), edgecolor=COL_BAND,
               linewidth=0.7, alpha=0.45, zorder=0, label=label)


def day_marks(ax, s, t0, label: bool) -> None:
    """Dashed line at each local midnight, labelled with the local date."""
    local = s["t"].dt.tz_convert(TZ)
    days = pd.date_range(local.min().normalize() + pd.Timedelta(days=1), local.max(), freq="D", tz=TZ)
    for d in days:
        h = (d.tz_convert("UTC") - t0).total_seconds() / 3600
        ax.axvline(h, color=(0.6, 0.6, 0.6), lw=0.6, ls=":", zorder=1)
        if label:
            ax.text(h + 0.6, 0.02, d.strftime("%a %d %b"), transform=ax.get_xaxis_transform(),
                    fontsize=7.5, color=(0.35, 0.35, 0.35), va="bottom", ha="left")


def page_overview(s, ev, ctrl, ms) -> plt.Figure:
    t0 = pd.Timestamp(ms.loc["plain_water_refill", "time_utc"])
    h_entry = float(ms.loc["tds_ema_ge_550ppm_deadband", "hours_since_plain_water"])
    x = s[s["h"] >= -0.5]
    fig, (ax_t, ax_p, ax_e) = plt.subplots(
        3, 1, figsize=(10.6, 8.6), sharex=True, gridspec_kw={"height_ratios": [1.25, 1.25, 0.8]}
    )
    fig.subplots_adjust(top=0.91, bottom=0.07, left=0.115, right=0.98, hspace=0.30)
    fig.suptitle(
        "Autonomous run from plain water to the vegetative band, 30 Sep - 5 Oct 2026",
        fontsize=13,
    )

    # TDS
    band(ax_t, *TDS_DEAD)
    for v in TDS_TARGET:
        ax_t.axhline(v, color=(0.55, 0.55, 0.55), ls=":", lw=0.8, zorder=0)
    line(ax_t, x["h"], x["tds_ppm"], color=COL_RAW, lw=0.7, label="TDS reading")
    line(ax_t, x["h"], x["tds_ppm_ema"], color=COL_MED, lw=1.3, zorder=3,
         label="TDS 3 min EMA (controller input)")
    ax_t.axvline(h_entry, color="black", lw=1.1, zorder=2)
    ax_t.annotate(
        f"TDS enters deadband\n{h_entry:.1f} h after plain water",
        xy=(h_entry, 550), xytext=(h_entry + 16, 380), fontsize=8.5,
        arrowprops=dict(arrowstyle="-|>", lw=0.7, color="black"), ha="center",
    )
    nut = ev[ev["pump"] == "nutrient"]
    for _, r in nut[nut["overrun"] == 1].iterrows():
        y = np.interp(r["hours_since_plain_water"] + 0.75, x["h"], x["tds_ppm_ema"])
        ax_t.annotate(
            f"overrun\n{r['controller_accounted_ml']:.1f} mL",
            xy=(r["hours_since_plain_water"], y), xytext=(r["hours_since_plain_water"] + 3.0, y - 115 if y > 200 else y + 60),
            fontsize=7.5, color=COL_OVERRUN, ha="left",
            arrowprops=dict(arrowstyle="-", lw=0.6, color=COL_OVERRUN),
        )
    style_ax(ax_t)
    ax_t.set_ylim(-20, 900)
    ax_t.set_ylabel("TDS (ppm)")
    ax_t.plot([], [], color=(0.55, 0.55, 0.55), ls=":", lw=0.8, label="target 600-800 ppm")
    ax_t.legend(loc="lower left", bbox_to_anchor=(0, 1.0), ncol=4, frameon=False)
    day_marks(ax_t, x, t0, label=True)

    # pH
    band(ax_p, *PH_DEAD)
    ax_p.axhline(PH_SETPOINT, color="black", lw=1.2, zorder=2, label="setpoint 6.0")
    line(ax_p, x["h"], x["ph_raw"], color=COL_RAW, lw=0.6, label="pH reading")
    line(ax_p, x["h"], x["ph_ema"], color=COL_FILT, lw=1.3, zorder=3,
         label="pH 3 min EMA (controller input)")
    cap = ctrl[ctrl["alert_flags"].fillna("").str.contains("ph_session_cap")]
    if len(cap):
        h_cap = float(cap["hours_since_plain_water"].iloc[0])
        ax_p.axvspan(h_cap, x["h"].max(), color=(0.93, 0.85, 0.85), zorder=0, lw=0,
                     label="pH dosing blocked (session cap)")
    ax_p.axvline(h_entry, color="black", lw=1.1, zorder=2)
    style_ax(ax_p)
    ax_p.set_ylim(4.8, 7.6)
    ax_p.set_ylabel("pH")
    ax_p.legend(loc="lower left", bbox_to_anchor=(0, 1.0), ncol=5, frameon=False)
    day_marks(ax_p, x, t0, label=False)

    # Control and pump events
    lanes = {"nutrient": 3, "ph_up": 2, "ph_down": 1}
    colours = {"nutrient": COL_NUTR, "ph_up": COL_UP, "ph_down": COL_DOWN}
    e = ev[ev["hours_since_plain_water"] >= -0.5]
    for pump, lane in lanes.items():
        p = e[e["pump"] == pump]
        norm = p[p["overrun"] == 0]
        ax_e.scatter(norm["hours_since_plain_water"], np.full(len(norm), lane), marker="|",
                     s=260, lw=1.8, color=colours[pump], zorder=3)
        over = p[p["overrun"] == 1]
        if len(over):
            ax_e.scatter(over["hours_since_plain_water"], np.full(len(over), lane), marker="v",
                         s=70, color=COL_OVERRUN, edgecolor="white", lw=0.8, zorder=4)
    c = ctrl[ctrl["hours_since_plain_water"] >= -0.5]
    for ev_name, lane, mk in (("ec_check", 0.25, "|"), ("ph_check", -0.25, "|")):
        cc = c[c["event"] == ev_name]
        ax_e.scatter(cc["hours_since_plain_water"], np.full(len(cc), lane), marker=mk, s=60,
                     lw=0.9, color=(0.4, 0.4, 0.4), zorder=3)
    ax_e.set_yticks([3, 2, 1, 0.25, -0.25])
    ax_e.set_yticklabels(["nutrient pump", "pH-up pump", "pH-down pump", "TDS check", "pH check"])
    ax_e.set_ylim(-0.8, 3.6)
    ax_e.axvline(h_entry, color="black", lw=1.1, zorder=2)
    style_ax(ax_e)
    ax_e.grid(axis="y", visible=False)
    ax_e.legend(bbox_to_anchor=(1.0, 1.0), frameon=False,
        handles=[Line2D([], [], marker="v", ls="", color=COL_OVERRUN, markersize=7,
                        label="nutrient overrun (about 7.6 mL booked, 0.3-0.5 mL commanded)")],
        loc="lower right",
    )
    ax_e.set_title("Control events: one tick per pump start or controller check", fontsize=9.5, loc="left")
    day_marks(ax_e, x, t0, label=False)
    ax_e.set_xlabel("Hours since the reservoir was refilled with plain water "
                    "(30 Sep 22:37 AEST)")
    ax_e.set_xlim(-0.5, float(x["h"].max()) + 0.5)
    ax_e.set_xticks(np.arange(0, x["h"].max() + 1, 12))
    return fig


def zoom_window(s, h0, h1):
    return s[(s["h"] >= h0) & (s["h"] <= h1)]


def page_filter(s, ev, ms, *, raw_cols, ema_col, title, ylabel, ylim, zoom_pump, dead,
                resid_label, colours) -> plt.Figure:
    t0 = pd.Timestamp(ms.loc["plain_water_refill", "time_utc"])
    x = s[s["h"] >= 0.5]
    fig = plt.figure(figsize=(10.6, 8.4))
    gs = fig.add_gridspec(3, 2, height_ratios=[1.25, 1.0, 0.85], width_ratios=[1, 1],
                          top=0.90, bottom=0.07, left=0.08, right=0.98, hspace=0.38, wspace=0.16)
    fig.suptitle(title, fontsize=13)

    ax_full = fig.add_subplot(gs[0, :])
    band(ax_full, *dead)
    for (col, lab), c in zip(raw_cols, colours):
        line(ax_full, x["h"], x[col], color=c, lw=0.6, label=lab)
    line(ax_full, x["h"], x[ema_col], color=COL_EMA, lw=1.3, zorder=3, label="3 min EMA (controller input)")
    style_ax(ax_full)
    ax_full.set_ylim(*ylim)
    ax_full.set_ylabel(ylabel)
    ax_full.set_xlabel("Hours since plain-water refill")
    ax_full.legend(loc="lower left", bbox_to_anchor=(0, 1.0), ncol=len(raw_cols) + 2, frameon=False)
    day_marks(ax_full, x, t0, label=True)

    # Two zooms: a pump start, and a quiet hour.
    p = ev[(ev["pump"].isin(zoom_pump)) & (ev["hours_since_plain_water"] > 1)]
    pick = p[p["overrun"] == 1] if (p["overrun"] == 1).any() else p
    h_ev = float(pick["hours_since_plain_water"].iloc[-1])
    windows = [
        (h_ev - 0.25, h_ev + 0.75, f"Around the {pick['pump'].iloc[-1].replace('_', '-')} start at "
         f"{pd.Timestamp(pick['time_utc'].iloc[-1]).tz_convert(TZ):%d %b %H:%M}"),
        (h_ev + 6.0, h_ev + 7.0, "One quiet hour, no pump"),
    ]
    for k, (a, b, ttl) in enumerate(windows):
        ax = fig.add_subplot(gs[1, k])
        w = zoom_window(s, a, b)
        mins = (w["h"] - h_ev) * 60
        for (col, lab), c in zip(raw_cols, colours):
            ax.plot(mins, w[col], color=c, lw=0.8, marker=".", ms=2.5, label=lab)
        ax.plot(mins, w[ema_col], color=COL_EMA, lw=1.4, label="3 min EMA")
        style_ax(ax)
        ax.set_title(ttl, fontsize=9)
        ax.set_xlabel("Minutes from pump start" if k == 0 else "Minutes from the same pump start (6 h later)")
        if k == 0:
            ax.set_ylabel(ylabel)
            ax.axvline(0, color=COL_OVERRUN if pick["overrun"].iloc[-1] else COL_BAND, lw=0.9, ls="--")

    ax_r = fig.add_subplot(gs[2, :], sharex=ax_full)
    resid = x[raw_cols[0][0]] - x[ema_col]
    line(ax_r, x["h"], resid, color=COL_FILT, lw=0.7, label=resid_label)
    ax_r.axhline(0, color=(0.3, 0.3, 0.3), lw=0.7)
    q = np.nanpercentile(np.abs(resid), 99)
    style_ax(ax_r)
    lim = max(q * 2.5, 1e-3)
    ax_r.set_ylim(-lim, lim)
    ax_r.set_ylabel(r"$\Delta$")
    ax_r.set_xlabel("Hours since plain-water refill")
    ax_r.set_title(f"Reading minus EMA. 99% of samples within $\\pm${q:.3g}; the axis clips pump-start steps.",
                   fontsize=9.5)
    ax_r.legend(loc="upper left")
    return fig


# Firmware waits after a dose before the next check (vegetative preset).
EC_SETTLE_MIN = 2700 / 60
PH_SETTLE_MIN = 900 / 60
RESP_GRID = np.arange(0.0, 50.01, 1 / 6)  # 10 s steps, minutes from pump start


def response_curve(s, t0, col, detrend):
    """Sensor change after a pump start, on RESP_GRID.

    The baseline is the median of the 5 min before the dose, or (detrend) a
    straight line fitted to the 15 min before it, so pH drift is removed.
    """
    pre = s.loc[t0 - pd.Timedelta(minutes=15): t0 - pd.Timedelta(seconds=5), col].dropna()
    post = s.loc[t0: t0 + pd.Timedelta(minutes=RESP_GRID[-1] + 1), col].dropna()
    tp = (pre.index - t0).total_seconds() / 60
    tq = (post.index - t0).total_seconds() / 60
    if detrend:
        k, c = np.polyfit(tp, pre.to_numpy(), 1)
        y = post.to_numpy() - (k * tq + c)
    else:
        y = post.to_numpy() - float(pre.tail(30).median())
    return np.interp(RESP_GRID, tq, y)


def first_reach(curve, final, frac):
    hit = np.flatnonzero(curve * np.sign(final) >= frac * abs(final))
    return RESP_GRID[hit[0]] if hit.size else np.nan


def mark_fractions(ax, curve, final, colour, label_prefix):
    """Dots at 63 / 90 / 95 % of the settled change."""
    for frac, marker in ((0.63, "o"), (0.9, "s"), (0.95, "^")):
        t = first_reach(curve, final, frac)
        if np.isfinite(t):
            ax.plot(t, frac * final, marker=marker, ms=5.5, color=colour,
                    mec="white", mew=0.7, zorder=5,
                    label=f"{label_prefix} {int(frac * 100)} % at {t:.1f} min")


def settle_line(ax, minutes, label):
    ax.axvline(minutes, color="black", ls="--", lw=0.9, zorder=1)
    ax.text(minutes, 0.97, f"{label} ", transform=ax.get_xaxis_transform(),
            fontsize=7.5, va="top", ha="right")


def page_dose_response(s, ev) -> plt.Figure:
    s = s.set_index("t").sort_index()
    ev = ev[ev["hours_since_plain_water"] > 0]
    nut = ev[(ev["pump"] == "nutrient") & (ev["overrun"] == 0)]
    over = ev[(ev["pump"] == "nutrient") & (ev["overrun"] == 1)]
    down = ev[ev["pump"] == "ph_down"]
    clean_up = ev[ev["pump"] == "ph_up"].iloc[0]  # 1 Oct 00:07, +1.2 pH in low-TDS water

    fig, axes = plt.subplots(2, 2, figsize=(10.6, 8.2))
    fig.subplots_adjust(top=0.90, bottom=0.07, left=0.08, right=0.98, hspace=0.42, wspace=0.2)
    fig.suptitle("Sensor response after a dose: mixing delay and the 3 min EMA", fontsize=13)
    tail = RESP_GRID >= 25

    # (a) TDS, normal doses
    ax = axes[0, 0]
    raw = np.array([response_curve(s, t, "tds_ppm", False) for t in nut["t"]])
    ema = np.array([response_curve(s, t, "tds_ppm_ema", False) for t in nut["t"]])
    for c in raw:
        ax.plot(RESP_GRID, c, color=COL_RAW, lw=0.5, alpha=0.45, zorder=1)
    raw_med, ema_med = np.median(raw, axis=0), np.median(ema, axis=0)
    ax.plot(RESP_GRID, raw_med, color=COL_MED, lw=1.4, label="TDS reading, median", zorder=3)
    ax.plot(RESP_GRID, ema_med, color=COL_EMA, lw=1.5, label="3 min EMA, median", zorder=3)
    mark_fractions(ax, ema_med, float(np.median(ema_med[tail])), COL_EMA, "EMA")
    settle_line(ax, EC_SETTLE_MIN, "EC wait 45 min")
    style_ax(ax)
    ax.set_xlim(-1, RESP_GRID[-1])
    ax.set_ylim(-5, 32)  # a few traces pick up the next, unrelated step after 30 min
    ax.set_ylabel(r"$\Delta$TDS (ppm)")
    ax.set_xlabel("Minutes from pump start")
    ax.set_title(f"(a) {len(nut)} nutrient doses of 0.3-0.5 mL (grey: each dose)", fontsize=9.5)
    ax.legend(loc="lower right", fontsize=7)

    # (b) TDS, the three overruns, normalised to their settled change
    ax = axes[0, 1]
    for _, e in over.iterrows():
        c = response_curve(s, e["t"], "tds_ppm", False)
        fin = float(np.median(c[tail]))
        ax.plot(RESP_GRID, c / fin, lw=1.0,
                label=f"{pd.Timestamp(e['t']).tz_convert(TZ):%d %b %H:%M}, +{fin:.0f} ppm")
    ema_n = np.median(np.array([
        (lambda c: c / np.median(c[tail]))(response_curve(s, t, "tds_ppm_ema", False))
        for t in over["t"]]), axis=0)
    ax.plot(RESP_GRID, ema_n, color=COL_EMA, lw=1.5, label="3 min EMA, median")
    ax.axhspan(0.9, 1.1, color=(0.85, 0.85, 0.85), zorder=0, lw=0)
    style_ax(ax)
    ax.set_xlim(-0.5, 15)
    ax.set_ylim(-0.1, 3.4)
    ax.set_ylabel("Change / settled change")
    ax.set_xlabel("Minutes from pump start")
    ax.set_title("(b) Three overrun doses (about 7.6 mL): large steps, normalised", fontsize=9.5)
    ax.legend(loc="upper right", fontsize=7)

    # (c) pH, the one clean step
    ax = axes[1, 0]
    c_raw = response_curve(s, clean_up["t"], "ph_raw", True)
    c_ema = response_curve(s, clean_up["t"], "ph_ema", True)
    ax.plot(RESP_GRID, c_raw, color=COL_FILT, lw=1.0, label="pH raw")
    ax.plot(RESP_GRID, c_ema, color=COL_EMA, lw=1.5, label="3 min EMA")
    mark_fractions(ax, c_raw, float(np.median(c_raw[tail])), COL_FILT, "raw")
    mark_fractions(ax, c_ema, float(np.median(c_ema[tail])), COL_EMA, "EMA")
    settle_line(ax, PH_SETTLE_MIN, "pH wait 15 min")
    style_ax(ax)
    ax.set_xlim(-1, 40)
    ax.set_ylabel(r"$\Delta$pH (drift removed)")
    ax.set_xlabel("Minutes from pump start")
    when = pd.Timestamp(clean_up["t"]).tz_convert(TZ)
    ax.set_title(f"(c) pH-up dose {when:%d %b %H:%M}: the only clean pH step (+1.2, TDS 170 ppm)",
                 fontsize=9.5)
    ax.legend(loc="lower right", fontsize=7, ncol=2)

    # (d) pH-down doses: response below drift and noise
    ax = axes[1, 1]
    curves = np.array([response_curve(s, t, "ph_raw", True) for t in down["t"]])
    q25, q50, q75 = np.percentile(curves, [25, 50, 75], axis=0)
    ema_med = np.median(np.array([response_curve(s, t, "ph_ema", True) for t in down["t"]]), axis=0)
    ax.fill_between(RESP_GRID, q25, q75, color=COL_DOWN, alpha=0.15, lw=0, label="raw, middle 50 %")
    ax.plot(RESP_GRID, q50, color=COL_DOWN, lw=1.0, label="raw, median")
    ax.plot(RESP_GRID, ema_med, color=COL_EMA, lw=1.5, label="3 min EMA, median")
    ax.axhline(0, color=(0.3, 0.3, 0.3), lw=0.7)
    settle_line(ax, PH_SETTLE_MIN, "pH wait 15 min")
    style_ax(ax)
    ax.set_xlim(-1, 40)
    ax.set_ylim(-0.15, 0.15)
    ax.set_ylabel(r"$\Delta$pH (drift removed)")
    ax.set_xlabel("Minutes from pump start")
    ax.set_title(f"(d) {len(down)} pH-down doses of 0.05-0.15 mL: change within drift and noise",
                 fontsize=9.5)
    ax.legend(loc="lower left", fontsize=7)
    return fig


def main() -> None:
    apply_style()
    s, ev, ctrl, ms = load()
    figs = [
        page_overview(s, ev, ctrl, ms),
        page_filter(
            s, ev, ms,
            raw_cols=[("tds_ppm", "TDS reading (burst average)")],
            ema_col="tds_ppm_ema",
            title=r"TDS reading and the $\tau = 3$ min EMA the controller doses on",
            ylabel="TDS (ppm)", ylim=(-20, 900), zoom_pump=["nutrient"], dead=TDS_DEAD,
            resid_label="TDS reading $-$ EMA (ppm)", colours=[COL_RAW],
        ),
        page_filter(
            s, ev, ms,
            raw_cols=[("ph_raw", "pH raw (trimmed-mean voltage)"), ("ph_filtered", "pH burst filter")],
            ema_col="ph_ema",
            title=r"pH readings and the $\tau = 3$ min EMA the controller doses on",
            ylabel="pH", ylim=(4.8, 7.6), zoom_pump=["ph_up", "ph_down"], dead=PH_DEAD,
            resid_label="pH raw $-$ EMA", colours=[COL_RAW, COL_FILT],
        ),
        page_dose_response(s, ev),
    ]
    with PdfPages(OUT_PDF) as pdf:
        info = pdf.infodict()
        info["Title"] = "Autonomous vegetative run, 30 Sep - 5 Oct 2026"
        for fig, png in zip(figs, OUT_PNG):
            pdf.savefig(fig)
            fig.savefig(png, dpi=200)
            plt.close(fig)
            print(png)
    print(OUT_PDF)


if __name__ == "__main__":
    main()
