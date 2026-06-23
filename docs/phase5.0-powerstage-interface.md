# Phase 5.0 — Power-Stage Hardware Interface and L0/L1/L2 Boundary

## 0. Summary

**Status: accepted on 2026-06-22.** Every verification executable with the
available bench equipment passed. Energized current calibration and the
differential-probe/oscilloscope measurements in §10.9 remain explicit gates
before sustained powered motor operation, but they do not block Phase 5.0
interface closure. The checked-in default remains DRY_RUN with powered approval
disabled.

Phase 5.0 is the pre-motor-control hardware phase for:

- LAUNCHXL-F28P65X
- BOOSTXL-DRV8323RS on BoosterPack Site 2
- Makerbase SF2804 PMSM
- AS5600 magnetic encoder over I2C on free Site 1 pins

This phase is not user FOC bring-up. It turns the Phase 4 single-channel demo into a safe, observable three-phase hardware substrate: ePWM sync group, a platform-timed ADC frame, DRV8323RS enable/fault/SPI, non-blocking AS5600 I2C, trip routing, and a protected three-duty output boundary. User code reads completed ADC results through TI DriverLib and owns physical conversion.

Phase 5 may start open-loop V/f and FOC work only after the Phase 5.0 dry-run and protection gates are recorded in BRINGUP.md.

## 1. Goals And Non-Goals

Goals:

- Establish the L0 power-stage wiring for BOOSTXL-DRV8323RS on Site 2.
- Expand the protected output contract to three duty commands.
- Let user code read platform-configured, completed ADC results through documented non-blocking DriverLib result/status APIs.
- Keep peripheral configuration, timing, output, interrupt, ownership, and protection access inside the platform.
- Configure DRV enable, nFAULT, SPI, AS5600 I2C, ADC SOCs, and three phase-locked ePWMs.
- Add read-back self-checks for safety-critical ePWM and ADC trigger settings.
- Keep IDLE outputs gated by OST with DRV ENABLE low; on FAULT, gate all
  outputs in hardware immediately, capture DRV diagnostics, then drive ENABLE
  low in bounded foreground service.
- Define hardware verification steps and required BRINGUP.md evidence.

Non-goals:

- No sustained open-loop V/f.
- No closed-loop current, speed, or position control.
- No platform-owned FOC library.
- No platform-owned ADC count-to-physical conversion or current-offset calibration.
- No trusted SF2804 constants baked into firmware before measurement.
- No CPU-serviced ADC fault as the first-power safety mechanism.

## 2. Hardware Layout Decision

Default layout:

| Item | Decision |
|---|---|
| BoosterPack site | Site 2 |
| Gate driver | BOOSTXL-DRV8323RS |
| Encoder | AS5600 over I2C |
| AS5600 wiring | GPIO105/I2CA_SCL, GPIO104/I2CA_SDA |
| Reason Site 1 is not used | Site 1 maps DRV ENABLE to GPIO42, which conflicts with the XDS110/Scope2000 SCIA TX backchannel, and maps nFAULT onto a boot-switch pin |

Site 2 PWM mapping is hardware-defined and intentionally not the same as logical phase order by ePWM number:

| Logical motor phase | BOOSTXL signal | F28P65x peripheral | LaunchPad GPIO |
|---|---|---|---|
| A | PWMA | EPWM2A/B | GPIO2/GPIO3 |
| B | PWMB | EPWM1A/B | GPIO0/GPIO1 |
| C | PWMC | EPWM8A/B | GPIO99/GPIO75 |

EPWM1 remains the master time base and ADC SOCA source. EPWM2 and EPWM8 are phase-locked slaves with zero phase shift.

## 3. Pin Audit

### 3.1 Power-Stage Digital Pins

| Function | Site 2 signal | GPIO | Owner | Notes |
|---|---:|---:|---|---|
| Phase A high/low PWM | PWMA | GPIO2/GPIO3 | CPU1 ePWM2 | Previously used by the Phase 2 ISR probe/TZ demo; no longer available for debug GPIO |
| Phase B high/low PWM | PWMB | GPIO0/GPIO1 | CPU1 ePWM1 | Master time base and ADC SOCA source |
| Phase C high/low PWM | PWMC | GPIO99/GPIO75 | CPU1 ePWM8 | Slave PWM |
| DRV enable | ENABLE | GPIO38 | CPU1 GPIO | Boot/IDLE/FAULT invariant: low |
| DRV fault | nFAULT | GPIO82 | CPU1 GPIO + INPUT X-BAR 1 | Active-low trip source into TZ1 |
| DRV SPI CS | manual chip select | GPIO103 | CPU1 GPIO | Hardware PTE is not used |
| DRV SPI CLK | SCLK | GPIO93/SPID_CLK | CPU1 SPID | 400 kHz initial bring-up |
| DRV SPI PICO | SDI | GPIO91/SPID_PICO | CPU1 SPID | Controller out |
| DRV SPI POCI | SDO | GPIO92/SPID_POCI | CPU1 SPID | Controller in |
| ISR probe | debug GPIO | GPIO41 | CPU1 GPIO | Moved off Site 2 PWM pins |

### 3.2 Encoder Pins

| Function | GPIO | Peripheral | Notes |
|---|---:|---|---|
| AS5600 SDA | GPIO104 | I2CA_SDA | Wired separately to free Site 1 pin |
| AS5600 SCL | GPIO105 | I2CA_SCL | 400 kHz initial setting |

### 3.3 ADC Channels

Initial SysConfig channels:

