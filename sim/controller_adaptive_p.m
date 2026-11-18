function st = controller_adaptive_p(st, tdsRaw, phRaw, levelPct, c)
%CONTROLLER_ADAPTIVE_P One control sample (every c.Ts) of the firmware 1.9.4 law.
%
%   Filters the readings (Butterworth-2), runs the settle-paced cycle
%   (nutrient check -> 20 min -> recheck + pH check -> 20 min -> recheck -> next),
%   and adds any dose to st.req (cumulative mL per pump [N; up; dn; W]).
%   Simplified from shared/stage_controller.h: no level loop (water pump held when
%   uncalibrated), no pH-salt TDS correction, no MQTT/manual paths.

% ---- Filters ---------------------------------------------------------------------------
[st.tdsF, st.zT] = bw_step(tdsRaw, st.zT, c);
[st.phF,  st.zP] = bw_step(phRaw,  st.zP, c);
tds = st.tdsF; ph = st.phF;

capV = c.vAssumed;
if levelPct >= 0
    capV = min(max((levelPct + c.sensorOffset) * c.A_mL_per_mm / 1000 * c.capDerate, ...
                   c.capVMin), c.capVMax);
end

if st.t >= st.tNext
    switch st.phase
        case {0, 1, 4}                      % nutrient check
            st.dosedThisCycle = 0;
            st = ec_check(st, tds, capV, c);
            if st.ecLastDose > 0
                st.phase = 2; st.tNext = st.t + c.ecSettle;
            else
                st = ph_check(st, ph, capV, c);   % same cycle, no wait
                st = after_ph(st, c);
            end
        case 2                              % EC settled: recheck, then pH check
            st = ec_recheck(st, tds, c);
            st = ph_check(st, ph, capV, c);
            st = after_ph(st, c);
        case 3                              % pH settled: recheck, next cycle now
            st = ph_recheck(st, ph, c);
            st.phase = 1; st.tNext = st.t;
    end
end
st.t = st.t + c.Ts;
end

% ===========================================================================================
function st = after_ph(st, c)
if st.phLastDose > 0
    st.phase = 3; st.tNext = st.t + c.phSettle;
elseif st.dosedThisCycle
    st.phase = 1; st.tNext = st.t;          % keep going while correcting
else
    st.phase = 4; st.tNext = st.t + c.cycle;
end
end

function st = ec_check(st, tds, capV, c)
st.ecLastDose = 0;
if tds >= c.ecDeadLow || st.ecCum >= c.ecSessionCap
    return
end
err = (c.ecTargetMin + c.ecTargetMax) / 2 - tds;
kWorst = c.ecGainWorst * c.ecGainRefV / capV;
capGain = kWorst;
if st.ecConfirmed
    capGain = max(st.kEc, c.ecLearnedFloor * kWorst);
end
capMl = max(0, c.ecTargetMin - tds) / capGain;
u = min([c.lambdaEc * err / st.kEc, capMl, c.ecHardMax, c.ecSessionCap - st.ecCum]);
% Mass-balance budget from the session's first check
if isnan(st.ecBase) || st.ecCum <= 0
    st.ecBase = tds;
end
st.ecWorstSess = max(st.ecWorstSess, kWorst);
budget = max(0, c.ecDeadHigh - c.ecBudgetMargin - st.ecBase) / st.ecWorstSess;
u = min(u, budget - st.ecCum);
if u < c.ecMinDose
    return
end
st.ecLastDose = u; st.ecBefore = tds; st.ecCum = st.ecCum + u;
st.req(1) = st.req(1) + u; st.dosedThisCycle = 1;
st = log_dose(st, 1, u);
end

function st = ec_recheck(st, tds, c)
if st.ecLastDose > 0
    delta = tds - st.ecBefore;
    if delta >= c.ecMinDelta
        st.kEc = min(max((1 - c.gainAlpha) * st.kEc + c.gainAlpha * delta / st.ecLastDose, ...
                         c.ecGainMin), c.ecGainMax);
        st.ecConfirmed = 1;
    end
end
st.ecLastDose = 0;
end

function st = ph_check(st, ph, capV, c)
st.phLastDose = 0; st.phLastDir = 0;
low = ph < c.phDeadLow; high = ph > c.phDeadHigh;
if ~(low || high) || st.phCum >= c.phSessionCap
    return
end
err = abs(ph - c.phCenter);
label = 100 / (c.phLabelMlPer100LPerUnit * capV);
if low
    kHat = st.kUp; capGain = max(label, c.phUpWorst * c.phUpRefV / capV); dir = 1;
else
    kHat = st.kDn; capGain = label; dir = -1;
end
capMl = err / capGain;                  % dead-range middle = phCenter
u = min([c.lambdaPh * err / kHat, capMl, c.phHardMax, c.phSessionCap - st.phCum]);
if u < c.phMinDose
    return
end
st.phLastDose = u; st.phLastDir = dir; st.phBefore = ph; st.phCum = st.phCum + u;
pump = 2; if dir < 0, pump = 3; end
st.req(pump) = st.req(pump) + u; st.dosedThisCycle = 1;
st = log_dose(st, pump, u);
end

function st = ph_recheck(st, ph, c)
if st.phLastDose > 0
    signed = (ph - st.phBefore) * st.phLastDir;
    if signed >= c.phMinDelta
        k = signed / st.phLastDose;
        if st.phLastDir > 0
            st.kUp = min(max((1 - c.gainAlpha) * st.kUp + c.gainAlpha * k, c.phGainMin), c.phGainMax);
        else
            st.kDn = min(max((1 - c.gainAlpha) * st.kDn + c.gainAlpha * k, c.phGainMin), c.phGainMax);
        end
    end
end
st.phLastDose = 0; st.phLastDir = 0;
end

function st = log_dose(st, pump, u)
if st.nDose < size(st.doseLog, 1)
    st.nDose = st.nDose + 1;
    st.doseLog(st.nDose, :) = [st.t, pump, u];
end
end

function [y, z] = bw_step(x, z, c)
% Direct form II transposed; first sample initialises the state at steady state.
b = c.bw_b; a = c.bw_a;
if isnan(z(1))
    z(2) = b(3) * x - a(3) * x;
    z(1) = b(2) * x - a(2) * x + z(2);
end
y = b(1) * x + z(1);
z1 = b(2) * x - a(2) * y + z(2);
z(2) = b(3) * x - a(3) * y;
z(1) = z1;
end
