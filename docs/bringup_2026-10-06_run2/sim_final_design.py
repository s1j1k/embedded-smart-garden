"""Final nutrient design vs measured mixing: 20 min waits, cap to 600 at the worst-case
gain then the learned gain (floor 0.6 x worst), mass-balance budget to 750 ppm.
Mixing: first-order with time constant tau (run 1 ~0.2 min, run 2 ~9 min)."""
import math

def run(gamma, tau, V=7.84, base=9.0, wait=20.0):
    capV = V * 0.9
    worst = 31 * 8.4 / capV
    budget = (750 - base) / worst
    kh, conf, cum, doses = 14.0, False, 0.0, []
    t = 0.0
    def tds(at):
        return base + sum(gamma / V * u * (1 - math.exp(-(at - td) / tau)) for td, u in doses if at > td)
    while True:
        y = tds(t)
        if y >= 550 or len(doses) > 8:
            break
        capg = worst if not conf else max(kh, 0.6 * worst)
        u = min(0.8 * (700 - y) / kh, (600 - y) / capg, 25.0, budget - cum)
        if u < 0.1:
            break
        doses.append((t, u)); cum += u
        seen = tds(t + wait)
        kh = min(max(0.5 * kh + 0.5 * (seen - y) / u, 3), 60); conf = True
        t += wait
    final = base + gamma / V * cum
    settled = t if tds(t) >= 550 else None
    return [round(u, 1) for _, u in doses], round(final), round(tds(t)), settled

print("gamma(ppm L/mL) tau(min)  doses(mL)          final  seen  TDS-settled(min)")
for gamma in (200, 230, 260):
    for tau in (0.2, 9, 15):
        d, f, s, st = run(gamma, tau)
        print(f"{gamma:6} {tau:6}   {str(d):22} {f:5} {s:5}  {st}")
