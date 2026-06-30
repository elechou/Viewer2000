# Phase 3 — Executor and observability: operating and verification checklist

> This checklist is the **bring-up procedure to be executed**, not an acceptance record. Measured values, waveforms, and CCS Graph screenshots go, as usual, into [BRINGUP.md](../BRINGUP.md) Phase 3 area; the tag `phase3-executor-observability` is created only after everything has been run through and recorded.

Phase 2 proved a bare time base (ePWM→ADC→EOC ISR) and a pure-hardware protection chain. Phase 3 **grows the platform's real job** on top of that time base:

- **Executor** — a fixed-order control ISR (`acquire → parameter commit → user_step → apply → scope sample → trigger evaluation → tick++`). The user owns only the one `user_step` slot; every other step belongs to L1. This is the mechanization, in the time dimension, of rule 7 (observability day 0) and rule 4 (L3 sees only completed results / physical quantities, touches no configuration): **there is no un-sampled control path**.
- **Observability** — of the four shared interfaces frozen in Phase 0, this phase lands the producer side of three: the variable catalog (runtime enumeration), the parameter double-buffer (host→control), and the Stream/Capture shared RAM scope (control→host, Stream continuous flow + Capture trigger-freeze). The command/status plane was landed in Phase 2.

> **2026-06-17 Scope contract update**: the scope keeps the two entries Stream/Capture, but no longer exposes a fixed 8-channel group. CPU1 has only one `scope_prod`, CPU2 has only one `scope_cfg/scope_bind/scope_cons`. `STREAM` is a continuous flow, `CAPTURE_ARMED` is the Capture entry enabling trigger-freeze, and after `CAPTURE_FROZEN` you can drain via the CCS view or `BLOCK_REQ`.

**This phase's hardware-acceptance target is the executor itself, not FOC.** Acceptance only proves the ISR keeps sampling, scheduling, and scoping across the three states IDLE/RUNNING/FAULT, and that the parameter/scope planes are usable end to end. A real motor application (the start/stop semantics of a stateful PI/PLL/observer) is a Phase 5 matter.

The division of labor follows Phase 2 and extrapolates: **SysConfig owns static hardware** (Phase 3 only adds one CPUTIMER1 static instance), **C owns runtime** (ISR content, multi-rate scheduling, parameter commit, scope state machine, CMPA, TBCLKSYNC release), **plus the contract self-check** — at power-up `v2k_tb_check` reads safety/performance-critical config back from the registers and reconciles it, halting at `ESTOP0` on mismatch, never running on with invalid performance stats.

## The parts I've already done (for reference)

| Artifact | Content |
|---|---|
| `cpu1/v2k_executor.c/.h` | fixed-order control ISR; down-counter-divided multi-rate scheduling (1 kHz / 100 Hz / parameter-commit, three slots phase-staggered); CPUTIMER1 self-timed stopwatch segmenting control/scope/total cycles; ADC overflow and ISR budget counters |
| `cpu1/app/v2k_platform.h` | historical compatibility include for the user-facing API, removed after the `v2k.h` boundary became the only main entry |
| `cpu1/v2k_user.c` | the default L3 example (**weak symbol**, an application may override with a strong definition): passes `pwm1_duty_cmd` straight to the output, no internal state |
| `cpu1/v2k_registry.c/.h` | CPU1 catalog registration; background mechanical validation of parameter batches + ISR-safe-point atomic commit; CAL_READ on-demand read service |
| `cpu1/v2k_scope_runtime.c/.h` | Stream/Capture shared scope producer; background config/bind sequence handshake and capacity calculation; post-freeze CCS view de-interleave |
| `common/v2k_scope_consumer.h` | the SPSC consumer API for CPU2 / unit tests (peek/release/begin_frozen), inline read-only |
| `cpu1/runtime/v2k_main.c` | background super-loop: services the four shared planes by `g_v2k_tick` deadline, without blocking on the comms core |
| Phase 4.5 baker | post-link computes the final-image hash and patches it into the user catalog blob; CPU1 publishes it in the catalog header |

## Key decisions (finalized)

