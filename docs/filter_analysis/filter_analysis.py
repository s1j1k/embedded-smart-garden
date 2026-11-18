"""Sensor filter analysis for the report's filtering section.

Data: docs/autonomous_run_2026-09-30/sensor_readings.csv (10 s telemetry,
30 Sep - 5 Oct 2026, firmware 1.6.0, 3 min EMA on pH and TDS).

What it does
  1. Noise and spectrum (FFT / Welch PSD) of the burst-filtered pH and TDS
     readings in the longest quiet stretch (no pump within -5/+40 min).
  2. Lag-versus-noise trade-off of the first-order EMA (current firmware)
     against 2nd- and 4th-order Butterworth low-pass filters, on the measured
     noise and a unit step.
  3. The filters replayed causally on two real dose steps.
  4. Frequency response of the 16-sample ADC burst average at mains
     frequencies (the stage before the 10 s samples).
  5. The other sensors over one quiet day, with their existing filtering.

Run:  python docs/filter_analysis/filter_analysis.py
Writes PNG figures and filter_summary.csv next to this file.
A MATLAB version of steps 1-4 is in filter_analysis.m.
"""
from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parents[1]))
import report_style  # noqa: E402  LaTeX fonts, MATLAB axes
import numpy as np
import pandas as pd
from scipy import signal

HERE = Path(__file__).resolve().parent
DATA = HERE.parent / "autonomous_run_2026-09-30" / "sensor_readings.csv"
DT = 10.0            # s, telemetry period
FS = 1.0 / DT        # Hz
TAU_EMA = 180.0      # s, firmware SensorEma::TAU_S
TAU_LIGHT = 30.0     # s, candidate light filter for the control input
FC_CHOSEN = 0.002    # Hz, Butterworth-2 cutoff now in firmware (shared/sensor_ema.h)
PUMPS = ["nutrient_pump_on", "ph_up_pump_on", "ph_down_pump_on", "water_pump_on"]

report_style.apply()


# --------------------------------------------------------------------------
# Loading
# --------------------------------------------------------------------------
def load() -> pd.DataFrame:
    d = pd.read_csv(DATA, low_memory=False)
    d = d[(d.row_source == "live") & (d.hours_since_plain_water >= 0)].copy()
    d["t"] = pd.to_datetime(d.time_utc, utc=True)
    d = d.sort_values("t").reset_index(drop=True)
    d["ts"] = (d.t - d.t.iloc[0]).dt.total_seconds()
    for p in PUMPS:
        d[p] = d[p].astype(str).str.lower().isin(["1", "true", "1.0"])
    d["pump"] = d[PUMPS].any(axis=1)
    return d


def quiet_mask(d: pd.DataFrame, before_s=300.0, after_s=2400.0) -> np.ndarray:
    """True where no pump ran within -before/+after of the sample."""
    quiet = np.ones(len(d), bool)
    for tp in d.ts[d.pump].to_numpy():
        quiet &= ~((d.ts.to_numpy() > tp - before_s) & (d.ts.to_numpy() < tp + after_s))
    return quiet


def longest_run(d: pd.DataFrame, mask: np.ndarray, max_gap=30.0) -> tuple[int, int]:
    """Index range of the longest stretch where mask holds and sampling has no gap."""
    ts = d.ts.to_numpy()
    best, start = (0, 0), None
    for i in range(len(d)):
        ok = mask[i] and (i == 0 or ts[i] - ts[i - 1] <= max_gap)
        if ok and start is None:
            start = i
        if (not ok or i == len(d) - 1) and start is not None:
            end = i if ok else i - 1
            if end - start > best[1] - best[0]:
                best = (start, end)
            start = None if not ok else start
            if not mask[i]:
                start = None
            elif not ok:
                start = i
    return best


def uniform(d: pd.DataFrame, col: str, i0: int, i1: int) -> tuple[np.ndarray, np.ndarray]:
    seg = d.iloc[i0:i1 + 1]
    t = np.arange(seg.ts.iloc[0], seg.ts.iloc[-1], DT)
    x = np.interp(t, seg.ts.to_numpy(), seg[col].astype(float).to_numpy())
    return t, x


