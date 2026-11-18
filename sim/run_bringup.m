function res = run_bringup(varargin)
%RUN_BRINGUP Closed-loop plain-water -> vegetative bring-up, plain MATLAB (no Simulink).
%
%   res = run_bringup();                                 % defaults: 50 % fill, tau_m 9 min
%   res = run_bringup('tau_m', 30, 'Gamma_N', 260);      % override plant_params fields
%   res = run_bringup('V0', 7.0, 'levelBias', 15, 'plot', false);
%
%   Uses the same plant_step / pump_step / controller_adaptive_p as the Simulink model.
%   res: peak and final TDS, overshoot flag, settling time (C1: from the first dose to
%   the end of the last dose's settling wait, if both readings then stay inside the
%   dead limits), dose log and time series.
opts = struct('V0', 7.84, 'duration', 4 * 3600, 'levelBias', 0, 'seed', 1, 'plot', true);
p = plant_params(); c = controller_params();
for k = 1:2:numel(varargin)
    name = varargin{k}; val = varargin{k + 1};
    if isfield(opts, name), opts.(name) = val;
    elseif isfield(p, name), p.(name) = val;
    elseif isfield(c, name), c.(name) = val;
    else, error('run_bringup: unknown option %s', name);
    end
end
rng(opts.seed);

x = operating_point(opts.V0, p.tdsTap, [], p);
st = controller_init(c);
delivered = zeros(4, 1);
nDelay = round(p.theta / p.dt);
qLine = zeros(4, nDelay + 1);                      % transport delay line on the doses
d = [p.evap_Lps; p.uptake_N; p.acid_root];
N = round(opts.duration / p.dt);
log = zeros(N, 8);                                 % t, TDS, pH, level, tdsF, phF, qN, qPh
ctrlEvery = round(c.Ts / p.dt);
for k = 1:N
    [q, delivered] = pump_step(st.req, delivered, p);
    qLine = [q, qLine(:, 1:end - 1)];
    [x, y] = plant_step(x, qLine(:, end), d, p);
    if mod(k - 1, ctrlEvery) == 0
        ym = y + [p.noise_tds; p.noise_ph; p.noise_lvl] .* randn(3, 1);
        ym(3) = ym(3) + opts.levelBias;
        st = controller_adaptive_p(st, ym(1), ym(2), ym(3), c);
    end
    log(k, :) = [k * p.dt, y(1), y(2), y(3), st.tdsF, st.phF, q(1), q(2) + q(3)];
end

% ---- Metrics ----------------------------------------------------------------------------
t = log(:, 1) / 60; tds = log(:, 2); ph = log(:, 3);
doses = st.doseLog(1:st.nDose, :);
res.doses = doses; res.log = log; res.p = p; res.c = c;
res.peakTds = max(tds); res.finalTds = tds(end); res.finalPh = ph(end);
res.minPh = min(ph);
res.overshoot = res.peakTds > c.ecDeadHigh;
res.settled_min = NaN;
if ~isempty(doses)
    t0 = doses(1, 1);
    settleEnd = doses(end, 1) + max(c.ecSettle, c.phSettle);
    after = log(:, 1) >= settleEnd;
    inside = tds(after) >= c.ecDeadLow & tds(after) <= c.ecDeadHigh & ...
             ph(after) >= c.phDeadLow & ph(after) <= c.phDeadHigh;
    if any(after) && all(inside)
        res.settled_min = (settleEnd - t0) / 60;
    end
end
fprintf('peak %.0f ppm, final %.0f ppm, pH %.2f (min %.2f), %d doses, settled %s, overshoot %d\n', ...
        res.peakTds, res.finalTds, res.finalPh, res.minPh, size(doses, 1), ...
        num2str(res.settled_min, '%.0f min'), res.overshoot);

if opts.plot
    figure('Name', 'Bring-up');
    subplot(2, 1, 1); plot(t, tds, t, log(:, 5)); hold on;
    yline([c.ecDeadLow c.ecDeadHigh], 'r:'); yline([c.ecTargetMin c.ecTargetMax], 'g:');
    for i = 1:size(doses, 1)
        if doses(i, 2) == 1, xline(doses(i, 1) / 60, '--', sprintf('%.1f mL', doses(i, 3))); end
    end
    ylabel('TDS (ppm)'); legend('probe', 'control filter', 'Location', 'southeast'); grid on;
    subplot(2, 1, 2); plot(t, ph, t, log(:, 6)); hold on;
    yline([c.phDeadLow c.phDeadHigh], 'r:');
    for i = 1:size(doses, 1)
        if doses(i, 2) > 1, xline(doses(i, 1) / 60, '--', sprintf('%.2f mL', doses(i, 3))); end
    end
    ylabel('pH'); xlabel('minutes since boot'); grid on;
end
end
