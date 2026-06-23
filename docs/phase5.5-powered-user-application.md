# Phase 5.5 — Powered User Application and First Motor Rotation

> **Document status (2026-06-23)**: implemented and accepted for the first
> low-energy observed rotation. The accepted FLASH image was `0x2D869C78`.
>
> **Entry gate used for this accepted node**: the normal entry is a complete
> [Phase 5.2](phase5.2-minimum-powered-commissioning.md): POWERED neutral
> START/STOP, motor-connected neutral current offsets, and the powered nFAULT
> lifecycle passed. The current 2026-06-23 first-rotation branch starts from the
> narrower verified node: flashed image `0x4EE46EA6` passed
> motor-disconnected POWERED neutral START/STOP over SCI. Therefore the Phase
> 5.5 application must acquire neutral current offsets before allowing V/f,
> remain at the same supply voltage/current limit, run unloaded and briefly, and
> avoid claiming the remaining Phase 5.2 captures as passed.
>
> **Accepted result**: the implemented `cpu1/app/user.c` application completed
> APP_START, neutral current-offset acquisition, explicit `motor_enable=1`, a
> one-second 2 Hz / 0.10-modulation V/f window, `motor_enable=0`, and APP_STOP
> with no fault. With VM at 12 V and the supply current limit at 0.25 A, the
> operator observed approximately one quarter of a mechanical revolution.
>
> **Post-acceptance refactor**: the current commissioning application keeps the
> accepted V/f behavior but is intentionally written as a streaming embedded
> control path inside `control()`: sample, offset, neutral-or-V/f, output. The
> accepted first-rotation evidence remains the tagged image above.
>
> **Post-acceptance commissioning baseline**: the current FLASH source builds
> as image `0xF0E2A1E2` and uses `freq_cmd_hz=2.0`, `freq_slew=5.0`,
> `vf_v_per_hz=0.20`, `vf_boost_V=0.40`, `mod_max=0.15`, and
> `offset_ticks=4000`. The operator reported an unloaded 12 V run for about
> 20 minutes with normal user disable, APP stop/restart behavior, and a
> functional nFAULT emergency stop. This is still not loaded operation,
> calibrated current, gate timing, or measured shutdown latency.

## 0. Continuation Checkpoint

The accepted node should be committed and tagged as
`phase5.5-first-rotation`.

Current implementation facts:

- `APP_START` alone still commands neutral. Motion requires a parameter write
  setting `motor_enable=1` after the application reaches `app_state=READY`.
- Startup offset averaging uses `offset_ticks=4000` by default.
- The current user layer has no app-level fault latch, reason-code gate, VBUS
  gate, or current-deviation gate. VBUS and current deviation are observables;
  platform hardware protection remains the safety boundary.
- The current commissioning defaults are `freq_cmd_hz=2.0`, `freq_slew=5.0`,
  `vf_v_per_hz=0.20`, `vf_boost_V=0.40`, `mod_max=0.15`, and
  `offset_ticks=4000`.
- The historical accepted first-rotation image used `freq_cmd_hz=2.0`,
  `freq_slew=5.0`, `vf_v_per_hz=0.15`, `vf_boost_V=0.30`, `mod_max=0.10`,
  `i_dev_limit=320.0`, `vbus_min_V=8.0`, and `vbus_max_V=18.0`.
- The largest sampled CPU-side raw current deviation in the accepted run was
  `i_dev_abs=29.1616`, far below the configured 320-count application limit.
- The historical accepted first-rotation test ended in platform `IDLE`,
  `fault_code=0`, app `READY`, `app_fault=0`, all duties neutral, and the SCI
  port released.

Recommended next-session starting point:

1. Repeat the accepted endurance baseline only if a baseline check is needed:
   12 V, unloaded, `freq_cmd_hz=2.0`, `vf_v_per_hz=0.20`,
   `vf_boost_V=0.40`, `mod_max=0.15`.
2. For the next motion increment, change only one variable at a time. Keep
   later increases short and unloaded until current calibration and gate timing
   are captured.
3. Record `app_state`, `freq_run_hz`, `mod_cmd`, duty commands, `i_dev_abs`,
   platform state/fault, and the observed mechanical motion.
4. Do not claim loaded operation, calibrated ampere current, gate timing, or
   shutdown latency from these low-energy runs.