# --------------------------------------------------------------------------
# Filters (same discrete forms the ESP32 would run)
# --------------------------------------------------------------------------
def ema_ba(tau: float) -> tuple[np.ndarray, np.ndarray]:
    a = 1.0 - np.exp(-DT / tau)
    return np.array([a]), np.array([1.0, a - 1.0])


def butter_ba(order: int, fc: float) -> tuple[np.ndarray, np.ndarray]:
    return signal.butter(order, fc, btype="low", fs=FS)


def butter2_closed_form(fc: float) -> tuple[np.ndarray, np.ndarray]:
    """2nd-order Butterworth by bilinear transform with pre-warping (ESP32 form)."""
    k = np.tan(np.pi * fc / FS)
    q = np.sqrt(2.0)
    norm = 1.0 / (1.0 + q * k + k * k)
    b = np.array([k * k, 2 * k * k, k * k]) * norm
    a = np.array([1.0, 2 * (k * k - 1) * norm, (1 - q * k + k * k) * norm])
    return b, a


def step_metrics(b, a, n=2000) -> dict:
    y = signal.lfilter(b, a, np.ones(n))
    t = np.arange(n) * DT
    outside = np.abs(y - 1.0) > 0.05
    t95 = t[np.nonzero(outside)[0][-1] + 1] if outside.any() else 0.0
    t50 = t[np.argmax(y >= 0.5)]
    return {"t95_s": t95, "t50_s": t50, "overshoot_pct": max(0.0, (y.max() - 1.0) * 100)}


def noise_out(b, a, noise: np.ndarray, warm=400) -> float:
    y = signal.lfilter(b, a, noise - noise.mean())
    return float(np.std(y[warm:]))


def ema_t95(tau):
    return step_metrics(*ema_ba(tau))["t95_s"]


# --------------------------------------------------------------------------
# 1. Noise and spectrum
# --------------------------------------------------------------------------
def residual(x: np.ndarray, win=181) -> np.ndarray:
    """High-pass residual: x minus a centred 30 min moving average."""
    k = np.ones(win) / win
    trend = np.convolve(x, k, mode="same")
    r = (x - trend)[win:-win]
    return r


