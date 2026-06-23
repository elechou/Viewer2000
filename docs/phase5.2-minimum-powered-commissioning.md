# Phase 5.2 — POWERED Neutral Commissioning

> **Status (2026-06-23)**: software baseline implemented. A flashed FLASH image
> `0x4EE46EA6` passed the motor-disconnected POWERED neutral START/STOP
> lifecycle over SCI with no fault and with both CPU heartbeats alive. The
> motor-connected neutral offset/noise capture and the powered nFAULT lifecycle
> are not yet claimed.
>
> **Available equipment**: current-limited DC supply, multimeter, logic
> analyzer, Scope2000/SCI. No oscilloscope or differential probe is available.
>
> **Exact purpose**: change the platform from DRY_RUN to POWERED, but keep the
> user application physically incapable of commanding a rotating voltage
> vector. Prove that the real POWERED START/STOP/DRV/current-protection startup
> path works before Phase 5.5 adds motor actuation.

## 1. Software Baseline

Phase 5.2 makes two deliberate code changes:

```c
#define WIRE_POWERSTAGE_MODE WIRE_POWERSTAGE_MODE_POWERED
#define WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED 1u
```

The tracked CPU1 RAM and FLASH compiler predefines are in `cpu1/.cproject`.
`cpu1/wire/wire_f28p65x.c` deliberately retains its fail-safe DRY_RUN/approval
off fallback when those project settings are absent.

`cpu1/app/user.c` has no tunable duty variable. Every control tick writes:

```c
v2k_io.out.duty_a = V2K_DUTY_NEUTRAL;
v2k_io.out.duty_b = V2K_DUTY_NEUTRAL;
v2k_io.out.duty_c = V2K_DUTY_NEUTRAL;
```

Therefore APP_START may wake/configure the DRV and release the protected PWM
path, but this application can only request equal 50% duties. Phase 5.2
contains no V/f generator, frequency command, phase accumulator, or motor
enable parameter.

Current build results:

| Configuration | Result | Baked build hash |
|---|---|---:|
| CPU1 FLASH | full build, 0 errors, 5 known SysConfig board-connection warnings | `0xCB2C96A6` |
| CPU1 RAM | full build, 0 errors, 5 known SysConfig board-connection warnings | `0xE757B88E` |

The warnings are the existing DRV ENABLE/CS/SPID LaunchPad hardware-route
warnings already audited in Phase 5.0.

## 2. What We Will Do on Hardware

This phase is one short powered-neutral session. It does not rotate the motor.

### 2.1 Program

1. Turn VM off and leave the motor disconnected from J5.
2. Terminate any CCS GUI debug session that owns XDS110.
3. Flash CPU1/CPU2 with `tools/ccs/flash_dual_core_f28p65x.sh`.
4. Set S3 to Flash boot and power-cycle.

Programming remains CPU1-only through the repository Flash tool; do not load
CPU2 from a second debug session.

After the power cycle, use SCI only. Do not attach DSS/CCS for a live health
check: this target configuration's GEL connect hook can return CPU2 to the
Boot-ROM wait state after CPU1's one-time boot command has already completed.
That stops the CPU2 LED and SCI service until the next physical power cycle.

### 2.2 POWERED Start, Motor Disconnected

1. Use the previously verified approximately 10–12 V supply point and start
   with the existing 0.25 A current limit.
2. Confirm IDLE before issuing APP_START.
3. Issue APP_START once and read the following values through Scope2000/SCI:

| Variable | Required value |
|---|---:|
| `pwr_mode` | `0` (POWERED) |
| `pwr_cfg_ok` | `1` |
| `sys_state` | RUNNING |
| `start_state` | `3` (READY) |
| `start_block` | `0` |
| `curr_trip_arm` | `1` |
| `curr_trip_cfg` | `0` |
| `drv_cfg_valid` | `1` |
| `drv_spi_errors` | no increase |
| `drv_status1/2` | no active fault |

4. Confirm `vbus_V` agrees with the multimeter and supply current is stable.
5. Issue APP_STOP and confirm IDLE. Measure DRV ENABLE low if a convenient
   test point is available.

Any failed START check already leaves OST asserted and disables the DRV. Do not
bypass `start_block` or current-window startup rejection.

### 2.3 POWERED Neutral, Motor Connected

1. Turn VM off, connect the secured and unloaded motor, then restore the same
   voltage/current limit.
2. APP_START once. Equal three-phase duty must produce no intended line-to-line
   voltage and no commanded rotation.
3. Observe `adc_ia_raw`, `adc_ib_raw`, and `adc_ic_raw` for a short fixed
   interval and record their neutral offsets and min/max noise.
4. Confirm no unexpected motion, supply-current rise, nFAULT, reset, ADC
   overflow growth, or DRV status fault.
5. APP_STOP and return to IDLE.

This gives the offset/noise information needed by the Phase 5.5 software
interlock. It does not claim a measured counts-per-amp calibration.

### 2.4 One Functional nFAULT Check

While running the neutral application at the same limited supply setting,
assert nFAULT using the already accepted GPIO82-to-hot-ground method.

Required result:

- immediate transition to FAULT;
- `fault_code=1` and one trip-count increment;
- foreground service disables the DRV;
- CLEAR_FAULT remains blocked while nFAULT is held low;
- release followed by CLEAR_FAULT returns to IDLE.

This checks the complete powered lifecycle. Without an oscilloscope it does not
measure edge-to-gate latency.

## 3. Logic Analyzer Boundary

The logic analyzer is not required for the POWERED session. An ordinary
USB-connected analyzer may reconnect the hot-side digital ground to the PC and
defeat the LaunchPad isolation arrangement.

If its grounding/isolation is not independently established, use it only in
DRY_RUN and disconnect it before applying VM. Never connect it to a gate,
source, phase, or switch node.

## 4. Explicitly Deferred Measurements

The following cannot be measured with the available instruments and do not
block the first low-energy Phase 5.5 rotation:

- gate-source amplitude, ringing, and switch-node overshoot;
- actual gate dead time under power;
- nFAULT/current-trip edge-to-all-six-gate shutdown latency;
- physical all-six-gate capture during a calibrated current trip;
- measured current-sense counts per ampere.

They remain mandatory before increasing to sustained operation, materially
higher bus voltage/current, or loaded closed-loop control. Their absence must
remain recorded as residual commissioning risk; supply current limiting does
not turn the provisional `512/3584` count window into a calibrated motor limit.

## 5. Exit Criteria

Phase 5.2 completes when:

- [x] tracked CPU1 baseline selects POWERED and approval enabled;
- [x] user application is hard-locked to three neutral duties;
- [x] CPU1 RAM and FLASH full builds pass;
- [ ] the images are flashed and cold-boot to IDLE;
- [ ] motor-disconnected POWERED START reaches READY with clean DRV/current
  diagnostics and stable supply current;
- [ ] APP_STOP returns to IDLE and disables the DRV;
- [ ] motor-connected neutral START produces no commanded motion or abnormal
  current and records three current offsets/noise ranges;
- [ ] one powered nFAULT lifecycle returns cleanly to IDLE.

After those items pass, Phase 5.5 may replace the neutral application with a
strictly bounded V/f first-rotation application. Phase 5.2 itself never turns
the motor.
