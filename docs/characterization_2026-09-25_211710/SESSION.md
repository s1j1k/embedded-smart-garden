# Characterization session

- Started (local): 2026-09-25 21:17:10 +10:00
- Firmware target: 1.5.2 (DOSE + AUTO PAUSE/RESUME)
- Crop context: pre-transplant, ungerminated, vegetative setpoints
- Water level sensor: TRUSTED=false (operator ground-truth volume only)

## Operator ground-truth volume

- Session start: **PENDING — reply with mL or L**
- After refill: —
- Session end: —

## Real APIs used

| Action | Serial | MQTT cmd topic `smart-garden/garden-node-01/cmd` |
| --- | --- | --- |
| Stage | `MODE VEGETATIVE` | `{"cmd":"stage","value":"vegetative"}` |
| Cycle | `CYCLE` | `{"cmd":"cycle"}` |
| Pause auto | `AUTO PAUSE` | `{"cmd":"auto_pause"}` |
| Resume auto | `AUTO RESUME` | `{"cmd":"auto_resume"}` |
| Metered dose | `DOSE PH_UP 0.15` | `{"cmd":"dose","pump":"ph_up","ml":0.15}` |
| Cal readback | `CAL SHOW` | telemetry `*_ml_per_burst` / `water_ml_per_s` |
| Gains | telemetry | `gain_ph_up`, `gain_ph_down`, `gain_ec` |