def spectra(d, i0, i1, summary):
    """PSD of pH and TDS with the old EMA and the chosen Butterworth, and the
    filters' magnitude responses with the cutoff and band choices marked."""
    fig, axes = plt.subplots(2, 2, figsize=(10.5, 6.6), sharex=True,
                             gridspec_kw={"height_ratios": [2.2, 1]})
    f_mix = 1.0 / (2 * np.pi * 108.0)          # pH mixing corner, tau_m = 1.8 min
    f_ema = 1.0 / (2 * np.pi * TAU_EMA)
    b_bw, a_bw = butter_ba(2, FC_CHOSEN)
    b_e, a_e = ema_ba(TAU_EMA)
    for col_i, (col, ema_col, label, unit) in enumerate([
            ("ph_filtered", "ph_ema", "pH", "pH"),
            ("tds_ppm", "tds_ppm_ema", "TDS", "ppm")]):
        ax, axh = axes[0, col_i], axes[1, col_i]
        t, x = uniform(d, col, i0, i1)
        _, xe = uniform(d, ema_col, i0, i1)
        xb = signal.lfilter(b_bw, a_bw, x, zi=signal.lfilter_zi(b_bw, a_bw) * x[0])[0]
        nper = min(1024, len(x) // 4)
        f, p = signal.welch(x, fs=FS, nperseg=nper, detrend="linear")
        _, pe = signal.welch(xe, fs=FS, nperseg=nper, detrend="linear")
        _, pb = signal.welch(xb, fs=FS, nperseg=nper, detrend="linear")
        fm = f[1:] * 1e3
        # bands
        ax.axvspan(fm[0], f_mix * 1e3, color="#77AC30", alpha=0.07)
        ax.axvspan(f_mix * 1e3, 5.0, color="#D95319", alpha=0.07)
        ax.axvspan(5.0, 50.0, color="#A2142F", alpha=0.06)
        ytxt = p[1:].max() * 0.4
        ax.text(fm[0] * 1.1, ytxt, "signal:\ndrift and\ndose steps", fontsize=7, va="top")
        ax.text(f_mix * 1e3 * 1.05, ytxt, "transition", fontsize=7, va="top")
        ax.text(6.0, ytxt, "noise floor\n(white)", fontsize=7, va="top")
        ax.loglog(fm, p[1:], color="0.45", label="burst reading (input)")
        ax.loglog(fm, pe[1:], color="#77AC30", lw=1, label="3 min EMA (firmware up to 1.8)")
        ax.loglog(fm, pb[1:], color="#0072BD", label=f"Butterworth-2, {FC_CHOSEN*1e3:g} mHz (chosen)")
        ax.axvline(FC_CHOSEN * 1e3, color="#0072BD", ls="--", lw=1.2)
        ax.axvline(f_ema * 1e3, color="#77AC30", ls="--", lw=1)
        ax.set_ylim(p[1:].min() * 1e-4, p[1:].max() * 3)
        ax.set_ylabel(f"PSD ({unit}$^2$/Hz)")
        ax.set_title(f"{label}: {len(x) * DT / 3600:.1f} h quiet window, 10 s samples")
        ax.legend(fontsize=7, loc="lower left")
        # magnitude responses
        w = f[1:]
        _, he = signal.freqz(b_e, a_e, worN=w, fs=FS)
        _, hb = signal.freqz(b_bw, a_bw, worN=w, fs=FS)
        axh.semilogx(fm, 20 * np.log10(np.abs(he)), color="#77AC30", label="EMA 3 min")
        axh.semilogx(fm, 20 * np.log10(np.abs(hb)), color="#0072BD",
                     label=f"Butterworth-2 {FC_CHOSEN*1e3:g} mHz")
        axh.axhline(-3, color="0.5", lw=0.8, ls=":")
        axh.axvline(FC_CHOSEN * 1e3, color="#0072BD", ls="--", lw=1.2)
        axh.axvline(f_ema * 1e3, color="#77AC30", ls="--", lw=1)
        axh.axvline(f_mix * 1e3, color="#A2142F", ls=":", lw=1)
        axh.text(f_mix * 1e3 * 1.05, -55, "mixing\ncorner", fontsize=7, color="#A2142F")
        axh.set_ylim(-60, 5)
        axh.set_ylabel("Gain (dB)")
        axh.set_xlabel("Frequency (mHz)   [Nyquist 50 mHz]")
        axh.legend(fontsize=7, loc="lower left")
        # numbers
        r = residual(x)
        rho1 = float(np.corrcoef(r[:-1], r[1:])[0, 1])
        hi = f >= 5e-3
        summary += [
            {"item": f"{label} burst noise std ({unit})", "value": float(np.std(r))},
            {"item": f"{label} burst noise lag-1 autocorrelation", "value": rho1},
            {"item": f"{label} EMA output noise std ({unit})", "value": float(np.std(residual(xe)))},
            {"item": f"{label} Butterworth-2 {FC_CHOSEN*1e3:g} mHz output noise std ({unit})",
             "value": float(np.std(residual(xb)))},
            {"item": f"{label} share of burst variance above 5 mHz (white floor)",
             "value": float(np.trapezoid(p[hi], f[hi]) / np.trapezoid(p[1:], f[1:]))},
        ]
    fig.suptitle("Frequency analysis of pH and TDS (Welch PSD) and the chosen low-pass cutoff")
    fig.savefig(HERE / "filter_psd.png")
    plt.close(fig)


# --------------------------------------------------------------------------
# 2. Lag versus noise trade-off
# --------------------------------------------------------------------------
def tradeoff(d, i0, i1, summary):
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.8))
    chosen = {}
    for ax, (col, label, unit) in zip(axes, [("ph_filtered", "pH", "pH"),
                                             ("tds_ppm", "TDS", "ppm")]):
        _, x = uniform(d, col, i0, i1)
        r = residual(x)
        taus = np.geomspace(20, 900, 40)
        e_t95 = [ema_t95(tau) / 60 for tau in taus]
        e_n = [noise_out(*ema_ba(tau), r) for tau in taus]
        ax.plot(e_t95, e_n, label="EMA (1st order)", color="#77AC30")
        fcs = np.geomspace(2e-4, 2.5e-2, 60)
        for order, colr in [(2, "#0072BD"), (4, "#7E2F8E")]:
            pts = [(step_metrics(*butter_ba(order, fc))["t95_s"] / 60,
                    noise_out(*butter_ba(order, fc), r)) for fc in fcs]
            ax.plot(*zip(*pts), label=f"Butterworth order {order}", color=colr)
        n_ema = noise_out(*ema_ba(TAU_EMA), r)
        t_ema = ema_t95(TAU_EMA) / 60
        ax.plot(t_ema, n_ema, "o", color="#77AC30", ms=7, label="firmware EMA, 3 min")
        m_ch = step_metrics(*butter_ba(2, FC_CHOSEN))
        n_ch = noise_out(*butter_ba(2, FC_CHOSEN), r)
        ax.plot(m_ch["t95_s"] / 60, n_ch, "D", color="#A2142F", ms=7,
                label=f"Butterworth-2, {FC_CHOSEN*1e3:g} mHz (chosen)")
        summary += [{"item": f"{label} chosen Butterworth-2: t95 (min)", "value": m_ch["t95_s"] / 60},
                    {"item": f"{label} chosen Butterworth-2: output noise ({unit})", "value": n_ch}]
        n30, t30 = noise_out(*ema_ba(TAU_LIGHT), r), ema_t95(TAU_LIGHT) / 60
        ax.plot(t30, n30, "^", color="#D95319", ms=7, label=f"EMA {TAU_LIGHT:.0f} s (proposed)")
        summary += [{"item": f"{label} EMA 30 s: t95 (min)", "value": t30},
                    {"item": f"{label} EMA 30 s: output noise ({unit})", "value": n30}]
        # Butterworth-2 with the same output noise as the firmware EMA
        fc_eq = min(fcs, key=lambda fc: abs(noise_out(*butter_ba(2, fc), r) - n_ema))
        m_eq = step_metrics(*butter_ba(2, fc_eq))
        ax.plot(m_eq["t95_s"] / 60, n_ema, "s", color="#0072BD", ms=7,
                label=f"Butterworth-2, same noise (fc {fc_eq*1e3:.2f} mHz)")
        ax.axhline(np.std(r), ls=":", color="0.5", lw=1)
        ax.text(0.2, np.std(r) * 1.05, "unfiltered", fontsize=7, color="0.4")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("Time to settle within 5 % of a step (min)")
        ax.set_ylabel(f"Output noise std ({unit})")
        ax.set_title(label)
        ax.legend(fontsize=6.5)
        chosen[label] = (fc_eq, m_eq, n_ema, t_ema)
        summary += [
            {"item": f"{label} EMA 3 min: t95 (min)", "value": t_ema},
            {"item": f"{label} EMA 3 min: output noise ({unit})", "value": n_ema},
            {"item": f"{label} Butterworth-2 equal-noise cutoff (mHz)", "value": fc_eq * 1e3},
            {"item": f"{label} Butterworth-2 equal-noise t95 (min)", "value": m_eq["t95_s"] / 60},
            {"item": f"{label} Butterworth-2 equal-noise overshoot (%)",
             "value": m_eq["overshoot_pct"]},
        ]
    fig.suptitle("Lag versus noise: EMA and Butterworth low-pass filters on the measured noise")
    fig.savefig(HERE / "filter_tradeoff.png")
    plt.close(fig)
    return chosen


