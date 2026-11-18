"""Simulated plain-water bring-up on firmware 1.9.8 (prediction before the real run).

Plant (same structure as sim/plant_derivs.m): volume V, dissolved solids with
first-order mixing to the probe, charge-balance pH with one effective buffer, the
nutrient's acidity released with a lag tau_a, pH-up as strong base + carbonate.
Constants from the 6-7 Oct runs; pH-up strength calibrated so a dose gives
~0.3 pH/mL at 8.4 L and ~700 ppm, as measured on 7 Oct.

Controller = firmware 1.9.8: Butterworth-2 control filter (10 s samples), dead
band, fixed tuned gains (nutrient 260 ppm L/mL / V; pH-up 2.52 pH L/mL / V x
700/TDS capped at the bottle rate), lambda 0.8, caps, 20 min waits with settle
detection, settle-paced cycle.

    python sim_bringup_198.py  ->  sim_bringup_1.9.8.png / .pdf, prints the timeline
"""
import math
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

TAU_M = float(sys.argv[1]) * 60 if len(sys.argv) > 1 else 5 * 60   # mixing time constant, s
GAMMA = float(sys.argv[2]) if len(sys.argv) > 2 else 245.0        # nutrient, ppm L / mL
PH0 = float(sys.argv[3]) if len(sys.argv) > 3 else 6.55           # tap-water pH at the start
NUTRIENT_FIRST = len(sys.argv) > 4 and sys.argv[4] == "nutrient-first"  # finish TDS before pH

# ---------------- plant ---------------------------------------------------------------
V = 7.84
P = dict(pKa=6.8, Kw=1e-14, cTap=0.30, bTap=0.1077, alpha_N=0.022, c_N=0.050,
         beta_up=0.50, c_up=0.25, tau_a=20 * 60, kappa_up=250.0)


def ph_of(B, C):
    b, c = B * 1e-3 / V, C * 1e-3 / V
    lo, hi = 1.0, 13.0
    for _ in range(45):
        m = 0.5 * (lo + hi)
        h = 10 ** -m
        f = b + h - P["Kw"] / h - c / (1 + 10 ** (P["pKa"] - m))
        lo, hi = (m, hi) if f > 0 else (lo, m)
    return 0.5 * (lo + hi)


def calibrate_beta_up():
    """beta_up so 1 mL of pH-up raises pH by ~0.3 x 8.4/V at ~700 ppm, pH ~5.3."""
    n = (700 - 9) * V / GAMMA
    C = P["cTap"] * V + P["c_N"] * n
    B0 = P["bTap"] * V - P["alpha_N"] * n
    target = 0.30 * 8.4 / V
    lo, hi = 0.02, 2.0
    for _ in range(40):
        beta = 0.5 * (lo + hi)
        # base B so that pH starts at 5.3
        Bs = None
        bl, bh = -5.0, 5.0
        for _ in range(60):
            bm = 0.5 * (bl + bh)
            if ph_of(bm, C) < 5.3:
                bl = bm
            else:
                bh = bm
        Bs = 0.5 * (bl + bh)
        gain = ph_of(Bs + beta, C + P["c_up"]) - 5.3
        lo, hi = (beta, hi) if gain < target else (lo, beta)
    return 0.5 * (lo + hi)


P["beta_up"] = calibrate_beta_up()

# ---------------- controller constants (firmware 1.9.8) -------------------------------
capV = min(max(V * 0.9, 2.0), 7.0)
K_EC = 31 * 8.4 / capV
LABEL = 100 / (10 * capV)
B_BW = [0.0036217, 0.0072434, 0.0036217]
A_BW = [1.0, -1.822695, 0.837182]


class BW:
    def __init__(self):
        self.z = None

    def step(self, x):
        if self.z is None:
            self.z = [B_BW[1] * x - A_BW[1] * x + (B_BW[2] * x - A_BW[2] * x), B_BW[2] * x - A_BW[2] * x]
        y = B_BW[0] * x + self.z[0]
        z1 = B_BW[1] * x - A_BW[1] * y + self.z[1]
        self.z[1] = B_BW[2] * x - A_BW[2] * y
        self.z[0] = z1
        return y


