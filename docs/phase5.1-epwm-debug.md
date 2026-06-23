# Phase 5.1 — ePWM Output Debug

## 0. Summary

**Status: ROOT CAUSE FOUND + firmware fix implemented and flashed (2026-06-22).
Hardware cold-boot + scope re-verification is the remaining gate.**

This note captures the Phase 5.1 bench/debug state after the Phase 5.0
power-stage interface was accepted. The immediate issue is that Viewer2000
appeared to produce valid low-side PWM outputs but distorted, low-amplitude
high-side outputs on the Phase 5.0 three-phase PWM pins.

**Outcome (see section 11):** the high-side (EPWMxA) pins were floating because
the `TZCTL.DCAEVT1` digital-compare output action was left at its reset value
`0 = High-Impedance` and is applied to `EPWMxA` whenever the DCAEVT1 condition is
asserted — independent of whether the current trip is armed in `TZSEL`. In
DRY_RUN the gate driver sleeps, the current-sense amplifiers are unpowered, the
CMPSS low comparator asserts `TRIPIN7` with no real current, and that idle
DCAEVT1 floated all three high-side outputs while the low side kept switching.
The cause was confirmed live on hardware (a real-time register write that
removed only the DCAEVT1->A force recovered the high side instantly), and the
firmware now defaults the DCAEVT1 output action to `DISABLE` when disarmed.

The earlier conclusion still holds and explains the failed bisect: the TI
`epwm_ex8_deadband` example could not reproduce the fault even with the DCAEVT1
configuration added, because the example has no CMPSS/DRV/X-BAR overcurrent chain
to actually assert `TRIPIN7` — so its DCAEVT1 condition was never true.

## 1. Observed Symptoms

The bench symptom was first seen with the Viewer2000 firmware and the external
power stage setup, then reproduced after removing the power stage and measuring
the LaunchPad alone.

Reported waveform:

- Around 20.2 kHz periodic bipolar spike response.
- Each switching edge produces a positive or negative spike.
- The signal then decays exponentially back toward the near-zero baseline.
- It looks like an edge-excited damped transient or RC recovery, not a driven
  3.3 V CMOS square wave.

Reported side grouping:

| Logical side | ePWM output | GPIOs | Bench observation |
|---|---:|---:|---|
| High side / A output path | EPWM1A, EPWM2A, EPWM8A | GPIO0, GPIO2, GPIO99 | Distorted, low amplitude |
| Low side / B output path | EPWM1B, EPWM2B, EPWM8B | GPIO1, GPIO3, GPIO75 | Normal 3.3 V square wave |

The early spoken notes included one duplicated GPIO number while describing the
groups. The stable interpretation used for this debug note is the Phase 5.0
pin mapping above: ePWMxA = GPIO0/GPIO2/GPIO99 and ePWMxB =
GPIO1/GPIO3/GPIO75.

## 2. Known-Good Hardware Checks

The hardware and the specific GPIO2 pin are not inherently bad:

- The TI `epwm_ex8_deadband` example in
  `C:\Users\SHOU\Desktop\Code\TI_workspace\epwm_ex8_deadband` drove
  PWM2A/GPIO2 as a normal 3.3 V square wave.
- A temporary copy was brought into the Viewer2000 workspace at
  `C:\Users\SHOU\Desktop\Code\20260610_Viewer2000\Viewer2000\epwm_ex8_deadband`.
- The copied example continued to drive GPIO2 normally.
- After manually changing the example's action qualifier to match the Viewer
  style, GPIO2 still remained normal:
  - B AQ events all `No change`.
  - A zero event `No change`.
  - A period event `No change`.
  - A up-count CMPA event `High`.
  - A down-count CMPA event `Low`.
- Adding a foreground software CMPA duty update also remained normal.

This makes a simple launchpad pin failure, scope setup error, or "AQB must be
configured for EPWMxB to be valid" explanation unlikely.

## 3. Controlled Example Bisect

The copied `epwm_ex8_deadband` project was used as a clean carrier to add
Viewer2000 features one layer at a time. All observed scope checks on GPIO2
remained normal 3.3 V square waves.

