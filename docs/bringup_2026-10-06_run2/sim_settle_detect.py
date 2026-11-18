"""Firmware 1.9.5 nutrient loop vs mixing speed and gain (1 s steps, first-order mixing).

Per-dose cap at the worst-case gain aiming at 700 (800 within a band-width), no session
budget, 20 min wait then settle detection (< 2 ppm/min over 5 min, 5 min steps, +20 min max,
then skip the dose). Prints the peak/final TDS and when the last dose's wait ended."""
import math

def run(gamma, tau_min, V=7.84, base=9.0):
    capV = min(max(V * 0.9, 2), 7.0); worst = 31 * 8.4 / capV
    kh = 14.0; doses = []; t = 0.0; wait_end = 0.0; skip = False; last_end = None
    def tds(at): return base + sum(gamma / V * u * (1 - math.exp(-(at - td) / (tau_min * 60)))
                                   for td, u in doses if at > td)
    while t < 4 * 3600:
        y = tds(t)
        if y >= 550 or skip:
            skip = False
            if y >= 550: break
            t += 40 * 60; continue
        tgt = 700 if 700 - y > 200 else 800
        u = min(0.8 * (700 - y) / kh, (tgt - y) / worst, 25.0)
        if u < 0.1: break
        doses.append((t, u)); y0 = y
        t += 20 * 60; extra = 0
        while True:
            slope = (tds(t) - tds(t - 300)) / 5
            if slope <= 2.0: break
            if extra >= 20 * 60: skip = True; break
            t += 300; extra += 300
        kh = min(max(0.5 * kh + 0.5 * (tds(t) - y0) / u, 3), 60)
        last_end = t
    final = base + gamma / V * sum(u for _, u in doses)
    peak = max(tds(x) for x in range(0, int(t) + 7200, 30))
    return [round(u, 1) for _, u in doses], round(peak), round(final), round(last_end / 60) if last_end else None

print("gamma tau(min)  doses(mL)              peak final settled-at(min)")
for g in (150, 180, 230, 260, 289):
    for tau in (0.2, 9, 15, 30):
        d, p, f, e = run(g, tau)
        print(f"{g:5} {tau:6}  {str(d):24} {p:5} {f:5} {e}")
