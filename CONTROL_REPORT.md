# Control, Calibration, and Prototype Review

**Smart hydroponic garden — firmware control design note**  
**Status:** DWC + air stone (Kratky retired); water pump = safety top-up or EC dilution only  
**Audience:** future-us, and anyone who has to explain why the first tank failed

This note records what went wrong on the first prototype, why the original
threshold (bang-bang) controller made it worse, how pumps are now calibrated
over Arduino Serial, which feedback laws are available once those rates
exist, and whether **feed-forward** (and other strategies) fit pH, water,
and nutrients. It is the companion to `embedded-smart-garden.ino`
(`ACTIVE_CONTROL_LAW`, DWC settle timings) and `shared/control_law.h`.

**Current water policy (DWC):** GPIO 18 does **not** chase stage level. It
runs only when the surface hits the absolute safety floor (~100 mm + hysteresis
below net-pot touch) and refills to the stage air-gap line (50 mm seedling/veg,
100 mm flower/harvest), or when TDS is ≥10% above the stage max (dilution).
pH doses no longer chase with fresh water. See README grow-stage section.

---

## 1. What the first prototype actually did

The first build was a **passive Kratky** reservoir: plants sat in nutrient
solution with **no air stone and no recirculation**. The goal at that stage
was **seed propagation** — hold a sensible **pH** and get the **water level**
(including temporary reverse drain) into a workable band. We were **not**
running a nutrient / TDS campaign yet.

Firmware already contained a three-loop skeleton (water, pH, nutrients), but
**only pH and water level were actually under control**. The nutrient pump
path was written early and never used in anger while we were still trying to
propagate seeds.

| Loop | Sensor | Actuator | In the code | What we actually ran |
| --- | --- | --- | --- | --- |
| Water level | Grove I2C pads | Peristaltic GPIO18 | If level ≤ target, run **1.5 s**, wait 30 s; reverse drain when over-full | **Yes** — fill / drain toward a wetness band |
| pH | SEN0169 | pH-up GPIO23 / pH-down GPIO17 | If outside the band, run **2 s**, then **chase with 2 s fresh water**, wait **60 s**, decide again | **Yes** — and the chase was a mistake |
| Nutrients | Grove TDS | Nutrient pump GPIO19 | If TDS low, run **2 s**, wait 60 s | **No** — logic present, pump unused |

There was no model of how many millilitres a “2 second pulse” actually moved.
There was no model of how long acid or base needed to **diffuse** through a
still tank before the probe could see it. Fresh-water “chase” after a pH dose
was treated as mixing. In a Kratky bucket it is not mixing — it is a second
unmixed pocket of liquid. **We should never have chased pH with water** —
it added volume, disturbed level/TDS bookkeeping, and did not mix. GPIO 18
belongs only to safety top-up and (later) deliberate EC dilution, not to
every pH pulse.

That combination produced the failure mode below.

---

## 2. Prototype failure analysis

### 2.1 Foul smell, bacteria, and a full water change

Still, warm, nutrient-rich water with **almost no dissolved oxygen** is an
incubator. Roots and the solution went anaerobic. The tank developed a **rot /
foul smell**. The practical recovery was to **dump and replace the reservoir**
rather than trust the old solution.

This is a **process** failure first, and only secondarily a control failure.
A pH or TDS loop cannot oxygenate water. Kratky can work in a well-designed
passive system, but this reservoir did not have enough gas exchange. The
**DWC + air stone** conversion is the hardware fix now in service.

### 2.2 Too much pH liquid (overdose)

pH adjuster was dosed on a **60-second** retry timer. In a Kratky tank the
probe sits in one place. Acid or base leaves the tube as a plume, sinks or
floats, and may not reach the probe for **hours**. The controller therefore
saw “still out of band” and dosed again. And again.

Fresh-water chase after each pulse **did not fix this** — and **should not
have been there**. Chase water also does not circulate. It dilutes a local
region and can even push the plume *away* from the probe, which makes the
reading look more wrong and invites another dose. For seed propagation we
needed pH settle time and aeration, not more jug water after every acid/base
burst.

