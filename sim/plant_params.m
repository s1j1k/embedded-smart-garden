function p = plant_params()
%PLANT_PARAMS Constants for the reservoir plant model (SI-ish units below).
%
%   Units: time s, volume L, dose flow mL/s, dissolved solids mg (so TDS ppm = mg/L),
%   acid/base/buffer amounts mmol.
%
%   Source tags:  [meas]  measured on the rig (docs/bringup_2026-10-06*)
%                 [label] reagent label
%                 [PH]    PLACEHOLDER: replace from the bench titration and step tests
%                         (sim/README.md, identification procedure)
%
%   Codegen-safe (used inside MATLAB Function blocks): plain struct of doubles.

% ---- Tank geometry ---------------------------------------------------------------
p.A_mL_per_mm   = 112;      % [meas] 280 x 400 mm footprint
p.sensorOffset  = 20;       % [meas] level 0 % sits 20 mm above the floor; 1 % = 1 mm

% ---- Pumps (mL/s while running) ----------------------------------------------------
p.q_N   = 0.82;             % [meas] 18.9 mL in 23.1 s (6 Oct run 2)
p.q_up  = 0.80;             % [PH] pH-up peristaltic, set from pump calibration
p.q_dn  = 0.80;             % [PH] pH-down peristaltic
p.q_W   = 5.0;              % [PH] water pump (uncalibrated on 6 Oct)

% ---- Dissolved solids (TDS) --------------------------------------------------------
p.Gamma_N   = 245;          % [meas] nutrient, mg per mL (= ppm·L/mL); runs: 237, 259
p.kappa_up  = 250;          % [PH] pH-up salt, mg per mL (firmware provisional value)
p.kappa_dn  = 200;          % [PH] pH-down salt, mg per mL
p.tdsTap    = 9;            % [meas] tap water, ppm

% ---- Acid/base chemistry (single effective buffer) ---------------------------------
% Charge balance:  B/V + [H+] - Kw/[H+] - (C/V)/(1 + 10^(pKa - pH)) = 0  (mol/L)
p.pKa       = 6.8;          % [PH] effective pKa (carbonate 6.35 / phosphate 7.2 mix)
p.Kw        = 1e-14;
p.cTap      = 0.30;         % [PH] tap-water buffer, mmol/L
p.bTap      = 0.1077;       % [derived] tap base equivalents, mmol/L: pH 6.55 with cTap
p.alpha_N   = 0.022;        % [PH] nutrient acidity, mmol per mL (fits run 2: 29 mL -> pH 5.6)
p.c_N       = 0.050;        % [PH] nutrient buffer added, mmol per mL
p.beta_up   = 0.50;         % [PH] pH-up strong-base equivalents, mmol per mL
p.c_up      = 0.25;         % [PH] pH-up carbonate buffer, mmol per mL
p.beta_dn   = 0.60;         % [PH] pH-down acid equivalents, mmol per mL
p.c_dn      = 0.60;         % [PH] pH-down phosphate buffer, mmol per mL

% ---- Dynamics -----------------------------------------------------------------------
p.tau_m     = 9 * 60;       % [meas] mixing time constant: run 2 ~9 min, run 1 < 1 min
p.tau_a     = 15 * 60;      % [meas, rough] nutrient's slow pH drop (pH fell for ~30 min)
p.theta     = 20;           % [PH] dose-to-probe transport delay, s (Transport Delay block)
p.tau_probe = 30;           % [PH] pH electrode response, s

% ---- Disturbances (no plants: zero; identify from a 24 h drift run) ----------------
p.evap_Lps  = 0;            % evaporation + transpiration, L/s
p.uptake_N  = 0;            % nutrient uptake, mg/s
p.acid_root = 0;            % root/CO2 acidification, mmol/s

% ---- Measurement noise (1-sigma, from the filter-analysis PSD work) ----------------
p.noise_tds = 5;            % ppm
p.noise_ph  = 0.01;         % pH
p.noise_lvl = 2;            % % (level sensor jitter seen on 6 Oct was larger, ~5 %)

% ---- Simulation ----------------------------------------------------------------------
p.dt        = 1;            % plant step, s
end