### 3.1 Baseline example

Original TI example behavior was normal on GPIO2/PWM2A.

Baseline runtime ePWM2 values included:

| Register | Value | Note |
|---|---:|---|
| `TBCTL` | `0x0902` | Example timing |
| `AQCTLA` | `0x0060` | A set on up CMPA, clear on down CMPA |
| `AQCTLB` | `0x0000` | B AQ no-change path |
| `DBCTL` | `0x000B` | Dead-band enabled path |
| `DBRED` | `400` | Original example value |
| `DBFED` | `200` | Original example value |
| `TBPRD` | `2000` | Original example period |
| `CMPA` | `1000` | Read from the active CMPA word |

### 3.2 Viewer timing without sync or trip-zone

SysConfig was changed through the SysConfig MCP to match the Viewer timing:

- EPWMCLKDIV `/1`.
- ePWM2 free run.
- Clock dividers `/1`.
- `TBPRD = 5000`.
- `CMPA = 2500`.
- RED/FED shadow mode enabled.
- `DBRED = 200`.
- `DBFED = 200`.

Scope result: GPIO2 remained normal.

Runtime ePWM2 snapshot:

| Register | Value |
|---|---:|
| `TBCTL` | `0x8002` |
| `CMPCTL` | `0x0000` |
| `DBCTL` | `0x0C0B` |
| `DBCTL2` | `0x0000` |
| `AQCTLA` | `0x0060` |
| `AQCTLB` | `0x0000` |
| `DBRED` | `200` |
| `DBFED` | `200` |
| `TBPRD` | `5000` |
| `CMPA` | `2500` |
| TZ flags | `0` |

### 3.3 Viewer sync path

Added EPWM1-to-EPWM2 sync:

- EPWM1 configured as the zero-event sync-out master.
- EPWM2 phase load enabled.
- EPWM2 sync-in source set to EPWM1 sync-out.
- Software forced an EPWM1 sync pulse after TBCLKSYNC enable.

Scope result: GPIO2 remained normal.

Runtime checks included:

| Register | Value |
|---|---:|
| `EPWM1_SYNCOUTEN` | `0x0003` |
| `EPWM2_SYNCINSEL` | `0x0001` |
| `EPWM2_TBCTL` | `0xA006` |
| `EPWM2_TZFLG` | `0x0000` |

### 3.4 Static trip-zone path

Added Viewer-style static trip-zone settings:

- TZA/TZB force low.
- One-shot TZ1 source.
- CBC TZ6 source.
- GPIO82 mapped to INPUT X-BAR INPUT1.

Scope result while CPU was running: GPIO2 remained normal.

Important debugger finding: with CBC6 configured, halting the CPU under the
debugger can set `TZCBCFLG = 0x0020` and force PWM low. This is expected for the
debug-protection path and must not be confused with the running waveform. Scope
judgment must be made while the CPU is running.

### 3.5 Software OST force/release path

Added Viewer-style initial/output-lock behavior:

- Force OST before `Board_init()`.
- Force OST again after `Board_init()`.
- Add a debugger-controlled release command that clears `EPWM_TZ_INTERRUPT |
  EPWM_TZ_FLAG_OST`.

Scope result after release: GPIO2 remained normal.

### 3.6 DCAEVT1 configuration

Added Viewer-style DCAEVT1 digital-compare configuration:

- DCAH selects `TRIPIN7`.
- DCAEVT1 condition = DCAH high.
- DCAEVT1 source = original signal.
- DCAEVT1 sync mode = not synced.

With DCAEVT1 configured but disabled, GPIO2 remained normal.

With DCAEVT1 enabled and no active `TRIPIN7` source, GPIO2 still remained
normal.

## 4. Viewer2000 Static Configuration Facts

The Viewer2000 generated SysConfig output currently maps the three phases as
expected:

| Phase role | ePWM | GPIO A/B |
|---|---:|---:|
| PWM_TB / phase B / master | EPWM1 | GPIO0 / GPIO1 |
| PWM_PHASE_A | EPWM2 | GPIO2 / GPIO3 |
| PWM_PHASE_C | EPWM8 | GPIO99 / GPIO75 |

