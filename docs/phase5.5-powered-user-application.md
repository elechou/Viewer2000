# Phase 5.5 — Powered User Application and First Motor Rotation

> **Document status**: planned.
>
> **Entry gate**: the normal entry is a complete
> [Phase 5.2](phase5.2-minimum-powered-commissioning.md): POWERED neutral
> START/STOP, motor-connected neutral current offsets, and the powered nFAULT
> lifecycle passed. The current 2026-06-23 first-rotation branch starts from the
> narrower verified node: flashed image `0x4EE46EA6` passed
> motor-disconnected POWERED neutral START/STOP over SCI. Therefore the Phase
> 5.5 application must acquire neutral current offsets before allowing V/f,
> remain at the same supply voltage/current limit, run unloaded and briefly, and
> avoid claiming the remaining Phase 5.2 captures as passed.
>
> **Deliverable**: replace the Phase 5.0 neutral smoke application with a
> readable plain-C user application that performs current-offset acquisition,
> guarded open-loop V/f, explicit enable/ramping, and the first low-energy motor
> rotation through the existing `setup()` / `control()` boundary.

## 1. Goals and Non-Goals

Goals:

- make POWERED operation an ordinary user-code client of the platform;
- keep APP_START neutral so a START command alone cannot rotate the motor;
- publish raw/offset-relative current, physical bus voltage, encoder, command,
  and application-state data;
- require an explicit user `motor_enable` after readiness checks;
- ramp frequency and voltage with bounded slew and modulation;
- produce balanced three-phase sinusoidal PWM for first-motion V/f;
- return to neutral on user disable or any application-level interlock;
- retain hardware Trip Zone/CMPSS/DRV protection as the safety authority.

Non-goals:

- no FOC, Clarke/Park current control, observer, speed PI, or position loop;
- no motor-parameter identification beyond what the V/f demo needs;
- no field weakening, regenerative-braking strategy, or direction reversal
  while spinning;
- no rated-speed, rated-current, loaded, endurance, or unattended run;
- no platform API expansion unless implementation proves the user boundary is
  insufficient.

## 2. File and Ownership Layout

Keep the application under `cpu1/app/` so Phase 4.1 reset ownership and Phase
4.5 symbol baking cover every mutable object automatically.

Recommended layout:

```text
cpu1/app/user.c       setup/control, ADC and AS5600 reads, app state machine
cpu1/app/user_vf.c    phase ramp and three-phase V/f waveform generation
cpu1/app/user_vf.h    user-owned V/f instance and operation API
```

If the implementation remains short, `user.c` alone is acceptable. Do not put
application state into `runtime/`, `wire/`, a header-level mutable singleton,
or a vendor archive that is outside `cpu1/tools/user_boundary.json`.

The platform remains responsible for ADC timing, ePWM application, DRV
lifecycle, and all protection routing. User code only reads completed results,
copies the AS5600 cache, performs math, and writes `v2k_io.out.duty_a/b/c`.

## 3. User Parameters and Observables

All mutable user scalars are automatically `USER|PARAM|SCOPE`. Keep visible
names at 15 ASCII characters or fewer.

Minimum tunables:

| Name | Meaning | Reset value |
|---|---|---:|
| `motor_enable` | Explicit run request inside platform RUNNING | `0` |
| `freq_cmd_hz` | Signed electrical-frequency request | `0.0f` |
| `freq_slew` | Frequency slew limit in Hz/s | conservative bench value |
| `vf_v_per_hz` | Phase-peak V/Hz slope | measured bench value |
| `vf_boost_V` | Optional low-speed voltage boost | `0.0f` initially |
| `mod_max` | Maximum sinusoidal modulation index | conservative bench value |
| `vbus_min_V` | Application undervoltage interlock | measured bench bound |
| `vbus_max_V` | Application overvoltage interlock | measured bench bound |
| `i_dev_limit` | CPU-side early-neutral limit on absolute raw-count deviation from the Phase 5.2 offset | above measured neutral noise and below the provisional hardware window |
| `offset_ticks` | Zero-current averaging window | fixed commissioning value |

Minimum observables:

```text
motor_state, user_trip, freq_run_hz, phase_rad, vphase_cmd
adc_ia_raw, adc_ib_raw, adc_ic_raw, ia_zero, ib_zero, ic_zero
ia_delta, ib_delta, ic_delta
adc_vbus_raw, vbus_V, enc_raw, enc_angle, enc_seq, enc_ok
```

`i_dev_limit` is a user-code commissioning interlock, not an ampere limit and
not protection. It may command neutral on the next ISR application point, but
the asynchronous hardware trip and supply current limit remain independently
authoritative. Ampere conversion waits for proper current-calibration
equipment.

## 4. Application State Machine

Use an explicit application state published as `motor_state`:

```text
OFFSET -> READY -> RUN
   |        |       |
   +------> SOFT_TRIP
```

### OFFSET

- `setup()` initializes the V/f instance and leaves all duties neutral.
- `control()` reads the completed current frame every tick.
- Accumulate A/B/C zero-current offsets while all three duties remain 0.5.
- Reject the window if the ADC range or noise violates the Phase 5.2 bounds.
- Require valid VBUS and a fresh `wire_as5600_get_latest()` sample before READY.
  The encoder is not used for commutation, but first motion must not be blind.

### READY

- Continue publishing ADC, VBUS, and encoder data with neutral output.
- Ignore `freq_cmd_hz` until `motor_enable != 0`.
- Enter RUN only when VBUS is in range, encoder publication is fresh, current
  is near the calibrated zero point, and no application latch is active.

### RUN

- Slew `freq_run_hz` toward `freq_cmd_hz`; never step electrical frequency.
- Refuse a sign reversal until frequency and modulation have returned to zero.
- Compute the phase-voltage command:

