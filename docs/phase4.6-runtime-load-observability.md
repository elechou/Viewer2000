# Phase 4.6 — Runtime load observability

> **Document status**: implemented and accepted on the FLASH/20 kHz baseline.
> Scope OFF, sustainable 8-channel Stream, full-rate 8-channel Capture,
> STATUS/CAL reconciliation, link loss, and deliberate overrun isolation pass
> with no budget violation or ADC interrupt overflow. Exact measurements are in
> [BRINGUP.md](../BRINGUP.md). The 100 kHz target, baud-rate ladder, controlled
> 1-8-channel optimization matrix, GPIO profiler-overhead comparison, and
> external Trip waveform remain separate deferred work and are not claimed as
> passing. No Phase 4.6 completion tag is created.

## 1. Goal and boundaries

Phase 4.6 makes CPU1 control-cycle headroom visible in Scope2000 by appending a
system-diagnostic snapshot to STATUS and the CPU1-to-CPU2 status contract. This
bumps the contract version while leaving the wire version and Scope binding
unchanged. The metric is **control-cycle budget**, not operating-system CPU
utilization: CPU1 runs a foreground/background super-loop, and ADC conversion
latency is hardware time rather than CPU execution.

The primary display is the most expensive ADC/EOC-plus-ISR timeline observed in
the last complete one-second control-tick window. Its ADC, Control, Scope, and
Runtime segments are copied from that same tick. Independent lifetime maxima
must never be stacked, because they may originate from different ticks.

## 2. Firmware profiler

The executor reuses the existing ISR-boundary timer reads and adds two timer
reads around the user `control()` body on RUNNING ticks. Before the ADC
interrupt acknowledgement and final timer read, the profiler ingests
the previous tick's completed record with a 64-bit sum, peak comparison, and
fixed-sample window count. The load value is `latency + isr_cycles`, where
latency is the PWM-counter value at ISR entry. This one-tick delay keeps every
record coherent while making aggregation cost part of the following tick's
measured Runtime segment and keeping it inside ADC overflow detection. A new
peak copies that record's user `control()` body cycles, scope cycles, ADC/EOC
entry latency, and tick. Outside RUNNING, `ctrl_at_peak` is zero so the UI does
not label fixed platform ISR overhead as user control. The remaining ISR cost
is still represented by Runtime. This adds two `wire_cycle_count()` reads only
on RUNNING ticks. The acceptance threshold remains that the GPIO ISR pulse
growth stays below one percent of the control period. The window is exactly
`V2K_ISR_HZ` samples and covers IDLE, RUNNING, and FAULT.

At window completion the ISR publishes a pending snapshot. The existing 1 ms
background service validates that snapshot, calculates the integer average,
copies public scalar values, and writes `prof_seq` last. Sequence zero means
that the first window is still collecting.

The platform descriptors are:

| Name | Type | Meaning |
|---|---|---|
| `prof_seq` | U32 | Snapshot publication sequence; written last |
| `cycle_budget` | U32 | `V2K_EPWMCLK_HZ / V2K_ISR_HZ` |
| `load_avg` | U32 | Mean `latency + isr_cycles` in the completed window |
| `load_peak` | U32 | Largest `latency + isr_cycles` in the window |
| `ctrl_at_peak` | U32 | User `control()` body cycles on the peak tick; zero in IDLE/FAULT |
| `scope_at_peak` | U32 | Scope epilogue cycles on the peak tick |
| `lat_at_peak` | U16 | PWM counter at ISR entry on the peak tick |
| `peak_tick` | U32 | Control tick that produced the peak |

`runtime_at_peak = load_peak - lat_at_peak - ctrl_at_peak - scope_at_peak` is
derived by the host. Existing `isr_budget` remains the cumulative violation
count and `isr_overflow` remains the ADC interrupt-overflow count. Five of the
ten previously-free platform descriptor slots are registered as system Variables
— `load_avg`, `load_peak`, `ctrl_at_peak`, `scope_at_peak`, and `lat_at_peak`.
`prof_seq`, `cycle_budget`, and `peak_tick` ride in `STATUS` only: a publication
sequence, a build constant, and a hidden correlation id are not useful as
bound/plotted Variables and would otherwise spend scarce platform slots, so five
slots stay free. Descriptor content changes the build hash. Note that the
`Control` segment — and therefore the existing `ctrl_cycles_max` Variable — now
times the user `control()` body alone; before Phase 4.6 it spanned acquire→apply,
so historical `ctrl_cycles_max` readings are not directly comparable. `wire_apply`
and the remaining ISR work now fall into `Runtime`. Phase 4.6 also appends the profiler snapshot to `STATUS` and
therefore bumps `V2K_CONTRACT_VER`; `V2K_WIRE_VER` is unchanged because the
message is tail-extended.

The GPIO ISR probe still spans a slightly wider region than the internal cycle
measurement and remains the hardware authority for before/after overhead.

### 2.1 ADC/EOC segment interpretation