def ph_gain(tds):
    tds = max(tds, 50.0)
    return min(LABEL, 2.52 / capV * 700.0 / tds)


# ---------------- simulation ------------------------------------------------------------
dt = 1.0
T_END = 4 * 3600
M = 9.0 * V                     # mg dissolved solids (tap 9 ppm)
C = P["cTap"] * V               # mmol buffer
_bl, _bh = -5.0, 5.0            # base equivalents giving the starting pH
for _ in range(60):
    _bm = 0.5 * (_bl + _bh)
    _bl, _bh = (_bm, _bh) if ph_of(_bm, C) < PH0 else (_bl, _bm)
B = 0.5 * (_bl + _bh)
Pend = 0.0                      # mmol nutrient acidity not yet released
tdsP, phP = 9.0, ph_of(B, C)    # at the probe
pumps = {"N": 0.0, "U": 0.0}    # mL still to deliver
rate = {"N": 0.82, "U": 0.80}
bwT, bwP = BW(), BW()
tdsF = phF = None
histT, histP = [], []           # (t, value) one per minute for settle detection
log, doses = [], []
state, t_wait, nominal, cycle_dosed, settled_at = "check", 0.0, 0.0, False, None
skip_ec = False
pending = None                  # ("EC"/"PH", t_dose, before)


def slope(hist, window=300):
    pts = [(t, v) for t, v in hist if t >= hist[-1][0] - window] if hist else []
    if len(pts) < 2 or pts[-1][0] - pts[0][0] < 0.8 * window:
        return float("nan")
    return (pts[-1][1] - pts[0][1]) / ((pts[-1][0] - pts[0][0]) / 60)


t = 0.0
while t < T_END:
    # pumps -> plant
    qN = min(rate["N"], pumps["N"] / dt); pumps["N"] -= qN * dt
    qU = min(rate["U"], pumps["U"] / dt); pumps["U"] -= qU * dt
    M += GAMMA * qN * dt + P["kappa_up"] * qU * dt
    rel = Pend / P["tau_a"] * dt
    Pend += P["alpha_N"] * qN * dt - rel
    B += P["beta_up"] * qU * dt - rel
    C += (P["c_N"] * qN + P["c_up"] * qU) * dt
    tdsP += (M / V - tdsP) / TAU_M * dt
    phP += (ph_of(B, C) - phP) / (TAU_M + 30) * dt

    if int(t) % 10 == 0:                    # sensor read + control every 10 s
        tdsF, phF = bwT.step(tdsP), bwP.step(phP)
        if int(t) % 60 == 0:
            histT.append((t, tdsF)); histP.append((t, phF))
        log.append((t / 60, tdsP, phP, tdsF, phF))

        if state == "check" and t >= t_wait:
            cycle_dosed = False
            if tdsF < 550 and not skip_ec:
                e = 700 - tdsF
                tgt = 700 if e > 200 else 800
                u = min(0.8 * e / K_EC, (tgt - tdsF) / K_EC, 25.0)
                pumps["N"] += u; doses.append((t / 60, "Nutrient", u)); cycle_dosed = True
                state, t_wait, histT, histP = "ec_wait", t + u / rate["N"] + 20 * 60, [], []
            else:
                skip_ec = False
                state, t_wait, nominal = "ph_check", t, t
        elif state == "ec_wait" and t >= t_wait:
            s = slope(histT)
            extra = t - (t_wait if not histT else t_wait)
            if (math.isnan(s) or s > 2.0) and t < (doses[-1][0] * 60 + 40 * 60):
                t_wait = t + 300
            else:
                if math.isnan(s) or s > 2.0:
                    skip_ec = True
                if NUTRIENT_FIRST and tdsF < 550 and not skip_ec:
                    state, t_wait = "check", t          # more nutrient before pH
                else:
                    state, t_wait, nominal = "ph_check", t, t
        elif state == "ph_check" and t >= t_wait:
            s = slope(histP)
            if (math.isnan(s) or abs(s) > 0.006) and t - nominal < 20 * 60:
                t_wait = t + 300
            else:
                if phF < 5.5:
                    e = 6.0 - phF
                    k = ph_gain(tdsF)
                    u = min(0.8 * e / k, e / k, 3.0)
                    pumps["U"] += u; doses.append((t / 60, "pH-up", u)); cycle_dosed = True
                    state, t_wait, nominal, histP = "ph_settle_wait", t + u / rate["U"] + 20 * 60, \
                        t + u / rate["U"] + 20 * 60, []
                else:
                    if not cycle_dosed and 550 <= tdsF <= 850 and settled_at is None:
                        settled_at = t / 60
                    state, t_wait = "check", t + (0 if cycle_dosed else 40 * 60)
        elif state == "ph_settle_wait" and t >= t_wait:
            state, t_wait, nominal = "ph_check", t, t_wait
    if settled_at is not None and t / 60 > settled_at + 30:
        break
    t += dt

