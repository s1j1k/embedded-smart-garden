function st = controller_init(c)
%CONTROLLER_INIT Controller state (all numeric fields: codegen-safe persistent).
st.t          = 0;           % s since boot
st.phase      = 0;           % 0 boot hold, 1 check due, 2 EC settle, 3 pH settle, 4 idle
st.tNext      = c.bootHold;  % when the current phase ends
st.dosedThisCycle = 0;
% Filters (DF2T states; NaN until the first sample initialises them at steady state)
st.zT = [NaN; NaN]; st.zP = [NaN; NaN];
st.tdsF = NaN; st.phF = NaN;
% Nutrient loop
st.kEc = c.ecGainSeed; st.ecConfirmed = 0;
st.ecCum = 0; st.ecBase = NaN; st.ecWorstSess = 0;
st.ecLastDose = 0; st.ecBefore = NaN;
% pH loop
st.kUp = c.phGainSeed; st.kDn = c.phGainSeed; st.phCum = 0;
st.phLastDose = 0; st.phLastDir = 0; st.phBefore = NaN;   % dir: +1 up, -1 down
% Cumulative volume requested per pump [N; up; dn; W] (the pump model delivers it)
st.req = zeros(4, 1);
% Log of doses: [t, pump, mL] (fixed size for codegen; count in nDose)
st.doseLog = zeros(32, 3); st.nDose = 0;
end
