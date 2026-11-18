#!/usr/bin/env python3
"""Report figures for the 25–27 Sep 2026 characterization logs.

MATLAB-style axes (boxed ticks, light grid, the same series colours as the
analysis scripts in docs/ph_last_24h) with Computer Modern for text and math.
Writes one multi-page PDF and one PNG per page.

    python scripts/plot_characterization_figures.py
"""

from __future__ import annotations

import csv
from datetime import datetime, timedelta, timezone
from pathlib import Path

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.backends.backend_pdf import PdfPages

ROOT = Path(__file__).resolve().parents[1]
PH_CSV = ROOT / "docs" / "ph_last_24h" / "ph_last_24h.csv"
SENSOR_CSV = (
    ROOT
    / "docs"
    / "since_characterization_2026-09-25"
    / "sensor_readings_since_characterization.csv"
)
CLOSED_CSV = (
    ROOT
    / "docs"
    / "characterization_2026-09-25_211710"
    / "phase1"
    / "closed_loop_trace.csv"
)
OUT_PDF = ROOT / "docs" / "characterization_weekend_figures.pdf"
OUT_PNG = [
    ROOT / "docs" / "characterization_closed_loop.png",
    ROOT / "docs" / "characterization_weekend_log.png",
    ROOT / "docs" / "characterization_ph_filters.png",
    ROOT / "docs" / "characterization_tds_filters.png",
    ROOT / "docs" / "characterization_pump_voltage.png",
]

# 25-27 Sep 2026 is before the October DST change, so local time is AEST (UTC+10).
AEST = timezone(timedelta(hours=10))

# Series colours from docs/ph_last_24h/plot_ph_voltage_ema.m and
# plot_ph_last_24h.m, drawn with MATLAB-style axes.
COL_RAW = (0.65, 0.65, 0.65)
COL_FILT = (0.15, 0.35, 0.75)
COL_MED = (0.85, 0.35, 0.10)
COL_EMA = (0.10, 0.55, 0.25)
COL_UP = (0.10, 0.45, 0.85)
COL_DOWN = (0.80, 0.15, 0.15)
COL_BAND = (0.45, 0.45, 0.45)

PH_TARGET = (5.5, 5.8)
PH_DEAD = (5.4, 5.9)
PH_SETPOINT = 5.65
TDS_TARGET = (800.0, 1200.0)
TDS_DEAD = (750.0, 1250.0)
TDS_SETPOINT = 0.5 * (TDS_TARGET[0] + TDS_TARGET[1])
EMA_TAU = timedelta(minutes=3)
GAP_S = 180.0


def apply_style() -> None:
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.serif": ["cmr10"],
            "mathtext.fontset": "cm",
            "axes.formatter.use_mathtext": True,
            "axes.unicode_minus": False,
            "font.size": 10,
            "axes.labelsize": 11,
            "axes.titlesize": 11,
            "legend.fontsize": 8,
            "xtick.labelsize": 9,
            "ytick.labelsize": 9,
            "axes.linewidth": 0.8,
            "lines.linewidth": 1.1,
            "figure.facecolor": "white",
            "axes.facecolor": "white",
            "savefig.facecolor": "white",
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
            "legend.frameon": True,
            "legend.edgecolor": "0.75",
            "legend.fancybox": False,
        }
    )


def style_ax(ax) -> None:
    ax.set_axisbelow(True)
    ax.grid(True, color=(0.82, 0.82, 0.82), linewidth=0.6)
    ax.tick_params(direction="in", top=True, right=True, length=3.5, width=0.7)
    for spine in ax.spines.values():
        spine.set_linewidth(0.8)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def parse_utc(text: str) -> datetime:
    return datetime.strptime(text.replace("Z", ""), "%Y-%m-%dT%H:%M:%S").replace(
        tzinfo=timezone.utc
    )


def to_local(times: list[datetime]) -> list[datetime]:
    return [t.astimezone(AEST) for t in times]


def as_float(rows: list[dict[str, str]], key: str) -> np.ndarray:
    out = np.empty(len(rows), dtype=float)
    for i, row in enumerate(rows):
        text = row.get(key, "").strip()
        out[i] = float(text) if text else np.nan
    return out


def epoch_s(times: list[datetime]) -> np.ndarray:
    t0 = times[0]
    return np.array([(t - t0).total_seconds() for t in times], dtype=float)