The result was **far more pH liquid than the tank chemistry required**, a
probe that lagged the true bulk pH, and a solution that had to be thrown
out with the rest of the water.

### 2.3 Lack of diffusion and lack of oxygen flow

Two missing physical transports, often confused:

1. **Solute transport (diffusion / advection).** Without flow, pH and
   nutrient concentration are not uniform. A single probe is not the tank.
2. **Gas transport (oxygen).** Without bubbles or a falling film, dissolved
   oxygen collapses. Roots and aerobic microbes lose; anaerobes win. Smell
   follows.

Chasing with fresh water addresses neither. It adds volume and can raise
level into the root zone (the reason for the 80 % hard ceiling and the
temporary reverse-drain path). It does not stir, and it does not aerate.

### 2.4 Uncalibrated pumps made every law worse

The peristaltic pumps were run for a fixed time at an unverified servo
direction and an unknown mL/s. A “small” 2 s pulse might be 0.4 mL or 8 mL.
Until each pump is measured into a cup, **every** controller — threshold,
P, PI, or PID — is guessing.

---

## 3. What we changed in firmware after that failure

- **Kratky pH re-dose wait is 18 hours** (12–24 h diffusion band), not 60 s.
  Switch to `DWC_MODE` (30 minute wait) only after the air stone is
  installed and bubbling.
- **Safety caps** on auto pH-down (band overshoot lockout, doses per window
  / boot, stuck-probe pause). These are last-resort brakes, not a substitute
  for mixing.
- **Serial pump calibration** into a **10 mL measuring cup**, rates stored
  on LittleFS (`/pump_cal.txt`) so they survive reboot.
- **Selectable control law:** `THRESHOLD` (default, original behaviour),
  `P`, `PI`, or `PID`, using those stored rates. Do not leave `THRESHOLD`
  until the pumps you will use are calibrated.
- **Nutrient dosing stays deferred at seedling** until pH/level are stable;
  the GPIO19 path existed in the first prototype but was unused then too.
- **Feed-forward is not in the live loop yet.** Section 6 argues it is
  the right *next* structure for water and nutrients (known dilutions and
  uptake), and only a *companion* to pH after a titration curve exists.
  Pure open-loop FF on Kratky would have made the overdose worse.

Hardware still required: **air** (DWC) and **clean water** after the
bacterial event. Firmware cannot undo an anaerobic tank.

---

## 4. Repeatable pump calibration (Arduino Serial)

Use the Arduino Serial Monitor at **115200 baud**, line ending **Newline**.

```text
HELP
CAL                 all four pumps, one by one
CAL WATER           1 L top-up jug — continuous until STOP (max 8 s)
CAL NUTRIENT        1 L — 500 ms bursts
CAL PH_UP / PH_DOWN 600 mL — 500 ms bursts
CAL SHOW / CAL CLEAR
STOCK / FILL / SETCAP / SETREM
YES / STOP
```

**Priming (before seed):** flash `water_fill_cal.ino` and fill with GPIO 18
until water **just touches the net pots**, then `TOUCH` (persists
`/net_pot_touch.txt`). Set pH on that solution, soak rockwool in it by hand,
then seed. GPIO 18 **never drains** and, in the main sketch, only runs for
**safety floor top-up** or **≥10% EC overshoot dilution** — not to chase
stage level. Ongoing jug volume still comes from the **1 L** bottle.

**pH-up, pH-down, and nutrient** are calibrated and later dosed as **500 ms
bursts**. Saved unit: **mL / burst**. Auto control only ever runs whole
bursts (threshold default = one burst).

**Water** is a longer top-up. `YES` runs **continuously** until `STOP` (or
8 s). That duration is replayed three times. Saved unit: **mL/s**. Auto
top-ups stay continuous.

Sequence for each pump:

1. Firmware names the pump and GPIO and **waits**.
2. You put the correct liquid on the **inlet**. If the tubing is dry, put a
   **waste cup** on the outlet and type `PRIME`. Type `STOP` when liquid
   (not air) comes out. Empty the cup.
3. Put an empty **10 mL cup** on the outlet and type `YES`.
4. Chemistry: 500 ms bursts. Water: one continuous run. Type `STOP` when
   the cup is readable (about 2 mL+).