| Quantity | ADC SOC | ADC pin | User read site | Status |
|---|---|---|---|---|
| Phase voltage A | ADCA SOC0 | A15/B15/C15 analog mux input | `ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC0)` | User scaling TBD |
| Phase voltage B | ADCA SOC1 | A8 | `ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC1)` | User scaling TBD |
| Phase voltage C | ADCB SOC0 | B3 | `ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC0)` | User scaling TBD |
| Phase current A | ADCB SOC1 | B6 | `ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC1)` | User offset/scale TBD |
| Phase current B | ADCA SOC2 | A10 | `ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC2)` | User offset/scale TBD |
| Phase current C | ADCC SOC0 | C5 | `ADC_readResult(myADC2_RESULT_BASE, myADC2_SOC0)` | User offset/scale TBD |
| DC bus voltage | ADCA SOC3 | A5 | `ADC_readResult(myADC0_RESULT_BASE, myADC0_SOC3)` | 52.29859719 V full scale validated at 9.95 V and 11.94 V |

All seven configured SOCs are triggered by EPWM1 SOCA on every 20 kHz control tick. ADCA has the longest sequence, so ADCINT1 is sourced from ADCA EOC3; ADCB and ADCC complete their shorter same-window sequences before it. Firmware reads back all seven trigger selections and the ADCA ADCINT1 enable/EOC3 source. User code neither triggers nor waits for this frame.

The VBUS conversion uses the TI BOOSTXL-DRV8323RS external-reference board
value `52.29859719 V / 4096 counts`. The LAUNCHXL-F28P65X supplies VREFHI from
its on-board 3.0 V REF6230 when J15 is fitted. Two hardware points measured on
2026-06-22 matched this nominal conversion within the 0.01 V DMM resolution:
935.516 counts -> 11.9449 V versus 11.94 V measured, and 778.938 counts ->
9.9456 V versus 9.95 V measured. No fitted offset is justified by these data.
The example publishes the per-tick, unfiltered physical value as `vbus_V`;
application-level voltage-loop filtering remains user-owned.

At this baseline, "slow voltage loop" means that the application consumes an already-complete voltage result less often. It does not mean that the user manually starts a separate ADC conversion. A future genuinely slow analog channel remains platform-scheduled and is presented as a latest completed result.

### 3.4 SysConfig Non-Default Audit

This subsection is the checked-in audit of TI SysConfig settings. It was produced from SysConfig MCP getInstanceConfiguration with changesOnly enabled. Values not listed here remain at the C2000Ware 26.01 / SysConfig 1.28 module default.

The Source column distinguishes:

- User: explicitly selected in the project configuration.
- Derived: changed automatically by a selected board component, hardware use case, or parent module.

#### Analog PinMux: myANALOGPinMux0

| Configurable | Non-default value | Source |
|---|---|---|
| Use Case | CUSTOM | User |
| Pins Used | A5, A10/GPIO213, A15/B15/C15, A8/GPIO211, B3, B6/GPIO207, C5/GPIO204 | User |

#### ADC: myADC0 / ADCA

| Configurable | Non-default value | Source |
|---|---|---|
| ADC Clock Prescaler | Input clock / 4.0 | User |
| Alternate timings (tDMA) | Disabled | User |
| Enabled SOCs | SOC0, SOC1, SOC2, SOC3 | User |
| SOC0 | ADCIN15, EPWM1 ADCSOCA, 20 SYSCLK sample window | User |
| SOC1 | ADCIN8, EPWM1 ADCSOCA, 20 SYSCLK sample window | User |
| SOC2 | ADCIN10, EPWM1 ADCSOCA, 20 SYSCLK sample window | User |
| SOC3 | ADCIN5, EPWM1 ADCSOCA, 20 SYSCLK sample window | User |
| Interrupt pulse mode | End of conversion | User |
| Enabled ADC interrupts | ADCINT1 | User |
| ADCINT1 source | EOC3 | User |
| Generate interrupt handler | Disabled; runtime registers INT_ADCA1 | User |
| Analog PinMux | Shared myANALOGPinMux0 | Derived |

#### ADC: myADC1 / ADCB

| Configurable | Non-default value | Source |
|---|---|---|
| ADC Instance | ADCB | User |
| ADC Clock Prescaler | Input clock / 4.0 | User |
| Enabled SOCs | SOC0, SOC1 | User |
| SOC0 | ADCIN3, EPWM1 ADCSOCA, 20 SYSCLK sample window | User |
| SOC1 | ADCIN6, EPWM1 ADCSOCA, 20 SYSCLK sample window | User |
| Generate interrupt handler | Disabled | User |
| Analog PinMux | Shared myANALOGPinMux0 | Derived |

#### ADC: myADC2 / ADCC

| Configurable | Non-default value | Source |
|---|---|---|
| ADC Instance | ADCC | User |
| ADC Clock Prescaler | Input clock / 4.0 | User |
| Enabled SOCs | SOC0 | User |
| SOC0 | ADCIN5, EPWM1 ADCSOCA, 20 SYSCLK sample window | User |
| Generate interrupt handler | Disabled | User |
| Analog PinMux | Shared myANALOGPinMux0 | Derived |

#### ePWM: PWM_TB / EPWM1

