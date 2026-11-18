%% Sensor filter analysis (MATLAB version of filter_analysis.py)
% Frequency analysis of the pH and TDS readings, the chosen Butterworth
% cutoff, and the lag-versus-noise comparison with the old 3 min EMA.
%
% Data: ../autonomous_run_2026-09-30/sensor_readings.csv (10 s telemetry,
% 30 Sep - 5 Oct 2026). Needs the Signal Processing Toolbox (pwelch, butter,
% freqz). Run from this folder:  >> filter_analysis
%
% Mirrors filter_analysis.py steps 1-3; compare the printed numbers with
% filter_summary.csv.

clear; close all;
DT = 10;                 % s, telemetry period
FS = 1/DT;               % Hz
TAU_EMA = 180;           % s, old firmware EMA
FC = 0.002;              % Hz, chosen Butterworth-2 cutoff (shared/sensor_ema.h)

%% Load live rows after the plain-water refill
T = readtable(fullfile('..', 'autonomous_run_2026-09-30', 'sensor_readings.csv'), ...
              'TextType', 'string');
T = T(T.row_source == "live" & T.hours_since_plain_water >= 0, :);
t = datetime(T.time_utc, 'InputFormat', 'yyyy-MM-dd''T''HH:mm:ss.SSSXXX', ...
             'TimeZone', 'UTC');
ts = seconds(t - t(1));
pump = T.nutrient_pump_on | T.ph_up_pump_on | T.ph_down_pump_on | T.water_pump_on;

%% Longest quiet window: no pump within -5 / +40 min, no gap > 30 s
quiet = true(height(T), 1);
for tp = ts(pump)'
    quiet = quiet & ~(ts > tp - 300 & ts < tp + 2400);
end
ok = quiet & [true; diff(ts) <= 30];
d = diff([0; ok; 0]);
starts = find(d == 1); ends = find(d == -1) - 1;
[~, k] = max(ends - starts);
i0 = starts(k); i1 = ends(k);
fprintf('Quiet window: %s, %.1f h\n', string(t(i0)), (ts(i1) - ts(i0)) / 3600);

%% Filters
a_ema = 1 - exp(-DT / TAU_EMA);
b_e = a_ema;  a_e = [1, a_ema - 1];
[b_b, a_b] = butter(2, FC / (FS / 2));           % Butterworth-2, 2 mHz

sensors = {'ph_filtered', 'ph_ema', 'pH', 'pH'; ...
           'tds_ppm', 'tds_ppm_ema', 'TDS', 'ppm'};
figure('Position', [100 100 1050 650]);
for s = 1:2
    tu = (ts(i0):DT:ts(i1))';
    x  = interp1(ts(i0:i1), T.(sensors{s, 1})(i0:i1), tu);
    xe = interp1(ts(i0:i1), T.(sensors{s, 2})(i0:i1), tu);
    xb = filter(b_b, a_b, x, filtic(b_b, a_b, x(1) * [1 1], x(1) * [1 1]));

    % 1. Welch PSD (FFT of overlapping Hann-windowed segments)
    nper = min(1024, floor(numel(x) / 4));
    [p,  f] = pwelch(detrend(x),  hann(nper), nper / 2, nper, FS);
    [pe, ~] = pwelch(detrend(xe), hann(nper), nper / 2, nper, FS);
    [pb, ~] = pwelch(detrend(xb), hann(nper), nper / 2, nper, FS);
    subplot(3, 2, s);
    loglog(f(2:end) * 1e3, p(2:end), 'Color', [0.45 0.45 0.45]); hold on;
    loglog(f(2:end) * 1e3, pe(2:end), 'g');
    loglog(f(2:end) * 1e3, pb(2:end), 'b');
    xline(FC * 1e3, 'b--'); xline(1e3 / (2 * pi * TAU_EMA), 'g--');
    xline(1e3 / (2 * pi * 108), 'r:');                % pH mixing corner
    ylabel(sprintf('PSD (%s^2/Hz)', sensors{s, 4}));
    title(sprintf('%s: Welch PSD, %.1f h quiet window', sensors{s, 3}, numel(x) * DT / 3600));
    legend('burst reading', '3 min EMA (old)', 'Butterworth-2 2 mHz', 'Location', 'southwest');
    grid on;

    % Filter magnitude responses
    subplot(3, 2, 2 + s);
    [he, fh] = freqz(b_e, a_e, f(2:end), FS);
    hb = freqz(b_b, a_b, f(2:end), FS);
    semilogx(fh * 1e3, 20 * log10(abs(he)), 'g', fh * 1e3, 20 * log10(abs(hb)), 'b');
    xline(FC * 1e3, 'b--'); yline(-3, ':'); ylim([-60 5]);
    xlabel('Frequency (mHz)'); ylabel('Gain (dB)'); grid on;
    legend('EMA 3 min', 'Butterworth-2 2 mHz', 'Location', 'southwest');

    % Noise: residual after removing a centred 30 min moving average
    r = x - movmean(x, 181); r = r(182:end - 181);
    re = filter(b_e, a_e, r - mean(r)); rb = filter(b_b, a_b, r - mean(r));
    fprintf('%s noise std: raw %.4g, EMA %.4g, Butterworth %.4g %s\n', sensors{s, 3}, ...
            std(r), std(re(401:end)), std(rb(401:end)), sensors{s, 4});

    % 2. Step response: time to stay within 5 %
    n = 2000; tt = (0:n - 1)' * DT / 60;
    ye = filter(b_e, a_e, ones(n, 1)); yb = filter(b_b, a_b, ones(n, 1));
    fprintf('  settle to 5 %%: EMA %.2f min, Butterworth %.2f min (overshoot %.1f %%)\n', ...
            tt(find(abs(ye - 1) > 0.05, 1, 'last') + 1), ...
            tt(find(abs(yb - 1) > 0.05, 1, 'last') + 1), (max(yb) - 1) * 100);
end

%% 3. Replay on a real pH-up dose step (1 Oct 2026 00:07 UTC)
t0 = datetime(2026, 10, 1, 0, 7, 0, 'TimeZone', 'UTC');
w = t > t0 - minutes(60) & t < t0 + minutes(45);
x = T.ph_filtered(w); hold_ = pump(w);
for i = 2:numel(x)                     % pump-on samples held, as on the ESP32
    if hold_(i) || isnan(x(i)), x(i) = x(i - 1); end
end
tm = minutes(t(w) - t0);
ye = filter(b_e, a_e, x, filtic(b_e, a_e, x(1), x(1)));
yb = filter(b_b, a_b, x, filtic(b_b, a_b, x(1) * [1 1], x(1) * [1 1]));
subplot(3, 2, 5);
plot(tm, x, 'Color', [0.6 0.6 0.6], tm, ye, 'g', tm, yb, 'b'); xlim([-15 40]);
xlabel('Minutes from pump start'); ylabel('pH'); grid on;
title('pH-up dose step, filters replayed causally');
legend('burst reading', 'EMA 3 min', 'Butterworth-2 2 mHz', 'Location', 'southeast');

%% 4. ADC burst average (16 samples, 5 ms apart) at mains frequencies
subplot(3, 2, 6);
fb = linspace(0.1, 250, 5000); N = 16; Ts = 5e-3;
H = abs(sin(pi * fb * N * Ts) ./ (N * sin(pi * fb * Ts)));
semilogy(fb, H); xline([50 100 150 200], ':'); ylim([1e-3 1.5]);
xlabel('Frequency (Hz)'); ylabel('Gain'); grid on;
title('16-sample burst average: nulls at 50 Hz and harmonics');