5. Type that volume. Firmware replays the **same motion three more times**;
   enter the mL after each. The saved value is the average.
6. `RETRY` repeats the current run, `SKIP` leaves the pump unchanged,
   `ABORT` exits.

A measurement into air is useless — prime first. `PRIME` auto-stops after
45 s if you forget `STOP`. Cup volumes are subtracted from the bottle
estimate.

Auto water / pH / nutrient control is **paused** for the whole session so
the wizard cannot fight the cup.

Rates persist across reboot. They are erased by `CAL CLEAR` or by formatting
LittleFS. Bottle levels (`/pump_stock.txt`) are kept across `CAL CLEAR`.

### 4.1 Bottle stock (live remaining)

Programmed capacities (change with `SETCAP`):

| Bottle   | Default |
|----------|---------|
| Water top-up jug | 1000 mL |
| Nutrient | 1000 mL |
| pH-up    | 600 mL  |
| pH-down  | 600 mL  |

After each calibrated run the firmware subtracts the estimated mL and
publishes `*_rem_ml` / `*_empty` / `stock_alert` on MQTT. When remaining
hits the reserve (one burst for chemistry, ~25 mL for water) **auto dosing
for that pump stops**. Type `FILL` (or `FILL PH_DOWN`) after you replace
the bottle. The dashboard shows the live estimate and alerts on empty.

**Calibration alerts (Pi):** when any pump lacks a stored rate, telemetry
includes `cal_alert` (e.g. `uncal:water,uncal:ph_down`), per-pump
`*_calibrated` booleans, `cal_needed`, and an `alerts` array. The same
snapshot is retained on `smart-garden/garden-node-01/alerts`. Clear the
banner by finishing Serial `CAL` for the listed pumps.

### 4.2 Future trigger from the Raspberry Pi

The node already **subscribes** to:

```text
smart-garden/garden-node-01/cmd
```

A JSON body that contains `calibrate` prints a Serial prompt. It does
**not** start a pump. A human still has to type `CAL` / `YES` with a cup in
place. A dashboard button that runs acid into the floor is not a feature.

TODO: add a Pi UI control that publishes `{"cmd":"calibrate","pump":"water"}`
and shows the same checklist (inlet + 10 mL cup) before the operator walks
to the Serial Monitor.

---

## 5. Control laws — options and contrast

All four laws share the same **outer** structure: measure → if outside the
band, run a pump for some time → **wait for the mode-specific settle** →
measure again. They differ in **how large the pulse is**.

Set the law in `embedded-smart-garden.ino`:

```cpp
const ControlLaw::Mode ACTIVE_CONTROL_LAW = ControlLaw::THRESHOLD;
// ControlLaw::P
// ControlLaw::PI_LAW   // not PI — Arduino.h already #defines PI
// ControlLaw::PID_LAW
```

If the law is not `THRESHOLD` but that pump has no stored rate, firmware
falls back to the old fixed pulse and prints a warning.

### 5.1 Threshold (bang-bang) — what we shipped first

**Idea:** if the reading is on the wrong side of a limit, apply a **constant**
dose.

**Strengths**

- Easy to explain and to debug on Serial.
- No calibration required to get *some* motion.
- Natural deadband (the grow-stage min/max).

**Weaknesses (this prototype)**

- Dose size is unrelated to how far you are from the target.
- Retry rate was much faster than the plant’s mixing time, so the loop
  integrated **ignorance**, not error.
- Coupled badly with Kratky: each “I don’t see a change yet” looked like
  “I need more chemical.”
- Water chase after pH looked like a mixing step and was not.

**Use when:** pumps are not calibrated yet, or you want a known-small
manual-style nudge after you have lengthened the settle time (current
Kratky default).

### 5.2 P — proportional volume

**Idea:** `dose_mL = Kp × |error|`, converted to milliseconds with the
calibrated mL/s. Larger error → larger (but capped) pulse. Inside the band,
dose is zero.

**Strengths**

