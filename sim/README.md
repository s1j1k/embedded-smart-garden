# Reservoir plant and controller simulation (MATLAB / Simulink)

A MIMO model of the reservoir, in two forms that share one parameter file:

- **Nonlinear** (`plant_derivs.m`): mass balances for volume, dissolved solids,
  strong-base equivalents and buffer, a charge-balance pH (titration curve), the
  nutrient's slow acid release, mixing lag and dose-to-probe delay. Used as the
  "real" plant in closed-loop tests (bring-up from plain water, disturbances).
- **Linear** (`linearise_vegetative.m`): the Jacobian of the nonlinear model at the
  vegetative point (7.84 L, 700 ppm, pH 6.0), as state space and transfer matrix,
  plus the rate-gain matrix K, RGA and a static decoupler. Used for controller
  design and analysis (it is only valid within about ±0.3 pH of that point).

Inputs `u = [q_N; q_up; q_dn; q_W]` (mL/s: nutrient, pH-up, pH-down, water),
outputs `y = [TDS ppm; pH; level %]`, disturbances `d = [evaporation; uptake; root acid]`.

```
        ⎡ K11 D(s)        K12 D(s)   K13 D(s)   K14/s ⎤
G(s) =  ⎢ K21 D(s)A(s)    K22 D(s)   K23 D(s)   K24/s ⎥     D(s) = e^(−θs) / (s (τm s + 1))
        ⎣ 0               0          0          K34/s ⎦     A(s) = 1 / (τa s + 1)
```

Every dosing channel is an integrator (a dose changes the tank permanently) with the
mixing lag and transport delay. Signs at the operating point: K13 > 0 (pH-down adds
salt), K14 < 0 (dilution), K21 < 0 (nutrient acidifies), K23 < 0.

## Files

| File | Purpose |
|---|---|
| `plant_params.m` | All plant constants, tagged `[meas]`, `[label]` or `[PH]` (placeholder to identify) |
| `plant_derivs.m`, `plant_step.m` | Nonlinear plant: derivatives/outputs, and one RK4 step (codegen-safe) |
| `ph_from_charge.m` | Charge-balance pH solve (bisection, codegen-safe) |
| `operating_point.m` | Steady state for a given volume, TDS and pH (tap water or vegetative) |
| `linearise_vegetative.m` | Linear MIMO model, K, RGA, decoupler → `vegetative_linear.mat` |
| `controller_params.m`, `controller_init.m`, `controller_adaptive_p.m` | Firmware 1.9.4 adaptive dead-band P (filter, settle-paced cycle, caps, mass-balance budget) |
| `pump_step.m` | Pumps deliver the controller's cumulative requested volume at their rates |
| `run_bringup.m` | Closed-loop plain-water bring-up in plain MATLAB; metrics and plots |
| `sweep_bringup.m` | Regression sweep over gain, mixing time, fill and level bias (includes the run 2 failure case) |
| `build_bringup_model.m` | Generates `bringup_sim.slx`: Pump → Delay → Plant → Sensors → Controller |

Start with:

```matlab
cd sim
res = run_bringup();                 % firmware law on the default plant
sweep_bringup                        % every case should report overshoot = 0
linearise_vegetative                 % K, RGA, decoupler, G_tf / G_an
build_bringup_model; out = sim('bringup_sim');
```

To test another controller or filter, write `controller_<name>.m` with the same
signature as `controller_adaptive_p.m` and call it from the Controller block (or
from `run_bringup.m`).

**Status:** written without MATLAB on the build machine; not yet run. The
chemistry placeholders were checked in Python: tap water pH 6.55, 29 mL nutrient →
915 ppm / pH 5.6 (matches 6 Oct run 2); at the vegetative point pH-up +0.45, pH-down
−0.72, nutrient −0.03 pH/mL.

## Identification (at the vegetative point)

0. Calibrate the probes and all four pumps; measure the volume by jug; note the
   temperature; fix the air stone, probe and outlet positions.
1. **Bench titration** (500 mL samples): pH-up in 0.05 mL steps to pH 7.5, pH-down to
   4.5, and nutrient into tap water in 0.5 mL steps to 1.2× strength. Fits `pKa`,
   `beta_*`, `c_*`, `alpha_N`, `Gamma_N`, `kappa_*` without mixing dynamics.
2. **Quiet baseline**, 2 h without dosing: noise, drift, evaporation.
3. **In-tank steps**, one actuator at a time, 40 min holds, paired to return:
   pH-down 0.3 mL, pH-up 0.3 mL, nutrient 3 mL, water 500 mL; then pH steps at
   2× amplitude (linearity) and one nutrient step with the air stone off or at a
   different temperature (bounds `tau_m`). Each gives a column of G(s).
4. **Validation**: a pseudo-random sequence of small doses over 2-3 h, plus the
   closed-loop bring-ups (not used for fitting).
5. **Fit**: linear columns with `procest` ('P1DI'), the nonlinear model with
   `nlgreyest` or Simulink Design Optimization from the titration values; judge on
   the validation data (fit %, residual whiteness, peak error).
6. **With plants**: a 24 h drift run for uptake, root acidification and transpiration.