first = doses[0][0] if doses else 0.0
print(f"beta_up calibrated {P['beta_up']:.3f} mmol/mL; tau_m {TAU_M/60:.0f} min; gamma {GAMMA:.0f}")
for m, pump, u in doses:
    print(f"  {m - first:6.1f} min  {pump:8s} {u:5.2f} mL")
peak = max(r[1] for r in log)
print(f"peak TDS {peak:.0f} ppm, final TDS {log[-1][3]:.0f}, final pH {log[-1][4]:.2f}, "
      f"min pH {min(r[2] for r in log):.2f}")
print("settled (no-dose check, both inside) at",
      f"{settled_at - first:.0f} min after the first dose" if settled_at else "not within the run")

# ---------------- plot ----------------------------------------------------------------------
TEAL, GREEN, CORAL, BAND = "#0b5d7a", "#3a9d5d", "#e4572e", "#cfe8d6"
plt.rcParams.update({"font.size": 13, "axes.spines.top": False, "axes.spines.right": False})
x = [r[0] - first for r in log]
fig, (a1, a2) = plt.subplots(2, 1, figsize=(9, 6.4), sharex=True)
a1.axhspan(600, 800, color=BAND); [a1.axhline(v, color=CORAL, ls=":") for v in (550, 850)]
a1.plot(x, [r[1] for r in log], color="#2a78d6", alpha=0.4, lw=1.2, label="probe (simulated)")
a1.plot(x, [r[3] for r in log], color=TEAL, lw=2.6, label="control value")
a2.axhspan(5.8, 6.2, color=BAND); [a2.axhline(v, color=CORAL, ls=":") for v in (5.5, 6.5)]
a2.plot(x, [r[2] for r in log], color="#2a78d6", alpha=0.4, lw=1.2)
a2.plot(x, [r[4] for r in log], color=TEAL, lw=2.6)
for m, pump, u in doses:
    ax = a1 if pump == "Nutrient" else a2
    ax.axvline(m - first, color=GREEN if pump == "Nutrient" else CORAL, ls="--", lw=1.2)
    ax.annotate(f"{u:.1f} mL", (m - first, ax.get_ylim()[0]), xytext=(3, 4),
                textcoords="offset points", fontsize=9, rotation=90, va="bottom")
if settled_at:
    for ax in (a1, a2):
        ax.axvline(settled_at - first, color="k", lw=1.6)
    a1.annotate(f"settled: {settled_at - first:.0f} min", (settled_at - first, 830),
                xytext=(5, 0), textcoords="offset points", fontsize=11)
a1.set_ylabel("TDS (ppm)"); a2.set_ylabel("pH"); a2.set_xlabel("minutes after the first dose")
a1.set_title(f"Simulated bring-up from plain water, firmware 1.9.8 (mixing {TAU_M/60:.0f} min, start pH {PH0:.2f}{', nutrient first' if NUTRIENT_FIRST else ''})",
             loc="left", color=TEAL, fontweight="bold", fontsize=13)
a1.legend(loc="center right", frameon=False, fontsize=10)
fig.tight_layout()
tag = f"tau{TAU_M/60:.0f}_g{GAMMA:.0f}_ph{PH0:.2f}" + ("_nutrient_first" if NUTRIENT_FIRST else "")
for ext in ("png", "pdf"):
    fig.savefig(f"sim_bringup_1.9.8_{tag}.{ext}", dpi=200)