- **The fixed ISR sequence belongs to L1; the user has only the one `user_step` slot.** Acquisition, atomic parameter commit, scope sampling, and trigger evaluation are all done in the ISR by the platform; user code can neither bypass nor disable sampling. The default `user_step` is a weak-symbol placeholder (duty pass-through), to be replaced by an application's strong definition — L3 application bring-up is Phase 5; this phase accepts the platform skeleton with the pass-through version.
- **Only "what must be finished this tick" goes in the ISR; everything else goes into the background super-loop.** Anything that walks tables, copies config, does capacity calculation, de-interleaves, or transfers across cores runs in `runtime/v2k_main.c`'s `for(;;)`, triggered by `g_v2k_tick` deadline (parameter validation, on-demand read, scope config/bind, CCS-view generation, heartbeat, CPU2 health check). This answers the question the roadmap's Phase 3 left open — **platform chores go to the background, no low-priority soft interrupt**; if the user wants multi-rate control, they divide it themselves inside `user_step` using the `due_mask` passed in by the ISR. The background is an event loop with no fixed `Delay`; with no new `seq`/request it returns immediately, so an idle control core doesn't keep poking CPU2-plane/MSGRAM data.
- **Multi-rate scheduling = down-counter division + phase stagger.** The three slots — 1 kHz, 100 Hz, parameter commit — each have their own phase offset and **never fall on the same tick** — slow loops don't bunch onto the same `k%N==0` tick, so WCET is flattened. The stagger relations are pinned by compile-time `V2K_STATIC_ASSERT` (parameter phase `= 3/4 period`, 100 Hz phase `= 1/2 period`, mutually distinct and in range), so a phase collision when changing frequency fails the compile outright.
- **CPUTIMER1 is the ISR's self-timed stopwatch.** 32-bit free-run down-counter, no interrupt (the only SysConfig static instance Phase 3 adds). The ISR uses it to measure three segments: the **control segment** of acquire→apply, the **scope segment** of the scope epilogue, and the **total duration** of the whole ISR, each recording a max. It is not the same thing as the ePWM TBCTR — TBCTR wraps every period and only serves as a proxy for interrupt latency; CPUTIMER1 gives the absolute cycle account. Its config also goes into `v2k_tb_check` read-back backstop.
- **The scope defaults to `OFF`.** Phase 3 originally installed an eight-channel boot binding; Phase 4 removed it, so the host now binds 1..`V2K_SCOPE_MAX_CH=16` `(addr,type)` channels on demand while in `OFF`. The `prescaler` in the descriptor is only a GUI default-sampling suggestion; the actual sampling division is decided by the global `prescaler` of `DAQ_CTRL`. When not sampling, the ISR scope path is just one active check. Changing channels must go through `OFF` first (no mixing two channel layouts in one ring).
- **The parameter commit does only mechanical validation.** Each write is handled by `(addr,type)`: the type must be legal, the address must be in a CPU1 data region allowed for writing, and a 32-bit type must be aligned; if it hits the CPU1 catalog, it additionally requires `kind&PARAM` and a matching type. The firmware does no `min/max` range check, no clamp, no `scale/offset` back-calculation. **The batch is atomic**: if any item is mechanically illegal the whole batch is not written, and `fail_idx` points at the first illegal entry.
- **Cross-core handshake uses a uniform sequence-number scheme, no dual-writer flags.** Shared-RAM write protection makes "the other side clearing my flag" impossible where the target provides it, so every request/response is: the requester **writes `xxx_seq` last** in its own owner region (the publish action), and the responder writes `xxx_ack_seq` + a result code in reply. CCS poking the shadow region to tune also obeys the same protocol (fill fields first, increment seq last).

## 1. SysConfig and pre-build prerequisites

Done only with the CCS Project/SysConfig tools; manually editing project metadata or `*.syscfg` is forbidden:

1. Unify the CPU1/CPU2 project device to `TMS320F28P650DK9`.
2. Add a **CPUTIMER1** instance to CPU1 (the only new static hardware in Phase 3):

   | Field | Value |
   |---|---|
   | Period | `0xFFFFFFFF` |
   | Prescaler | 0 |
   | Emulation Mode | Run Free |
   | Start Timer | enabled |
   | Interrupt | disabled |
   | Register Interrupt Handler | disabled |

3. Phase 4.5 supersedes the original Git-HEAD pre-build hash. The post-link baker hashes the normalized final ELF plus the generated catalog records, patches that value into the reserved blob, and `v2k_registry_init` publishes it in the catalog header. When the host reconnects and finds the hash changed, it forces re-enumeration, preventing an old catalog from reading new firmware.

`cpu1/f28p65x_dbgier.asm` is taken from C2000Ware 26.01; CPU1 startup calls `SetDBGIER(INTERRUPT_CPU_INT1)`, marking PIE Group 1 (where ADCA1 lives) as **time-critical** — when the background is halted by CCS, the control ISR and tick keep executing (the precondition of real-time mode, see Phase 2 FREE_RUN decision layer ③).

`v2k_tb_check` reads back five states of CPUTIMER1: period, prescaler, run bit, Run Free, interrupt-disabled. If SysConfig isn't configured per the table above, it halts at `ESTOP0` on power-up, and won't run on with wrong performance stats.

⚠️ Switching 20/100 kHz: same as Phase 2, **change both sides** — SysConfig's ePWM Period (5000↔1000) and the compiler Predefined Symbol `V2K_ISR_HZ` (20000↔100000). Changing only one halts at `v2k_tb_check`'s `ESTOP0`, which is exactly the self-check's use.

## 2. Memory layout (where this tick's data lives)

| Region | Address | Use |
|---|---|---|
| GS0 first half | `0x10000..0x10FFF` | catalog ENUM staging + parameter status + the single scope-producer control block |
| GS0 second half + GS1–GS3 | `0x11000..0x17FFF` | Stream/Capture shared scope ring (`0x7000` words) |
| GS4 first `0x200` words | `0x18000..` | parameter shadow + scope cfg/bind/cons (**CPU2 owner**) |
| RAMD2 | `0x1A000..` | post-freeze CCS view: de-interleaved contiguous `float data[2048]` |

