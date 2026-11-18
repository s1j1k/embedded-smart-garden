%SWEEP_BRINGUP Regression sweep of the bring-up over plant uncertainty.
%   Includes the 6 Oct run-2 case (Gamma 237 ppm L/mL, tau_m 9 min) that overshot on
%   firmware 1.9.1. Every row should have overshoot = 0 for firmware 1.9.4.
gammas = [200 230 260 289];          % ppm L / mL (measured 237-259; 289 = worst + 10 %)
taus   = [0.2 9 15 30] * 60;         % mixing time constant, s
fills  = [7.0 7.84];                 % true volume, L
biases = [0 20];                     % level reading bias, %  (20 = stuck-high sensor)
rows = [];
for g = gammas
    for tm = taus
        for V = fills
            for b = biases
                r = run_bringup('Gamma_N', g, 'tau_m', tm, 'V0', V, 'levelBias', b, ...
                                'plot', false);
                rows = [rows; g, tm / 60, V, b, r.peakTds, r.finalTds, r.minPh, ...
                        size(r.doses, 1), r.settled_min, r.overshoot]; %#ok<AGROW>
            end
        end
    end
end
T = array2table(rows, 'VariableNames', {'Gamma', 'tau_m_min', 'V_L', 'levelBias', ...
    'peakTDS', 'finalTDS', 'minPH', 'doses', 'settled_min', 'overshoot'});
disp(T);
fprintf('Overshoot in %d of %d cases.\n', sum(T.overshoot), height(T));
writetable(T, 'sweep_bringup.csv');
