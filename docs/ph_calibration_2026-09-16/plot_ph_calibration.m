% plot_ph_calibration.m
% Plot SEN0169 two-point calibration session from InfluxDB-export CSV.
% Run this script from its folder, or set dataDir below.
%
% Requires: ph_calibration_timeseries.csv, ph_calibration_events.csv
% MATLAB R2019b+ recommended (datetime, tiledlayout).

clear; close all; clc;

dataDir = fileparts(mfilename('fullpath'));
if isempty(dataDir)
    dataDir = pwd;
end

T = readtable(fullfile(dataDir, 'ph_calibration_timeseries.csv'), ...
    'TextType', 'string');
E = readtable(fullfile(dataDir, 'ph_calibration_events.csv'), ...
    'TextType', 'string');

% Prefer AEST wall-clock for report figures (session was local evening).
t = datetime(T.time_aest, 'InputFormat', 'yyyy-MM-dd HH:mm:ss');

V = T.ph_voltage;
pH = T.ph;                 % control / filtered value stored as ph
pH_raw = T.ph_raw;

% Calibration line coefficients after save (last non-empty slope/offset).
slope = NaN;
offset = NaN;
for i = height(T):-1:1
    sVal = T.ph_slope(i);
    oVal = T.ph_offset(i);
    if isnumeric(sVal)
        if ~isnan(sVal)
            slope = double(sVal);
            offset = double(oVal);
            break;
        end
    else
        sStr = string(sVal);
        if strlength(sStr) > 0 && sStr ~= "NaN"
            slope = str2double(sStr);
            offset = str2double(string(oVal));
            break;
        end
    end
end

% Event times
te = datetime(E.time_aest, 'InputFormat', 'yyyy-MM-dd HH:mm:ss');
ev = string(E.event);

fig = figure('Color', 'w', 'Position', [100 100 980 720]);
tl = tiledlayout(fig, 2, 1, 'TileSpacing', 'compact', 'Padding', 'compact');

% ---- (a) Board voltage vs time ----
nexttile;
hold on; grid on; box on;
plot(t, V, '-', 'LineWidth', 1.4, 'DisplayName', 'Board voltage');
yl = ylim;
for k = 1:height(E)
    xline(te(k), '--', 'LineWidth', 1.0, 'HandleVisibility', 'off');
    text(te(k), yl(2), " " + replace(ev(k), "_", "\_"), ...
        'Interpreter', 'latex', 'FontSize', 9, 'Rotation', 90, ...
        'VerticalAlignment', 'top', 'HorizontalAlignment', 'left');
end
% Mark captured calibration voltages
acidMask = ev == "acid_capture";
alkMask = ev == "alkali_capture";
if any(acidMask)
    Va = str2double(string(E.board_voltage_V(acidMask)));
    plot(te(acidMask), Va, 'o', 'MarkerSize', 9, 'LineWidth', 1.5, ...
        'DisplayName', sprintf('Acid capture (%.3f V @ pH 4.00)', Va));
end
if any(alkMask)
    Vb = str2double(string(E.board_voltage_V(alkMask)));
    plot(te(alkMask), Vb, 's', 'MarkerSize', 9, 'LineWidth', 1.5, ...
        'DisplayName', sprintf('Alkali capture (%.3f V @ pH 9.18)', Vb));
end
xlabel('Local time (AEST)', 'Interpreter', 'latex');
ylabel('SEN0169 board voltage $V_{\mathrm{board}}$ (V)', 'Interpreter', 'latex');
title('(a) Probe board voltage during two-point calibration', ...
    'Interpreter', 'latex');
legend('Location', 'best', 'Interpreter', 'latex');
hold off;

% ---- (b) Reported pH vs time ----
nexttile;
hold on; grid on; box on;
plot(t, pH, '-', 'LineWidth', 1.4, 'DisplayName', 'Reported $\mathrm{pH}$');
plot(t, pH_raw, ':', 'LineWidth', 1.1, 'DisplayName', 'Raw $\mathrm{pH}$');
yl = ylim;
for k = 1:height(E)
    xline(te(k), '--', 'LineWidth', 1.0, 'HandleVisibility', 'off');
end
yline(4.00, ':', 'HandleVisibility', 'off');
yline(9.18, ':', 'HandleVisibility', 'off');
xlabel('Local time (AEST)', 'Interpreter', 'latex');
ylabel('Reported $\mathrm{pH}$', 'Interpreter', 'latex');
title('(b) Converted $\mathrm{pH}$ (factory defaults before save; fitted line after)', ...
    'Interpreter', 'latex');
legend('Location', 'best', 'Interpreter', 'latex');
hold off;

title(tl, 'SEN0169 software two-point calibration (16 Sep 2026)', ...
    'Interpreter', 'latex', 'FontWeight', 'bold');

% ---- Optional third figure: calibration line ----
figure('Color', 'w', 'Position', [120 120 640 480]);
hold on; grid on; box on;
if ~isnan(slope) && ~isnan(offset)
    vv = linspace(min(V)-0.05, max(V)+0.05, 200);
    plot(vv, slope .* vv + offset, '-', 'LineWidth', 1.6, ...
        'DisplayName', sprintf('$\\mathrm{pH}=%.3f\\,V%+.3f$', slope, offset));
end
if any(acidMask)
    plot(Va, 4.00, 'o', 'MarkerSize', 10, 'LineWidth', 1.6, ...
        'DisplayName', 'Acid point $(V_a,4.00)$');
end
if any(alkMask)
    plot(Vb, 9.18, 's', 'MarkerSize', 10, 'LineWidth', 1.6, ...
        'DisplayName', 'Alkali point $(V_b,9.18)$');
end
% Overlay late rinse samples (after save)
savedIdx = find(string(T.ph_cal_message) == "saved", 1, 'first');
if ~isempty(savedIdx)
    scatter(V(savedIdx:end), pH(savedIdx:end), 18, ...
        'filled', 'DisplayName', 'Post-save samples');
end
xlabel('Board voltage $V_{\mathrm{board}}$ (V)', 'Interpreter', 'latex');
ylabel('$\mathrm{pH}$', 'Interpreter', 'latex');
title('Two-point calibration line $\mathrm{pH}=mV_{\mathrm{board}}+b$', ...
    'Interpreter', 'latex');
legend('Location', 'best', 'Interpreter', 'latex');
hold off;

% Save figures next to the CSV
exportgraphics(fig, fullfile(dataDir, 'ph_calibration_timeseries.png'), 'Resolution', 200);
exportgraphics(gcf, fullfile(dataDir, 'ph_calibration_line.png'), 'Resolution', 200);

fprintf('Saved figures to %s\n', dataDir);
fprintf('Acid capture: %s\n', string(E.time_aest(acidMask)));
fprintf('Alkali capture: %s\n', string(E.time_aest(alkMask)));
fprintf('Fit: pH = %.6f * V + %.6f\n', slope, offset);