Default tier: the first 8 platform observables are bound, prescaler=1, internal `block_n_ticks=10`, mode=`OFF`. The low-rate health/protection quantities are still in the catalog; the host can rebind and sample them with a larger prescaler.

CPU1's background is a bare-metal event loop with no fixed `Delay`: roughly one poll point per ms checks the `seq` changes of parameter/scope/command/CCS view; heartbeat, CPU2 health check, and the LED each compute a deadline from `g_v2k_tick`. **The tick only provides time**; periodic tasks still run in the background and can be preempted by the next control ISR; if the deadline hasn't arrived, it doesn't touch CPU2-plane/MSGRAM data.

In Phase 3, CPU2 still only runs a roughly 1 ms **local low-rate heartbeat** to prove the comms core is itself alive — it does not participate in the control tick, sample timestamps, or block time (rule 5). CPU1 sends a ping roughly every ms, and CPU2 replies on seeing the IPC flag. After SCI is connected in Phase 3.5, CPU2's background switches to being driven by comms events/timeouts.

## 3. Executor: the fixed ISR sequence

`v2k_executor_isr` (hung off ADCA1 EOC, so the ADC result is ready on ISR entry) walks a dead sequence each tick:

```
probe GPIO2 ↑ + read CPUTIMER1 (cycle_start) + read TBCTR (latency proxy)
  → acquire(in)          read ADC A0, fill plat_in (tick / adc / sys_state / fault_code)
  → schedule             down-counter-divide to produce due_mask and param_due
  → if param_due: apply_ready   at the staggered safe point, atomically write the whole approved batch in one shot
  → user_step(in, out)   the only L3 slot (default weak symbol = duty pass-through)
  → apply(out.pwm1_duty) clamp to [0.02, 0.98], write CMPA = PRD×(1−duty)
  ──────────────────────  the control segment ends here, record g_v2k_control_cycles[_max]
  → scope_sample_all(tick)  walk active groups: sample + trigger evaluation + freeze
  ──────────────────────  the scope segment ends here, record g_v2k_scope_cycles[_max]
  → g_v2k_tick++         the platform's single control/sampling time advances here
  → record latency min/max, check ADC overflow, clear interrupt, ACK
  ──────────────────────  the ISR total duration ends here, record g_v2k_isr_cycles[_max] + budget violation
probe GPIO2 ↓
```

The parameter commit is scheduled at the safe point **before** `user_step` (mandated by the contract `v2k_param.h`), guaranteeing a batch of parameters either all take effect on the same tick or none do. Observables must be addressable static storage (not stack locals); each fast channel budgets ~5 cycles/tick into the ISR.

**CPUTIMER1 is a stopwatch, not a second time base (rule 5 clarification).** The platform's single control/sampling time is still only `g_v2k_tick`, produced only by the ePWM→ADC→EOC ISR chain above. The CPUTIMER1 that appears several times in the sequence **produces no time, triggers nothing, and fires no interrupt** — it is configured as a 32-bit free-run down-counter (interrupt disabled), and the ISR **reads** it once each at entry, end of control segment, end of scope segment, and exit, subtracting to get each segment's cycle count (`control_cycles` / `scope_cycles` / `isr_cycles` and budget violation). It is a measuring instrument, not a metronome: the metronome is the ePWM, the stopwatch is CPUTIMER1.

- **Why not use the ePWM's TBCTR as the stopwatch**: TBCTR wraps every PWM period (@20 kHz full scale is only 5000, and it's a non-monotonic up-down triangle), so it can't measure an ISR approaching or even exceeding one tick; CPUTIMER1 is a clean 32-bit monotonic count, wrapping only ~21 s @200 MHz, so the difference of two readings is unambiguous.
- **Why it must be free-run + interrupt-disabled**: interrupt-disabled = it can't schedule anything, otherwise it'd become a second time source and break rule 5; `v2k_tb_check` therefore **reads back `TIE==0` to reconcile**, using the self-check to hard-guarantee it can only be a stopwatch. free-run = it keeps running while the debugger is halted, so cross-halt cycle measurement isn't distorted (same source as Phase 2 FREE_RUN).

**Phase 3's L3 has no internal state** (the duty command passes straight through), so this phase does not verify the cleanup of user state like PI integrator, PLL, observer, ramp, etc. But "`user_step` executes in all three states" must not be hard-coded as the final contract: before Phase 5 enters a real motor, the L1/L3 interface must add a RUNNING-session boundary (each `APP_START` lets the user redo initialization, `CLEAR_FAULT` only returns to IDLE, and the next RUNNING does not reuse old controller state). This is a design debt of the executor API, noted here, not part of Phase 3 acceptance.

## 4. Session and debug entry