| Configurable | Non-default value | Source |
|---|---|---|
| Hardware / peripheral / pins | EPWM1 BP / EPWM1 / GPIO0 A, GPIO1 B | User |
| Emulation mode | Free run | User |
| High-speed clock divider | /1 | User |
| Time-base period | 5000 | User |
| Counter mode | Up-down | User |
| Sync-out pulse | Counter-zero event | User |
| CMPA | 2500 | User |
| AQ on up-count CMPA | Set EPWMxA high | User |
| AQ on down-count CMPA | Set EPWMxA low | User |
| Dead-band FED polarity | Inverted | User |
| Dead-band RED/FED | Both enabled | User |
| RED/FED shadow modes | Both enabled | User |
| RED/FED counts | 200 / 200 | User |
| TZA / TZB actions | Force low / force low | User |
| DCAEVT1 / DCBEVT1 actions | Disable / disable baseline | User |
| One-shot trip source | TZ1 | User |
| Cycle-by-cycle trip source | TZ6 | User |
| DCAH / DCBH input | TRIP7 / TRIP7 | User |
| DCAEVT1 / DCBEVT1 condition | DCxH high / DCxH high | User |
| DCAEVT1 / DCBEVT1 mode | Original unfiltered signal, asynchronous | User plus module default |
| SOCA | Enabled, every first event | User |

#### ePWM: PWM_PHASE_A / EPWM2

| Configurable | Non-default value | Source |
|---|---|---|
| Hardware / peripheral / pins | EPWM2 BP / EPWM2 / GPIO2 A, GPIO3 B | User |
| Emulation mode | Free run | User |
| High-speed clock divider | /1 | User |
| Time-base period | 5000 | User |
| Counter mode | Up-down | User |
| Counter mode after sync | Count up | User |
| Phase-shift load | Enabled, phase value remains default zero | User |
| CMPA | 2500 | User |
| AQ on up-count / down-count CMPA | Set A high / set A low | User |
| Dead-band FED polarity | Inverted | User |
| Dead-band RED/FED and shadow modes | Both enabled | User |
| RED/FED counts | 200 / 200 | User |
| TZA / TZB actions | Force low / force low | User |
| DCAEVT1 / DCBEVT1 actions | Disable / disable baseline | User |
| One-shot / CBC trip sources | TZ1 / TZ6 | User |
| DCAH / DCBH input | TRIP7 / TRIP7 | User |
| DCAEVT1 / DCBEVT1 condition | DCxH high / DCxH high | User |
| DCAEVT1 / DCBEVT1 mode | Original unfiltered signal, asynchronous | User plus module default |

#### ePWM: PWM_PHASE_C / EPWM8

| Configurable | Non-default value | Source |
|---|---|---|
| Hardware / peripheral / pins | EPWM8 BP / EPWM8 / GPIO99 A, GPIO75 B | User |
| Emulation mode | Free run | User |
| High-speed clock divider | /1 | User |
| Time-base period | 5000 | User |
| Counter mode | Up-down | User |
| Counter mode after sync | Count up | User |
| Phase-shift load | Enabled, phase value remains default zero | User |
| CMPA | 2500 | User |
| AQ on up-count / down-count CMPA | Set A high / set A low | User |
| Dead-band FED polarity | Inverted | User |
| Dead-band RED/FED and shadow modes | Both enabled | User |
| RED/FED counts | 200 / 200 | User |
| TZA / TZB actions | Force low / force low | User |
| DCAEVT1 / DCBEVT1 actions | Disable / disable baseline | User |
| One-shot / CBC trip sources | TZ1 / TZ6 | User |
| DCAH / DCBH input | TRIP7 / TRIP7 | User |
| DCAEVT1 / DCBEVT1 condition | DCxH high / DCxH high | User |
| DCAEVT1 / DCBEVT1 mode | Original unfiltered signal, asynchronous | User plus module default |

EPWM2 and EPWM8 sync-in selection from EPWM1 is applied and read back by the wire runtime because SysConfig exposes phase loading but does not emit the required explicit slave sync-in selection for this configuration.

SysConfig owns the complete static digital-compare topology. It intentionally
does not select DCAEVT1 or DCBEVT1 as one-shot sources: runtime arms those two
sources only for an approved POWERED START and restores both event actions to
`DISABLE` when disarmed.

#### GPIO

| Instance | Non-default values | Source |
|---|---|---|
| ISR_PROBE | Output; write initial value; GPIO41/header 73 | User |
| TZ_EXT | Pull-up; asynchronous qualification; write initial high; GPIO82/header 48 | User |
| DRV_ENABLE | Output; write initial low; GPIO38/Site 2 pin 44 | User |
| DRV_CS | Output; write initial high; GPIO103/Site 2 pin 52 | User |
| LED_CPU1_GPIO | Output; write initial high/off | User plus LED4-derived GPIO |
| LED_CPU2_GPIO | CPU2 controller; output; write initial high/off | User plus LED5-derived GPIO |

SysConfig reports GPIO38 as shared with the alternate SCIB XDS TX route. Phase 5.0 keeps the active XDS110/Scope2000 link on SCIA GPIO42/GPIO43, so the alternate SCIB route must not be selected. GPIO103 is also connected to the eQEP2 header path; the board routing switch must leave that path available to Site 2 when DRV SPI CS is used.

#### Input X-BAR: TZ_EXT_INPUT_XBAR

| Configurable | Non-default value | Source |
|---|---|---|
| Input | INPUT1 | User |
| Source | GPIO82 (`TZ_EXT`) | User |
| Input lock | Enabled | User |

The generated Input X-BAR configuration is read back before TBCLKSYNC is
released. Production C does not remap INPUT1.

#### I2C: AS5600_I2C

