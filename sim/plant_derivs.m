function [dx, y] = plant_derivs(x, q, d, p)
%PLANT_DERIVS Nonlinear MIMO reservoir plant: state derivatives and outputs.
%
%   States x (7x1):
%     1 V     solution volume, L
%     2 M     dissolved solids, mg            (bulk TDS = M/V ppm)
%     3 B     net strong-base equivalents, mmol
%     4 C     total effective buffer, mmol
%     5 P     nutrient acidity not yet released, mmol (slow pH drop, tau_a)
%     6 Tp    TDS at the probe, ppm           (mixing lag tau_m)
%     7 Hp    pH at the probe                 (mixing lag tau_m + electrode tau_probe)
%
%   Inputs q (4x1), flows in mL/s while each pump runs:
%     [q_N; q_up; q_dn; q_W]   nutrient, pH-up, pH-down, water
%   Disturbances d (3x1): [evaporation L/s; nutrient uptake mg/s; root acid mmol/s]
%
%   Outputs y (3x1): [TDS ppm; pH; level %] as the sensors see them (no noise here).
%
%   The dose-to-probe transport delay p.theta is applied outside (Transport Delay
%   block in Simulink, or a delay line in run_bringup.m).
V = x(1); M = x(2); B = x(3); C = x(4); P = x(5); Tp = x(6); Hp = x(7);
qN = q(1); qU = q(2); qD = q(3); qW = q(4);

dV = qW / 1000 - d(1);
dM = p.Gamma_N * qN + p.kappa_up * qU + p.kappa_dn * qD - d(2);
release = P / p.tau_a;                                   % nutrient acid released, mmol/s
dB = p.beta_up * qU - p.beta_dn * qD - release - d(3) + p.bTap * qW / 1000;
dC = p.c_N * qN + p.c_up * qU + p.c_dn * qD + p.cTap * qW / 1000;
dP = p.alpha_N * qN - release;

tdsBulk = M / V;
phBulk = ph_from_charge(B, C, V, p);
dTp = (tdsBulk - Tp) / p.tau_m;
dHp = (phBulk - Hp) / (p.tau_m + p.tau_probe);

dx = [dV; dM; dB; dC; dP; dTp; dHp];
levelPct = V * 1e6 / p.A_mL_per_mm / 1000 - p.sensorOffset;   % L -> mm -> %
y = [Tp; Hp; levelPct];
end
