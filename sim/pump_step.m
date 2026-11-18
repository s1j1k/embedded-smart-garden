function [q, delivered] = pump_step(req, delivered, p)
%PUMP_STEP Flow for one plant step: each pump runs at its rate until the cumulative
%   requested volume req (mL, from the controller) has been delivered.
rate = [p.q_N; p.q_up; p.q_dn; p.q_W];
q = min(rate, max(0, req - delivered) / p.dt);
delivered = delivered + q * p.dt;
end