# --------------------------------------------------------------------------
# 3. Replay on real dose steps
# --------------------------------------------------------------------------
def held_input(seg: pd.DataFrame, col: str) -> np.ndarray:
    """Burst readings with pump-on samples held (as the firmware does)."""
    x = seg[col].astype(float).to_numpy().copy()
    pump = seg.pump.to_numpy()
    for i in range(1, len(x)):
        if pump[i] or np.isnan(x[i]):
            x[i] = x[i - 1]
    return x


def replay(d, chosen, summary):
    events = [("2026-10-01T00:07:00Z", "ph_filtered", "pH", "pH-up dose, 1 Oct (TDS 170 ppm)"),
              ("2026-10-02T22:06:00Z", "tds_ppm", "TDS", "Nutrient step, 3 Oct (+195 ppm)")]
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.6))
    for ax, (when, col, label, title) in zip(axes, events):
        t0 = pd.Timestamp(when)
        seg = d[(d.t > t0 - pd.Timedelta(minutes=60)) & (d.t < t0 + pd.Timedelta(minutes=45))]
        x = held_input(seg, col)
        tm = (seg.t - t0).dt.total_seconds().to_numpy() / 60
        pre = x[tm < -1]
        zi_e = signal.lfilter_zi(*ema_ba(TAU_EMA)) * pre[0]
        y_ema, _ = signal.lfilter(*ema_ba(TAU_EMA), x, zi=zi_e)
        fc = FC_CHOSEN
        b2, a2 = butter_ba(2, fc)
        y_b2, _ = signal.lfilter(b2, a2, x, zi=signal.lfilter_zi(b2, a2) * pre[0])
        ax.plot(tm, x, color="0.6", lw=0.8, label="burst reading")
        ax.plot(tm, y_ema, color="#77AC30", label="EMA 3 min (firmware)")
        ax.plot(tm, y_b2, color="#0072BD", label=f"Butterworth-2, {fc*1e3:g} mHz (chosen)")
        b30, a30 = ema_ba(TAU_LIGHT)
        y_30, _ = signal.lfilter(b30, a30, x, zi=signal.lfilter_zi(b30, a30) * pre[0])
        ax.plot(tm, y_30, color="#D95319", lw=1.0, label=f"EMA {TAU_LIGHT:.0f} s (proposed)")
        base = np.median(pre[-30:])
        final = np.median(x[(tm > 25) & (tm < 40)])
        for y, name in [(y_ema, "EMA"), (y_b2, "Butterworth-2"), (y_30, "EMA 30 s"),
                        (x, "burst reading")]:
            # Settled: stays within a control-relevant tolerance of the final value
            # (0.1 pH / 10 ppm, a fifth of the dead-limit half-width or less) until
            # 35 min. A % band is not used: after a pH-up dose the pH peaks and then
            # relaxes slowly, which a % band would count against the fast filters.
            tol = 0.1 if label == "pH" else 10.0
            after = (tm >= 0) & (tm <= 35)
            outside = np.nonzero(np.abs(y[after] - final) > tol)[0]
            t_set = tm[after][outside[-1] + 1] if len(outside) else 0.0
            summary.append({"item": f"{label} dose step: {name} within {tol:g} of final at (min)",
                            "value": float(t_set)})
        ax.axvline(0, color="k", lw=0.8)
        ax.set_xlim(-15, 40)
        ax.set_xlabel("Minutes from pump start")
        ax.set_ylabel("pH" if label == "pH" else "TDS (ppm)")
        ax.set_title(title)
        ax.legend(fontsize=7)
    fig.suptitle("Filters replayed on real dose steps (causal, pump-on samples held)")
    fig.savefig(HERE / "filter_dose_steps.png")
    plt.close(fig)