The main acceptance uses a **RAM dual-core session**, with FLASH only doing a boot smoke test. The debug-session order is the same as phase1-sysconfig.md §4 (Connect CPU1 → Load → Resume → Connect CPU2 → Load → Resume); during the window CPU1's `g_nmi_cnt` +1 is expected (CPU2WDRS, proven in Phase 1).

⚠️ CPU1 stopped at `ESTOP0`: first look at PC — in `v2k_timebase.c` = `v2k_tb_check` (SysConfig and a C constant don't match); in `runtime/v2k_main.c` = `v2k_assert_layout` (.cmd mismatched `v2k_memmap.h`).

| Config | Acceptance scope |
|---|---|
| RAM / 20 kHz | full §5–§8 functionality, as the bring-up baseline |
| RAM / 100 kHz | full §5–§8 functionality, focusing on ISR budget and overflow |
| FLASH / 20 kHz | boot, dual-core handshake, Phase 2 protection smoke test |
| FLASH / 100 kHz | boot, `v2k_tb_check`, tick and ISR budget smoke test |

The CPU1 session keeps Expressions resident (enable Continuous Refresh):

| Expression | Meaning |
|---|---|
| `g_v2k_tick` | the platform's single ISR tick |
| `g_v2k_due_mask` | the `PLAT_DUE_1KHZ` / `PLAT_DUE_100HZ` set this tick |
| `g_v2k_isr_cycles` / `_max` | the ISR total cycles measured by CPUTIMER1 |
| `g_v2k_control_cycles` / `_max` | the acquire→apply cycles (including the stateless L3 example) |
| `g_v2k_scope_cycles` / `_max` | the scope-epilogue-only cycles |
| `g_v2k_isr_budget_violation_cnt` | count of ISR durations reaching the control-period budget |
| `g_v2k_isr_ovf_cnt` | ADC interrupt overflow (missed tick) count |
| `g_v2k_cpu1_plane.param_status` | parameter batch result / fail index / value mirror |
| `g_v2k_cpu1_plane.scope_prod` | scope state / capacity / config result / overrun / frozen range |
| `g_v2k_ccs_view` | the contiguous float data de-interleaved after a trigger freeze |

### 4.1 Equipment: Phase 3 acceptance needs no logic analyzer

§5–§8 are done entirely with **CCS** (JTAG memory read/write + Expressions + Graph) + the on-chip **CPUTIMER1** — the ISR budget, the parameter plane, and the two scope planes depend on no external instrument. The ISR budget can be read out on-chip precisely because CPUTIMER1, that stopwatch, was added (see §3 decision). The only external touchpoint is the **GPIO2 ISR probe** (`↑` on ISR entry, `↓` on exit, toggled every tick in `v2k_executor.c`), whose only use is an **optional cross-check** of CPUTIMER1's cycle account: pulse width = ISR duration, infinite-persistence spread = jitter. This is exactly the probe used in Phase 2 §5 (J8 header **80**), connected to the Phase 2 **oscilloscope** — without it you can still complete all of §5 acceptance. **The logic analyzer is not used in Phase 3**, left for the SCI / EtherCAT bus timing of 3.5/6.

### 4.2 CCS operating conventions and WATCH variables (common to §5–§8, said only once)

**First be clear about "which session sees which root symbol"** — target shared-RAM and local-RAM ownership means the entity exists only in the owner core's `.out`, and the cross-core side has no symbol of the same name (Expressions in the wrong session just gives `identifier not found`):

| Session | Root symbol (typed directly into Expressions) | Content | Use |
|---|---|---|---|
| **CPU1** | `g_v2k_cpu1_plane` | `catalog` / `param_status` / `scope_prod` | read-only reference (all acks/result codes seen here) |
| **CPU1** | `g_v2k_ccs_view` | trigger-freeze de-interleave buffer; **the request is also issued from the CPU1 session** (it is CPU1-owned, not part of the CPU2 plane) | read + write request |
| **CPU1** | `g_v2k_tick` / `g_v2k_due_mask` / `g_v2k_isr_cycles`(`_max`) / `g_v2k_control_cycles`(`_max`) / `g_v2k_scope_cycles`(`_max`) / `g_v2k_isr_ovf_cnt` / `g_v2k_isr_budget_violation_cnt` / `g_v2k_scope_overrun_total` | executor scalars (listed in the §4 table) | read-only |
| **CPU2** | `g_v2k_cpu2_plane` | `param_shadow` / `scope_cfg` / `scope_bind` / `scope_cons` | all parameter / scope **writes** |

The CPU2 session has **no** `g_v2k_cpu1_plane` entity (only a read-only pointer to the other side), so the flow inherently spans two sessions: **publish by writing `g_v2k_cpu2_plane.*` in the CPU2 session, and the ack/result code comes back in the CPU1 session at `g_v2k_cpu1_plane.*`** — keep both sessions' Expressions open for reference.

**Writing a shared plane = fill all fields first, write the seq as old value +1 last (publish), then poll the responder catching up + read the result code to confirm acceptance** (the cross-core handshake sequence scheme, see Key decisions). Publish/response sequence pairings:

| Action | Publish seq (CPU2 session writes, +1 last) | Response (CPU1 session polls its catch-up + reads result code) |
|---|---|---|
| Parameter commit | `g_v2k_cpu2_plane.param_shadow.commit_seq` | `g_v2k_cpu1_plane.param_status.applied_seq` + `.result` |
| Scope config | `g_v2k_cpu2_plane.scope_cfg.cfg_seq` | `g_v2k_cpu1_plane.scope_prod.cfg_ack_seq` + `.cfg_result` |
| Channel bind | `g_v2k_cpu2_plane.scope_bind.bind_seq` | `g_v2k_cpu1_plane.scope_prod.bind_ack_seq` + `.bind_result` |
| CCS view | `g_v2k_ccs_view.request_seq` | `g_v2k_ccs_view.done_seq` + `.result` (also in the CPU1 session) |

STREAM drain additionally watches: producer `g_v2k_cpu1_plane.scope_prod.wr_idx` (CPU1 session), consumer `g_v2k_cpu2_plane.scope_cons.rd_idx` (CPU2 session).

**The full path for filling fields** (paste into Expressions):

- **Parameter** (CPU2 session): `g_v2k_cpu2_plane.param_shadow.count`, `.writes[0].addr`, `.writes[0].value_bits`, `.writes[0].type`, then `.commit_seq` last. `writes[].addr` comes from the CPU1 catalog (enumerated by the host) or CPU1's `.map`; `value_bits` is the 32-bit bit pattern — to type a physical value directly for an F32 parameter, right-click the expression → Number Format → **Float** then enter a value like `0.5` (otherwise you'd type the IEEE-754 hex).
- **Bind** (CPU2 session): `g_v2k_cpu2_plane.scope_bind.n_ch`, `.ch[0].addr`, `.ch[0].type` (one addr / type group per channel), then `.bind_seq` last.
- **Config** (CPU2 session): `g_v2k_cpu2_plane.scope_cfg.mode_req` (`0`=OFF / `1`=STREAM / `2`=CAPTURE_ARMED), `.trig_ch_slot`, `.trig_level`, `.trig_edge`, `.pre_trig_pct`, then `.cfg_seq` last.

