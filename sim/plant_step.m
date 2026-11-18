function [xn, y] = plant_step(x, q, d, p)
%PLANT_STEP One fixed step (p.dt) of the nonlinear plant, RK4. Codegen-safe.
%   See plant_derivs for states, inputs and outputs.
h = p.dt;
[k1, y] = plant_derivs(x, q, d, p);
k2 = plant_derivs(x + 0.5 * h * k1, q, d, p);
k3 = plant_derivs(x + 0.5 * h * k2, q, d, p);
k4 = plant_derivs(x + h * k3, q, d, p);
xn = x + h / 6 * (k1 + 2 * k2 + 2 * k3 + k4);
end