| Configurable | Non-default value | Source |
|---|---|---|
| Hardware / peripheral | I2CA BP / I2CA | User |
| Bit count | 8 bits per data item | User |
| Interrupts | Disabled | User |
| Emulation mode | Continue operation during debug halt | User |
| Protocol | Mode 1 (CPOL=0, CPHA=1) | User |
| SDA | GPIO104 / Site 1 pin 10 | User |
| SCL | GPIO105 / Site 1 pin 9 | User |
| SDA/SCL pad | Pull-up enabled | Derived |
| SDA/SCL qualification | Asynchronous | Derived |
| FIFO | Disabled; the bounded 1-2 byte foreground state machine polls non-FIFO status flags | User |

The current 400 kHz controller bit rate is a module default and is therefore intentionally not listed as a non-default value.

#### SPI: DRV_SPI

| Configurable | Non-default value | Source |
|---|---|---|
| Mode | Controller | User |
| Use case | 3-wire controller | User |
| Peripheral | SPID | User |
| Emulation mode | Continue operation during debug halt | User |
| Transfer protocol | SPI_PROT_POL0PHA0 / mode 0 | User |
| Bit rate | 400 kHz | User |
| Data width | 16 bit | User |
| FIFO | Enabled | User |
| Interrupts | Disabled | User |
| PICO | GPIO91 / Site 2 pin 55 | User |
| POCI | GPIO92 / Site 2 pin 54 | User |
| CLK | GPIO93 / Site 2 pin 47 | User |
| PICO/POCI/CLK qualification | Asynchronous | Derived |

The SPI mode, 400 kHz initial bit rate, 16-bit word width, and FIFO usage now
match the TI MotorControl SDK DRV8323 examples for F28P65x bring-up. The
platform still owns manual chip select on GPIO103 through the `DRV_CS_GPIO`
compile symbol.

SysConfig-generated code is the sole owner of those static SPID settings.
Runtime applies only `FFCT.TXDLY=0x10`, which SysConfig 1.28 does not expose,
and clears transient FIFO status before the first transaction.

The checked-in DRV register image is now derived through TI's
`DRV8323_VARS_t` bitfields instead of project-local raw hex constants. The
adapter reads the existing gate-drive registers first, applies the TI
BOOSTXL-DRV8323RS baseline fields, writes through the TI helper, then verifies
the hardware by readback:

| Register | Baseline source |
|---|---|
| Driver control | TI bitfields: 6x PWM, OTW reported |
| Gate drive HS | Preserved from DRV8323 reset/readback image |
| Gate drive LS | Preserved from DRV8323 reset/readback image |
| OCP control | TI bitfields: 100 ns dead time, automatic retry, 1.7 V VDS threshold |
| CSA control | TI bitfields: 10 V/V gain, VREF/2, low-side reference disabled, external sense FETs |

These values provide a deterministic SPI/read-back baseline, not a claim that
the reset-default gate currents or protection thresholds are appropriate for the
installed FETs, supply, motor, and switching rate. Powered-mode approval remains
off until bench validation replaces or explicitly accepts this image.

#### CPU Timer: ISR_CYCLE_TIMER

| Configurable | Non-default value | Source |
|---|---|---|
| Instance | CPUTIMER1 | User |
| Emulation mode | Free run | User |
| Period | 0xFFFFFFFF | User |
| Start timer | Enabled | User |

#### SysCtl, LEDs, And Modules With No Overrides

| Module | Non-default value |
|---|---|
| SysCtl static | SCIA peripheral ownership assigned to CPU2 |
| LED_CPU1 | LaunchPad LED4 hardware selected |
| LED_CPU2 | LaunchPad LED5 hardware selected |
| Device Support static | No non-default values |
| Sync static | No non-default values |
| ASysCtl static | No non-default values |

## 4. Layer Boundary

### 4.1 L0/L1 Wire Boundary

cpu1/wire/ owns:

- ePWM register configuration reconciliation.
- ADC SOC/EOC schedule validation, ISR binding, and acknowledgement.
- DRV8323RS enable, nFAULT, and SPI polling.
- AS5600 non-blocking I2C service and coherent cached samples.
- Trip-zone lock/release operations.
- Platform descriptor registration for applied outputs and driver diagnostics.

The package is organized for two reading depths:

| File | Reader-facing role |
|---|---|
| wire.h | Small L0-to-L1 seam: apply, time base, background service, and protection lifecycle |
| wire_f28p65x.c | Board composition: shows how runtime timing/output/protection sequence the drivers |
| wire_adc.c/.h | Private ADC SOC/EOC validation, ISR binding, and acknowledgement |
| wire_pwm.c/.h | Private three-phase ePWM synchronization, compare update, read-back checks, X-BAR, and trip-zone operations |
| wire_as5600.h | Public L0 AS5600 cached-sample API; no I2C transaction work |
| wire_as5600_internal.h | Private foreground service and diagnostic address hooks |
| wire_as5600.c | Non-blocking I2C state machine |
| wire_drv8323rs.c/.h | Private bounded adapter around TI's DRV8323 driver plus platform GPIO lifecycle |

### 4.2 Read Access Versus Configuration Ownership

TI already provides a documented, unambiguous result API. Viewer2000 does not wrap it:

```c
uint16_t ia_raw = ADC_readResult(myADC1_RESULT_BASE, myADC1_SOC1);
```

The platform guarantees that this configured frame is complete before `control()` starts. User code owns offsets, scaling, calibration, filtering, and the resulting physical variables. Those static variables are observed through Phase 4.5 symbol baking.

Allowed in user `control()`:

- documented non-blocking result/status reads such as `ADC_readResult()`;
- count-to-A/V conversion and calibration state;
- DCL/MotorControl SDK/user control math;
- `wire_as5600_get_latest()`, which only copies a cache.