**Seeing a waveform = plot the frozen window after a trigger (§7), not the Expressions instantaneous value.** Any quantity that "changes with the tick" (`g_v2k_due_mask`, `pwm1_duty_cmd` vs `pwm1_duty`, the waveform around a trigger) can only be plotted via §7's CAPTURE_ARMED → CAPTURE_POST → CAPTURE_FROZEN → CCS view → Graph; Expressions is only good for reading scalar state / counts, and the STREAM ring is a binary stream for the machine consumer that can't be plotted directly (§8).

**Point CCS Graph at the contiguous buffer** (CPU1 session): Window → Show View → Graph → Single Time; Start Address = `&g_v2k_ccs_view.data`, Acquisition Buffer Size = `g_v2k_ccs_view.count`, DSP Data Type = **32-bit floating point**, Q value = 0; if you need physical time on the x-axis also fill Sampling Rate Hz = the equivalent sample rate.

## 5. Verification A — scheduling and ISR budget

Run once each at 20 kHz and 100 kHz, same steps:

1. Per §1, build and load RAM / 20 kHz, Resume both cores in §4 order; add the whole §4 group of `g_v2k_*` to Expressions and enable Continuous Refresh.
2. **Baseline**: confirm `g_v2k_tick` keeps incrementing, `g_v2k_isr_ovf_cnt == 0`, `g_v2k_isr_budget_violation_cnt == 0` (within a non-halt window).
3. **due staggering** (use trigger-freeze to plot the waveform, not RUN): the Expressions instantaneous value of `g_v2k_due_mask` refreshes too slowly to catch every tick. Run an A→D pass per §7 once, with both the trigger source and the viewed channel set to `due_mask` (default bind slot **6**): `g_v2k_cpu2_plane.scope_cfg.trig_ch_slot = 6`, `.trig_level = 0.5`, `.trig_edge = 0`, `.pre_trig_pct = 50`, and after freezing plot `g_v2k_ccs_view.channel_slot = 6`. The curve is each tick's due bit pattern (`1`=1 kHz, `2`=100 Hz, `3`=both same tick): count the tick interval of adjacent non-zero samples against the due-interval table below, and confirm **there is no sample with value 3 throughout** (the two dues are never on the same tick). If the ring depth isn't enough to show the 100 Hz 200-tick interval, increase `g_v2k_cpu2_plane.scope_cfg.record_points` then trigger.
4. **Parameter-commit slot**: run a §6 legal write once, confirming the commit slot is also staggered from the two dues and that publish-to-effect is < 2 ms end to end.
5. **ISR budget**: run for a while (toggle the scope once per §7/§8 in the middle), read `g_v2k_isr_cycles_max` / `control_cycles_max` / `scope_cycles_max` against the pass criteria below; the difference of `scope_cycles_max` before/after turning the scope on/off should match the channel count, and it should stop growing once off.
6. **(Optional scope cross-check)**: connect the scope to the GPIO2 probe (J8 pin 80), trigger on the CH1 PWM rising edge, infinite persistence — the pulse width should be ≈ `isr_cycles_max × 5 ns`, the spread ≈ the software-view jitter, cross-confirming the CPUTIMER1 numbers. Skipping this step doesn't affect acceptance.
7. Per §1 switch to RAM / 100 kHz (**change both sides**), repeat 2–6, focusing on budget / overflow constantly 0 and `control_cycles_max` still leaving a stable margin.
8. **Cross-state continuity** (one separate run): keep Scope Stream, and switch IDLE/RUNNING/FAULT via the Phase 2 START/STOP/TZ/CLEAR_FAULT flow. The state channel should reflect the transitions, but `g_v2k_tick`, block `start_tick`, and the scope producer **must not reset or stop due to a software state switch**; only an explicit overrun caused by a tardy consumer is allowed to produce a sequence gap.