## 1. Goals and Non-Goals

Goals:

- make POWERED operation an ordinary user-code client of the platform;
- keep APP_START neutral so a START command alone cannot rotate the motor;
- publish raw/offset-relative current, physical bus voltage, encoder, command,
  and application-state data;
- require an explicit user `motor_enable` after readiness checks;
- ramp frequency and voltage with bounded slew and modulation;
- produce balanced three-phase sinusoidal PWM for first-motion V/f;
- return to neutral on user disable;
- retain hardware Trip Zone/CMPSS/DRV protection as the safety authority.

Non-goals:

- no FOC, Clarke/Park current control, observer, speed PI, or position loop;
- no motor-parameter identification beyond what the V/f demo needs;
- no field weakening, regenerative-braking strategy, or direction reversal
  while spinning;
- no rated-speed, rated-current, loaded, or unattended run;
- no broad endurance claim beyond the recorded unloaded low-energy baseline;
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

The platform remains responsible for ADC timing, the recommended ePWM command
path, DRV lifecycle, and all protection routing. User code reads the completed
semantic raw frame from `v2k_io.adc`, copies the AS5600 cache, performs math,
and submits duty commands with `v2k_pwm_apply(a, b, c)`. Advanced users may
directly call native TI read/write APIs, but direct EPWM writes are intentionally
outside the `duty_*_cmd` diagnostics path.

## 3. User Parameters and Observables

All mutable user scalars are automatically `USER|PARAM|SCOPE`. Keep visible
names at 15 ASCII characters or fewer.

Minimum tunables:

| Name | Meaning | Reset value |
|---|---|---:|
| `motor_enable` | Explicit run request inside platform RUNNING | `0` |
| `freq_cmd_hz` | Signed electrical-frequency request | `2.0f` |
| `freq_slew` | Frequency slew limit in Hz/s | `5.0f` |
| `vf_v_per_hz` | Phase-peak V/Hz slope | `0.20f` |
| `vf_boost_V` | Low-speed voltage boost | `0.40f` |
| `mod_max` | Maximum sinusoidal modulation index | `0.15f` |
| `offset_ticks` | Zero-current averaging window | `4000` |

Minimum observables:

```text
app_state, freq_run_hz, elec_phase, mod_cmd
app_duty_a, app_duty_b, app_duty_c
adc_ia_raw, adc_ib_raw, adc_ic_raw, ia_offset, ib_offset, ic_offset
ia_dev, ib_dev, ic_dev, i_dev_abs
adc_vbus_raw, vbus_V, enc_raw, enc_angle, enc_seq, enc_ok
```

`i_dev_abs` is only an observable raw-count deviation from the acquired offset;
it is not an ampere limit and not protection. The asynchronous hardware trip
and supply current limit remain independently authoritative. Ampere conversion
waits for proper current-calibration equipment.

## 4. Application State Machine

Use an explicit application state published as `app_state`:

```text
OFFSET -> READY -> RUN
```

### OFFSET

- `setup()` initializes the V/f instance and leaves all duties neutral.
- `control()` reads the completed current frame every tick.
- Accumulate A/B/C zero-current offsets while all three duties remain 0.5.
- Publish the latest `wire_as5600_get_latest()` sample before READY. The
  encoder is observed but is not yet used as a first-rotation interlock or for
  commutation.

### READY

- Continue publishing ADC, VBUS, and encoder data with neutral output.
- Ignore `freq_cmd_hz` until `motor_enable != 0`.
- Enter RUN when `motor_enable != 0`.

### RUN

- Slew `freq_run_hz` toward `freq_cmd_hz`; never step electrical frequency.
- Refuse a sign reversal until frequency and modulation have returned to zero.
- Compute the phase-voltage command:

```text
vphase_cmd = vf_boost_V + vf_v_per_hz * abs(freq_run_hz)
modulation = clamp(2 * vphase_cmd / vbus_V, 0, mod_max)
```

The accepted implementation computes `mod_cmd` from
`vf_boost_V + vf_v_per_hz * abs(freq_run_hz)` and clamps it with `mod_max`
before writing the duty commands.

- Advance electrical phase from the CPU1 control rate only:

```text
elec_phase += freq_run_hz / V2K_ISR_HZ
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

The first implementation uses a small deterministic sine approximation over a
unit phase range instead of pulling in a math-library `sinf()` dependency.

### No App-Level Fault Latch

The current user application has no app-level FAULT latch and no reason-code
gate. It stays neutral during offset acquisition or when `motor_enable=0`, then
runs the V/f calculation when enabled. VBUS and current deviation are observed
but not adjudicated by user code.

A platform hardware fault may preempt any state above. User code does not
clear, reconfigure, or mask that fault.

## 5. Math Implementation

The accepted implementation uses unit-cycle phase and computes the three phases
as:

```text
duty_a = 0.5 + 0.5*mod_cmd*sin01(elec_phase)
duty_b = 0.5 + 0.5*mod_cmd*sin01(elec_phase + 2/3)
duty_c = 0.5 + 0.5*mod_cmd*sin01(elec_phase + 1/3)
```

Use `float`, explicit range clamps, and phase wrapping that behaves for
negative frequency. No dynamic allocation and no blocking call is permitted.

## 6. Implementation Sequence

### 6.1 Neutral Physical-I/O Client

- refactor the existing Phase 5.0 ADC/VBUS/AS5600 reads into small user helpers;
- add Phase 5.2 zero-offset subtraction and raw-count deviation observables;
- publish application state and measured inputs;
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
2. APP_START and wait for `app_state=READY`; `motor_enable` remains zero.
3. Confirm all duties are neutral.
4. Set a small electrical-frequency request and low V/f magnitude.
5. Set `motor_enable=1` for a short run.
6. Confirm rotor movement, encoder direction/progress, and raw-current shape
   without a hardware trip.
7. Set `motor_enable=0`, confirm neutral output, then APP_STOP.

If direction is wrong, power down before changing phase order, encoder sign, or
command sign. Do not correct wiring/mapping while the bridge is enabled.

### 6.4 Bounded Ramp

After first rotation succeeds, increase frequency and V/f only within the
predeclared commissioning bounds. Capture one start, steady low-speed interval,
and stop. Stop before thermal or load behavior becomes relevant; those belong
to later motor-control phases.

## 7. Acceptance

Accepted for the 2026-06-23 low-energy first-rotation node:

- [x] CPU1 FLASH build passed with the application under the user ownership and
  reset boundary.
- [x] The user-rebuilt, flashed image `0x2D869C78` enumerated 42 baked user
  descriptors.
- [x] APP_START alone remained neutral long enough to acquire offsets and reach
  `app_state=READY`.
- [x] Current offsets were acquired with `offset_count=4000`; pre-enable
  `i_dev_abs` was about 2 raw counts.
- [x] Explicit enable produced a bounded V/f ramp and visible motor rotation.
- [x] User disable returned the application to neutral, and APP_STOP returned
  the platform to IDLE with `fault_code=0`.
- [x] One short first-motion record was added to `BRINGUP.md` with build hash,
  bus voltage/current limit, V/f settings, sampled current deviation, duration,
  stop result, and observed motion.

Not claimed by this node:

- [ ] CPU1/CPU2 RAM-build acceptance for this application.
- [ ] Encoder sequence/angle consistency with the observed rotor movement.
- [ ] ISR overflow/budget counters sampled before and after the V/f window.
- [ ] Loaded operation, current calibration in amperes, gate-source/switch-node
  timing, measured nFAULT-to-PWM shutdown latency, or calibrated-current
  shutdown.

No repeated endurance run is required for this Phase 5.5 first-rotation
acceptance.

Post-acceptance streaming commissioning baseline:

- [x] CPU1 FLASH build `0x0506B814` passes with the streaming user application
  baked into the user descriptor table: 37 user descriptors, skipped 0.
- [x] CPU1 FLASH build `0xF0E2A1E2` passes with the current `0.20/0.40/0.15`
  commissioning defaults baked into the user descriptor table: 37 user
  descriptors, skipped 0.
- [x] Motor-connected, unloaded, 12 V operation ran for about 20 minutes with
  normal user disable and APP stop/restart behavior.
- [x] A functional nFAULT emergency-stop check was exercised successfully.
- [ ] Loaded operation, calibrated current in amperes, oscilloscope
  gate/switch-node timing, and measured shutdown latency remain open.

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