# --------------------------------------------------------------------------
# 4. ADC burst average at mains frequencies
# --------------------------------------------------------------------------
def burst_response(summary):
    f = np.linspace(0.1, 250, 5000)
    fig, ax = plt.subplots(figsize=(6, 3))
    for ts_ms, colr in [(5.0, "#0072BD"), (5.2, "#D95319")]:
        n, ts = 16, ts_ms / 1000
        h = np.abs(np.sin(np.pi * f * n * ts) / (n * np.sin(np.pi * f * ts)))
        ax.semilogy(f, h, color=colr, label=f"16 samples, {ts_ms} ms apart")
        h50 = abs(np.sin(np.pi * 50 * n * ts) / (n * np.sin(np.pi * 50 * ts)))
        summary.append({"item": f"burst average gain at 50 Hz, {ts_ms} ms spacing",
                        "value": float(h50)})
    for m in (50, 100, 150, 200):
        ax.axvline(m, color="0.6", ls=":", lw=0.8)
    ax.set_ylim(1e-3, 1.5)
    ax.set_xlabel("Frequency (Hz)")
    ax.set_ylabel("Gain")
    ax.set_title("ADC burst average: response at mains and LED-flicker frequencies")
    ax.legend(fontsize=7)
    fig.savefig(HERE / "filter_burst_response.png")
    plt.close(fig)


