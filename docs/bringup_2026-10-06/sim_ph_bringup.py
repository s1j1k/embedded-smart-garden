import math
def sim(nut_ml, Kup=1.47, tau_a=15.0, tau_up=5.0, lam=0.8, khat=1.5, capg=1.63, hard=1.0, drop_per_ml=2.7/32.7, label=""):
    D=drop_per_ml*nut_ml; doses=[]; t_check=12.0; tmin=99; dt=0.1
    def ph(t): 
        v=6.6-D*(1-math.exp(-t/tau_a))
        for (td,u) in doses:
            if t>td: v+=Kup*u*(1-math.exp(-(t-td)/tau_up))
        return v
    lo=9; hi=0; settled=None; t=0; quiet=0
    while t<120:
        t+=dt; p=ph(t); lo=min(lo,p); hi=max(hi,p) if t>1 else hi
        if abs(t-t_check)<dt/2:
            if p<5.5:
                e=6.0-p; u=min(lam*e/khat, e/capg, hard); doses.append((t,u)); t_check=t+12; quiet=0
            else:
                quiet+=1; t_check=t+21
                if settled is None and quiet>=1: settled=t
    print(f"{label} nutrient {nut_ml:4.1f} mL: min pH {lo:.2f}, max {hi:.2f}, pH-up doses {[ (round(a),round(b,2)) for a,b in doses]}, first no-dose pH check at {settled and round(settled)} min, pH at 60 min {ph(60):.2f}, final {ph(120):.2f}")
for n in [20, 22]:
    sim(n)
sim(20, Kup=1.0, label="weak pH-up")
sim(20, Kup=2.0, label="strong pH-up")
sim(20, tau_a=25, label="slow acidification")
print("--- lambda 1.0 for pH")
sim(20, lam=1.0); sim(22, lam=1.0)
sim(20, lam=1.0, Kup=1.0, label="weak pH-up")
sim(20, lam=1.0, Kup=1.63*1.0, label="worst-case pH-up")
sim(20, lam=1.0, Kup=2.0, label="stronger than worst")
sim(20, lam=1.0, tau_a=25, label="slow acidification")