Generated ePWM settings for EPWM1, EPWM2, and EPWM8 all follow the same basic
pattern:

- Counter mode: up-down.
- `TBPRD = 5000`.
- `CMPA = 2500`.
- AQ A:
  - zero: no change.
  - period: no change.
  - up-count CMPA: high.
  - down-count CMPA: low.
- AQ B:
  - all listed events no change.
- Dead-band:
  - RED enabled.
  - FED enabled.
  - FED polarity inverted.
  - RED/FED count = 200.
- TZ:
  - TZA/TZB force low.
  - one-shot TZ1 enabled.
  - CBC TZ6 enabled.
- EPWM1 additionally provides SOCA and zero-event sync-out.

The generated pinmux also looks correct:

| GPIO | Expected function |
|---:|---|
| GPIO0 | EPWM1_A |
| GPIO1 | EPWM1_B |
| GPIO2 | EPWM2_A |
| GPIO3 | EPWM2_B |
| GPIO75 | EPWM8_B |
| GPIO99 | EPWM8_A |

## 5. Runtime Viewer2000 Snapshot

A DSS attach-only helper was created:

```text
tools/ccs/read_pwm_runtime_regs.js
```

The helper intentionally does not load a program, erase Flash, or reset the
target. It attaches to CPU1 through the project-local `TMS320F28P650DK9.ccxml`,
loads symbols only for expression evaluation, reads ePWM/GPIO registers, and
disconnects.

The first useful Viewer snapshot was not a valid active-PWM failure snapshot
because the state machine was still in IDLE:

| Symbol | Value | Meaning |
|---|---:|---|
| `g_v2k_sm_state` | `1` | `V2K_STATE_IDLE` |
| `g_v2k_fault_code` | `0` | no latched platform fault |
| `g_v2k_app_enabled` | `0` | user control disabled |
| `g_v2k_tz_int_cnt` | `0` | no counted real TZ interrupt |

`V2K_STATE_IDLE` means "ready with PWM outputs locked." A scope reading taken
in this state is not evidence of the RUNNING PWM waveform.

### 5.1 ePWM register values in IDLE

The ePWM register state in that IDLE snapshot still looked structurally correct:

| Register | EPWM1 | EPWM2 | EPWM8 |
|---|---:|---:|---:|
| `TBCTL` | `0x8002` | `0xA006` | `0xA006` |
| `SYNCINSEL` | `0x0001` | `0x0001` | `0x0001` |
| `SYNCOUTEN` | `0x0003` | `0x0001` | `0x0001` |
| `CMPCTL` | `0x0100` | `0x0000` or `0x0100` in separate reads | `0x0100` |
| `DBCTL` | `0x0C0B` | `0x0C0B` | `0x0C0B` |
| `DBCTL2` | `0x0000` | `0x0000` | `0x0000` |
| `AQCTLA` | `0x0060` | `0x0060` | `0x0060` |
| `AQCTLB` | `0x0000` | `0x0000` | `0x0000` |
| `DBRED` | `0x00C8` | `0x00C8` | `0x00C8` |
| `DBFED` | `0x00C8` | `0x00C8` | `0x00C8` |
| `TBPRD` | `0x1388` | `0x1388` | `0x1388` |
| `CMPA` | `0x09C4` | `0x09C4` | `0x09C4` |
| `TZSEL` | `0x0120` | `0x0120` | `0x0120` |
| `TZDCSEL` | `0x0002` | `0x0002` | `0x0002` |
| `TZCTL` | `0x000A` | `0x000A` | `0x000A` |
| `DCTRIPSEL` | `0x0006` | `0x0006` | `0x0006` |
| `DCACTL` | `0x0082` | `0x0082` | `0x0082` |

The nonzero TZ flag values in IDLE should not be over-interpreted until a
RUNNING snapshot is taken. The output-lock state intentionally uses trip-zone
gating, and debugger halts can also interact with CBC6.

### 5.2 GPIO mux values in IDLE

The same snapshot showed pinmux values consistent with the expected ePWM pins:

| Register | Value |
|---|---:|
| `GPIO.GPAMUX1` | `0x00000055` |
| `GPIO.GPAGMUX1` | `0x00000000` |
| `GPIO.GPACSEL1` | `0x00000000` |
| `GPIO.GPCMUX1` | `0x00400000` |
| `GPIO.GPCGMUX1` | `0x00000000` |
| `GPIO.GPCCSEL2` | `0x00000000` |
| `GPIO.GPDMUX1` | `0x000500C0` |
| `GPIO.GPDGMUX1` | `0x00000000` |
| `GPIO.GPDCSEL1` | `0x00000000` |

An attempted GPIO pin-level sampling loop was not treated as evidence because
it was performed while the firmware was in IDLE and did not match the
bench-observed low-side square waves. The next useful sampling must be done
only after `g_v2k_sm_state == V2K_STATE_RUNNING`.

## 6. Firmware Bug Found During Debug

One real safety-path bug was found in `wire_pwm_output_is_locked()`:

```c
return wire_pwm_base_is_locked(WIRE_PWM_PHASE_A_BASE) &&
       wire_pwm_base_is_locked(WIRE_PWM_PHASE_B_BASE) &&
       wire_pwm_base_is_locked(WIRE_PWM_PHASE_C_BASE);
```

This reported "unlocked" if any one phase had cleared OST, even if another
phase was still locked. The correct predicate is an OR: the output set is still
locked if any phase is locked.

The local fix is:

```c
return wire_pwm_base_is_locked(WIRE_PWM_PHASE_A_BASE) ||
       wire_pwm_base_is_locked(WIRE_PWM_PHASE_B_BASE) ||
       wire_pwm_base_is_locked(WIRE_PWM_PHASE_C_BASE);
```

`cpu1` built successfully after this one-line change. This bug is worth fixing
for safety and release-path determinism, but by itself it does not yet explain
a side-A-only distorted waveform.

## 7. Current Working Hypotheses

### 7.1 Mostly ruled out

These are now unlikely to be the primary cause:

- A damaged GPIO2 output buffer.
- A scope setup problem.
- Static AQ A configuration.
- Static AQ B no-change configuration.
- Dead-band enable/polarity by itself.
- EPWM1-to-EPWM2 sync by itself.
- TZ1/TZ6 static configuration by itself.
- DCAEVT1 configured/enabled with no active trip source by itself.
- Viewer SysConfig pinmux for GPIO0/1/2/3/75/99, at least in the IDLE snapshot.

### 7.2 Still plausible

These still need direct evidence:

- The failing measurement may have been taken while Viewer2000 was still in
  `V2K_STATE_IDLE`, where PWM outputs are intentionally locked. In that state,
  low-amplitude edge spikes can be capacitive/measurement artifacts rather than
  actively driven PWM.
- The START/release sequence may behave differently in the full Viewer2000
  firmware than in the example scaffold. The next snapshot must prove
  `g_v2k_sm_state == V2K_STATE_RUNNING` and `g_v2k_app_enabled == 1`.
- A RUNNING-only register or force path not yet captured may be overriding the
  A outputs. The next DSS snapshot should include additional ePWM force/global
  load registers if the symptom persists in RUNNING.
- A measurement-path difference may still exist, but this is less likely after
  the bare-LaunchPad and TI-example checks.

## 8. Next Debug Steps

### 8.1 Produce a valid RUNNING-state snapshot

Do not judge the Viewer2000 PWM waveform until all of these are true:

- CPU is running, not halted at a breakpoint.
- `g_v2k_sm_state == V2K_STATE_RUNNING`.
- `g_v2k_app_enabled == 1`.
- `g_v2k_fault_code == V2K_FAULT_NONE`.
- TZ/CBC flags are checked after the output release, not immediately after a
  debugger halt.

The command-plane START path is:

1. Read `cmd_seq` from `CPU2TOCPU1 MSGRAM` at `0x03B000`.
2. Write `cmd_code = V2K_CMD_APP_START` (`1`) at the command request.
3. Write `arg0 = 0`, `arg1 = 0`.
4. Write `cmd_seq = old + 1` last.
5. Wait until CPU1 status reports the matching `ack_seq`.
6. Confirm `g_v2k_sm_state == 2`.