Platform-only operations:

- pinmux, peripheral clocks, ownership, ADC SOC triggers, and interrupt sources;
- ePWM time base, synchronization, compare application, and gate release;
- CMPSS, X-BAR, TZ routing, trip clearing, and DRV register writes;
- any blocking peripheral transaction in the control ISR.

### 4.3 User-Supplied L2 Control Code

The platform deliberately provides no ADC pass-through adapter and no FOC library. A wrapper around `ADC_readResult()` would create a second naming surface without hiding meaningful complexity.

C2000Ware MotorControl SDK transforms, SVGEN, estimators, or user control objects consume user-owned physical variables and write `v2k_io.out` duty commands. DCL remains available for generic control blocks. The L2 term is a development map; the C API still uses physical names, not layer names.

## 5. User I/O Contract

`v2k_io.in` contains only platform state and scheduling information:

| Field | Unit / type | Source |
|---|---|---|
| tick | control ticks | CPU1 control-time owner |
| due_mask | U16 | platform slow-rate schedule |
| sys_state | U16 | IDLE/RUNNING/FAULT state |
| fault_code | U16 | latched platform fault reason |

The AS5600 driver exposes:

```c
uint16_t wire_as5600_get_latest(wire_as5600_sample_t *sample);
```

This call performs no I2C work. The sample contains raw 12-bit angle, mechanical radians, AS5600 status, validity, and a publication sequence. Electrical angle and pole-pair handling belong to user L2 control code.
`as5600_errors`, `as5600_seq`, and `as5600_status` are platform diagnostics
enumerated through the normal descriptor/CAL path.
The STATUS register uses `MD=bit5` (magnet detected), `ML=bit4` (magnet too
weak), and `MH=bit3` (magnet too strong), per the AS5600 register map. The
provided external LibDriver header has the correct bit values but incorrect
comments for MD and MH; Viewer2000 follows the datasheet meanings.

Outputs exposed through v2k_io.out:

| Field | Meaning | Safety behavior |
|---|---|---|
| duty_a | Logical phase A command | Clamped by wire_apply |
| duty_b | Logical phase B command | Clamped by wire_apply |
| duty_c | Logical phase C command | Clamped by wire_apply |

Neutral zero-vector duty is 0.5 on all three phases. IDLE keeps DRV ENABLE low and all three ePWM modules latched by OST. A hardware trip latches OST immediately; foreground fault service captures the DRV status registers and then drives ENABLE low.

## 6. Protection Model

Boot/IDLE invariant:

- DRV ENABLE is low.
- EPWM1, EPWM2, and EPWM8 are latched by OST.
- User code cannot release output directly.

On FAULT, the hardware trip locks all three ePWM outputs without waiting for firmware. The foreground loop then reads both DRV status registers while SPI is still awake and disables the DRV. Fault clearing remains blocked until shutdown has completed, the 1 ms sleep interval has elapsed, and nFAULT is high.

Hardware trip inputs for Phase 5.0:

| Trip source | Route | Acceptance condition |
|---|---|---|
| DRV nFAULT | GPIO82 -> INPUT X-BAR 1 -> ePWM TZ1 OST | Pulling low inhibits all three phases and latches FAULT without CPU2 or host participation |
| Debug halt trip | ePWM CBC6 | Halt-safe behavior verified on all three ePWM modules |
| Software force | EPWM_forceTripZoneEvent(OST) on all three modules | STOP and initial lockout inhibit all phases |
| Current threshold | B6 -> CMPSS7 H/L, A10 -> CMPSS8 H/L, and C5 -> ADCC PPB1; all five events OR into XBAR TRIP7 -> asynchronous DCAEVT1/DCBEVT1 OST on ePWM1/2/8 | SysConfig route and boot-time register read-back implemented; symmetric all-six-output effect remains a POWERED acceptance gate |

The F28P65x C2000Ware 26.01 SysConfig input map confirms that B6 and A10 can
feed both comparator halves of CMPSS7 and CMPSS8 respectively, while C5 can
feed only the low comparator positive input of CMPSS2. Phase C therefore
cannot implement a symmetric high/low analog window with CMPSS alone on the
fixed BoosterPack route. The implemented layered protection is:

- DRV8323RS internal OCP remains the fastest all-half-bridge protection;
- phase A/B use symmetric CMPSS7/CMPSS8 high/low windows with 32-sample,
  30-vote digital filters;
- phase C uses ADCC PPB1 high/low limit events as a conversion-synchronous
  hardware backup;
- the two A events, two B events, and the combined ADCCEVT1 source are ORed in
  ePWM X-BAR TRIP7, which drives mirrored asynchronous DCAEVT1/DCBEVT1
  one-shot trips on all three ePWM modules.

The provisional raw windows are 512 through 3584 counts, matching the TI
MotorControl SDK bring-up baseline. These are not yet calibrated ampere limits.
Phase A/B use CMPSS DAC counts referenced to VDDA; phase C uses ADC result
counts referenced to the board's external ADC reference, so physical threshold
equivalence must be measured rather than inferred from equal numeric counts.
The CMPSS filter adds bounded rejection latency, and phase C is bounded by the
20 kHz ADC conversion schedule; DRV OCP remains the first-line fast protection.

The current route is disarmed in IDLE, FAULT, and checked-in DRY_RUN operation.
Powered START arms it only after DRV configuration/status checks and
only when all three current ADC results are already within the provisional
window. START then checks the current-trip source both before and after clearing
the existing output-lock OST, so a trip during release fails closed.