def plot_time(ax, times, t_s, y, gap_s: float = GAP_S, **kwargs):
    """Plot y, breaking the line across logging gaps without dropping samples."""
    t_out = []
    y_out = []
    for i, value in enumerate(y):
        if i > 0 and (t_s[i] - t_s[i - 1]) > gap_s:
            t_out.append(times[i])
            y_out.append(np.nan)
        t_out.append(times[i])
        y_out.append(value)
    return ax.plot(t_out, y_out, **kwargs)


def causal_median(x: np.ndarray, window: int) -> np.ndarray:
    y = np.empty_like(x)
    for i in range(len(x)):
        y[i] = np.nanmedian(x[max(0, i - window + 1) : i + 1])
    return y


def causal_ema(
    t_s: np.ndarray, x: np.ndarray, tau_s: float, hold: np.ndarray | None = None
) -> np.ndarray:
    """Causal EMA. alpha = 1 - exp(-dt/tau). Held samples keep the last value."""
    y = np.empty_like(x)
    y[0] = x[0]
    for i in range(1, len(x)):
        if hold is not None and hold[i]:
            y[i] = y[i - 1]
            continue
        if not np.isfinite(x[i]):
            y[i] = y[i - 1]
            continue
        dt = t_s[i] - t_s[i - 1]
        if dt <= 0:
            y[i] = y[i - 1]
            continue
        alpha = 1.0 - np.exp(-dt / tau_s)
        prev = y[i - 1]
        if not np.isfinite(prev):
            prev = x[i]
        y[i] = (1.0 - alpha) * prev + alpha * x[i]
    return y