`lat_at_peak` is the ePWM TBCTR value sampled at ISR entry. With the current
clock configuration it has the same 5 ns tick as the CPU cycle counter, so it
can be added to the ISR duration when presenting a control-period timeline.
It is not an isolated measurement of ADC CPU work. It includes the fixed path
from the ePWM SOC event through ADC sample/conversion, interrupt assertion, and
CPU interrupt entry. Consequently:

- the stacked bar is a control-cycle occupancy timeline, not pure CPU utilization;
- the red ADC/EOC segment must not be interpreted as cycles executed by the CPU;
- TBCTR min/max spread is useful for jitter, while absolute latency remains a
  GPIO/oscilloscope bring-up measurement.

This interpretation is an open UI/terminology review item. Any later relabeling
or separation of hardware latency from CPU execution must preserve the raw
`lat_at_peak` diagnostic.

## 3. Scope2000 service and UI

Scope2000 reads the profiler snapshot from the regular 250 ms `STATUS` poll,
separate from user Watch traffic and the Scope binding. CPU2 serializes the
snapshot in this order:

```text
prof_seq → profile fields → budget violations/overflow → prof_seq
```

A zero or changed sequence, a zero budget, an average above the peak, or an
ADC+Control+Scope sum above the peak invalidates that sample. A firmware with
an older contract is rejected at HELLO; a connected current-contract device
shows `Collecting performance data…` until the first one-second window is
published.

The System panel shows `Control Cycle Budget`, one full-width stacked row for
ADC / Control / Scope / Runtime / Headroom, then one text row:

```text
Peak 73% · Avg 42% · Violations 0
```

Peak and Avg refer to the last completed window. Violations are cumulative
since boot. The bar clips at 100 percent while text preserves the real value;
a peak at or above budget, a nonzero violation count, or a nonzero overflow
uses the fault color. The tooltip reports the colored segment cycle values,
budget, and overflow count; `peak_tick` remains a hidden diagnostic descriptor
for bring-up correlation rather than a normal user-facing field.

## 4. Acceptance

Software acceptance requires the host-compiled profiler test, Viewer2000
contract/build checks, Scope2000 tests, format, clippy, and regenerated golden
vectors synchronized across Viewer2000 and Scope2000.

Hardware acceptance uses the LAUNCHXL-F28P65X at the current 20 kHz baseline,
with FLASH as the deployment-authoritative configuration:

1. The GPIO before/after overhead comparison is deferred to the performance
   follow-up; the internal profiler is the current 20 kHz acceptance instrument.
2. Measure Scope OFF, Stream, and full 8-channel Capture windows.
3. At 20 kHz full load, observe no budget violation or ADC interrupt overflow.
4. Reconcile Scope2000's displayed snapshot against raw STATUS and
   system-Variable CAL_READ values. The five CAL entries point directly at the
   same profiler globals published to STATUS; debugger symbol reads are a
   bring-up cross-check, not a standalone gate because late CCS attach restarts
   the dual-core debug context on this target.
5. Stop host consumption and force a Scope overrun; CPU1 profiling and control
   execution must continue without waiting on CPU2 or the host.

## 5. Open investigation: Stream channel scaling

A preliminary hardware observation reported the Scope segment as 376 cycles
for 1-channel Stream and 809 cycles for 8-channel Stream. The delta is 433
cycles, or approximately 61.9 cycles for each of the seven added channels. A
two-point linear fit is therefore:

```text
scope_cycles ~= 314 + 61.9 * channel_count
```

These readings are retained as evidence, but they are not yet a controlled
benchmark: the control rate, firmware build, channel types, channel order, and
whether both readings used identical variables were not recorded.

The current UI field is `scope_at_peak`: the Scope duration from the same tick
that produced the largest total ADC/EOC-plus-ISR load. It is neither the Scope
window average nor the Scope-local maximum. A total-load peak will often select
an expensive block-boundary tick, but that is not guaranteed. The 376/809 pair
therefore cannot by itself close the Scope performance investigation.

Address-range and type validation run when a binding is applied, outside the
ISR hot path. The ISR still performs one indirect volatile source read and one
ring write per native word for every bound channel. Its current fixed work also
includes repeated block-address calculation, sample-offset multiplication,
block bookkeeping, a header write at the start of each 10-tick block, and block
publication at its end. The observed slope is consistent with per-channel
indirect copy cost, while the approximately 314-cycle intercept indicates that
the fixed path also needs measurement. Neither attribution is considered
proven until the controlled measurements below are complete.

This issue remains open. Closure requires:

1. Run one firmware build at the recorded 20 kHz baseline, with prescaler 1,
   using the same native type and stable variables for 1 through 8 channels.
2. Separate normal sample ticks, block-start ticks, and block-publish ticks;
   record Scope-window average and Scope-local maximum rather than relying only
   on `scope_at_peak`. Reset and record `g_v2k_scope_cycles_max` between cases.
3. Reconcile the internal counter results with a GPIO timing measurement and
   record violations and ADC interrupt overflows for every case.
4. Quantify candidate hot-path changes independently: cache the current sample
   destination, avoid duplicate block-address/offset calculations, and prepare
   a width-aware copy plan when the binding is applied.
5. Repeat the matrix after optimization. Only then decide whether a 100 kHz
   acceptance target is technically justified.