Runtime may observe these events afterward, but no CPU decision is in the
shutdown path. First-power safety must never depend on CPU polling an ADC
result and then requesting a trip.

## 7. START Sequence

APP_START is asynchronous. The command remains unacknowledged while the foreground state machine observes the millisecond-scale DRV timing; the CPU1 control ISR never waits:

1. Keep OST asserted, force neutral duty, disable DRV, and wait at least 1 ms for tSLEEP when powered mode is selected.
2. In powered mode, require an explicitly approved register configuration, enable DRV, and wait at least 1 ms for tWAKE while OST remains asserted.
3. Require nFAULT high, write the baseline DRV configuration, read every written register back, and capture both full 11-bit status registers.
4. Arm the current-window DCAEVT1/DCBEVT1 route only if its static and runtime-state register read-back passes and all three current samples are inside the provisional window.
5. Only after hardware readiness succeeds, reset all user-owned mutable state from the Phase 4.1 golden image and run `setup()`.
6. Force user output back to three-phase neutral and apply neutral duty to all three ePWM modules.
7. Clear TZ flags and release OST only if every precondition still passes; recheck the current-trip source before and after release.
8. Set state to RUNNING, enable TZ interrupts, and acknowledge APP_START.

The checked-in default is `WIRE_POWERSTAGE_MODE_DRY_RUN`. It leaves DRV ENABLE low but permits the state machine and MCU PWM pins to be tested with the inverter bus disconnected. Powered operation requires both `WIRE_POWERSTAGE_MODE=0` and `WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED=1` as CPU1 compiler predefines. Approval must not become the checked-in motor-motion baseline until the register image, current-limit behavior, pin mapping, and bench-supply procedure have been physically verified. [Phase 5.2](phase5.2-minimum-powered-commissioning.md) defines the one supervised commissioning-build exception needed to collect the energized evidence itself.

Visible START diagnostics decode as follows: `start_state` is `0=IDLE`,
`1=SLEEP_WAIT`, `2=WAKE_WAIT`, `3=READY`, or `4=FAILED`.
`start_block` is a bit mask: `0x0001=configuration not approved`,
`0x0002=nFAULT low`, `0x0004=SPI transaction failed`,
`0x0008=configuration read-back failed`, `0x0010=DRV status fault`, and
`0x0020=current protection not ready`.
`pwr_mode` is `0=POWERED` or `1=DRY_RUN`; `pwr_cfg_ok` reports the
compile-time approval gate.

`fault_code=2` identifies a hardware current-window trip. The platform
diagnostics `curr_trip_arm`, `curr_limit_lo`, `curr_limit_hi`, and
`curr_trip_last` are enumerable. `curr_trip_cfg` is zero when every
startup read-back passes; its bits are `0x0001/0x0002=A DAC high/low`,
`0x0004/0x0008=B DAC high/low`, `0x0010=XBAR enable`, `0x0020=XBAR mux`,
`0x0040=phase-C PPB`, `0x0080/0x0100/0x0200=A/B/C ePWM DCA`,
`0x0400/0x0800/0x1000=A/B/C ePWM DCB`, and
`0x2000=runtime arm/disarm state read-back`.
An error leaves the route disarmed and causes powered START to fail closed; it
does not halt the complete DRY_RUN platform. `curr_trip_last` uses bits
`0x0001/0x0002=A high/low`, `0x0004/0x0008=B high/low`,
`0x0010/0x0020=C high/low`, and `0x8000=aggregate DCAEVT1 seen after the
short-lived detailed source had cleared`.

The production interface exposes no bench-only DRV or per-source current-trip
commands. Phase 5.0 temporarily used commands 4 through 6 to exercise SPI and
the five current-trip routes. Their measured evidence remains in §10.4 and
BRINGUP.md, but the command handlers, injection paths, and dedicated diagnostic
Variables were removed during closure. `drv_cfg_valid` and
`drv_ctrl_rd`/`drv_ghs_rd`/`drv_gls_rd`/`drv_ocp_rd`/`drv_csa_rd` remain as
production START and fault diagnostics.

AS5600 validity is application-level data, not a platform START gate. An application that needs rotor position before torque production must enforce that requirement in its own state machine while commanding neutral output.

Current-offset calibration is user application state. Phase 5 applications may accumulate samples while commanding the neutral vector before enabling their current controller; the platform does not invent board-specific gains or offsets.

APP_STOP disables the user app, applies neutral duty, disables TZ interrupts, forces OST on all three ePWM modules, and pulls DRV ENABLE low. FAULT gets immediate hardware OST first, then the foreground status-capture and ENABLE-low sequence described above.

## 8. Read-Back Self-Checks

Firmware must halt on mismatch for:

- ePWM clock divider = /1.
- Period = V2K_TB_PRD on EPWM1, EPWM2, and EPWM8.
- Up-down counter mode on all three modules.
- FREE_SOFT free-run on all three modules.
- EPWM1 phase loading disabled.
- EPWM2 and EPWM8 phase loading enabled, count-up-after-sync, sync-in from EPWM1.
- EPWM1 sync-out enabled on counter zero.
- CMPA shadow load on counter zero.
- Dead-band RED/FED enabled, FED active-low, RED/FED counts = 200.
- TZ OSHT1 and CBC6 enabled; TZA/TZB force-low.
- EPWM1 SOCA enabled and sourced from counter zero.
- Every configured ADC SOC trigger source reads back as ADC_TRIGGER_EPWM1_SOCA.
- ADCA ADCINT1 enabled and sourced from EOC3, the final conversion in the longest configured ADC sequence.
- CPUTIMER1 profiler assumptions remain valid.