def pump_masks(rows: list[dict[str, str]]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    up = np.array([float(r["ph_up_pump_on"]) > 0 for r in rows])
    down = np.array([float(r["ph_down_pump_on"]) > 0 for r in rows])
    other = np.array(
        [
            float(r["water_pump_on"]) > 0 or float(r["nutrient_pump_on"]) > 0
            for r in rows
        ]
    )
    return up, down, up | down | other


def mark_pumps(ax, t, y, up, down) -> None:
    if np.any(up):
        ax.scatter(
            np.asarray(t)[up],
            y[up],
            s=28,
            c=[COL_UP],
            zorder=4,
            label="pH-up pump sample",
        )
    if np.any(down):
        ax.scatter(
            np.asarray(t)[down],
            y[down],
            s=28,
            c=[COL_DOWN],
            zorder=4,
            label="pH-down pump sample",
        )


def format_time(ax, hours: float) -> None:
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%d %b %H:%M", tz=AEST))
    step = 1 if hours <= 6 else 4
    ax.xaxis.set_major_locator(mdates.HourLocator(interval=step, tz=AEST))
    ax.set_xlabel("Local time (AEST)")


def span_hours(times: list[datetime]) -> float:
    return (times[-1] - times[0]).total_seconds() / 3600.0


def shade_deadband(ax, low: float, high: float):
    return ax.axhspan(
        low,
        high,
        facecolor=(0.75, 0.75, 0.75),
        edgecolor=(0.45, 0.45, 0.45),
        linewidth=0.7,
        alpha=0.45,
        zorder=0,
        label="deadband",
    )


def mark_setpoint(ax, value: float, label: str):
    return ax.axhline(value, color="black", ls="-", lw=1.35, zorder=2, label=label)


def ph_guides(ax, ylim) -> None:
    ax.axhline(PH_DEAD[0], color=COL_BAND, ls="--", lw=0.8, zorder=0)
    ax.axhline(PH_DEAD[1], color=COL_BAND, ls="--", lw=0.8, zorder=0)
    ax.axhline(PH_TARGET[0], color=(0.55, 0.55, 0.55), ls=":", lw=0.8, zorder=0)
    ax.axhline(PH_TARGET[1], color=(0.55, 0.55, 0.55), ls=":", lw=0.8, zorder=0)
    ax.set_ylim(*ylim)


def new_fig(nrows: int, height: float, ratios=None):
    fig, axes = plt.subplots(
        nrows,
        1,
        figsize=(10.6, height),
        sharex=True,
        gridspec_kw={"height_ratios": ratios} if ratios else None,
    )
    if nrows == 1:
        axes = [axes]
    fig.subplots_adjust(top=0.88, bottom=0.10, left=0.08, right=0.98, hspace=0.28)
    return fig, axes


def page_closed_loop() -> plt.Figure:
    rows = read_csv(CLOSED_CSV)
    times = to_local([parse_utc(r["ts_utc"]) for r in rows])
    t_s = epoch_s(times)
    ph = as_float(rows, "ph")
    tds = as_float(rows, "tds_ppm")

    fig, (ax_ph, ax_tds) = new_fig(2, 6.6, [1.0, 1.0])
    fig.suptitle(
        "Closed-loop characterization, 25-26 September 2026",
        fontsize=13,
    )

    shade_deadband(ax_ph, PH_DEAD[0], PH_DEAD[1])
    mark_setpoint(ax_ph, PH_SETPOINT, "setpoint 5.65")
    plot_time(ax_ph, times, t_s, ph, color=COL_FILT, lw=1.2, zorder=3, label="logged pH")
    style_ax(ax_ph)
    ax_ph.set_ylabel("pH")
    ax_ph.set_ylim(5.25, 6.15)
    ax_ph.set_title("Vegetative closed loop")
    ax_ph.legend(loc="lower left")

    shade_deadband(ax_tds, TDS_DEAD[0], TDS_DEAD[1])
    mark_setpoint(ax_tds, TDS_SETPOINT, "setpoint 1000 ppm")
    plot_time(ax_tds, times, t_s, tds, color=COL_MED, lw=1.2, zorder=3, label="logged TDS")
    style_ax(ax_tds)
    ax_tds.set_ylabel("TDS (ppm)")
    ax_tds.set_title("Vegetative TDS")
    ax_tds.set_ylim(700, float(np.nanmax(tds)) + 40)
    ax_tds.legend(loc="lower right")
    format_time(ax_tds, span_hours(times))
    return fig


def page_weekend() -> plt.Figure:
    rows = read_csv(SENSOR_CSV)
    times = to_local([parse_utc(r["recorded_at_utc"]) for r in rows])
    t_s = epoch_s(times)
    ph = as_float(rows, "ph_filtered")
    tds = as_float(rows, "tds_ppm")
    up, down, _ = pump_masks(rows)

    fig, (ax_ph, ax_tds) = new_fig(2, 6.8, [1.15, 1.0])
    fig.suptitle(
        "On-device log after characterization, 26-27 September 2026",
        fontsize=13,
    )

    shade_deadband(ax_ph, PH_DEAD[0], PH_DEAD[1])
    mark_setpoint(ax_ph, PH_SETPOINT, "setpoint 5.65")
    plot_time(ax_ph, times, t_s, ph, color=COL_FILT, lw=1.05, zorder=3, label="firmware pH")
    mark_pumps(ax_ph, times, ph, up, down)
    style_ax(ax_ph)
    ax_ph.set_ylabel("pH")
    ax_ph.set_ylim(3.0, 7.0)
    ax_ph.set_title("Markers are samples taken with a pump on.")
    ax_ph.legend(loc="lower right")

    shade_deadband(ax_tds, TDS_DEAD[0], TDS_DEAD[1])
    mark_setpoint(ax_tds, TDS_SETPOINT, "setpoint 1000 ppm")
    plot_time(
        ax_tds, times, t_s, tds, color=COL_MED, lw=1.05, zorder=3, label="firmware TDS"
    )
    mark_pumps(ax_tds, times, tds, up, down)
    style_ax(ax_tds)
    ax_tds.set_ylabel("TDS (ppm)")
    ax_tds.set_title("Vegetative TDS")
    ax_tds.set_ylim(700, float(np.nanmax(tds)) + 40)
    ax_tds.legend(loc="lower right")
    format_time(ax_tds, span_hours(times))
    return fig


def page_ph_filters(ph_rows, times, t_s, up, down, hold) -> plt.Figure:
    voltage = as_float(ph_rows, "ph_voltage")
    raw = as_float(ph_rows, "ph_raw")
    filt = as_float(ph_rows, "ph_filtered")
    med5 = causal_median(filt, 5)
    ema = causal_ema(t_s, filt, EMA_TAU.total_seconds(), hold)

    fig, axes = new_fig(3, 8.2, [1.0, 1.35, 1.0])
    fig.suptitle(
        r"pH voltage and software filters, $\tau = 3$ min EMA",
        fontsize=13,
    )
    ax_v, ax_ph, ax_d = axes

    plot_time(ax_v, times, t_s, voltage, color=(0.25, 0.25, 0.25), lw=0.8)
    mark_pumps(ax_v, times, voltage, up, down)
    style_ax(ax_v)
    ax_v.set_ylabel("Board voltage (V)")
    ax_v.set_title("pH-board voltage after the 40 mV trimmed mean")
    ax_v.legend(loc="lower left")

    plot_time(ax_ph, times, t_s, raw, color=COL_RAW, lw=0.7, label="pH raw")
    plot_time(ax_ph, times, t_s, filt, color=COL_FILT, lw=1.0, label="burst filter")
    plot_time(
        ax_ph,
        times,
        t_s,
        med5,
        color=COL_MED,
        lw=1.15,
        label="causal median, 5 samples",
    )
    plot_time(
        ax_ph,
        times,
        t_s,
        ema,
        color=COL_EMA,
        lw=1.35,
        label=r"EMA 3 min, hold during pump",
    )
    mark_pumps(ax_ph, times, filt, up, down)
    ph_guides(ax_ph, (3.0, 7.0))
    style_ax(ax_ph)
    ax_ph.set_ylabel("pH")
    ax_ph.set_title(
        "Raw Nernst value, intra-burst filter, causal median, and EMA. "
        "Pump-on samples stay in the log; the EMA holds."
    )
    ax_ph.legend(loc="upper left", ncol=2)

    delta = filt - ema
    plot_time(ax_d, times, t_s, delta, color=COL_FILT, lw=0.9, label="burst $-$ EMA")
    ax_d.axhline(0.0, color=(0.3, 0.3, 0.3), lw=0.7)
    ax_d.axhline(0.15, color=COL_BAND, ls="--", lw=0.8)
    ax_d.axhline(-0.15, color=COL_BAND, ls="--", lw=0.8)
    mark_pumps(ax_d, times, delta, up, down)
    style_ax(ax_d)
    ax_d.set_ylabel(r"$\Delta$pH")
    ax_d.set_title(r"Burst filter minus EMA. Dashed lines are $\pm 0.15$.")
    ax_d.legend(loc="lower left")
    format_time(ax_d, span_hours(times))
    return fig


def page_tds_filters(sensor_rows, times, t_s, up, down, hold) -> plt.Figure:
    tds = as_float(sensor_rows, "tds_ppm")
    adc = as_float(sensor_rows, "tds_adc")
    med5 = causal_median(tds, 5)
    ema = causal_ema(t_s, tds, EMA_TAU.total_seconds(), hold)

    fig, axes = new_fig(3, 8.2, [1.0, 1.25, 1.0])
    fig.suptitle(
        r"TDS software low-pass, $\tau = 3$ min EMA",
        fontsize=13,
    )
    ax_a, ax_t, ax_d = axes

    plot_time(ax_a, times, t_s, adc, color=(0.25, 0.25, 0.25), lw=0.8)
    mark_pumps(ax_a, times, adc, up, down)
    style_ax(ax_a)
    ax_a.set_ylabel("TDS ADC")
    ax_a.set_title("Grove TDS count after the intra-burst average")
    ax_a.legend(loc="lower left")

    plot_time(ax_t, times, t_s, tds, color=COL_RAW, lw=0.8, label="firmware TDS")
    plot_time(
        ax_t, times, t_s, med5, color=COL_MED, lw=1.15, label="causal median, 5 samples"
    )
    plot_time(
        ax_t,
        times,
        t_s,
        ema,
        color=COL_EMA,
        lw=1.35,
        label=r"EMA 3 min, hold during pump",
    )
    mark_pumps(ax_t, times, tds, up, down)
    ax_t.axhline(TDS_DEAD[1], color=COL_BAND, ls="--", lw=0.8, zorder=0)
    style_ax(ax_t)
    ax_t.set_ylabel("TDS (ppm)")
    ax_t.set_title(
        "Same EMA used on pH. Dashed line: dead-zone maximum 1250 ppm."
    )
    lo = min(float(np.nanmin(tds)), TDS_DEAD[1]) - 15
    ax_t.set_ylim(lo, float(np.nanmax(tds)) + 15)
    ax_t.legend(loc="upper left")

    delta = tds - ema
    plot_time(
        ax_d, times, t_s, delta, color=COL_FILT, lw=0.9, label="firmware $-$ EMA"
    )
    ax_d.axhline(0.0, color=(0.3, 0.3, 0.3), lw=0.7)
    style_ax(ax_d)
    ax_d.set_ylabel(r"$\Delta$TDS (ppm)")
    ax_d.set_title("Firmware TDS minus the 3 min EMA")
    ax_d.legend(loc="best")
    format_time(ax_d, span_hours(times))
    return fig


def pump_clusters(hold: np.ndarray) -> list[np.ndarray]:
    idx = np.flatnonzero(hold)
    clusters: list[list[int]] = []
    for i in idx:
        if not clusters or i - clusters[-1][-1] > 3:
            clusters.append([int(i)])
        else:
            clusters[-1].append(int(i))
    return [np.array(c, dtype=int) for c in clusters]


def page_voltage_drop(ph_rows, times, up, down) -> plt.Figure:
    voltage = as_float(ph_rows, "ph_voltage")
    raw = as_float(ph_rows, "ph_raw")
    hold = up | down
    clusters = pump_clusters(hold)

    fig, axes = plt.subplots(2, 3, figsize=(10.6, 6.4))
    fig.suptitle(
        "pH-board voltage around each logged pump sample",
        fontsize=13,
    )
    fig.subplots_adjust(top=0.86, bottom=0.10, left=0.07, right=0.98, hspace=0.42, wspace=0.18)

    for ax, cluster in zip(axes.ravel(), clusters):
        i0 = int(cluster[0])
        i1 = int(cluster[-1])
        t_pump = times[i0]
        a = max(0, i0 - 8)
        b = min(len(times), i1 + 9)
        rel_s = np.array([(times[i] - t_pump).total_seconds() for i in range(a, b)])
        v = voltage[a:b]
        pre_v = voltage[a:i0]
        pre_v = pre_v[np.isfinite(pre_v)]
        baseline = float(np.median(pre_v)) if pre_v.size else float(v[0])
        pre_ph = raw[a:i0]
        pre_ph = pre_ph[np.isfinite(pre_ph)]
        which = "pH-up" if up[i0] else "pH-down"
        dv_mv = (float(np.median(voltage[cluster])) - baseline) * 1000.0
        dph = (
            float(np.median(raw[cluster]) - np.median(pre_ph)) if pre_ph.size else 0.0
        )

        ax.plot(rel_s, v, color=(0.25, 0.25, 0.25), lw=1.0)
        ax.axhline(baseline, color=COL_BAND, ls="--", lw=0.8)
        on = hold[a:b]
        ax.scatter(rel_s[on], v[on], s=26, c=[COL_UP if up[i0] else COL_DOWN], zorder=4)
        ymin = float(np.nanmin(v))
        ymax = float(np.nanmax(v))
        pad = max(0.004, 0.2 * (ymax - ymin))
        ax.set_ylim(ymin - pad, ymax + pad)
        style_ax(ax)
        ax.set_title(
            f"{which}, {t_pump.strftime('%d %b %H:%M')}\n"
            rf"$\Delta V = {dv_mv:+.0f}$ mV, $\Delta$pH $ = {dph:+.2f}$",
            fontsize=9,
        )
        ax.set_xlabel("Seconds from pump sample")

    axes[0, 0].set_ylabel("Board voltage (V)")
    axes[1, 0].set_ylabel("Board voltage (V)")
    return fig


def load_ph():
    rows = read_csv(PH_CSV)
    times = to_local([parse_utc(r["time_utc"]) for r in rows])
    t_s = epoch_s(times)
    up, down, hold = pump_masks(rows)
    return rows, times, t_s, up, down, hold


def load_sensor_aligned(ph_times: list[datetime]):
    """Sensor rows on the same clock as the pH extract, for the TDS figure."""
    rows = read_csv(SENSOR_CSV)
    times = to_local([parse_utc(r["recorded_at_utc"]) for r in rows])
    # Keep the overlap with the pH extract so both filter pages share an x range.
    t0, t1 = ph_times[0], ph_times[-1]
    keep = [i for i, t in enumerate(times) if t0 <= t <= t1]
    rows = [rows[i] for i in keep]
    times = [times[i] for i in keep]
    t_s = epoch_s(times)
    up, down, hold = pump_masks(rows)
    return rows, times, t_s, up, down, hold


def main() -> None:
    apply_style()
    ph_rows, ph_times, ph_t, up, down, hold = load_ph()
    sensor_rows, sensor_times, sensor_t, s_up, s_down, s_hold = load_sensor_aligned(
        ph_times
    )

    figures = [
        page_closed_loop(),
        page_weekend(),
        page_ph_filters(ph_rows, ph_times, ph_t, up, down, hold),
        page_tds_filters(sensor_rows, sensor_times, sensor_t, s_up, s_down, s_hold),
        page_voltage_drop(ph_rows, ph_times, up, down),
    ]

    OUT_PDF.parent.mkdir(parents=True, exist_ok=True)
    with PdfPages(OUT_PDF) as pdf:
        info = pdf.infodict()
        info["Title"] = "Characterization and sensor-filter figures, 25-27 Sep 2026"
        info["Author"] = "embedded-smart-garden"
        for fig, png in zip(figures, OUT_PNG):
            pdf.savefig(fig)
            fig.savefig(png, dpi=200)
            plt.close(fig)
            print(png)
    print(OUT_PDF)


if __name__ == "__main__":
    main()
