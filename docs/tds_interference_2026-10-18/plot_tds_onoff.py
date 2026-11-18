"""pH with the TDS probe unplugged, then plugged in (18 Oct 2026, pumps disconnected).

    python plot_tds_onoff.py  ->  tds_onoff.png / .pdf, and the statistics on stdout

tds_onoff.csv comes from extract_window.py. The TDS board was plugged in at
16:27 AEDT (05:27 UTC); the first reading with it on is the first non-zero TDS.
"""
import csv
import statistics as st
import sys
from datetime import datetime, timedelta
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.dates as mdates  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import report_style  # noqa: E402

report_style.apply()
AEDT = timedelta(hours=11)

t, ph, volts, tds = [], [], [], []
with open(HERE / "tds_onoff.csv", newline="") as f:
    for r in csv.DictReader(f):
        t.append(datetime.fromisoformat(r["received_at_utc"].replace("Z", "")) + AEDT)
        ph.append(float(r["ph"]))
        volts.append(float(r["ph_voltage"]))
        tds.append(float(r["tds_ppm"] or 0))

on_at = next(i for i, x in enumerate(tds) if x > 0)
# Steady windows only (AEDT): TDS off from 15:52, after the 15:45 restart dip
# had settled, to the plug-in at 16:27; TDS on from 16:33, once the plug-in
# dip had settled, to 16:38, when the probe came out for calibration.
d0 = t[0].replace(second=0, microsecond=0)
off = [i for i in range(on_at) if t[i] >= d0.replace(hour=15, minute=52)]
on = [i for i in range(on_at + 1, len(ph)) if t[i] >= d0.replace(hour=16, minute=33)]


def stats(idx, name):
    p = [ph[i] for i in idx]
    v = [volts[i] for i in idx]
    print(f"{name}: n={len(p)}  pH median {st.median(p):.3f}  sd {st.pstdev(p):.3f}  "
          f"range {min(p):.2f}-{max(p):.2f}  |  V median {st.median(v):.4f}  sd {st.pstdev(v):.4f}")
    return st.median(p), st.median(v)


ph_off, v_off = stats(off, "TDS unplugged")
ph_on, v_on = stats(on, "TDS connected")
print(f"plug-in reading: pH {ph[on_at]:.2f}, {volts[on_at]:.3f} V")
print(f"shift: {ph_on - ph_off:+.3f} pH, {1000 * (v_on - v_off):+.1f} mV")

fig, (a1, a2) = plt.subplots(2, 1, figsize=(6.0, 4.2), sharex=True,
                             gridspec_kw={"height_ratios": [2.2, 1]})
a1.axvspan(t[on_at], t[-1], color="#d9d9d9", alpha=0.5, lw=0)
a1.plot(t, ph, color=report_style.BLUE, lw=0.9, label="pH reading (10 s)")
a1.hlines(ph_off, t[off[0]], t[off[-1]], color=report_style.ORANGE, lw=1.4,
          label=f"median, TDS unplugged: {ph_off:.2f}")
a1.hlines(ph_on, t[on[0]], t[on[-1]], color=report_style.GREEN, lw=1.4,
          label=f"median, TDS connected: {ph_on:.2f}")
a1.annotate(f"plug-in transient\npH {ph[on_at]:.2f}", xy=(t[on_at], ph[on_at]),
            xytext=(t[on_at] - timedelta(minutes=14), ph[on_at] + 0.15),
            arrowprops={"arrowstyle": "->", "lw": 0.6}, fontsize=8)
a1.axhline(6.5, color="black", lw=0.6, ls="--")
a1.text(t[1], 6.52, "dead limit 6.5", fontsize=8, va="bottom")
a1.set_ylabel("pH")
a1.set_ylim(5.7, 6.8)
a1.legend(loc="lower left", fontsize=8)

a2.axvspan(t[on_at], t[-1], color="#d9d9d9", alpha=0.5, lw=0)
a2.plot(t, tds, color=report_style.PURPLE, lw=0.9)
a2.set_ylabel("TDS (ppm)")
a2.set_xlabel("Time, 18 Oct 2026 (AEDT)")
a2.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))

fig.tight_layout()
for ext in ("png", "pdf"):
    fig.savefig(HERE / f"tds_onoff.{ext}", dpi=200)