- Matches the physical question: “how much liquid does this error need?”
- A 0.15 pH miss no longer gets the same slug as a 0.6 miss.
- Caps (`maxMl`) keep a 10 mL-scale mistake from emptying a bottle.

**Weaknesses**

- Never removes a **constant** bias (wrong probe offset, slow leak) — you
  stop at a residual error of about `min_dose / Kp`.
- If `Kp` is large and settle is still too short, it can still overdose,
  just in one bigger hit instead of many small ones.

**Use when:** pumps are calibrated, you are still on Kratky (or newly on
DWC), and you want the safest step up from threshold. **Recommended next
law after calibration.**

### 5.3 PI — proportional + integral

**Idea:** add `Ki × ∫ error dt` so a stubborn offset is slowly wound out.
The integrator only runs on settle-interval samples, and it **does not
accumulate while the output is hard-clamped** (anti-windup).

**Strengths**

- Can finish the last tenth of a pH or the last 50 ppm of TDS that P leaves
  behind.
- Sensible on **DWC**, where each sample after 30 minutes is a real bulk
  measurement.

**Weaknesses**

- On **Kratky**, a long-wrong probe reading is not a real bias — it is
  unmixed fluid. Integral will “help” by pouring more chemical into a
  pocket. That is how the first tank was overdosed, with extra memory.
- `Ki` too large + 18 h samples still winds up over days.

**Use when:** air stone is on, `DWC_MODE` is set, pH/TDS readings after
settle match a test kit, and P alone leaves a consistent residual.

### 5.4 PID — PI + derivative

**Idea:** add `Kd × d(error)/dt` to ease off if the reading is already
moving toward the band.

**Strengths**

- On a well-mixed DWC tank it can shorten the last approach and reduce
  overshoot from a large P term.

**Weaknesses**

- The pH probe and TDS probe are noisy. Derivative amplifies noise into
  twitchy pulse lengths.
- Kratky “derivative” is mostly plume motion past the glass, not plant
  chemistry.
- Three gains to tune, one 10 mL cup, and a crop in the tank is a poor
  lab.

**Use when:** DWC has been stable on PI for a while and you have logged
overshoot you can see on the Pi CSV. Start with a **small** `Kd`.

### 5.5 Comparison

| | Threshold | P | PI | PID |
| --- | --- | --- | --- | --- |
| Needs pump mL/s? | No | Yes | Yes | Yes |
| Dose scales with error? | No | Yes | Yes | Yes |
| Removes constant bias? | No | No | Yes | Yes |
| Noise sensitive? | Low | Low | Low–med | High |
| Safe on unmixed Kratky? | Only with a **long** settle | Best of the feedback options | Risky | Risky |
| Fits aerated DWC? | Wasteful / slow | Good | **Best default** | Optional polish |
| Typical failure | Many blind retries | Residual offset | Overdose if probe lags mix | Jittery doses |

**Plant recommendation**

1. Keep `THRESHOLD` + Kratky 18 h settle until `CAL SHOW` lists every pump
   you will use.
2. Switch to **P** for the first calibrated grow, still Kratky or early DWC.
3. After the air stone is running and a test kit agrees with the probe,
   set `DWC_MODE` and try **PI**.
4. Leave **PID** off until you have a week of DWC logs that show overshoot
   worth damping.

Gains live next to `ACTIVE_CONTROL_LAW` (`WATER_CONTROL_GAINS`,
`PH_CONTROL_GAINS`, `NUTRIENT_CONTROL_GAINS`). They are starting points,
not crop-specific truth. Change one gain at a time. Measure the cup again
if you change a pump’s run speed (`WATER_REFILL_RUN_SPEED` etc.) — mL/s is
only valid at that servo command.

Feedback alone is not the whole design space. The next sections ask whether
a **basic feed-forward** term is appropriate on each loop, and which other
strategies are worth the code.

---

## 6. Feed-forward — is it appropriate here?

### 6.1 What “basic feed-forward” means on this bench

Feedback waits for an error on the probe, then doses. **Feed-forward (FF)**
computes a dose from something we already know — a setpoint change, a
measured disturbance, or a model of the last actuator move — **without
waiting for the probe to catch up**.

```text
u = u_ff(disturbance, model) + u_fb(setpoint − measurement)
```