**Pass criteria**:

| Observable | Pass criterion |
|---|---|
| `PLAT_DUE_1KHZ` | appears once every `V2K_ISR_HZ/1000` tick |
| `PLAT_DUE_100HZ` | appears once every `V2K_ISR_HZ/100` tick |
| due stagger | the two dues are not set on the same tick |
| parameter-commit slot | staggered from both dues; publish-to-effect < 2 ms end to end |
| `g_v2k_isr_cycles_max` | `< 200 MHz / V2K_ISR_HZ` (i.e. the ISR budget) |
| `g_v2k_control_cycles_max` | less than the ISR budget, leaving a stable margin at 100 kHz |
| `g_v2k_scope_cycles_max` | the before/after-scope difference matches the channel count, no sustained growth |
| `g_v2k_isr_budget_violation_cnt` | constantly 0 within a non-halt window |
| `g_v2k_isr_ovf_cnt` | constantly 0 |

The due intervals for step 3 (after trigger-freeze plotting due_mask, count the tick interval of adjacent non-zero samples):

| ISR frequency | 1 kHz due interval | 100 Hz due interval |
|---|---:|---:|
| 20 kHz | 20 tick | 200 tick |
| 100 kHz | 100 tick | 1000 tick |

## 6. Verification B — parameter double-buffer

Operate case by case (writing per §4.2, all in the **CPU2 session**):

