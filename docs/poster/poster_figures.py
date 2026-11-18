"""Poster figures in the report style (LaTeX fonts, MATLAB axes), no titles or versions.

    python poster_figures.py

Writes (the firmware version is kept in the file name only, as an internal record):
  poster_bringup_plain_water_2026-10-07_fw1.9.9.png / .pdf
      Run 5: tap water to the vegetative band, TDS and pH, nutrient doses.
      Data ../bringup_2026-10-07_run5/export_run5.csv.gz, boot 27, first dose 06:12:47 UTC.
  poster_ph_correction_2026-10-07_fw1.9.12.png / .pdf
      pH step: two pH-up doses bring pH from 5.07 into the 5.5-6.5 band.
      Data ph_loop_1.9.12.csv.gz, boots 28-29, first dose 10:12:41 UTC.
Dose volumes are the delivered volumes logged on each recheck.
"""
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import report_style as rs  # noqa: E402

rs.apply(font_size=12)
BAND = "#dcefdc"
DEAD = rs.RED
RAW = "#9bbbe0"


def load(path, boots, t0_utc):
    d = pd.read_csv(path, low_memory=False)
    d = d[d.boot_id.isin(boots)].copy()
    d["t"] = pd.to_datetime(d["timestamp"], utc=True)
    d["min"] = (d["t"] - pd.Timestamp(t0_utc)).dt.total_seconds() / 60
    return d[d.kind == "reading"].sort_values("min"), d[d.kind == "control_event"].sort_values("min")


def metrics_box(ax, lines, loc=(0.99, 0.05), ha="right", va="bottom"):
    ax.text(*loc, "\n".join(lines), transform=ax.transAxes, ha=ha, va=va, fontsize=11,
            bbox=dict(boxstyle="square,pad=0.4", fc="white", ec="black", lw=0.6))


def dose_line(ax, m, text, color, y, ha="left"):
    ax.axvline(m, color=color, ls="--", lw=1.3, zorder=2)
    ax.annotate(text, (m, y), xytext=(4 if ha == "left" else -4, 0), textcoords="offset points",
                rotation=90, fontsize=10.5, color=color, va="top", ha=ha)


# ---- Bring-up from plain water (run 5, 7 Oct) ---------------------------------------------
r, ev = load(HERE.parent / "bringup_2026-10-07_run5" / "export_run5.csv.gz", [27],
             "2026-10-07T06:12:47Z")
r = r[(r["min"] >= -5) & (r["min"] <= 50)]
doses = ev[ev.event == "ec_recheck"][["min", "ec_dose_ml"]].values.tolist()
doses = [(m - 20.27 if i == 0 else m - 20.17, ml) for i, (m, ml) in enumerate(doses)]
doses = [(0.0, 12.393), (20.27, 7.9542)]             # issued at the ec_checks; delivered mL
settled = 40.43

fig, (a1, a2) = plt.subplots(2, 1, figsize=(8, 6), sharex=True,
                             gridspec_kw={"hspace": 0.08})
a1.axhspan(600, 800, color=BAND, zorder=0, label="target band")
for v in (550, 850):
    a1.axhline(v, color=DEAD, ls=":", lw=1.3)
a1.plot(r["min"], r.tds_ppm, ".", color=RAW, ms=3, label="reading")
a1.plot(r["min"], r.tds_ppm_ema, color=rs.BLUE, lw=2.2, label="controller input")
for m, ml in doses:
    dose_line(a1, m, f"nutrient {ml:.1f} mL", rs.GREEN, 880)
a1.axvline(settled, color="black", lw=1.4)
a1.set(ylabel="TDS (ppm)", ylim=(0, 900))
a1.legend(loc="center right", fontsize=10)
metrics_box(a1, [r"Tap water (44 ppm) to the band in 40 min",
                 r"2 nutrient doses, no overshoot (peak 600 ppm)"], loc=(0.985, 0.06))

a2.axhspan(5.8, 6.2, color=BAND, zorder=0)
for v in (5.5, 6.5):
    a2.axhline(v, color=DEAD, ls=":", lw=1.3)
a2.plot(r["min"], r.ph, ".", color=RAW, ms=3)
a2.plot(r["min"], r.ph_ema, color=rs.BLUE, lw=2.2)
for m, _ in doses:
    a2.axvline(m, color=rs.GREEN, ls="--", lw=1.3)
a2.axvline(settled, color="black", lw=1.4)
a2.annotate("both in range, no dose: 40 min", (settled, 7.45), xytext=(-5, 0),
            textcoords="offset points", ha="right", va="top", fontsize=11)
a2.set(ylabel="pH", ylim=(5.4, 7.6), xlabel="Time after the first dose (min)", xlim=(-5, 50))
metrics_box(a2, [r"pH 7.46 to 6.01 from the nutrient alone,",
                 r"no pH reagent needed"], loc=(0.985, 0.06))
stem = "poster_bringup_plain_water_2026-10-07_fw1.9.9"
for ext in ("png", "pdf"):
    fig.savefig(HERE / f"{stem}.{ext}")
plt.close(fig)

# ---- pH correction (7 Oct) -----------------------------------------------------------------
r, ev = load(HERE / "ph_loop_1.9.12.csv.gz", [28, 29], "2026-10-07T10:12:41Z")
r = r[(r["min"] >= -2) & (r["min"] <= 60)]
doses = [(0.0, 3.16), (20.1, 8.24 - 3.06 - 3.16)]
settled = 37.7

fig, ax = plt.subplots(figsize=(8, 4.2))
ax.axhspan(5.5, 6.5, color=BAND, zorder=0, label="pass band 5.5 to 6.5")
ax.plot(r["min"], r.ph, ".", color=RAW, ms=3, label="reading")
ax.plot(r["min"], r.ph_ema, color=rs.BLUE, lw=2.2, label="controller input")
for m, ml in doses:
    dose_line(ax, m, f"pH-up {ml:.1f} mL", rs.ORANGE, 6.05)
ax.axvline(settled, color="black", lw=1.4)
ax.annotate("in range,\nno dose:\n38 min", (settled, 6.42), xytext=(5, 0),
            textcoords="offset points", ha="left", va="top", fontsize=11)
ax.set(ylabel="pH", ylim=(5.0, 6.5), xlabel="Time after the first pH-up dose (min)",
       xlim=(-2, 50))
ax.legend(loc="upper left", fontsize=10)
start = r[r["min"] <= 0].ph_ema.dropna().iloc[-1]
end = r[r["min"] >= settled].ph_ema.dropna().iloc[0]
metrics_box(ax, [rf"pH {start:.2f} to {end:.2f} in {settled:.0f} min",
                 r"2 pH-up doses, no overshoot"], loc=(0.985, 0.06))
stem = "poster_ph_correction_2026-10-07_fw1.9.12"
for ext in ("png", "pdf"):
    fig.savefig(HERE / f"{stem}.{ext}")
print("saved; pH", round(start, 2), "->", round(end, 2))