# --------------------------------------------------------------------------
# 5. Other sensors
# --------------------------------------------------------------------------
def other_sensors(d, summary):
    t0 = d.t.iloc[0] + pd.Timedelta(hours=72)
    seg = d[(d.t >= t0) & (d.t < t0 + pd.Timedelta(hours=24))]
    hrs = (seg.t - t0).dt.total_seconds() / 3600
    panels = [
        ("water_temp_c", "Water temp (°C)", "DS18B20: mean of 3 conversions"),
        ("water_level_raw_pct", "Water level (%)", "Grove level: mean of reads + trust filter"),
        ("ambient_temp_c", "Air temp (°C)", "DHT11: mean of 3 reads (1 °C steps)"),
        ("ambient_humidity_pct", "Humidity (%RH)", "DHT11: mean of 3 reads (1 % steps)"),
        ("co2_ppm", "CO$_2$ (ppm)", "PAS CO$_2$: on-chip processing, single shot"),
        ("light_adc", "Light (ADC)", "LDR: 16-sample burst average"),
    ]
    fig, axes = plt.subplots(3, 2, figsize=(10, 7), sharex=True)
    for ax, (col, ylab, note) in zip(axes.ravel(), panels):
        if col == "water_level_raw_pct":
            # Level changes over days, so show the whole run (twin x-axis label below).
            full_h = d.hours_since_plain_water.astype(float)
            raw = d[col].astype(float).where(d[col].astype(float) >= 0)
            ax2 = ax.twiny()
            ax2.plot(full_h, raw, lw=0.6, color="0.35", label="raw")
            ax2.plot(full_h, d.water_level_pct.astype(float).where(d.water_level_pct >= 0),
                     lw=1.0, color="#0072BD", label="trusted (control input)")
            ax2.set_xlabel("Hours since plain water (whole run)", fontsize=7)
            ax2.legend(fontsize=7, loc="lower left")
            ax.set_xticks([])
            y = raw
        else:
            y = seg[col].astype(float).where(seg[col].astype(float) >= 0)
            ax.plot(hrs, y, lw=0.6, color="0.35")
        diffs = np.diff(y.dropna().to_numpy())
        noise = float(np.std(diffs) / np.sqrt(2)) if len(diffs) else np.nan
        steps = np.unique(np.round(np.abs(diffs[diffs != 0]), 3)) if len(diffs) else []
        res = float(steps[0]) if len(steps) else np.nan
        ax.set_ylabel(ylab)
        ax.set_title(f"{note}\nsample-to-sample noise {noise:.3g}, smallest step {res:.3g}",
                     fontsize=8)
        summary.append({"item": f"{col} sample noise (std diff/sqrt2)", "value": noise})
        summary.append({"item": f"{col} smallest step", "value": res})
    for ax in axes[-1]:
        ax.set_xlabel("Hours (one quiet day, 3-4 Oct)")
    fig.suptitle("Other sensors with their existing filtering, 10 s telemetry")
    fig.savefig(HERE / "filter_other_sensors.png")
    plt.close(fig)


def main():
    d = load()
    q = quiet_mask(d)
    i0, i1 = longest_run(d, q)
    summary = [{"item": "quiet window start (UTC)", "value": str(d.t.iloc[i0])},
               {"item": "quiet window length (h)", "value": (d.ts.iloc[i1] - d.ts.iloc[i0]) / 3600}]
    # Check the closed-form ESP32 biquad against scipy.
    fc_chk = 2e-3
    b_s, a_s = butter_ba(2, fc_chk)
    b_c, a_c = butter2_closed_form(fc_chk)
    summary.append({"item": "closed-form vs scipy Butterworth-2 max coeff diff",
                    "value": float(max(np.abs(b_s - b_c).max(), np.abs(a_s - a_c).max()))})
    spectra(d, i0, i1, summary)
    chosen = tradeoff(d, i0, i1, summary)
    replay(d, chosen, summary)
    burst_response(summary)
    other_sensors(d, summary)
    out = pd.DataFrame(summary)
    out.to_csv(HERE / "filter_summary.csv", index=False)
    with pd.option_context("display.max_colwidth", 80, "display.width", 140):
        print(out.to_string(index=False))


if __name__ == "__main__":
    main()
