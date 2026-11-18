function build_bringup_model(modelName)
%BUILD_BRINGUP_MODEL Generate the Simulink closed-loop bring-up model (bringup_sim.slx).
%
%   Pump (1 s) -> Delay (theta) -> Plant (1 s, nonlinear MIMO, RK4)
%        -> Sensors (10 s, noise) -> Controller (10 s, firmware 1.9.4 law) -> Pump
%
%   Each block is a MATLAB Function that calls the same .m files as run_bringup.m,
%   so the scripts and the model cannot disagree. Swap the Controller block's code
%   to try other controllers (PI, decoupled P, MPC) or filters.
if nargin < 1, modelName = 'bringup_sim'; end
p = plant_params(); c = controller_params();
x0 = operating_point(7.84, p.tdsTap, [], p);

if bdIsLoaded(modelName), close_system(modelName, 0); end
new_system(modelName); open_system(modelName);
set_param(modelName, 'Solver', 'FixedStepDiscrete', 'FixedStep', num2str(p.dt), ...
          'StopTime', num2str(4 * 3600), 'AutoInsertRateTranBlk', 'on');

addFn('Pump', [100 100 220 160], p.dt, sprintf([ ...
    'function q = Pump(req)\n%%#codegen\n' ...
    'persistent delivered\nif isempty(delivered), delivered = zeros(4,1); end\n' ...
    'p = plant_params();\n[q, delivered] = pump_step(req, delivered, p);\n']));

add_block('simulink/Discrete/Delay', [modelName '/DoseDelay'], ...
          'DelayLength', num2str(round(p.theta / p.dt)), 'InitialCondition', '0', ...
          'Position', [260 110 300 150]);

x0s = mat2str(x0', 8);
addFn('Plant', [340 100 460 160], p.dt, sprintf([ ...
    'function y = Plant(q, d)\n%%#codegen\n' ...
    'persistent x\nif isempty(x), x = %s''; end\n' ...
    'p = plant_params();\n[x, y] = plant_step(x, q, d, p);\n'], x0s));

add_block('simulink/Sources/Constant', [modelName '/Disturbance'], 'Value', '[0;0;0]', ...
          'Position', [260 180 300 210]);

addFn('Sensors', [500 100 620 160], c.Ts, sprintf([ ...
    'function ym = Sensors(y)\n%%#codegen\n' ...
    'p = plant_params();\nym = y + [p.noise_tds; p.noise_ph; p.noise_lvl] .* randn(3,1);\n']));

addFn('Controller', [660 100 800 160], c.Ts, sprintf([ ...
    'function [req, tdsF, phF] = Controller(ym)\n%%#codegen\n' ...
    'persistent st\nc = controller_params();\n' ...
    'if isempty(st), st = controller_init(c); end\n' ...
    'st = controller_adaptive_p(st, ym(1), ym(2), ym(3), c);\n' ...
    'req = st.req; tdsF = st.tdsF; phF = st.phF;\n']));

addLog('y_plant', [500 220 560 250]);
addLog('y_meas',  [660 220 720 250]);
addLog('req_mL',  [860 60 920 90]);
addLog('filtered', [860 160 920 190]);
add_block('simulink/Signal Routing/Mux', [modelName '/MuxF'], 'Inputs', '2', ...
          'Position', [830 150 835 200]);

add_line(modelName, 'Pump/1', 'DoseDelay/1', 'autorouting', 'on');
add_line(modelName, 'DoseDelay/1', 'Plant/1', 'autorouting', 'on');
add_line(modelName, 'Disturbance/1', 'Plant/2', 'autorouting', 'on');
add_line(modelName, 'Plant/1', 'Sensors/1', 'autorouting', 'on');
add_line(modelName, 'Plant/1', 'y_plant/1', 'autorouting', 'on');
add_line(modelName, 'Sensors/1', 'Controller/1', 'autorouting', 'on');
add_line(modelName, 'Sensors/1', 'y_meas/1', 'autorouting', 'on');
add_line(modelName, 'Controller/1', 'Pump/1', 'autorouting', 'on');
add_line(modelName, 'Controller/1', 'req_mL/1', 'autorouting', 'on');
add_line(modelName, 'Controller/2', 'MuxF/1', 'autorouting', 'on');
add_line(modelName, 'Controller/3', 'MuxF/2', 'autorouting', 'on');
add_line(modelName, 'MuxF/1', 'filtered/1', 'autorouting', 'on');

save_system(modelName);
fprintf('Built %s.slx. Run: out = sim(''%s''); then plot out.y_plant etc.\n', modelName, modelName);

    function addFn(name, pos, ts, code)
        path = [modelName '/' name];
        add_block('simulink/User-Defined Functions/MATLAB Function', path, 'Position', pos);
        chart = find(sfroot, '-isa', 'Stateflow.EMChart', 'Path', path);
        chart.Script = code;
        chart.SampleTime = num2str(ts);
    end

    function addLog(name, pos)
        add_block('simulink/Sinks/To Workspace', [modelName '/' name], ...
                  'VariableName', name, 'SaveFormat', 'Timeseries', 'Position', pos);
    end
end
