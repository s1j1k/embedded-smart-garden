%LINEARISE_VEGETATIVE Linear MIMO model of the reservoir about the vegetative point.
%
%   Numerical Jacobian of plant_derivs at (V0, TDS0, pH0) -> state space G_ss with
%   the dose-to-probe delay on every dosing input, its transfer matrix G_tf, the
%   analytic gain-table form G_an(s) for comparison, and the steady-state "rate
%   gain" matrix K (output change per mL delivered) used for the RGA and a static
%   decoupler. Needs the Control System Toolbox.
%
%   Outputs y = [TDS ppm; pH; level %], inputs u = [q_N; q_up; q_dn; q_W] mL/s.

p  = plant_params();
V0 = 7.84; TDS0 = 700; PH0 = 6.0;          % vegetative point, 50 % fill
[x0, info] = operating_point(V0, TDS0, PH0, p);
u0 = zeros(4, 1); d0 = zeros(3, 1);
fprintf('Operating point: V %.2f L, TDS %.0f ppm, pH %.2f (%.1f mL nutrient)\n', ...
        V0, TDS0, info.pH, info.nutrient_mL);

% ---- Numerical Jacobians (central differences) ---------------------------------------
nx = numel(x0); nu = numel(u0);
[f0, y0] = plant_derivs(x0, u0, d0, p); %#ok<ASGLU>
ny = numel(y0);
A = zeros(nx); Bm = zeros(nx, nu); Cm = zeros(ny, nx); Dm = zeros(ny, nu);
for i = 1:nx
    e = zeros(nx, 1); e(i) = max(1e-6, 1e-6 * abs(x0(i)));
    [fp, yp] = plant_derivs(x0 + e, u0, d0, p);
    [fm, ym] = plant_derivs(x0 - e, u0, d0, p);
    A(:, i) = (fp - fm) / (2 * e(i));
    Cm(:, i) = (yp - ym) / (2 * e(i));
end
for j = 1:nu
    e = zeros(nu, 1); e(j) = 1e-4;
    [fp, yp] = plant_derivs(x0, u0 + e, d0, p);
    [fm, ym] = plant_derivs(x0, u0 - e, d0, p);
    Bm(:, j) = (fp - fm) / (2e-4);
    Dm(:, j) = (yp - ym) / (2e-4);
end

names_u = {'q_N', 'q_up', 'q_dn', 'q_W'};
names_y = {'TDS', 'pH', 'level'};
G_ss = ss(A, Bm, Cm, Dm, 'InputName', names_u, 'OutputName', names_y, ...
          'StateName', {'V', 'M', 'B', 'C', 'P', 'Tp', 'Hp'});
G_ss.InputDelay = [p.theta; p.theta; p.theta; 0];
G_tf = minreal(tf(G_ss), 1e-6);

% ---- Rate-gain matrix K: output change per mL delivered, after mixing settles --------
% Each dosing channel integrates, so K = lim s->0 of s*G(s) (per mL/s per s = per mL).
s = tf('s');
K = zeros(ny, nu);
for i = 1:ny
    for j = 1:nu
        g = minreal(G_tf(i, j) * s, 1e-6);
        K(i, j) = dcgain(g);
    end
end
disp('Rate gains K (output per mL delivered):'); disp(array2table(K, ...
    'VariableNames', names_u, 'RowNames', names_y));

% ---- Analytic form (gain table x shapes), for the report --------------------------------
D  = exp(-p.theta * s) / (s * (p.tau_m * s + 1));      % dosing: integrator + mixing + delay
Aa = 1 / (p.tau_a * s + 1);                            % nutrient's slow pH drop
% (the electrode lag tau_probe is folded into the pH channel's mixing lag)
W  = 1 / s;                                            % water: integrator
G_an = [K(1,1)*D,      K(1,2)*D, K(1,3)*D, K(1,4)*W;
        K(2,1)*D*Aa,   K(2,2)*D, K(2,3)*D, K(2,4)*W;
        0,             0,        0,        K(3,4)*W];

% ---- RGA of the dosing block (TDS, pH vs nutrient, pH-up) ------------------------------
K2 = K(1:2, 1:2);
RGA = K2 .* inv(K2).';
disp('RGA (TDS,pH x nutrient,pH-up):'); disp(RGA);

% ---- Static decoupler for the nutrient / pH-up pair --------------------------------------
Dcp = inv(K2) * diag(diag(K2));     % u = Dcp * v keeps each loop's own gain
disp('Static decoupler (u = Dcp * v):'); disp(Dcp);

% ---- Compare against the measured gains ----------------------------------------------------
fprintf(['Measured for comparison: nutrient->TDS %.1f ppm/mL (230-260 ppm L/mL at V0),\n' ...
         '  pH-up->pH 1.47 pH/mL at pH 4.5 / 1000 ppm (not at this point).\n'], 245 / V0);

save('vegetative_linear.mat', 'G_ss', 'G_tf', 'G_an', 'K', 'RGA', 'Dcp', 'x0', 'p');
fprintf('Saved vegetative_linear.mat\n');
