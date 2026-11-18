function c = controller_params()
%CONTROLLER_PARAMS Firmware 1.9.4 controller constants (shared/control_design.h,
%   shared/grow_stage.h), vegetative stage. Keep in step with the firmware: the
%   code is the source of truth for what runs.
c.Ts            = 10;          % control sample (sensor read) period, s
c.bootHold      = 600;         % POST_BOOT_HOLD_MS
c.ecSettle      = 20 * 60;     % EC_SETTLE_MS
c.phSettle      = 20 * 60;     % PH_SETTLE_MS
c.cycle         = 40 * 60;     % CONTROL_CYCLE_MS

% Bands (vegetative)
c.ecTargetMin = 600; c.ecTargetMax = 800; c.ecDeadLow = 550; c.ecDeadHigh = 850;
c.phCenter = 6.0; c.phDeadLow = 5.5; c.phDeadHigh = 6.5;

% Adaptive P
c.lambdaEc = 0.8; c.lambdaPh = 1.0; c.gainAlpha = 0.5;
c.ecGainSeed = 14;  c.ecGainMin = 3;   c.ecGainMax = 60;   c.ecMinDelta = 5;
c.phGainSeed = 1.5; c.phGainMin = 0.2; c.phGainMax = 6.0;  c.phMinDelta = 0.05;

% Caps
c.vAssumed = 6.5; c.capDerate = 0.9; c.capVMin = 2.0; c.capVMax = 7.0;
c.ecGainWorst = 31; c.ecGainRefV = 8.4; c.ecLearnedFloor = 0.6;
c.ecBudgetMargin = 100; c.ecSessionCap = 100;
c.phUpWorst = 1.47; c.phUpRefV = 8.4; c.phLabelMlPer100LPerUnit = 10;
c.ecMinDose = 0.1; c.ecHardMax = 25.0; c.phMinDose = 0.05; c.phHardMax = 1.0;
c.phSessionCap = 10;

% Control-input filter: Butterworth-2 at 2 mHz, fs = 0.1 Hz (sensor_ema.h)
c.bw_b = 0.0036217 * [1 2 1];
c.bw_a = [1 -1.822695 0.837182];

% Sensor geometry for the volume used by the caps
c.A_mL_per_mm = 112; c.sensorOffset = 20;
end