## 9. Vendor SDK Policy

Use vendor components rather than duplicating motor-control primitives:

- C2000Ware driverlib remains the peripheral base.
- C2000Ware DCL remains the generic control-block source.
- C2000Ware MotorControl SDK is the preferred source for PMSM transforms, SVGEN, estimator/control examples, and motor-control utilities.
- The DRV8323RS register driver is vendored from C2000Ware MotorControl SDK 6.00.00.00 under `cpu1/third_party/ti/c2000ware_motorcontrol_sdk_6_00_00_00/libraries/drvic/drv8323/`.

Current repo state during Phase 5.0 implementation: only the small DRVIC driver
needed by the firmware is vendored. Future Phase 5 motor-control code should
continue to pull narrowly scoped MotorControl SDK components into
`third_party`/project-local integration points rather than duplicating TI
control primitives.

## 10. Acceptance Plan

### 10.1 Build And Contract Checks

- Build CPU1 RAM.
- Build CPU1 FLASH.
- Build CPU2 RAM.
- Build CPU2 FLASH.
- Run contract checks.
- Run user-boundary verifier.
- Run descriptor baking.
- Verify Scope2000/SCI compatibility: descriptor enumeration still works; user raw ADC/encoder variables appear through symbol baking and platform driver diagnostics remain enumerable.

### 10.2 Pin Audit

- Generated SysConfig pinmux must match the tables in this document.
- GPIO2/GPIO3 must be ePWM2 pins, not debug GPIO.
- GPIO41 must be the ISR probe.
- GPIO82 must be DRV nFAULT and INPUT X-BAR 1 source.
- Site 1 SCIA backchannel remains GPIO42/GPIO43 for XDS110/Scope2000.

### 10.3 PWM Dry Run, VM Disconnected

Record with a scope:

- All six PWM outputs present on Site 2.
- Frequency matches V2K_ISR_HZ.
- Center-aligned carrier.
- EPWM2 and EPWM8 phase-locked to EPWM1.
- Dead-time present on every half bridge.
- Neutral duty = 50% before START and after STOP, but outputs gated while OST/DRV disable are active.
- Halt-safe behavior.
- START/STOP/FAULT gating on all three phases.

### 10.4 Trip Tests

Each test must inhibit all three phases and latch FAULT:

- Force software OST on EPWM1, EPWM2, and EPWM8.
- Pull DRV nFAULT low.
- Inject or force the A/B CMPSS window trips and the phase-C ADC PPB limit trip
  after their routes are selected.

The DRY_RUN `CURRENT_DIAG` acceptance on build `0x741FE67E` independently
passed A-low CMPSS7, B-low CMPSS8, and C-low ADCC PPB1 through XBAR TRIP7 and
DCAEVT1. Each produced fault code 2, advanced the trip count once, restored the
full production XBAR mask, and cleared back to IDLE with zero ISR overflow.

The DRY_RUN `CURRENT_HIGH_DIAG` acceptance on build `0xC141DF35` independently
passed A-high CMPSS7 and B-high CMPSS8. Because the disabled CSA outputs read 0
counts (measured `adc_ia_raw`/`adc_ib_raw`/`adc_ic_raw` = 0), this diagnostic
enables the DRV with OST latched (inverter inputs forced low, like `DRV_DIAG`)
so the bidirectional CSA biases near mid-scale, then drops the selected high DAC
to 1024 counts so the real high comparator asserts. The firmware verified the
CMPSS high filter latch plus the DCAEVT1 one-shot flag on all three ePWM modules
before reporting PASS; `curr_trip_last` read `0x0001` (A) / `0x0004` (B),
`curr_diag_src` read 4 / 5, and `tz_trip_cnt` stayed unchanged because OST never
released, so the high source route is proven independent of the TZ-interrupt
sink the low routes exercised. Each trial restored the high DAC to 3584 and the
full production XBAR mask and returned to IDLE with `curr_trip_cfg=0` and zero
ISR overflow. Physical all-six-output scope timing/dead-time evidence, the
nFAULT-edge-to-PWM shutdown-latency capture, and measured ampere limits to
replace the provisional 512/3584 counts remain pending school-lab work (§10.9).

The visible protection event must not wait for CPU2 or Scope2000.

### 10.5 ADC Tests

- Verify direct `ADC_readResult()` sites correspond to the expected physical pins and SOCs.
- Verify all seven values update from the same ePWM-triggered control frame without user-triggered conversion or waiting.
- Measure user-owned current-sense zero offsets with a Phase 5 calibration state.
- Verify user current scaling after known-current injection or bench measurement.
- Verify user phase-voltage scaling.
- Verify the A5 VBUS route and measure its divider before powered motor tests.
- Confirm ADC interrupt overflow count remains zero at the 20 kHz baseline.
- Confirm direct result reads and the cached AS5600 copy do not violate the ISR budget.

### 10.6 AS5600 Tests

Static I2C/cache publication passed on build `0x0EE49558` after correcting a
physically reversed SCL/SDA connection. The same firmware's rising
`as5600_errors` and zero publication sequence correctly diagnosed the reversed
bus without blocking the 20 kHz ISR. A subsequent 15-second rotation test
covered about 2.2 turns and two wrap events with monotonic publication, zero
I2C errors, and zero ISR overflow. The fixed mechanical structure reports
`MD=1, ML=1, MH=0` at its best available alignment; weak-field margin is a
recorded mechanical limitation, but it did not cause an invalid or dropped
sample during the acceptance run. A live SDA disconnect then invalidated
`enc_ok` and increased the error counter; reconnecting SDA without rebooting
restored publication with a stable error count and advancing sequence.

