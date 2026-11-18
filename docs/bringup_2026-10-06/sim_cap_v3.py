# New design: 20 min waits (full mixing seen), cap aims at 600, worst-case gain until confirmed,
# then learned gain floored at 0.6 x worst. Gain update alpha 0.5 from seed 14.
def run(K, Vm=7.84, y=9.0, creep=0.0):
    worst=31*8.4/(Vm*0.9); kh=14.0; conf=False; t=0; doses=[]; peak=y
    while y<550 and len(doses)<8:
        capg = worst if not conf else max(kh, 0.6*worst)
        u=min(0.8*(700-y)/kh, (600-y)/capg, 25.0)
        y2=y+K*u; kh=min(max(0.5*kh+0.5*(y2-y)/u,3),60); conf=True
        y=y2; peak=max(peak,y); doses.append(round(u,1)); t+=20
    return doses, round(peak), t
print("K   doses            final  TDS settle (min, end of last wait)")
for K in [10,14,20,26,31,34,40]:
    d,p,t=run(K); print(K, d, p, t)