That two-term form is **two-degree-of-freedom (2DOF)** control. FF does the
bulk of the move; P/PI only trims what the model got wrong. This is the
architecture we should grow into, not “FF *instead of* a probe.”

Open-loop FF (no trim) is only as good as the model. The first prototype’s
model of mixing was “wait 60 s.” That model was false. Open-loop FF with a
false model is just a faster way to empty a pH bottle.

### 6.2 Water level — **yes, FF is appropriate**

The plant is a slow, one-way leak (transpiration + uptake). Refill,
dilution, and pH chase are **known** volume events. Reverse drain is off
(`water_fill_cal.ino` maps Grove % to pumped mL for a better tank model).

| Disturbance | Do we know it when it happens? | FF action |
| --- | --- | --- |
| Daily uptake | Yes, after a few logged days (Δ% × tank mL) | Morning top-up of yesterday’s consumed mL |
| Nutrient dilution pulse | Yes — we commanded it | Subtract that mL from the next refill (already “we added water”) |
| pH water chase | Yes — we commanded it | Same; stop treating chase as free |
| Reverse drain | Yes — we commanded it | Do not refill the same water back in the next cycle |
| Grow-stage target step (65 % → 20 %) | Yes — we change `ACTIVE_GROW_STAGE` | Drain or hold; do not “P-control” down with a refill pump |

A basic water FF is almost bookkeeping:

```text
V_needed = (target% − level%) / 100 × RESERVOIR_WORKING_VOLUME_ML
run_ms   = V_needed / water_mL_per_s
```

That is feed-forward from **geometry**, not from waiting for another 1.5 s
pulse to see if the pads moved. Feedback (P) only corrects a wrong tank
volume constant or a drippy pump.

**Verdict:** implement water FF as soon as the water pump is calibrated and
`RESERVOIR_WORKING_VOLUME_ML` is measured with a jug. It is the most
honest water law we can write. Threshold refill was FF with a broken
model (fixed time, unknown mL).

### 6.3 Nutrients (TDS) — **yes, especially paired with water**

Two FF paths are stronger than waiting for the TDS probe:

1. **Dilution compensation (ratio / material balance).**  
   When we add ΔV of fresh water into volume V at concentration C:

   ```text
   C_after = C × V / (V + ΔV)
   nutrient_mL ≈ (C_target − C_after) × (V + ΔV) / stock_ppm_equivalent
   ```

   Stock strength is unknown until we calibrate “1 mL nutrient → Δppm in
   this tank.” That is one cup-plus-TDS experiment, then FF.

2. **Uptake schedule.**  
   Plants eat on a slow curve. Stage presets (200–400 ppm seedling, …)
   are already a **setpoint feed-forward**. A daily “replace yesterday’s
   ppm drop” dose is disturbance FF. The probe then trims.

The first tank never exercised the nutrient pump in production; TDS logic
was scaffolding while we held pH and water level for seed propagation.
Water chase and level refill still diluted the solution whenever they ran,
which is why a later nutrient loop must not stay fully independent of
water — but that coupling was not the active failure mode yet.

**Verdict:** nutrient FF is appropriate **after** (a) nutrient pump mL/s,
(b) a one-shot “mL → Δppm” tank test, and (c) we treat every fresh-water
add as a known dilution. Do this before PI on TDS. On Kratky, still wait
the long settle before the **feedback trim**, or the probe will invent
uptake that was only a plume.

### 6.4 pH — **only as a companion, never alone**

pH is the loop that already overdosed. FF is tempting because chemistry
looks like a formula. It is the **least** safe loop for open-loop FF.

**When FF helps**

- **Known coupling:** many nutrient stocks are acidic; fresh tap/RO water
  is often higher pH than the band. After a nutrient FF dose or a large
  refill, a **small pre-compensating pH pulse** (from a table, not from
  the lagging probe) avoids the “wait, still wrong, dose again” spiral.
- **Empirical titration curve:** `mL_acid(V, pH_now → pH_target)` measured
  once in *this* reservoir with *this* nutrient mix. Then  
  `u_ff = table(V, ΔpH)` and `u_fb` only after the Kratky/DWC settle.
