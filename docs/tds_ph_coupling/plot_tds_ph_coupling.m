% plot_tds_ph_coupling.m
% Evidence / mechanism figure for pH-reagent salt inflating TDS.
%
% Run from this folder (docs/tds_ph_coupling) in MATLAB R2019b+.
%
% Field archive (Influx pull 2026-08-01 → 2026-09-20):
%   - Only stage=sensor_baseline; ph_*_pump_on=0; ph_session_ml=0.
%   - No automatic pH dose events exist in the cloud bucket, so Panel 1
%     shows the best available co-recorded pH/TDS window (probe work on
%     2026-09-16) and Panel 2 illustrates the report observer with a
%     first-principles dose response (eq. TDS_nutrient).

clear; close all; clc;

dataDir = fileparts(mfilename('fullpath'));
if isempty(dataDir)
    dataDir = pwd;
end

%% ---- Panel 1: field window (no auto-dose in archive) ----
W = readtable(fullfile(dataDir, 'window_2026-09-16_calibration.csv'), ...
    'TextType', 'string');
tField = datetime(W.time_utc, 'InputFormat', 'yyyy-MM-dd''T''HH:mm:ssXXXXX', ...
    'TimeZone', 'UTC');
% Fallback if timezone suffix parsing differs across MATLAB releases.
if all(isnat(tField))
    tField = datetime(erase(W.time_utc, ["+00:00", "Z"]), ...
        'InputFormat', 'yyyy-MM-dd''T''HH:mm:ss');
end
phField = double(W.ph);
tdsField = double(W.tds_ppm);

% Annotated events from the pull notes.
evPath = fullfile(dataDir, 'events_notes.csv');
evTimes = datetime.empty(0, 1);
evLabels = strings(0, 1);
if isfile(evPath)
    E = readtable(evPath, 'TextType', 'string');
    for i = 1:height(E)
        ti = datetime(E.time_utc(i), 'InputFormat', 'yyyy-MM-dd''T''HH:mm:ssXXXXX', ...
            'TimeZone', 'UTC');
        if isnat(ti)
            ti = datetime(erase(E.time_utc(i), ["+00:00", "Z"]), ...
                'InputFormat', 'yyyy-MM-dd''T''HH:mm:ss');
        end
        if ~isnat(ti) && ti >= tField(1) && ti <= tField(end)
            evTimes(end + 1, 1) = ti; %#ok<AGROW>
            evLabels(end + 1, 1) = string(E.event(i)); %#ok<AGROW>
        end
    end
end

%% ---- Panel 2: model illustration of TDS rise after pH fluid ----
% Matches report observer:
%   TDS_sensor = TDS_nutrient_true + (kappa_down * mL_down + kappa_up * mL_up) / V
% A single pH-Down pulse at t=0, then mixing lag, then settle.
V_L = 6.5;
kappa_down = 200;  % ppm·L/mL  (provisional firmware default)
kappa_up = 250;
dose_mL = 2.0;     % early bang-bang overshoot scale (visible salt bump)
tau_mix_min = 3;   % air-stone mixing time constant
theta_min = 2;     % transport delay to probe
t_min = (-5:0.1:40)';
u = zeros(size(t_min));
u(t_min >= 0) = dose_mL;  % step in cumulative delivered volume after dose instant
% First-order + delay response of observed salt offset
salt_ss = (kappa_down * dose_mL) / V_L;
salt_ppm = zeros(size(t_min));
for i = 1:numel(t_min)
    tEff = t_min(i) - theta_min;
    if tEff <= 0
        salt_ppm(i) = 0;
    else
        salt_ppm(i) = salt_ss * (1 - exp(-tEff / tau_mix_min));
    end
end
tds_nutrient_true = 950 * ones(size(t_min));  % plant-available target mid-band
tds_sensor = tds_nutrient_true + salt_ppm;
tds_observer = tds_sensor - salt_ppm;          % firmware correction once κ known
ph_model = 5.95 * ones(size(t_min));
ph_drop = 0.35 * (1 - exp(-max(t_min - theta_min, 0) / tau_mix_min));
ph_model = ph_model - ph_drop;
ph_model(t_min < 0) = 5.95;

% Persist model series for the report / re-plot without MATLAB regenerating.
modelOut = table(t_min, ph_model, tds_sensor, tds_observer, salt_ppm, ...
    'VariableNames', {'t_min', 'ph', 'tds_sensor_ppm', 'tds_nutrient_ppm', 'salt_offset_ppm'});
writetable(modelOut, fullfile(dataDir, 'model_ph_dose_tds.csv'));

%% ---- Figure ----
fig = figure('Color', 'w', 'Position', [80 80 980 720]);
tl = tiledlayout(fig, 2, 1, 'TileSpacing', 'compact', 'Padding', 'compact');

% Panel 1
ax1 = nexttile(tl);
yyaxis(ax1, 'left');
plot(ax1, tField, phField, 'LineWidth', 1.2);
ylabel(ax1, 'pH');
ylim(ax1, [4.5 7.5]);
yyaxis(ax1, 'right');
plot(ax1, tField, tdsField, 'LineWidth', 1.2);
ylabel(ax1, 'TDS (ppm)');
grid(ax1, 'on');
title(ax1, {'Influx field window 2026-09-16 (sensor\_baseline)', ...
    'No automatic pH dose in archive — probe/tank transition only'});
xlabel(ax1, 'UTC');
for k = 1:numel(evTimes)
    xline(ax1, evTimes(k), '--', evLabels(k), 'LabelOrientation', 'horizontal', ...
        'LabelVerticalAlignment', 'bottom');
end

% Panel 2
ax2 = nexttile(tl);
yyaxis(ax2, 'left');
plot(ax2, t_min, ph_model, 'LineWidth', 1.4);
ylabel(ax2, 'pH (model)');
ylim(ax2, [5.3 6.1]);
yyaxis(ax2, 'right');
hold(ax2, 'on');
plot(ax2, t_min, tds_sensor, 'LineWidth', 1.4);
plot(ax2, t_min, tds_observer, '--', 'LineWidth', 1.4);
plot(ax2, t_min, tds_nutrient_true, ':', 'LineWidth', 1.1);
hold(ax2, 'off');
ylabel(ax2, 'TDS (ppm)');
xline(ax2, 0, '-', sprintf('pH Down %.2f mL', dose_mL));
grid(ax2, 'on');
title(ax2, {sprintf(['Model: TDS rises by \\kappa_{down}·mL/V = %.1f ppm after pH fluid'], salt_ss), ...
    'Observer recovers TDS_{nutrient}; nutrient loop must not chase the salt bump'});
xlabel(ax2, 'Minutes relative to pH dose');
legend(ax2, {'pH', 'TDS_{sensor}', 'TDS_{nutrient} (observer)', 'true nutrient'}, ...
    'Location', 'eastoutside');

title(tl, 'pH reagent → TDS coupling (field archive + report observer)');
exportgraphics(fig, fullfile(dataDir, 'tds_ph_coupling_figure.png'), 'Resolution', 200);
fprintf('Wrote %s\n', fullfile(dataDir, 'model_ph_dose_tds.csv'));
fprintf('Wrote %s\n', fullfile(dataDir, 'tds_ph_coupling_figure.png'));
