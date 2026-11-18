function pH = ph_from_charge(B_mmol, C_mmol, V_L, p)
%PH_FROM_CHARGE Solve the charge balance for pH (bisection, codegen-safe).
%
%   B/V + [H+] - Kw/[H+] - (C/V)/(1 + 10^(pKa - pH)) = 0      (mol/L)
%
%   B: net strong-base equivalents (mmol), C: total effective buffer (mmol).
%   f is monotonic decreasing in pH, so 50 bisection steps on [1, 13] give
%   ~1e-14 pH resolution.
b = B_mmol * 1e-3 / V_L;
c = C_mmol * 1e-3 / V_L;
lo = 1; hi = 13;
for k = 1:50
    mid = 0.5 * (lo + hi);
    h = 10^(-mid);
    f = b + h - p.Kw / h - c / (1 + 10^(p.pKa - mid));
    if f > 0
        lo = mid;   % too acidic a guess: [H+] term dominates, raise pH
    else
        hi = mid;
    end
end
pH = 0.5 * (lo + hi);
end