- Read 12-bit angle over I2C.
- Verify magnet/status bits.
- Rotate through wraparound and confirm mechanical-angle continuity.
- Confirm the foreground service has no polling/wait loop and advances by bounded state-machine steps.
- Confirm `wire_as5600_get_latest()` performs only a coherent cache copy in the 20 kHz ISR.
- Confirm the publication sequence advances only when a complete status+angle sample is committed and bus errors invalidate health until recovery.

### 10.7 DRV8323RS Tests

- The temporary IDLE-only DRV diagnostic verified bounded SPI transfers,
  register writes, read-back, status capture, and ENABLE-low shutdown with
  J5/motor disconnected and VM current-limited; it was removed at closure.
- Production powered START reads the DRV status registers, configures the
  PWM/protection/CSA image through the vendored TI driver, and verifies every
  written register before output release.
- Clear faults.
- Confirm `drv_status1`, `drv_status2`, `drv_spi_errors`, `drv_cfg_valid`,
  `drv_ctrl_rd`, `drv_ghs_rd`, `drv_gls_rd`, `drv_ocp_rd`, `drv_csa_rd`,
  `start_state`, and `start_block` are observable in Scope2000.
- Confirm powered START remains blocked unless the register configuration is explicitly approved.
- Confirm dry-run START leaves DRV ENABLE low.
- Confirm L2/L3 user code has no DRV register write path.

### 10.8 Scope2000 Regression

- Enumerate user raw ADC/encoder variables plus applied-duty and driver-diagnostic Variables.
- Bind 8-channel stream/capture from the new platform descriptors.
- Verify no sequence loss at the 20 kHz baseline.
- Verify no new CPU1 dependency on the CPU2 comms core.

### 10.9 School-Equipment TODO

- Use an oscilloscope with suitable differential probing to inspect actual
  DRV8323 gate-source waveforms, switching-node behavior, dead time, and
  overshoot before sustained powered motor operation.
- Analog current-sense noise and switching-transient measurements also remain
  school-lab work. The available home logic analyzer cannot perform the needed
  edge-triggered nFAULT-to-MCU-PWM acquisition, so that digital shutdown-latency
  capture is also deferred. A logic analyzer must never be connected directly
  to gate or phase-switching nodes.

## 11. BRINGUP.md Recording Requirements

Record every Phase 5.0 hardware session with:

- Firmware build hash and build configuration.
- Final pin map, including any deviations from this document.
- Bench supply voltage and current limit.
- Motor disconnected/connected state.
- DRV8323RS jumper/switch settings.
- AS5600 wiring and magnet status.
- Scope screenshots or measured values for all six PWM outputs.
- Dead-time measurement.
- START/STOP/FAULT gating evidence.
- nFAULT trip evidence.
- CMPSS/current-threshold trip evidence when added.
- ADC raw values plus user scaling and current-offset calibration values when Phase 5 adds them.
- Profiler snapshot: ISR total, control, scope, runtime segments.
- Scope2000 descriptor enumeration and 8-channel stream/capture result.

## 12. Closure Status And Powered-Operation Gates

- [x] Vendor the required TI MotorControl SDK DRV8323 driver under the project
  third-party hierarchy; no build-time dependency on the external SDK path.
- [x] Confirm the BOOSTXL analog signal-to-ADC mapping against SysConfig and
  hardware route tests.
- [x] Measure the A5 VBUS resistor-divider scale and add the user-owned conversion.
- [x] Add B6/CMPSS7 and A10/CMPSS8 windows plus the C5/ADCC PPB1 window, OR all
  five sources through XBAR TRIP7, and route the original asynchronous
  DCAEVT1 OST path to all three ePWMs.
- [x] Move the static digital-compare topology into SysConfig and mirror TRIP7
  through DCBEVT1 with a DRY_RUN-safe `DISABLE` baseline on both sides.
- [x] Independently validate A-low CMPSS7, B-low CMPSS8, and C-low ADCC PPB1
  through XBAR TRIP7 and DCAEVT1 in DRY_RUN.
- [x] Inject the A/B high-window sources (DRY_RUN, build `0xC141DF35`): the
  temporary high-window diagnostic biased the CSA through the DRV and dropped
  the CMPSS7/CMPSS8 high DAC below that bias. Both high comparators propagated
  through XBAR TRIP7 and asynchronous DCAEVT1 into all three ePWM one-shot
  latches. The diagnostic was removed at closure; see §10.4.
- [x] Remove the temporary DRV/current diagnostic commands, injection paths,
  and dedicated Variables from the production interface.
- [x] Keep the checked-in firmware in DRY_RUN with powered approval disabled.

The following are Phase 5 motor-bring-up responsibilities and mandatory gates
before sustained powered operation, not unfinished Phase 5.0 interface work.
Their bounded execution plan is
[Phase 5.2](phase5.2-minimum-powered-commissioning.md):

- Measure current-sense gain/offset and replace the provisional 512/3584-count
  windows with calibrated limits.
- Measure phase-voltage scaling and add user-owned conversion parameters.
- Add application-level AS5600 readiness handling before any
  position-dependent torque command.
- Capture gate-source/switching-node behavior, all-six-output shutdown timing,
  nFAULT-to-PWM latency, and switching transients with suitable school-lab
  equipment.
- Approve the final DRV register image and set powered mode only after those
  measurements pass.
