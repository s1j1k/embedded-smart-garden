function [x0, info] = operating_point(V_L, tds_ppm, ph, p)
%OPERATING_POINT Steady state with the given volume, TDS and pH.
%
%   [x0, info] = operating_point(7.84, 700, 6.0, plant_params())   % vegetative
%   [x0, info] = operating_point(7.84, p.tdsTap, [], p)           % plain tap water
%
%   The tank is built the way the rig builds it: tap water, then nutrient up to
%   tds_ppm, then (if ph is given) the strong-base equivalents B that put the charge
%   balance at that pH (what pH-up/down dosing would converge to). Mixing and the
%   slow acid release are settled (P = 0, probe = bulk).
if nargin < 4, p = plant_params(); end
nutrient_mL = max(0, (tds_ppm - p.tdsTap) * V_L / p.Gamma_N);
C = p.cTap * V_L + p.c_N * nutrient_mL;
M = tds_ppm * V_L;

% Tap water B: the base equivalents that give pH 6.55 with the tap buffer,
% minus the nutrient's released acidity.
B_tap = base_for_ph(6.55, p.cTap * V_L, V_L, p);
B = B_tap - p.alpha_N * nutrient_mL;
if ~isempty(ph)
    B = base_for_ph(ph, C, V_L, p);
end
pH0 = ph_from_charge(B, C, V_L, p);
x0 = [V_L; M; B; C; 0; tds_ppm; pH0];
info = struct('nutrient_mL', nutrient_mL, 'pH', pH0, 'B_mmol', B, 'C_mmol', C);
end

function B = base_for_ph(ph, C, V, p)
% Invert the charge balance: B/V = Kw/H - H + (C/V)/(1+10^(pKa-pH)).
h = 10^(-ph);
B = V * 1e3 * (p.Kw / h - h + (C * 1e-3 / V) / (1 + 10^(p.pKa - ph)));
end
