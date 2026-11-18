% plot_ph_last_24h.m
% pH over the 24 h ending 2026-09-27 11:26 UTC (21:26 AEST), from the
% on-device sensor log. Firmware "filtered" is only an intra-burst trim
% (16 ADC samples, 5 ms apart). The extra traces are causal filters a
% later firmware pass could run across cycles.
%
% Run from this folder. MATLAB R2019b+ (datetime, tiledlayout).

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

phRaw = double(T.ph_raw);
phFw = double(T.ph_filtered);
up = double(T.ph_up_pump_on) > 0;
down = double(T.ph_down_pump_on) > 0;

% About 5 cycles (~2 min) and 15 cycles (~5 min) at the ~20 s log rate.
phMed5 = causalMedian(phFw, 5);
phMed15 = causalMedian(phFw, 15);
% 3 min time constant. alpha uses the real gap so a stalled log does not
% pretend the samples were evenly spaced.
phEma = causalEma(tUtc, phFw, minutes(3));

figure('Name', 'pH last 24 h', 'Color', 'w', 'Position', [80 80 1100 720]);
tiledlayout(2, 1, 'TileSpacing', 'compact', 'Padding', 'compact');

nexttile;
hold on;
plot(t, phRaw, 'Color', [0.75 0.75 0.75], 'LineWidth', 0.6, 'DisplayName', 'raw');
plot(t, phFw, 'Color', [0.15 0.35 0.75], 'LineWidth', 1.0, 'DisplayName', 'firmware filtered');
plot(t, phMed5, 'Color', [0.85 0.35 0.1], 'LineWidth', 1.3, 'DisplayName', 'causal median, 5 samples');
plot(t, phEma, 'Color', [0.15 0.55 0.25], 'LineWidth', 1.2, 'DisplayName', 'EMA, 3 min');
yline(5.4, '--', 'Color', [0.4 0.4 0.4], 'HandleVisibility', 'off');
yline(5.9, '--', 'Color', [0.4 0.4 0.4], 'HandleVisibility', 'off');
yline(5.5, ':', 'Color', [0.55 0.55 0.55], 'HandleVisibility', 'off');
yline(5.8, ':', 'Color', [0.55 0.55 0.55], 'HandleVisibility', 'off');
markPumps(t, up, down, phFw);
hold off;
grid on;
ylabel('pH');
title('Last 24 h (AEST). Dashed lines: dose dead zone 5.4–5.9. Dotted: target 5.5–5.8.');
legend('Location', 'best');
ylim([3 7]);

nexttile;
hold on;
plot(t, phFw - phMed5, 'Color', [0.85 0.35 0.1], 'DisplayName', 'firmware - median5');
plot(t, phFw - phEma, 'Color', [0.15 0.55 0.25], 'DisplayName', 'firmware - EMA 3 min');
plot(t, phFw - phMed15, 'Color', [0.45 0.2 0.6], 'DisplayName', 'firmware - median15');
yline(0, '-', 'Color', [0.3 0.3 0.3], 'HandleVisibility', 'off');
yline(0.15, '--', 'Color', [0.5 0.5 0.5], 'HandleVisibility', 'off');
yline(-0.15, '--', 'Color', [0.5 0.5 0.5], 'HandleVisibility', 'off');
hold off;
grid on;
ylabel('\Delta pH');
xlabel('Local time (AEST)');
title('How far a single cycle sits from a slower filter. Dashed: \pm0.15.');
legend('Location', 'best');

function y = causalMedian(x, window)
    n = numel(x);
    y = nan(n, 1);
    for i = 1:n
        a = max(1, i - window + 1);
        y(i) = median(x(a:i), 'omitnan');
    end
end

function y = causalEma(t, x, tau)
    n = numel(x);
    y = nan(n, 1);
    y(1) = x(1);
    tauSec = seconds(tau);
    for i = 2:n
        dt = seconds(t(i) - t(i - 1));
        if ~(dt > 0) || isnan(x(i))
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

function markPumps(t, up, down, ph)
    if any(up)
        scatter(t(up), ph(up), 28, [0.1 0.45 0.85], 'filled', ...
            'DisplayName', 'pH-up pump sample');
    end
    if any(down)
        scatter(t(down), ph(down), 28, [0.75 0.15 0.15], 'filled', ...
            'DisplayName', 'pH-down pump sample');
    end
end
