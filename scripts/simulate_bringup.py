"""Event-level simulation of the firmware 1.9 adaptive dead-band P bring-up.

Sampled model (settle-then-sample, so the dead time drops out):
  TDS_{k+1} = TDS_k + K_EC u_nut
  pH_{k+1}  = pH_k  + K_pH(TDS) u_pH + K_c(TDS) * dTDS
Parameters from the 30 Sep - 5 Oct run (Table identified params).
"""
import math

T_SETTLE = 20.0  # min (legacy single wait)
T_EC, T_PH = 6.0, 12.0  # min, firmware settle waits
LAMBDA = 0.8
ALPHA = 0.5
# Label cap: u_cap = |dead-range middle - y| / K_label (6.5 L assumed volume)
EC_GAIN_LABEL = 14.0              # ppm/mL, from the 30 Sep data
PH_GAIN_LABEL = 100.0 / (10.0 * 6.5)
EC_HARD, PH_HARD = 32.5, 1.0
EC_MIN, PH_MIN = 0.1, 0.05


def kc(tds):  # pH per ppm of nutrient, fitted to -0.0115 @67, -0.0047 @288, -0.0008 @643
    return -0.0115 * math.exp(-0.0045 * (tds - 67.0))


def kph(tds, k170=2.2):  # pH per mL, falls with buffer: 2.2 @170 ppm -> ~1.5 @700 ppm
    return k170 / (1.0 + max(tds - 170.0, 0.0) / 1140.0)


def run(k_ec_true, k170=2.2, tds0=14.0, ph0=7.2, sp_ec=(600, 800), dead_ec=(550, 850),
        sp_ph=6.0, dead_ph=(5.5, 6.5), k_ec_hat=14.0, k_ph_hat=1.5, verbose=False):
    t = 0.0
    tds, ph = tds0, ph0
    kec, kup, kdn = k_ec_hat, k_ph_hat, k_ph_hat
    conf_ec = conf_up = conf_dn = False
    log = []
    ec_mid = 0.5 * sum(sp_ec)
    ph_min_seen, ph_max_seen = ph, ph
    n_nut = n_ph = 0
    vol_nut = vol_ph = 0.0
    pending_ec = True   # stage entry: check straight away
    pending_ph = True
    for _ in range(40):
        dosed = False
        if pending_ec:
            pending_ec = False
            if tds < dead_ec[0]:
                e = ec_mid - tds
                cap = (0.5 * sum(dead_ec) - tds) / EC_GAIN_LABEL
                u = min(max(LAMBDA * e / kec, EC_MIN), min(cap, EC_HARD))
                d = k_ec_true * u
                ph += kc(tds + 0.5 * d) * d
                tds += d
                n_nut += 1
                vol_nut += u
                log.append((t, f"nutrient {u:.1f} mL -> TDS {tds:.0f}, pH {ph:.2f}"))
                # TDS rechecks at T_EC, but the pH check after a nutrient dose
                # waits until the pH reading has settled (T_PH from the dose).
                t += max(T_EC, T_PH)
                obs = d / u
                kec = (1 - ALPHA) * kec + ALPHA * obs
                conf_ec = True
                pending_ec = pending_ph = True
                dosed = True
                ph_min_seen, ph_max_seen = min(ph_min_seen, ph), max(ph_max_seen, ph)
                # pH is owed after a nutrient settle
                pending_ec_after = True
                # fall through to the pH check
        if pending_ph:
            pending_ph = False
            if ph < dead_ph[0] or ph > dead_ph[1]:
                up = ph < dead_ph[0]
                khat = kup if up else kdn
                e = abs(sp_ph - ph)
                cap = abs(0.5 * sum(dead_ph) - ph) / PH_GAIN_LABEL
                u = min(max(LAMBDA * e / khat, PH_MIN), min(cap, PH_HARD))
                d = kph(tds, k170) * u
                ph += d if up else -d
                n_ph += 1
                vol_ph += u
                log.append((t, f"pH-{'up' if up else 'down'} {u:.2f} mL -> pH {ph:.2f}"))
                t += T_PH
                obs = d / u
                if up:
                    kup = (1 - ALPHA) * kup + ALPHA * obs
                    conf_up = True
                else:
                    kdn = (1 - ALPHA) * kdn + ALPHA * obs
                    conf_dn = True
                pending_ec = pending_ph = True
                dosed = True
                ph_min_seen, ph_max_seen = min(ph_min_seen, ph), max(ph_max_seen, ph)
        if not dosed:
            break
    if verbose:
        for when, what in log:
            print(f"  t={when:5.0f} min  {what}")
    return dict(settled_min=t, tds=tds, ph=ph, n_nut=n_nut, n_ph=n_ph, vol_nut=vol_nut,
                vol_ph=vol_ph, ph_range=(ph_min_seen, ph_max_seen))


if __name__ == "__main__":
    for kec_true in (11.0, 14.0, 18.0):
        for k170 in (1.2, 2.2):
            r = run(kec_true, k170=k170, verbose=(kec_true == 14.0 and k170 == 2.2))
            print(f"K_EC={kec_true:>4} K_pH(170)={k170}: settled {r['settled_min']:.0f} min, "
                  f"TDS {r['tds']:.0f}, pH {r['ph']:.2f}, doses {r['n_nut']}+{r['n_ph']}, "
                  f"nutrient {r['vol_nut']:.1f} mL, pH {r['vol_ph']:.2f} mL, "
                  f"pH range {r['ph_range'][0]:.2f}-{r['ph_range'][1]:.2f}")
    print("transitional (300-400, dead 250-450):")
    r = run(14.0, sp_ec=(300, 400), dead_ec=(250, 450), verbose=True)
    print(r)