- **Dead-time awareness:** the 18 h Kratky wait *is* a primitive FF
  assumption: “the dose we already sent is still in transit.” That is
  closer to a Smith predictor’s intent than to PID.

**When FF hurts**

- Henderson–Hasselbalch / “pH is log10” formulas **do not** describe a
  mixed hydroponic buffer. Alkalinity, leftover acid, and biofilm change
  the slope every week.
- Open-loop FF on an unmixed Kratky tank assumes the probe equals the
  bulk. That assumption is what emptied the pH bottle.
- Split actuators (pH-up and pH-down) plus a wrong FF sign = neutralizing
  yourself and salting the tank.

**Verdict:** do **not** add a standalone pH feed-forward law yet. After
DWC + a titration experiment, add **pH FF only as a coupling term**
(nutrient add → tiny pH-up/down from a table) plus P trim after settle.
Keep the 0.5-above-`phMax` lockout. If the test kit and probe disagree,
FF is forbidden.

### 6.5 Feed-forward vs the laws we already have

| | Open-loop FF | FF + P trim (2DOF) | P / PI / PID only | Threshold |
| --- | --- | --- | --- | --- |
| Uses probe for the main move? | No | No, probe trims | Yes | Yes |
| Needs a model? | Yes | Yes, can be crude | No | No |
| Water | Good if volume known | **Best water design** | OK | Wasteful |
| Nutrients | Good if Δppm/mL known | **Best nutrient design** | OK on DWC | Blind |
| pH | **Unsafe** on Kratky | Good *after* titration + air | P now; PI on DWC | Only with 18 h wait |
| Failure if model wrong | Immediate overdose | Trim can save you | Slow, but bounded | Many small overdoses |

**Recommendation:** treat 2DOF (FF + P) as the **target architecture** for
water and nutrients. Keep pH on **P + long settle** until DWC and a cup
titration exist. Do not implement pH-only FF as a fourth `ACTIVE_CONTROL_LAW`
value until that table is real numbers, not a guess.

TODO: add `ControlLaw::FF_P` (2DOF) once `RESERVOIR_WORKING_VOLUME_ML` is
measured and a `nutrient_ppm_per_ml` constant has been cup-tested. pH FF
table is a separate TODO, gated on `DWC_MODE`.

---

## 7. Other strategies worth considering

Not every textbook controller belongs on a 10 mL cup and an ESP32.

### 7.1 Worth building (in roughly this order)

| Strategy | What it is | Fit on this garden |
| --- | --- | --- |
| **2DOF = FF + feedback** | Section 6 | Water and nutrients first |
| **Gain scheduling** | Different Kp / settle / max mL per grow stage and per Kratky vs DWC | We already schedule setpoints and settle time; schedule *gains* the same way when P is enabled |
| **Hysteresis / wider deadband** | Must enter a *stricter* inner band before dosing stops, or wait until error exceeds a larger outer band | Stops chatter on a noisy Grove level / TDS pad. Cheap. Add before PI |
| **Split-range with interlock** | pH-up and pH-down are one signed actuator; never both; minimum wait after a sign change | We already interlock pumps. Make the “don’t flip from down to up in the same day on Kratky” rule explicit |
| **Cascade on volume** | Outer loop requests **mL**; inner loop is `run_ms = mL / (mL/s)` | This is what calibration already enables. Keep all new laws in mL, never in raw seconds |
| **Selector / override** | Hard constraints beat the regulator | Already: 80 % water ceiling, pH-down lockout, drain preempts pH. Keep overrides above any PID |
| **Event / confirm dosing** | Dose once, require Serial or a test-kit `YES` before the next pH shot | Safest pH mode while the tank still smells or the probe is untrusted. Fits Kratky culturally |
| **Dead-time compensation (Smith-style)** | Predict bulk pH/TDS from last dose + elapsed mix time; don’t use the raw probe until the delay elapses | We approximate this with the 18 h / 30 min settle (ignore the probe for control). A full Smith predictor is extra code for little gain until DWC |

### 7.2 Maybe later, on the Pi, not on the ESP32