The default firmware mode is expected to be DRY_RUN unless overridden at build
time, so START should release only the MCU PWM pins and leave the DRV8323RS
asleep.

### 8.2 Scope checklist after START

After RUNNING is confirmed, scope these pairs:

| Pair | Expected if healthy |
|---|---|
| GPIO0/GPIO1 | Complementary 3.3 V PWM with dead-band |
| GPIO2/GPIO3 | Complementary 3.3 V PWM with dead-band |
| GPIO99/GPIO75 | Complementary 3.3 V PWM with dead-band |

If GPIO1/GPIO3/GPIO75 are normal but GPIO0/GPIO2/GPIO99 are still distorted,
capture another DSS register snapshot in that exact condition.

### 8.3 Extend the register snapshot if RUNNING still fails

Add these registers to the snapshot helper before the next deep comparison:

- `AQSFRC`
- `AQCSFRC`
- `TZCTL2`
- `TZCTLDCA`
- `TZCTLDCB`
- `DBCTL3` if available on this device header
- Global-load registers (`GLDCTL`, `GLDCFG`, etc.) if present
- Any output-XBAR or CLB path registers only if the pinmux no longer points
  directly to ePWM

The goal is to distinguish "ePWM A waveform is not being generated" from
"ePWM A waveform exists internally but is being forced/gated after AQ/dead-band."

## 9. Debug Artifacts

Temporary/in-progress artifacts created during this debug round:

| Path | Purpose |
|---|---|
| `tools/ccs/read_pwm_runtime_regs.js` | DSS attach-only IDLE register snapshot helper (first cut) |
| `tools/ccs/run_pwm_runtime_probe.js` | DSS attach-only RUNNING probe: injects APP_START via the CPU2->CPU1 MSGRAM command plane, polls to RUNNING, dumps EPWM/TZ/DC + CMPSS + X-BAR + pin levels, leaves both cores running |
| `tools/ccs/probe_dcaevt1_causal_test.js` | DSS live causal test: toggles `TZCTL.DCAEVT1` action (Hi-Z vs no-action) in real time and clears the debugger-halt OST/CBC artifact, to prove the high-side cause on the scope |
| `.ti_appdata/` | Local TI appdata/cache directory used so DSS can run without writing to an inaccessible user config directory (now git-ignored) |

The workspace-local `epwm_ex8_deadband/` carrier copy used during the bisect was
removed after the root cause was found. The DSS helpers are reusable bring-up
tooling, kept alongside the Flash launcher; `.ti_appdata/` is git-ignored.

## 10. Safety Notes

- Do not use `launchTargetConfiguration` for this repository's project-local
  target configs; use CCS GUI/debugProject/DSS as described in `AGENTS.md`.
- Do not program Flash through a generic CCS debug load without respecting the
  F28P65x CPU1/CPU2 Flash bank map rules in `AGENTS.md`.
- Do not scope/judge PWM while the CPU is halted when CBC6 is enabled.
- Do not move from DRY_RUN to POWERED mode until the ePWM high-side issue is
  understood and the Phase 5.0 protection checks are re-verified.

## 11. Root Cause and Resolution (2026-06-22)

### 11.1 The missing RUNNING snapshot

A new attach-only DSS probe (`tools/ccs/run_pwm_runtime_probe.js`) captured the
snapshot every previous attempt lacked. It attaches CPU1 (register reads) and
CPU2 (command injection) without loading a program or resetting either core,
writes `V2K_CMD_APP_START` into the CPU2->CPU1 MSGRAM command plane at `0x03B000`
(CPU2 is the hardware single-writer of that region, which is why the write goes
through a CPU2 session), polls CPU1 to `V2K_STATE_RUNNING`, and reads the ePWM /
trip-zone / digital-compare / CMPSS registers in real time without ever halting
(so CBC6 cannot force the pins low under a debugger halt).

With `sys_state = RUNNING`, `fault_code = NONE`, `s_powerstage_mode = DRY_RUN`,
all three motor EPWMs (EPWM1/2/8) read identically:

| Register | Value | Meaning |
|---|---:|---|
| `TZFLG` | `0x0008` | `DCAEVT1` event flag set — digital-compare A event 1 is asserting |
| `TZSEL` | `0x0120` | `OSHT1 | CBC6` only; `DCAEVT1` (bit `0x4000`) **not** armed |
| `TZCTL` | `0x000A` | `TZA/TZB = low`; **`DCAEVT1` action bits[5:4] = 0 = High-Impedance** |
| `TZCTL2` | `0x0000` | `ETZE = 0`, so the legacy `TZCTL.DCAEVT1` action is in force |
| `DCTRIPSEL` | `0x0006` | `DCAH = TRIPIN7` |
| `TZDCSEL` | `0x0002` | `DCAEVT1 = DCAH high` |
| CMPSS7/8 `COMPSTS` | `0x0300` | `COMPLSTS | COMPLLATCH` — the **low** comparator is asserting (raw + latched); `DACL=512`, `DACH=3584` |
| EPWMXBAR `TRIP7_ENABLE` | `0xF002` | CMPSS + ADC-PPB muxes OR'd into `TRIPIN7` |

The key, non-obvious fact: **`DCAEVT1` is flagged (`TZFLG.DCAEVT1=1`) and forcing
the output even though it is not selected in `TZSEL`.** On C2000, the per-event
`TZCTL.DCAEVTx` output action is applied whenever the digital-compare event is
asserted; `TZSEL.DCAEVT1` only adds the event to the one-shot (OST) latch. So
"disarmed" (TZSEL cleared, as in DRY_RUN) does **not** stop the DCAEVT1 output
force. `DCAEVT1` acts only on `EPWMxA`; there is no `DCBEVT1`, so `EPWMxB` keeps
switching.

### 11.2 Root-cause chain

```
DRY_RUN: DRV8323 asleep -> current-sense amplifiers unpowered -> CMPSS inputs ~0 V
  -> CMPSS7/8 LOW comparator asserts (input < DACL=512), COMPSTS=0x300
  -> EPWM X-BAR TRIP7 (ENABLE=0xF002) -> TRIPIN7 = high
  -> DCTRIPSEL DCAH = TRIPIN7 ; TZDCSEL DCAEVT1 = DCAH high  -> DCAEVT1 asserted (TZFLG=0x8)
  -> TZCTL.DCAEVT1 action = 0 (reset = High-Impedance), EPWMxA only
  -> all three EPWMxA forced high-impedance (float); all three EPWMxB switch normally
```

This matches the bench symptom exactly: high-side pins float (~150 mV, following
the adjacent switching pin through stray capacitance), low-side pins are clean
~48 % 3.3 V squares (48 % = 50 % minus dead-band). It is identical on all three
phases because the configuration is identical.

### 11.3 Why the example bisect never reproduced it

`epwm_ex8_deadband` could add the DCAEVT1 *configuration* and still stay normal
(section 3.6) because the example has no CMPSS / DRV8323 / power-trip X-BAR chain
to ever assert `TRIPIN7`. The DCAEVT1 condition was configured but never true, so
the `EPWMxA` force never fired. Reproduction needs DCAEVT1 configured **and**
`TRIPIN7` actually asserted — which only the full Viewer2000 firmware does.

### 11.4 Live causal proof (on hardware)

With the system left RUNNING in the fault state, a real-time register write
(`tools/ccs/probe_dcaevt1_causal_test.js`) changed only `TZCTL.DCAEVT1` from
`0` (Hi-Z) to `3` (no action) on EPWM1/2/8 and cleared the OST/CBC flags that a
debugger-halt had latched. The high-side pins (GPIO0/2/99) recovered to clean
complementary PWM immediately, while the low side (GPIO1/3/75) was unchanged.
Nothing else was touched, isolating `TZCTL.DCAEVT1` as the cause.

### 11.5 Firmware fix

`cpu1/wire/wire_pwm.c` now manages the DCAEVT1 output action with the arm state:

