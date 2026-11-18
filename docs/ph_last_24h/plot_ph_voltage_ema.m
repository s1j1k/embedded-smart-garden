% plot_ph_voltage_ema.m
% Last 24 h of the on-device log (ends 2026-09-27 11:26 UTC / 21:26 AEST).
%
%   ph_voltage   board voltage after the 40 mV trimmed mean
%   ph_raw       that voltage converted with the Nernst correction
%   ph_filtered  current firmware filter: median of the 16-sample burst,
%                then mean of samples within 0.25 pH of the median
%   EMA          causal 3 min exponential average of ph_filtered.
%                Samples with a pump on are not folded in; the EMA holds.
%
% This log was recorded before that hold existed, so pump-on points are
% still in ph_raw / ph_filtered and are marked on the figure.
%
% Run from this folder. MATLAB R2019b+.

clear; close all; clc;

dataDir = fileparts(mfilename('fullpath'));
if isempty(dataDir)
    dataDir = pwd;
end

T = readtable(fullfile(dataDir, 'ph_last_24h.csv'), 'TextType', 'string');
tUtc = datetime(T.time_utc, 'InputFormat', 'yyyy-MM-dd''T''HH:mm:ss''Z''', ...
    'TimeZone', 'UTC');
t = tUtc;
t.TimeZone = 'Australia/Sydney';

V = double(T.ph_voltage);
phRaw = double(T.ph_raw);
phFw = double(T.ph_filtered);
pumpOn = double(T.ph_up_pump_on) > 0 | double(T.ph_down_pump_on) > 0 ...
    | double(T.water_pump_on) > 0 | double(T.nutrient_pump_on) > 0;

phEma = causalEmaHold(tUtc, phFw, pumpOn, minutes(3));

figure('Name', 'pH voltage and EMA', 'Color', 'w', 'Position', [70 70 1100 780]);
tiledlayout(3, 1, 'TileSpacing', 'compact', 'Padding', 'compact');

nexttile;
plot(t, V, 'Color', [0.25 0.25 0.25], 'LineWidth', 0.8);
hold on;
if any(pumpOn)
    scatter(t(pumpOn), V(pumpOn), 22, [0.8 0.15 0.15], 'filled', ...
        'DisplayName', 'pump on');
end
hold off;
grid on;
ylabel('Board voltage (V)');
title('Raw pH-board voltage');
legend('Location', 'best');

nexttile;
hold on;
plot(t, phRaw, 'Color', [0.65 0.65 0.65], 'LineWidth', 0.7, 'DisplayName', 'pH raw');
plot(t, phFw, 'Color', [0.15 0.35 0.75], 'LineWidth', 1.0, 'DisplayName', 'current filter');
plot(t, phEma, 'Color', [0.1 0.55 0.25], 'LineWidth', 1.4, 'DisplayName', 'EMA 3 min, hold during pump');
if any(pumpOn)
    scatter(t(pumpOn), phFw(pumpOn), 28, [0.8 0.15 0.15], 'filled', ...
        'DisplayName', 'pump on');
end
yline(5.4, '--', 'Color', [0.45 0.45 0.45], 'HandleVisibility', 'off');
yline(5.9, '--', 'Color', [0.45 0.45 0.45], 'HandleVisibility', 'off');
hold off;
grid on;
ylabel('pH');
title('pH raw, current burst filter, and EMA');
legend('Location', 'best');
ylim([3 7]);

nexttile;
hold on;
plot(t, phFw - phEma, 'Color', [0.15 0.4 0.7], 'LineWidth', 0.9);
yline(0, '-', 'Color', [0.3 0.3 0.3], 'HandleVisibility', 'off');
yline(0.15, '--', 'Color', [0.5 0.5 0.5], 'HandleVisibility', 'off');
yline(-0.15, '--', 'Color', [0.5 0.5 0.5], 'HandleVisibility', 'off');
if any(pumpOn)
    scatter(t(pumpOn), phFw(pumpOn) - phEma(pumpOn), 22, [0.8 0.15 0.15], 'filled', ...
        'DisplayName', 'pump on');
end
hold off;
grid on;
ylabel('\Delta pH');
xlabel('Local time (AEST)');
title('Current filter minus EMA. Dashed lines are \pm0.15.');
legend('Location', 'best');

function y = causalEmaHold(t, x, holdSample, tau)
    n = numel(x);
    y = nan(n, 1);
    y(1) = x(1);
    tauSec = seconds(tau);
    for i = 2:n
        if holdSample(i) || isnan(x(i))
            y(i) = y(i - 1);
            continue;
        end
        dt = seconds(t(i) - t(i - 1));
        if ~(dt > 0)
            y(i) = y(i - 1);
            continue;
        end
        alpha = 1 - exp(-dt / tauSec);
        prev = y(i - 1);
        if isnan(prev)
            prev = x(i);
        end
        y(i) = (1 - alpha) * prev + alpha * x(i);
    end
end