```text
vphase_cmd = vf_boost_V + vf_v_per_hz * abs(freq_run_hz)
modulation = clamp(2 * vphase_cmd / vbus_V, 0, mod_max)
```

When `freq_run_hz` is zero, force `vphase_cmd` and modulation to zero regardless
of `vf_boost_V`; enabling a zero-frequency command must not silently apply a
stationary voltage vector.

- Advance electrical phase from the CPU1 control rate only:

```text
phase += 2*pi*freq_run_hz / V2K_ISR_HZ
```

- Generate balanced sinusoidal duty commands:

```text
duty_a = 0.5 + 0.5*modulation*sin(phase)
duty_b = 0.5 + 0.5*modulation*sin(phase - 2*pi/3)
duty_c = 0.5 + 0.5*modulation*sin(phase + 2*pi/3)
```

- Clamp the application modulation before the platform's final duty clamp.
  The platform clamp is a last boundary, not the normal control law.
- If `motor_enable` becomes zero, immediately command neutral for the first
  low-speed implementation and return to READY after frequency state resets.
  A controlled coast/ramp-down policy can replace this only when regenerative
  behavior is understood.

### SOFT_TRIP

Latch `user_trip` and command neutral when any of these occurs:

- qualified VBUS outside the configured range, with hysteresis/debounce based
  on the measured noise;
- absolute raw-count deviation from the Phase 5.2 zero offset above
  `i_dev_limit` for the configured short qualification interval;
- stale/invalid encoder publication during first-motion work;
- non-finite math result or duty outside the application's modulation bound;
- offset/noise validation failure.

Do not implement a user clear that can re-enable output in place. Recovery is
APP_STOP followed by APP_START, which invokes the accepted full user-state
reset before `setup()` and output release.

A platform hardware fault may preempt any state above. User code does not
clear, reconfigure, or mask that fault.

## 5. Math Implementation

Start with the clearest correct C implementation. One sine/cosine pair can
derive all three phases:

```text
sin_b = -0.5*sin_a - 0.8660254*cos_a
sin_c = -0.5*sin_a + 0.8660254*cos_a
```

Measure `ctrl_cycles_max` after integration. If the standard `sinf/cosf`
implementation does not fit the 20 kHz budget, replace only that primitive with
a narrowly integrated TI fast-math/MotorControl SDK implementation. Do not add
a platform-owned motor-control library or a hand-written approximation before
the measured need exists.

Use `float`, explicit finite/range checks, and phase wrapping that behaves for
negative frequency. No dynamic allocation and no blocking call is permitted.

## 6. Implementation Sequence

### 6.1 Neutral Physical-I/O Client

- refactor the existing Phase 5.0 ADC/VBUS/AS5600 reads into small user helpers;
- add Phase 5.2 zero-offset subtraction and raw-count deviation observables;
- publish application state and all interlock inputs;
- keep `motor_enable=0` and all duties neutral.

Done when a POWERED START reaches READY without rotor motion and Scope2000
shows coherent physical values.

### 6.2 V/f Generator with Output Suppressed

- implement frequency slew, phase accumulation, V/f magnitude, and three duty
  calculations;
- observe the calculated duties while still forcing the actual output neutral;
- confirm their sum/spacing, bound, direction sign, and reset behavior.

This is a code check, not a new hardware-verification campaign.

### 6.3 First Rotation

1. Secure and unload the motor; use the Phase 5.2 bus voltage and supply limit.
2. APP_START and wait for `motor_state=READY`; `motor_enable` remains zero.
3. Set a small electrical-frequency request and low V/f magnitude.
4. Set `motor_enable=1` for a short run.
5. Confirm rotor movement, encoder direction/progress, balanced raw-current
   shape, and deviations below `i_dev_limit` without a hardware trip.
6. Set `motor_enable=0`, confirm neutral output, then APP_STOP.

If direction is wrong, power down before changing phase order, encoder sign, or
command sign. Do not correct wiring/mapping while the bridge is enabled.

### 6.4 Bounded Ramp

After first rotation succeeds, increase frequency and V/f only within the
predeclared commissioning bounds. Capture one start, steady low-speed interval,
and stop. Stop before thermal or load behavior becomes relevant; those belong
to later motor-control phases.

## 7. Acceptance

Phase 5.5 is complete when:

- [ ] CPU1/CPU2 RAM and FLASH builds pass with the application under the user
  ownership/reset boundary;
- [ ] the baked catalog contains the tunables and observables with no skipped
  required scalar;
- [ ] APP_START alone remains neutral and reaches READY;
- [ ] current offsets are acquired under powered neutral switching and the raw
  deviations remain inside the Phase 5.2 commissioning bound;
- [ ] explicit enable produces a bounded V/f ramp and visible motor rotation;
- [ ] encoder sequence and angle move consistently with the observed rotor;
- [ ] user disable returns to neutral, APP_STOP locks outputs, and a hardware
  fault still bypasses user code;
- [ ] `isr_overflow` and `isr_budget_violation` do not increase during the run;
- [ ] one short first-motion record is added to `BRINGUP.md` with build hash,
  bus voltage/current limit, V/f settings, peak currents, direction, duration,
  and stop reason.

No repeated endurance run is required for Phase 5.5 acceptance.

## 8. Handoff to Later Phase 5 Work

The V/f application establishes that the platform can safely execute real user
motor code with tunable parameters and full observability. Later applications
may then proceed in small replacements:

1. current-loop application using vendor DCL/MotorControl SDK primitives;
2. encoder electrical-angle alignment and closed-loop commutation;
3. speed loop;
4. position loop.

Each remains app-owned. The platform boundary does not move when the control
algorithm becomes more sophisticated.