- `wire_pwm_configure_current_trip_base()` sets `TZCTL.DCAEVT1 = DISABLE` (no
  action) at configuration time, so a disarmed/idle/DRY_RUN DCAEVT1 can never
  force the high side.
- `wire_pwm_arm_current_trip()` installs `TZCTL.DCAEVT1 = LOW` (force low — the
  safe state, replacing the unsafe reset Hi-Z) on all phases when the current
  trip is actually armed (POWERED).
- `wire_pwm_disarm_current_trip()` restores `DISABLE`.

This fixes the DRY_RUN high-side float and, as a side benefit, upgrades the
armed overcurrent action on `EPWMxA` from High-Impedance (floating gate) to a
forced low.

The unrelated `wire_pwm_output_is_locked()` predicate fix from section 6
(`&&` -> `||`) is included in the same change.

### 11.6 Verification status

- Root cause: **proven on hardware** by the live causal test (11.4).
- Firmware fix: implemented; `cpu1` builds clean; both images re-flashed with
  `tools/ccs/flash_dual_core_f28p65x`.
- **Remaining gate:** cold-boot from Flash (S3 = Flash boot, power cycle), then
  re-run `run_pwm_runtime_probe.js` to confirm `TZCTL.DCAEVT1` reads the
  firmware-written `DISABLE` (TZCTL `0x3A`) in RUNNING, and scope GPIO0/2/99 to
  confirm clean complementary high-side PWM. Not yet done at the time of writing.

### 11.7 Follow-up (separate from this fix): overcurrent gates only the high side

Because the overcurrent trip uses `DCAEVT1` (which acts on `EPWMxA`) with no
matching `DCBEVT1`, a real armed overcurrent immediately forces only the high
side low; the low side keeps switching until the OST -> TZ ISR -> driver-disable
path runs. The DRV8323 hardware OCP and the nFAULT -> TZ1 -> OST (TZA/TZB force
low, both outputs) path remain the authoritative cutoffs, but the CMPSS path
should be made symmetric (add `DCBEVT1` force-low, or route the trip through a
TZ1-6 digital input that uses TZA/TZB) before sustained POWERED runs. This needs
POWERED-mode hardware verification and is intentionally out of scope here.

## 12. Follow-up plan — DCBEVT1 symmetric overcurrent shutdown

This section is the actionable plan for the §11.7 follow-up. It is **not done**;
it captures the refined finding, the code/SysConfig TODO, and the verification
that gates it. See also `docs/protection-architecture.md` §6/§8.

### 12.1 Refined finding (corrects the §11.7 wording)

§11.7 read as if the low side only lags transiently. It does not: the asymmetry
is **structural and permanent for this configuration**, not a transient. Per
driverlib (`cpu1/device/driverlib/epwm.h`):

- `EPWM_TZ_ACTION_EVENT_TZA` (output A) responds to `{TZ1..TZ6, DCAEVT1, DCAEVT2}`
- `EPWM_TZ_ACTION_EVENT_TZB` (output B) responds to `{TZ1..TZ6, DCBEVT1, DCBEVT2}`

`TZB` does **not** respond to `DCAEVT1`. So even when the current trip is armed
and `DCAEVT1` latches `OST`, only `EPWMxA` is forced; `EPWMxB` keeps switching
until the DRV OCP or the `OST -> TZ ISR -> driver-disable` path intervenes. Only
a `TZ1..TZ6` source forces both sides (it is in both action lists), which is why
nFAULT (TZ1) is symmetric and the CMPSS/DCAEVT1 path is not.

Key consequence: `DCBEVT1` is **available and fully usable**
(`EPWM_DC_MODULE_B`, `EPWM_TZ_SIGNAL_DCBEVT1`, `EPWM_TZ_ACTION_EVENT_DCBEVT1`);
the single-sidedness is a **configuration gap, not a hardware limit**. The fix
structure is DRY_RUN-safe and may be configured now; only its both-side *effect*
on a real overcurrent awaits POWERED verification. Do not conflate "cannot verify
the effect yet" with "cannot configure it yet."

### 12.2 Fix options (pick one before implementing)