| Strategy | Why not now |
| --- | --- |
| **MPC / optimal control** | Needs a plant model, a horizon, and a solver. Fine as a Pi experiment on logs; too heavy and too wrong on Kratky |
| **Adaptive / self-tuning PI** | Will adapt to the *probe lag*, not the chemistry — same overdose with better confidence |
| **Fuzzy / expert rules** | Readable, but we already have rules (bands, caps, modes). More rules without mixing just encode the old mistake |
| **Inferential (“soft sensor”) pH** | Estimate bulk pH from dose history when the probe is in a pocket. Useful research project; dangerous as the only input |
| **Extremum seeking / ML policy** | Needs many safe trials. We have one crop and a history of killing the solution |

### 7.3 Coupling and what not to decentralize forever

The three loops share the same bucket:

```text
water add  →  dilutes TDS  →  usually raises pH
nutrient add  →  raises TDS  →  often lowers pH
pH dose  →  almost no TDS  →  can change volume slightly
drain  →  loses everything in proportion  →  then refill repeats the story
```

Independent SISO loops (what we have) are fine **if** FF carries those
arrows. If they stay independent, each loop treats the other’s move as an
unmeasured disturbance and fights it. That is another reading of the
prototype: pH chase water disturbed level (and would have disturbed TDS);
the water and pH loops then “fixed” problems they had just created for
each other. Nutrient dosing was not part of that episode.

A full MIMO / decoupling matrix is more than we need. Three explicit FF
links (the table in §6.2–6.4) are enough.

### 7.4 Updated plant recommendation

1. Calibrate pumps (`CAL`). Measure tank volume into `RESERVOIR_WORKING_VOLUME_ML`.
2. Stay **Kratky + THRESHOLD or P** + 18 h pH settle. Optional: **confirm
   mode** for pH (one dose per operator `YES`).
3. Add **water FF** (volume bookkeeping) and **nutrient FF** (dilution +
   uptake) with P trim — 2DOF. Add hysteresis on level/TDS.
4. Install the **air stone**, set `DWC_MODE`, trust the probe against a kit.
5. Then **PI** on pH/TDS, and only then a **pH coupling FF** from a
   measured titration / nutrient-acid table.
6. Leave PID, MPC, and adaptive laws on the shelf until DWC logs are boring.

---

## 8. Why better maths does not replace the air stone

A PID loop on a dead tank is a precise way to overdose a plume. The
prototype’s smell and water change came from **biology and transport**, not
from missing derivative gain.

| Symptom | Root cause | What actually fixes it |
| --- | --- | --- |
| Foul / rot smell | Low oxygen, bacterial bloom | Dump bad water, clean, **aerate** (DWC) |
| pH liquid used up | Retry faster than diffusion | 18 h Kratky wait, then volume-based P; air stone before PI |
| Probe “ignored” the dose | No bulk mixing | Time, or bubbles / circulation |
| Fresh-water chase “did nothing” | Chase is not a mixer | Stop treating chase as settle |
| Roots at risk of drowning | Level into the pots | Never add above net-pot touch; no reverse drain |

Control can only be as honest as the sample. Calibrate the pumps so a
decision has units. Use feed-forward for moves we already commanded
(refill, dilution). Pick P/PI so the leftover has a size. Put oxygen
in the water so the plant survives the decision.

---

## 9. Operator checklist

1. Serial `CAL` each pump into the 10 mL cup; `CAL SHOW` to confirm.
2. Verify pump direction (0 vs 180) during that pulse — backwards calibration
   is worse than none.
3. Run `water_fill_cal`, `TOUCH` at net-pot contact; confirm
   `RESERVOIR_WORKING_VOLUME_ML` / tank geometry.
4. Leave `ACTIVE_CONTROL_LAW` on `THRESHOLD` or `P` under DWC. Do not turn
   on pH feed-forward yet.
5. Prefer stable pH at seedling before enabling nutrient dosing.
6. Confirm pH with a **liquid test kit** after the first calibrated dose;
   the probe lied to us once already.
7. If the water smells again, treat it as a **sanitation / aeration** event,
   not as a request for more pH-down.