1. In Expressions expand `g_v2k_cpu2_plane.param_shadow`.
2. Per the case fill `count` and `writes[]` (each item's addr / type / value), and **last** write `commit_seq` as old value +1.
3. Wait for `g_v2k_cpu1_plane.param_status.applied_seq == commit_seq`: CPU1's background, at the next ~1 ms poll point, stably copies and mechanically validates the whole batch, and the ISR atomically writes all approved entries in one shot at the staggered parameter slot (validate-to-effect < 1 ms, publish-to-effect < 2 ms end to end).
4. Read `result / fail_idx` against the table below; a single read-back goes through the CAL_READ on-demand read.

| Case | Action | Expected |
|---|---|---|
| legal write | `pwm1_duty_cmd`, type=`F32`, any F32 bit pattern | `V2K_CAL_OK`, the whole batch takes effect on the same tick |
| wrong type | use a non-F32 type on `pwm1_duty_cmd` | `V2K_CAL_BAD_TYPE`, the whole batch not written |
| wrong count | `count > V2K_PARAM_BATCH_MAX` | `V2K_CAL_BAD_COUNT`, the whole batch not written |
| wrong address | Flash/code/peripheral/scope-ring/misaligned 32-bit address | `V2K_CAL_BAD_ADDR`, the whole batch not written |
| unregistered but writable address | an unregistered test variable in CPU1 `.bss/.data/.bss:output` | write succeeds |
| batch atomicity | a legal item first, then a mechanically-illegal item in one batch | the whole batch rejected, the legal item also unchanged, `fail_idx` points at the first illegal item |

5. **Command vs application on the same line**: after a legal write, do a trigger freeze per §7 (the two are at default bind slots 2 / 3, plot `channel_slot = 2` and `= 3` respectively in the same frozen window), the two curves have the same `start_tick` → the command value and applied value are on the same control timeline.
6. **Run-through in three states**: in IDLE / FAULT repeat one legal write, confirming the parameter commit and 10 Hz mirror keep running, but the output is still inhibited by TZ. State switching goes through the Phase 2 command channel (CPU2 session writes `g_v2k_msg_2to1.cmd_req`: fill `cmd_code` then increment `cmd_seq`, `1=APP_START`, `2=APP_STOP`, `3=CLEAR_FAULT`). **Soft-trigger TZ into FAULT with no external jumper**: in RUNNING write the OST bit directly to EPWM1 `TZFRC` `*(uint16_t*)(EPWM1_BASE+0x9B) = 0x0004` (F28P65x: `EPWM1_BASE=0x3000`, write address `0x309B`; CCS Memory Browser / debug MCP `writeMemory` both work) — it goes through the EPWM-level TZ interrupt path, equivalent to a hardware trip, but `CLEAR_FAULT` still requires the external jumper pin to be pulled high to release (`v2k_fault.c` reads `V2K_FAULT_TZ_GPIO`), and since the jumper itself wasn't touched → a direct CLEAR_FAULT returns to IDLE.

## 7. Verification C — trigger freeze + CCS Graph

**What can be plotted as a waveform in CCS is the frozen window.** After freezing, CPU1's background `v2k_scope_ccs_view_service` de-interleaves the **interleaved multi-channel block in the ring** into a single-channel contiguous `float` array `g_v2k_ccs_view.data[]`, which Graph reads directly. **STREAM produces no plottable array** (the ring holds native-width interleaved binary blocks, and the de-interleaver only recognizes FROZEN, see §8) — to "see a waveform" go through this section.

When a host binding is active, the channel slot follows the ENUM/catalog order used by `DAQ_BIND`; for the historical Phase 3 default binding, the first 8 platform fast channels were:

| Default slot | Channel | Type |
|---:|---|---|
| 0 | `adc_a0_raw` | U16 |
| 1 | `adc_a0_v` | F32 |
| **2** | **`pwm1_duty_cmd`** | F32 |
| **3** | **`pwm1_duty`** | F32 |
| 4 | `isr_cycles` | U32 |
| 5 | `isr_latency` | U16 |
| **6** | **`due_mask`** | U16 |
| 7 | `sys_state` | U16 |

Below, **run through once end to end**, taking trigger source = `pwm1_duty_cmd` (slot 2) as the example:

**A. Arm the trigger freeze** (CPU2 session, write `g_v2k_cpu2_plane.scope_cfg`):

1. `.mode_req = 2` (ARMED), `.trig_ch_slot = 2`, `.trig_edge = 0` (RISE), `.trig_level = 0.5`, `.pre_trig_pct = 50`;
2. **last** `.cfg_seq = old value + 1`;
3. back in the **CPU1 session** confirm `g_v2k_cpu1_plane.scope_prod.cfg_ack_seq` caught up, `.cfg_result == 0`, `.mode == 2`.

**B. Cause a trigger transition** (make `pwm1_duty_cmd` rise from <0.5 to ≥0.5), either works:

- simple way (CPU1 session): directly change `pwm1_duty_cmd` to `0.6` in Expressions (writing the CPU1 variable directly triggers, bypassing the parameter plane);
- via the parameter plane (CPU2 session, incidentally verifying §6): commit `pwm1_duty_cmd = 0.6` per §6.

  Observe the CPU1 session `g_v2k_cpu1_plane.scope_prod`: `.mode` goes `2(ARMED) → 3(POST) → 4(FROZEN)`, `.state_seq` increments, `.trig_tick` lands near the transition, `.frozen_count > 0`.

**C. De-interleave into the CCS view** (CPU1 session — `g_v2k_ccs_view` is CPU1-owned, not part of the CPU2 plane). Fill the slot of whichever channel you want to see, e.g. to see the triggered `pwm1_duty_cmd`:

1. `g_v2k_ccs_view.channel_slot = 2`;
2. **last** `.request_seq = old value + 1`;
3. wait for `.done_seq == request_seq`, confirm `.result == 0` (OK), `.count > 0`, `.start_tick` = the first block tick of the frozen window.

**D. Plot it** (CPU1 session): Window → Show View → Graph → Single Time; Start Address = `&g_v2k_ccs_view.data`, Acquisition Buffer Size = `g_v2k_ccs_view.count`, DSP Data Type = **32-bit floating point**, Q value = 0. The curve is that channel's waveform vs tick (with prescaler=1 = one ISR tick). **Changing channels doesn't require re-triggering**: in the same frozen window change `.channel_slot` (e.g. `3` to see `pwm1_duty`) → increment `.request_seq` again → re-plot.

Re-walk A→D for each case (**don't rebind in a non-OFF state**; to change channel layout, commit `.mode_req = 0` (OFF) once first):

| Case | Action | Expected |
|---|---|---|
| rising edge | trigger source `pwm1_duty_cmd`, `trig_level=0.5`, `trig_edge=0`, step B rises to 0.6 | `mode` goes `2→3→4`, `trig_tick` lands near the transition |
| falling edge | `trig_edge=1` (FALL), step B reverses, drop `pwm1_duty_cmd` to 0.4 | state machine as above, hits the falling edge |
| pre-trigger | test `pre_trig_pct` at 0 / 30 / 50 / 100% respectively | the pre/post sample ratio matches the config; at 100% still leaves at least 1 post sample |
| partial last block | trigger-freeze at a non-block boundary | the last block's `hdr.n_ticks` may be less than `block_n_ticks` |
| repeated ARM | after FROZEN, OFF → ARM → trigger again | `state_seq` increases, the old frozen range doesn't pollute the new window |
| illegal config | illegal `trig_ch_slot` / `pre_trig_pct>100` / `trig_edge` | `cfg_result = BAD_PARAM` (non-0), the original running state isn't disturbed |

Frozen blocks are in time order starting from `frozen_end_idx − frozen_count`, and the trigger sample belongs to the post segment.

## 8. Verification D — Scope Stream + CPU2 consumer

When the STREAM ring is full it drops the new block and increments `overrun_cnt`; the ISR never waits on the consumer (rule 1). The producer-side config writing is per §4.2, all poked from the **CPU2 session**.

⚠️ **STREAM can't be fed directly to CCS Graph**: the ring holds native-width interleaved binary blocks, and the CCS-view de-interleaver only recognizes the FROZEN window. To see a waveform go through §7. This section only verifies the **block-header fields + SPSC index semantics**, viewed with Expressions / Memory Browser, not Graph: in the **CPU1 session** add the expression `*(v2k_block_hdr_t *)g_v2k_scope_ring`, and its fields refresh as RUN fills and wraps the ring.

1. Send `OFF` (`scope_cfg.mode_req = 0` + increment `cfg_seq`), confirm `cfg_ack_seq` advances, `cfg_result == OK`;
2. In `OFF` send a legal bind (`scope_bind` + increment `bind_seq`), confirm `bind_ack_seq` advances, `bind_result == OK`, and `scope_prod`'s `n_ch / block_slot_words / ring_capacity` match the bind;
3. Send `STREAM` (`scope_cfg.mode_req = 1` + increment `cfg_seq`), confirm `mode == 1`, `state_seq` increases;
4. By default a block is emitted every 10 ticks: watch `g_v2k_cpu1_plane.scope_prod.wr_idx` +1 every 10 ticks; use the `*(v2k_block_hdr_t *)g_v2k_scope_ring` above to check the block header `start_tick` / `block_seq` / `flags` / `n_ticks` / `n_ch` / `bind_seq` / `stride_octets`;
5. Pause the consumer until the ring fills: `overrun_cnt` and `g_v2k_scope_overrun_total` increase, but ISR overflow / budget violation **do not increase**;
6. Sending a new bind during STREAM must return `V2K_SCOPE_RESULT_BAD_STATE`.

Phase 3 does not verify SCI/EtherCAT throughput, only the **SPSC index semantics of the common consumer API**. Use a minimal diagnostic function on the CPU2 side or a unit test to call `v2k_scope_consumer_peek/release/begin_frozen`:

| Scenario | Pass criterion |
|---|---|
| STREAM empty ring | `peek` returns no data, `rd_idx` unchanged |
| STREAM has a block | the header `peek` returns matches the in-ring data on CPU1 |
| release | each call advances `rd_idx` by only 1, doesn't touch CPU1's `wr_idx` |
| continuous read | `block_seq` is contiguous when normal; after an overrun a seq jump reveals the gap |
| FROZEN window | starting from `frozen_end_idx − frozen_count`, reads exactly `frozen_count` blocks |
| FROZEN partial | preserves the last block's true `hdr.n_ticks`, the consumer doesn't assume a fixed N |

Phase 3 provides no single-core build path: CPU1 always hands over the CPU2 shared-RAM region and boots CPU2, and CPU2 must not write CPU1's producer fields.

## 9. Acceptance and exit

The ePWM/ADC/TZ and START/STOP/FAULT/CLEAR_FAULT already verified in Phase 2 are regressed per [Phase 2 bring-up](phase2-bringup.md), not copied here.

| Acceptance item | Source of operation | Pass condition |
|---|---|---|
| CPU1/CPU2 RAM and FLASH build | §1 | all four configs succeed via CCS `buildProject` |
| SysConfig vs linker reconcile | §1, §2 | `v2k_tb_check` and the layout assertion don't trigger `ESTOP0` |
| dual-core, time base, TZ, command state machine | Phase 2 | the original Phase 2 acceptance all regresses |
| scheduling, state continuity, ISR budget | §5 | due, cycle, overflow, cross-state scope all pass at 20/100 kHz |
| parameter double-buffer | §6 | legal, mechanically-illegal rejection, batch atomicity all pass |
| trigger freeze and CCS Graph | §7 | both edges, the four pre-trigger tiers, partial block, repeated ARM all pass |
| Scope Stream and CPU2 consumer | §8 | block header, overrun, rebind rejection, SPSC index semantics all pass |
| real-time mode (halt behavior) | Phase 2 + §5 | TZ6 stays safe on halt, the time-critical ISR/tick continues, overflow doesn't grow |

Record into the BRINGUP.md Phase 3 area:

- date, board, CCS version, RAM/FLASH, `V2K_ISR_HZ`, firmware build hash;
- CPU1/CPU2 build conclusion;
- one set each at 20 kHz and 100 kHz of `g_v2k_isr_cycles_max` / `g_v2k_control_cycles_max` / `g_v2k_scope_cycles_max` and overflow/budget counts;
- the parameter-plane case result table;
- the key producer fields of STREAM/CAPTURE_ARMED + CCS Graph screenshot or text record;
- START/STOP/FAULT/CLEAR_FAULT and real-time-mode regression results.

Only after all of the above is done and recorded is the tag `phase3-executor-observability` created.
