import math
LAM=0.8; SEED=14.0; MID=700; LOW=550; HIGH=850; HARD=25.0; SETTLE=9.0
TAU=8.7/3   # 95 % at 8.7 min (measured)
def capgain(Vmeas): v=min(max(Vmeas*0.9,2),13); return 31*8.4/v
def run(Ktrue, Vmeas, y0=22.0, creep=0.03):
    kh=SEED; y_final=y0; t=0; doses=[]; peak=y0
    for n in range(10):
        # measured value at settle: 95 % + residual creep not yet seen
        e=MID-y_final
        y_seen=y_final
        if y_seen>=LOW: break
        cap=(MID-y_seen)/capgain(Vmeas)
        u=min(LAM*(MID-y_seen)/kh, cap, HARD)
        prev=y_seen
        y_final=y_seen+Ktrue*u*(1+creep)   # slow creep beyond the step
        seen_at_settle=y_seen+Ktrue*u*(1-math.exp(-SETTLE/TAU))
        kh=min(max(0.5*kh+0.5*(seen_at_settle-prev)/u,3),60)
        y_final_seen=seen_at_settle
        peak=max(peak,y_final); doses.append(round(u,1)); t+=SETTLE
        y_final=y_final  # next check sees ~final (next settle period)
    return doses, peak, t
print("Ktrue Vmeas  doses(mL)              peakTDS  TDS-time(min)")
for K in [10,14,20,26,31,34]:
    for V in [8.4,9.3]:
        d,p,t=run(K,V); print(f"{K:5} {V:5}  {str(d):22} {p:7.0f}  {t:5.0f}")