- **Option A — mirror DCAEVT1 with DCBEVT1** (keeps the DC submodule's blanking
  window capability; the per-event action becomes symmetric):
  route `TRIPIN7 -> DCBH`, `DCBEVT1 = DCBH-high`, and manage
  `TZCTL.DCBEVT1 = DISABLE` (disarmed) / `LOW` (armed), exactly mirroring the
  current `DCAEVT1` handling.
- **Option B — route TRIP7 through a TZ1..6 digital trip input** (simplest; OST
  -> TZA/TZB is inherently both-sided; loses the DC blanking window): select the
  power-trip X-BAR output into an unused `TZx` and enable it as an `OSHT` source.

Option A matches "symmetric with DCAEVT1" and is the assumed default unless a DC
blanking window is judged unnecessary for the current loop.

### 12.3 TODO — SysConfig and code

> ⚠ This likely needs **SysConfig** (`cpu1/sysconfig_cpu1.syscfg` -> generated
> `board.c`) for the routing, which must be set in the SysConfig GUI by the user;
> it cannot all be done as a direct code edit. The split below is the expected
> shape; confirm against the actual generated symbols when implementing.

SysConfig (user, GUI):

- [ ] Option A: enable ePWM1/2/8 Digital Compare **B**, source `DCBH` from the
  same `TRIPIN7` the DCA path uses; set `DCBEVT1 = DCBH high`, async (not synced),
  matching the DCA settings. Or Option B: map the power-trip X-BAR output to an
  unused `TZx` digital input and add it as a one-shot source on all three ePWMs.
- [ ] Regenerate `board.c`; confirm the new `DCB*`/`TZ*` symbols and bases.

Code (`cpu1/wire/wire_pwm.c`, mirror the existing DCAEVT1 paths — Option A):

- [ ] `wire_pwm_configure_current_trip_base()`: add the DCB trip-input select,
  `DCBEVT1 = DCBH-high` condition, event source/sync, and
  `EPWM_setTripZoneAction(base, EPWM_TZ_ACTION_EVENT_DCBEVT1, EPWM_TZ_ACTION_DISABLE)`.
- [ ] `wire_pwm_set_current_trip_output_action()`: also set `DCBEVT1` to
  `LOW` (armed) / `DISABLE` (disarmed) on all three phases.
- [ ] `wire_pwm_arm_current_trip()` / `wire_pwm_disarm_current_trip()`: also
  enable/disable `EPWM_TZ_SIGNAL_DCBEVT1` in `TZSEL`.
- [ ] config read-back / `s_current_trip_config_error`: add DCB config-error
  bits, symmetric with the existing `PHASE_*_DCA` checks.
- [ ] `wire_pwm_clear_dcaevt1_all()` and the wake-time clears: also clear the
  `DCBEVT1` TZ flag and OST flag so a stale DCB latch cannot pollute START.
- [ ] Keep the change DRY_RUN-safe: the disarmed action **must** be `DISABLE` so
  an idle DCBEVT1 cannot float `EPWMxB` the way DCAEVT1 floated `EPWMxA`.

### 12.4 Verification (POWERED gate — none of this is done)

- [ ] **Bench (DRY_RUN, safe, do first):** cold-boot the new image, run
  `run_pwm_runtime_probe.js`, confirm `TZCTL.DCBEVT1` reads `DISABLE` in RUNNING
  and all six outputs are clean complementary PWM (no `EPWMxB` float — the
  symmetric DCAEVT1 regression check applied to the B side).
- [ ] **POWERED, calibrated:** with approved config, calibrated current limits,
  and motor connected, inject a real overcurrent and scope **all six** gate
  outputs with an edge-triggered capture; confirm `EPWMxA` **and** `EPWMxB` both
  force low within spec on the same trip. A register-level `TZOSTFLG`/`DCBEVT1`
  flag is **not** acceptable as a substitute (the Phase 5.0 verification-gap
  lesson).
- [ ] **Latency:** measure trip-edge -> all-six-output-low latency; record in
  `BRINGUP.md` as a Phase 5 powered gate.
- [ ] Only then update `protection-architecture.md` §6/§8 to mark Chain ②
  symmetric and remove the single-sided caveat.
