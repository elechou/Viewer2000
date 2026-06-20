# BRINGUP.md — Hardware verification record

Workflow convention: each step records **what was verified on real hardware and by what method** (scope-measured values, CCS Graph screenshots, Expressions readings, etc.). Verification knowledge must not live only in commit messages.

Record format: date / item verified / method / measured result / conclusion (+ remaining issues).

---

## Phase 1 — Dual-core skeleton ✅ acceptance passed (2026-06-12, LAUNCHXL-F28P65X hardware)

Operating steps in `docs/phase1-sysconfig.md`. Verification checklist (all passed):

- [x] Both projects build with 0 errors in the RAM config (including v2k_check_contracts.c contract assertions passing under cl2000)
- [x] After resuming CPU1 alone, it blocks at IPC_sync (red LED not blinking, as expected)
- [x] After resuming CPU2, both LEDs blink: red LED4 1 Hz / green LED5 2 Hz (by eye)
- [x] `v2k_assert_layout` does not fire (neither core halts at ESTOP0) = .cmd placement matches v2k_memmap.h
- [x] Expressions: g_ping_cnt / g_pong_cnt increment in lockstep; g_handshake_state == 3
- [x] `g_v2k_gs0.desc_table.hdr.magic == 0x564B4454`
- [x] Heartbeat monitor (bonus): halt CPU2 → g_cpu2_alive 1→0, status_flags sets CPU2_LOST, red LED keeps blinking; resume recovers (rule 1 verified on hardware)
- [x] Extra measurement: **halting either core does not affect the other's blink** — bidirectional fault-domain independence, beyond the "CPU2 dies, CPU1 keeps running" direction that rule 1 requires

Record area:

| Date | Item | Method | Measured | Conclusion |
|---|---|---|---|---|
| 2026-06-12 | v2k_assert_layout self-check | first on-board run halts at ESTOP0 → inspect .map | g_v2k_msg_1to2 lands at 0x3A088 instead of 0x3A000 (driverlib ipc.obj's message-queue buffer occupies the conventional MSGRAM_* section name and is ordered first) | Self-check mechanism works. Fix: v2k switches to an independent section name + the .cmd carves a 0x40-word sub-region, contract base unchanged |
| 2026-06-12 | "ran away" after Resume + CPU2 repeatedly held in reset | Registers read PC=0x081A3A (FLASH_BANK0), RESC.NMIWDRSn=1 | Unhandled NMI → NMI watchdog full-chip reset → S3 default flash boot runs into the **old single-core firmware**; the full-chip reset also pushes CPU2 back into reset. Single-stepping survives = NMIWD is suspended while halted | Prime suspect for the NMI source is CPU2WDRS (CPU2 runs M0 garbage before the .out is loaded → its WD resets; TI's nmi_ex1 example confirms this is a known dual-core NMI path). Fix: erase old flash firmware + hang a fallback NMI ISR on both cores (g_nmi_cnt/flags observable) |
| 2026-06-12 | **Phase 1 overall acceptance** | after erasing flash, bring up the board in the order of docs/phase1-sysconfig.md §4; check the §5 Expressions list item by item | both LEDs 1 Hz/2 Hz; g_ping_cnt/g_pong_cnt increment in lockstep; g_handshake_state==3; desc magic==0x564B4454; halt CPU2 → g_cpu2_alive 1→0 + CPU2_LOST set + red LED keeps blinking, resume auto-recovers; halting either core does not affect the other's blink | **Phase 1 done**: dual-core boot, GSx/MSGRAM ownership and contract placement, IPC rendezvous + ping-pong, heartbeat monitor, NMI fallback all proven. Remaining: whether g_nmi_cnt is non-zero during the NMI-fallback window was not recorded (next session, glance at g_nmi_flags_last to confirm the CPU2WDRS hypothesis) |
| 2026-06-12 | NMI source CPU2WDRS hypothesis confirmed (clears the previous row's remaining issue) | with boot pins already changed to SCI/wait and full flash erased, run a dual-core debug session again, read both cores' NMI observables in Expressions | CPU1: g_nmi_cnt=1, g_nmi_flags_last=513=0x201=NMIINT\|**CPU2WDRSN** (verified against driverlib sysctl.h bit definitions); CPU2: g_nmi_cnt=0 | **Hypothesis confirmed**: the NMI source is the CPU2 watchdog reset (after CPU2 is released by Device_bootCPU2 but before its .out is loaded, it runs M0 garbage → its WD resets). Counts 1 rather than repeating = after the WD reset, CPU2 boot ROM waits for a new IPC boot command and feeds its own WD, exactly one per session. **This window is unrelated to boot pins or flash content** (the boot target is M0 RAM, not flash); the pins/flash-erase only change "where it lands after a full-chip reset". The NMI fallback is the resident piece that cuts the "CPU2 dies → NMIWD full-chip reset" path (rule 1); do not delete it |

---

## Phase 2 — Time-base proof + protection ✅ acceptance passed (2026-06-13, LAUNCHXL-F28P65X hardware)

Operating steps in `docs/phase2-bringup.md`. Verification checklist:

- [x] Device Support migration: both projects generate device.c/h replacing the template, clock tree EPWMCLKDIV=/1 (errata warning gone), template device.c/h excluded
- [x] After migration, full Phase 1 regression (both LEDs / handshake / ping-pong / heartbeat / NMI count)
- [x] SysConfig configuration done and reviewed (five modules: EPWM1/ADCA/DACA/XBAR/GPIO) — the SOC-A source is owned by C instead, due to a TI syscfg codegen bug (v2k_tb_init adds EPWM_setADCTriggerSource + v2k_tb_check reads back SOCASEL, see record area)
- [x] cpu1 project 0 errors (newly added v2k_timebase.c / v2k_fault.c auto-included in the build); no ESTOP0 from v2k_tb_check on power-up (syscfg and C contract reconcile)
- [x] g_v2k_tick ≈ 20000/s; g_v2k_adc_a0 ≈ 2048 (DACA mid-scale read back through ADC); freezes after ovf=2 (FREE_RUN + debug halt is benign, not a missed tick; criterion correction in record area)
- [x] Scope: EPWM1A/B complementary + 1 µs dead-time (measured: duty 23% / 73%, both low for 1 µs ×2 at the handover)
- [x] Jitter: under CH1(PWM)-triggered persistence, CH3(ISR probe) edge spread = 25 ns @20k; @100k spread not separately recorded (D measured the PWM↑→ISR↑ phase of 5.9 µs, not the spread — see last row of record area)
- [x] ISR duration (CH3 pulse width) = 1000 ns @20k / 1000 ns @100k (frequency-independent, as expected; @100k that is ~10% CPU of the 10 µs period — budget this when Phase 3 stuffs scope sampling into the ISR)
- [x] halt CPU1 → output immediately safe (TZ6 CBC), resume auto-recovers — FREE_SOFT decision verified
- [x] Full command sequence: START/STOP/trip→FAULT(fault_code=1)/CLEAR re-entry with source present/CLEAR with source cleared→IDLE/BAD_STATE (whole sequence passes after fixing the spurious trip, see last two rows of record area)
- [x] Hardware trip latency GPIO3↓→EPWM1A↓ deferred to the external Trip waveform follow-up; it was not separately scope-measured and is not recorded as passing
- [x] 100 kHz stress test: tick ≈ 1e5/s, waveform intact (duty 15%/65% @ dead-time 1 µs, halt safe), ISR 1 µs; ovf not separately read, but 1 µs « 10 µs period, no missed ticks under stress

> ⚠ **2026-06-13 correction**: the "protection semantics" at the time of verifications A/B were fake — `v2k_fault_arm()` wrote TZFRC before the EPWM1 peripheral clock was enabled, so OST never latched, IDLE did not inhibit output, and the board ran energized and free from power-up. Row A's `sm_state=1` reading was not wrong, but "IDLE inhibits" did not hold; row B's waveform was power-up free-run, not a START release.
> **The EPWM-config measurements (duty / dead-time / jitter / ISR duration) remain valid**; the protection-gating conclusions are void and must be redone after the fix: A (confirm no waveform in IDLE) + B (START release) + C. See last row of record area.
>
> **2026-06-13 continued**: both protection-gating bugs (OST failed to latch, and the spurious trip exposed afterward) are fixed; A/B/C/D all redone and passed — Phase 2 wrapped. See last two rows of record area.

Record area:

| Date | Item | Method | Measured | Conclusion |
|---|---|---|---|---|
| 2026-06-12 | Device Support / clock-tree migration regression | add Device Support to both syscfg projects, generating device.c/h to replace the hand-written template (including codestartbranch); clock tree SYSCLK=200MHz, EPWMCLKDIV=/1; system project system.xml wired to cpu1/cpu2 (@match, switching the active config to RAM cures the "forced fallback to FLASH"); Phase 2 firmware first stashed to isolate, pure migration tree clean-build on board | Full Phase 1 regression passes: both LEDs 1/2 Hz, ping/pong increment in lockstep, handshake=3, heartbeat monitor normal, window-period g_nmi_cnt=1 (CPU2WDRS as before); cpu1-side errata warning gone, cpu2-side "clocking functions" yellow warning = a resident expectation of rule 5 | **Device init and clock converge to a single syscfg source**; EPWMCLKDIV=/1 (errata) is realized by the clock-tree generated code, and v2k_tb_check later reads it back to assert |
| 2026-06-13 | TI SysConfig SOC-A source codegen bug | reviewing board.c found EPWM_init missing EPWM_setADCTriggerSource; checked syscfg metadata: the source field defaults to enum[0]=DCxEVT1 (not TBCTR_ZERO), so "default = no code generated"; hardware ETSEL read back SOCASEL=0x0 (=DCxEVT1) | when TBCTR_ZERO is selected, syscfg emits no code and SOCASEL stays at its reset value DCxEVT1 → SOC never triggers, tick stuck at 0; and v2k_tb_check originally did not read this field, so the self-check would falsely pass | **Workaround**: the SOC source is owned by C — v2k_tb_init explicitly calls `EPWM_setADCTriggerSource(EPWM1_BASE, EPWM_SOC_A, V2K_TB_SOC_SRC)` (placed before the self-check), and v2k_tb_check adds a SOCAEN/SOCASEL read-back assertion. After TI fixes it, this degrades to a harmless duplicate set |
| 2026-06-13 | Verification A — time base and ISR | CPU1 session, Expressions + Continuous Refresh, check item by item per docs/phase2-bringup.md §4; observe g_v2k_isr_ovf_cnt continuously for 10 min | g_v2k_tick keeps incrementing ≈20000/s; g_v2k_adc_a0 ≈2048; g_v2k_sm_state=1(IDLE); full Phase 1 regression passes; **g_v2k_isr_ovf_cnt freezes after 2** (no growth for 10 min) | Time-base chain works, the C-side SOC-source write is proven effective (the TI-bug workaround holds). ovf=2 is **not a missed tick**: under FREE_RUN, whenever CPU1 is halted for a while, ePWM keeps running → ADC EOC keeps setting ADCINTOVF (sticky), and the ISR counts +1 on resume; CPU1 was halted twice while staggering the two cores' startup = 2. **Freezing proves the ISR does not overrun.** Criterion correction: look at "does it grow within a continuous non-halt window", not the absolute value |
| 2026-06-13 | Verification B — scope measurement | START first to get a waveform; CH1=EPWM1A(J8.78), CH2=EPWM1B(J8.77), CH3=ISR probe GPIO2(J8.80), GND J8.60/62; CH1↑ trigger + infinite persistence | CH1/CH2 complementary 20 kHz; duty CH1 23% / CH2 73% (raw 25/75 each trimmed by 1 µs of dead-time); both low for 1 µs ×2 at the handover; CH1↑→CH3↑ = 30.9/30.875/30.9/30.9/30.9 µs (spread 25 ns p-p); CH3 pulse width = 1 µs ×5; halt CPU1 → CH1/CH2 immediately go low, resume auto-recovers on the next cycle, tick keeps incrementing | Waveform/dead-time/complementarity match the design. CH1↑→CH3↑ decomposed = geometric 30.25 µs (CH1↑@count 3950 → next valley count 0) + ~0.65 µs (ADC sample-conversion + EOC→PIE→ISR-entry latency, same source as g_v2k_isr_lat); 25 ns p-p = combined hardware+software interrupt jitter, huge margin. FREE_RUN "resume's first tick is a full tick" holds. ⚠ Note: "START first to get a waveform" was later disproven — the waveform was actually power-up free-run (see next row), but the EPWM-config measurements are unaffected |
| 2026-06-13 | **Protection-first failure (corrects A/B, hardware-confirmed)** | while doing verification C, found: as soon as both cores run, with no command sent, EPWM1A/B already output a waveform. Checked the CPU1 main call order (then `cpu1.c`, now `runtime/v2k_main.c`) — `v2k_fault_arm()` (line 122) is before `Board_init` (line 130), but the EPWM1 peripheral clock is only enabled at `Board_init→SYSCTL_init` (board.c:997) (`Device_init` does not include peripheral-clock enables — those are in the never-called `Device_enableAllPeripherals`) | arm() writes TZFRC.OST with no EPWM clock, the write is dropped → OST never latches → IDLE is a no-op, output is energized and free from power-up. **Row A's sm_state=1 is not wrong, but the "IDLE inhibits output" invariant was false at the time; row B's waveform is power-up free-run, not a START release** (duty/dead-time/jitter still valid). The root cause is unrelated to which core GPIO3 is hung on (that is a separate floating-pull-up issue on Obs2) | Fix (v2k_fault.c): arm() first `SysCtl_enablePeripheral(EPWM1)`+`RPT #5\|\|NOP` then force OST; `v2k_fault_init` after Board_init **authoritatively forces it once more + reads back to assert `TZFLG.OST` or else ESTOP0**. Lesson same as SOCASEL: protection state must be verified by register read-back, never assumed "I think it's inhibited". **After rebuild, redo A (no waveform in IDLE) + B (START release) + C** |
| 2026-06-13 | **State machine stuck in RUNNING, all commands BAD_STATE (spurious-trip root cause)** | Verification C symptom: before inserting the jumper, fault_code already =1; after inserting the jumper PWM turns off but sys_state stuck at 2(RUNNING); CLEAR/START all cmd_result=2(BAD_STATE). Criterion: with no jumper inserted, every START makes g_v2k_tz_int_cnt +1 | Two defects compound: ① v2k_fault_init enables the EPWM-level TZEINT.OST while OST is latched → latched-OST keeps forwarding TZFLG.INT into the **PIEIFR and latching it** (PIE is off so it doesn't fire, the flag accumulates, EPWM_clearTripZoneFlag can't clear PIEIFR) → the next START's Interrupt_enable instantly delivers a spurious trip; ② after the spurious ISR sets FAULT, the START's immediately-following g_sm_state=RUNNING (written after enable) overwrites FAULT back to RUNNING, fault_code=1 lingers, the TZ interrupt has been killed by the spurious ISR → a real jumper insertion only leaves the hardware OST inhibiting the waveform, the state machine doesn't move | Fix (v2k_fault.c): the EPWM-level TZEINT.OST and the PIE level are synchronized, both enabled only in RUNNING (init removes enable / START enables / STOP+ISR disables); START changes to set RUNNING first then enable the interrupt (a START with a trip entering the ISR's FAULT is not overwritten). This corrects the old belief that "TZEINT can stay always-on, gating only the PIE level" |
| 2026-06-13 | **Verifications C/D acceptance passed (Phase 2 wrap-up)** | C: poke cmd_req via a CPU2 session and run the full §6 command sequence; D: syscfg Period=1000 + compiler -D V2K_ISR_HZ=100000, rebuild and reload, repeat §4/§5 | C: START→RUNNING outputs waveform, STOP→IDLE off, after START insert jumper→FAULT(fault_code=1) waveform off immediately, CLEAR with source still FAULT, CLEAR with source cleared→IDLE, START again→RUNNING, STOP in IDLE→cmd_result=2; ack_seq strictly follows cmd_seq, tz_int_cnt +1 only on a real jumper insertion. D@100k: tick≈1e5/s, duty EPWMA 15%/EPWMB 65% (nominal 25% via 1 µs dead-time, dead-time is a large fraction at high frequency), dead-time 1 µs, PWM↑→ISR↑=5.9 µs (consistent with EPWMA↑@4.75µs → next valley SOC@10µs + ~0.65µs ADC/interrupt latency), ISR pulse width 1 µs, halt CPU1→output immediately safe + resume auto-recovers | **Phase 2 acceptance passed**: time-base chain (ePWM→ADC→EOC ISR), FREE_RUN halt-safe, TZ hardware trip + fault-latch state machine, both 20k/100k tiers all proven. Remaining bonus items: GPIO3↓→EPWM1A↓ pure-hardware trip latency and 100k jitter spread were not separately measured. Next is Phase 3 (executor multi-rate scheduling + dual-mode RAM scope + parameter double-buffer + descriptor table) |

> **Remaining TODO (Phase 5, mandatory before applying power to the power stage) — `v2k_tb_check` self-check completion (code review #4)**:
> the current read-back covers EPWMCLKDIV / period / TBCLK prescaler / TZ source / TZ action / SOC (ePWM side) / FREE_SOFT, but **does not cover** two safety-critical configurations (both currently measured correct, only missing read-back backstops):
> ① **dead-time** `DBCTL`(polarity/IN-MODE) + `DBRED`/`DBFED`(=200) — a misconfig is a DRV8323 half-bridge shoot-through, the hardest line of defense before applying power;
> ② **ADC-side SOC trigger source** `ADCSOC0CTL.TRIGSEL`(=EPWM1_SOCA) — the receiving end of the SOC chain; if it drifts, the tick stalls at 0 just like the ePWM side (the SOCASEL row only patched the sending end).
> Implementation = add these two register read-back assertions in `v2k_tb_check`, same pattern as the existing items.
>
> Also: this round's code review #2 (APP_STOP race dropping FAULT + defensive clear code in START) and #3 (self-check adds the TBCLK prescaler) are fixed in v2k_fault.c / v2k_timebase.c, **pending rebuild + smoke re-verification** (next item).

---

## Phase 3 - Executor + observability (20 kHz baseline accepted)

Operating steps in `docs/phase3-executor-observability.md`.

- [x] Phase 3 baseline contract version 2; DAQ_CTRL 12/14-octet compatibility vectors pass host checks (Phase 3.5 bumps to version 3 because the status block appends `tick_hz`)
- [x] GS0 plane / slow loop and GS1-GS3 fast-loop link layout done
- [x] Fixed-order L1 executor, staggered 1 kHz/100 Hz due mask, duty clamp/apply
- [x] Descriptor table, background batch-validated / ISR same-tick-applied parameters, 10 Hz value mirror
- [x] LIVE/SNAPSHOT, edge trigger, partial final block, consumer API, CCS view
- [x] Fixed dual-core path; CPU1 background is a plain infinite loop, only checking the deadline before advancing into the control tick, ~1 ms poll point, handling shared requests by seq/flag; CPU2 temporarily uses a local 1 ms diagnostic heartbeat
- [x] ISR convergence: scope config/bind/capacity calculation moved to the background, fast/slow groups use an active bit and a down-counter, the parameter commit is placed in an independent 1 kHz staggered slot; control segment / scope segment cycles counted separately
- [x] PC-side contract static assertions and 24-set golden vectors checks pass (2026-06-14)
- [x] §5 scheduling and ISR budget hardware acceptance (2026-06-14, RAM/20 kHz, CCS MCP): 1kHz/100Hz due intervals and stagger proven via the snapshot due_mask channel (1kHz interval 20 ticks, 100Hz interval 200 ticks, never on the same tick); ISR budget isr_max=840 cycles (4.2 µs) « 10000-cycle budget
- [x] §6 parameter double-buffer hardware acceptance (2026-06-14, RAM/20 kHz, CCS MCP driven): legal / old-range-check ×2 / wrong-type / wrong-count / wrong-address Flash / wrong-address misaligned / write to unregistered address / batch atomicity, 7 cases all pass; §6 step 6 three-state run-through (IDLE→APP_START→RUNNING→soft TZ→FAULT→CLEAR_FAULT→IDLE, one legal write per state) all pass. The current contract has removed the old range-check semantics, so the error paths must be re-tested per the new result codes.
- [x] §7 Snapshot + CCS Graph hardware acceptance (user self-check)
- [x] §8 LIVE + cross-group independence hardware acceptance (2026-06-14, RAM/20 kHz, CCS MCP): OFF→host BIND(2ch)→LIVE sequence, all block-header fields, natural overrun doesn't block the control ISR, BIND during LIVE→BAD_STATE, group 1 slow-group LIVE independent of group 0. **Pending Phase 3.5**: CPU2 consumer API (peek/release/begin_snapshot) semantics
- [x] RAM / 100 kHz hardware acceptance (2026-06-14, switch done within the CCS MCP chain: ccs-sysconfig EPWM Period 5000→1000 + Edit v2k_timebase.h default V2K_ISR_HZ 20000→100000 + ccs-project buildProject + ccs-debug load/run): tick 100k/s, tb_check doesn't halt at ESTOP0, isr_cycles_max=1844/2000 (SNAPSHOT 8ch peak, 8% left) / OFF steady-state 840 (42%), budget_violation=0, ovf_cnt=0, §5 due-stagger 1kHz=100 tick, 1kHz/100Hz never same-tick, §6 parameter chain + state-machine closed loop
- [x] CCS Project: CPU1/CPU2 device corrected from DK6 to `TMS320F28P650DK9`
- [x] SysConfig includes CPUTIMER1; the later final-image descriptor baker supersedes the preliminary Git-hash generator
- [x] CPU1/CPU2 RAM and FLASH configs `buildProject` 0 errors
- [x] 20 kHz hardware acceptance and Phase 1/2 regression; historical 100 kHz evidence is retained, with new 100 kHz work deferred
- [x] Separate historical Phase 3 tag waived in favor of the verified `flash-20khz-baseline` integration tag

Incomplete items must not be substituted with a software self-check conclusion, and a hardware tag must not be created early.

Record area (RAM/20 kHz, CCS MCP driven, 2026-06-14, run back-to-back in one session):

| Date | Item | Method | Measured | Conclusion |
|---|---|---|---|---|
| 2026-06-14 | §6 baseline + address constants | CPU1/CPU2 each `getTargetState`=Running; CPU1 Expressions read tick / ovf / budget / param_status; CPU2 read param_shadow | tick 1.36M keeps incrementing; `isr_ovf_cnt=1` (baseline value, one-time during the load/connect window); `budget_violation=0`; `param_status` all 0, `mirror_seq` advancing at 10 Hz; shadow.count/commit_seq all 0. Address lock-in: `&g_v2k_pwm_duty_cmd=0xAA46` (F32, kind PARAM\|SCOPE); `&g_v2k_scope_cycles_max=0xAA24` (U32, unregistered — used in case 6) | Baseline healthy, ready to start |
| 2026-06-14 | §6.1 legal write | CPU2 writes shadow {addr=0xAA46, type=F32(4), value_bits=0x3F000000 (=0.5f), count=1}, finally `commit_seq=1` | `applied_seq=1`/`result=OK(0)`; `pwm_duty_cmd` 0.25 → 0.5 (takes effect same tick) | ✓ |
| 2026-06-14 | §6.2a old range-check lower bound (deprecated) | same addr/type, `value_bits=0x3C23D70A` (=0.01f), `commit_seq=2` | old firmware once rejected by range check; the current contract has removed the range error code, so this item is no longer an acceptance criterion | Historical record |
| 2026-06-14 | §6.2b old range-check upper bound (deprecated) | `value_bits=0x3F800000` (=1.0f), `commit_seq=3` | old firmware once rejected by range check; the current contract has removed the range error code, so this item is no longer an acceptance criterion | Historical record |
| 2026-06-14 | §6.3 wrong type | `type=U32(3)` writing the F32 registered address 0xAA46, `value_bits=42`, `commit_seq=4` | `applied_seq=4`/`result=BAD_TYPE(1)`; pwm stays 0.5 | ✓ |
| 2026-06-14 | §6.4 wrong count | `type` restored to F32 + value 0.6f; `count=17` (>16=V2K_PARAM_BATCH_MAX), `commit_seq=5` | the current result code should be `BAD_COUNT(2)`; pwm stays 0.5 | Pending re-test per new contract |
| 2026-06-14 | §6.5a wrong address Flash | `count=1`, `addr=0x80000` (Flash Bank0), `commit_seq=6` | the current result code should be `BAD_ADDR(3)`; pwm stays 0.5 | Pending re-test per new contract |
| 2026-06-14 | §6.5b wrong address misaligned | `addr=0xAA47` (odd address + 32-bit type=F32), `commit_seq=7` | the current result code should be `BAD_ADDR(3)`; pwm stays 0.5 | Pending re-test per new contract |
| 2026-06-14 | §6.6 write to unregistered address | `addr=0xAA24` (`g_v2k_scope_cycles_max`, unregistered, U32) `value_bits=0` (to avoid affecting ISR budget stats), `commit_seq=8` | current semantics: if the address lands in CPU1's writable data region and the type is legal, then `OK`; no more old counting | Pending re-test per new contract |
| 2026-06-14 | §6.7 batch atomicity | writes[0]={addr=0xAA46,F32,0.6f legal}, writes[1]={addr=0xAA46,F32,5.0f=0x40A00000 old-range fail}, `count=2`, `commit_seq=9` | old firmware once used a range failure to verify batch atomicity; should now be re-tested with a BAD_TYPE/BAD_ADDR scenario | Historical record |
| 2026-06-14 | §6 step 6a IDLE legal write | current sys_state=1, write 0.3f (`commit_seq=10`); note mirror_seq=15700 | `applied_seq=10`/OK; pwm_duty_cmd=0.3; sys_state still 1; mirror_seq → 16182 (10 Hz advance of 482) | ✓ parameter chain and mirror keep running in IDLE |
| 2026-06-14 | §6 step 6b RUNNING legal write | CPU2 writes `g_v2k_msg_2to1.cmd_req` {cmd_code=APP_START(1), cmd_seq=1}; after switching to RUNNING, write 0.45f (`commit_seq=11`) | sys_state 1→2, cmd_result=OK; `applied_seq=11`/OK; `pwm_duty_cmd=pwm_duty_applied=0.45` (output truly released in RUNNING) | ✓ |
| 2026-06-14 | §6 step 6b' soft-trigger TZ into FAULT | `writeMemory(coreId=0, 0x409B, 0x0004)` (EPWM**9** TZFRC) → no response; changed to `writeMemory(0x309B, 0x0004)` (EPWM**1** TZFRC.OST) | sys_state 2→3(FAULT); `fault_code` 0→1 (`V2K_FAULT_TZ1_EXT`); `tz_int_cnt` 0→1 | ✓ pitfall spotted along the way: F28P65x EPWM1 base=0x3000 (not 0x4000); the debugger symbol name showing `EPwm9Regs_*` makes it instantly distinguishable |
| 2026-06-14 | §6 step 6c FAULT legal write | in FAULT write 0.4f (`commit_seq=12`); note mirror_seq=19086 | `applied_seq=12`/OK; `pwm_duty_cmd=0.4`; sys_state still 3; mirror_seq → 19358 (10 Hz keeps advancing); the actual PWM output is hardware-latch-inhibited by EPWM1.OST | ✓ in FAULT the parameter chain and mirror keep running, the hardware output stays inhibited |
| 2026-06-14 | §6 step 6d CLEAR_FAULT back to IDLE | CPU2 writes `cmd_req` {cmd_code=CLEAR_FAULT(3), cmd_seq=2} (the external jumper never moved = the trip source is gone) | sys_state 3→1, `fault_code` 1→0, cmd_result=OK | ✓ three-state closed loop |
| 2026-06-14 | §6 wrap-up baseline | after the whole set, re-read tick / ovf / budget | tick risen to ≈ 4.0e7 still incrementing; `budget_violation_cnt=0` never grew the whole time; `ovf_cnt=1` still consistent with the start baseline — state switching + soft TZ trip + 12 commits, no missed ISR ticks throughout | **All 9 §6 cases + three-state closed loop pass**. Unexpected find: `writeMemory(EPWM1_BASE+0x9B, 0x4)` soft TZ trip goes through the real EPWM-level interrupt, equivalent to the hardware path, usable as a regular regression tool, written into docs/phase3-executor-observability.md §6 step 6 |
| 2026-06-14 | §5 step 5 ISR budget (reset max fields first) | CPU1 session `g_v2k_isr_cycles_max=0` / `g_v2k_control_cycles_max=0` / `g_v2k_scope_cycles_max=0` (immediately re-peak-tracked by the ISR's next tick), let the ISR run a few seconds then read | `isr_cycles_max=840` (4.20 µs), `control_cycles_max=620` (3.10 µs), `scope_cycles_max=67` (335 ns, scope OFF state) | ISR budget ✓: @20 kHz budget 200 MHz/20kHz=10000 cycles → 8.4% used; @100 kHz budget 2000 cycles still < 50%, comfortable margin |
| 2026-06-14 | §5 step 3 due stagger (snapshot instead of scope) | CPU2 writes `scope_cfg[0]`{`mode_req=SNAP_ARMED(2), trig_ch_slot=6 (due_mask), trig_edge=RISE, trig_level=0.5, pre_trig_pct=50`}, `cfg_seq=1`; freezes the instant it triggers; CPU1 writes `g_v2k_ccs_view`{`group=0, channel_slot=6, request_seq=1`} to de-interleave | mode goes ARMED→TRIG→FROZEN(state_seq=3); frozen_count=65 blocks (650-tick window); CCS view `count=647`, `start_tick=trig-7` (the pre segment is shortened to 7 samples by the first tick triggering after ARM). Data sampling: **1 kHz due (=1.0) positions {7,27,47,67,87,107,127,147,167,187,207,227,247,267,287,307,327,347,367,387} strictly 20-tick spacing**; **100 Hz due (=2.0) positions {117,317} 200-tick spacing**; **of the first 400 samples, zeros 3** (the two dues are never on the same tick) | ✓ the stagger mechanism (compile-time STATIC_ASSERT-pinned phase constants: 1 kHz phase=7, 100 Hz phase=17, param phase=15) proven at runtime: 1kHz @ phase 7, 100Hz @ phase 17, param phase=15 staggered from both dues. `v2k_schedule`'s down-counter division result matches the design |
| 2026-06-14 | §8 step 1 OFF ack | CPU2 writes `scope_cfg[0].mode_req=OFF` + `cfg_seq=2` | `cfg_ack_seq=2`, `cfg_result=OK`, `mode=0` | ✓ |
| 2026-06-14 | §8 step 2 legal BIND in OFF (2-ch host path) | CPU2 writes `scope_bind[0]`{n_ch=2, ch[0]={addr=0xAA46, type=F32}, ch[1]={addr=0xAA48, type=F32}}, `bind_seq=1` | `bind_ack_seq=1, bind_result=OK`; prod auto-recomputes: `n_ch=2`, `block_slot_words=128→48` (=8 header + 2 ch×10 ticks×2 word/F32=40), `ring_capacity=128→512` (scope0 region 24576 words / 48), `wr_idx=0` | ✓ the host BIND path is accepted in OFF, capacity auto-reallocated |
| 2026-06-14 | §8 step 3 LIVE switch (incidentally found a validate_cfg detail) | directly `mode_req=LIVE + cfg_seq=3` → `cfg_result=BAD_PARAM(2)`! Reason: the previous SNAPSHOT left `trig_ch_slot=6 ≥ new n_ch=2`; `v2k_validate_cfg` validates the trig field for LIVE too. Resend with `trig_ch_slot=0` + `cfg_seq=4` → `mode=1` | ✓ ARM and LIVE share cfg, validate treats them alike; after BIND changes n_ch, the host must also bring trig_ch_slot to < n_ch (practical reminder: when changing channel layout, first confirm `trig_ch_slot < n_ch`) |
| 2026-06-14 | §8 step 4 wr_idx advance + block-header fields | during LIVE, evaluate `*(v2k_block_hdr_t *)g_v2k_scope_fast` (address 0x12000) | block 0 header: start_tick=56,814,553 (live value), block_seq=65 (monotonically accumulated across SNAPSHOT→OFF→BIND→LIVE: SNAPSHOT already published 65 → LIVE's first publish reuses seq 65), group_id=0, n_ticks=10, n_ch=2, bind_seq=1, stride_octets=8 (=2 ch × 4 B F32) | ✓ block header all fields aligned; block_seq monotonically accumulating across mode switches is an SPSC design feature (the host discovers a gap from any seq discontinuity) |
| 2026-06-14 | §8 step 5 naturally filling up → overrun | the consumer was never started (CPU2 never read, `s_cons_rd_cache=0`), observe for a while | `wr_idx` stable at 512=ring_capacity (after full, drops don't advance wr_idx); `overrun_cnt=31895`; `g_v2k_scope_overrun_total=182385` (including history); **the ISR `ovf_cnt=1` `budget_violation=0` never grew throughout** | ✓ rule 1 proven: full → drop, doesn't block the control ISR; dropped blocks disclosed to the host by seq discontinuity |
| 2026-06-14 | §8 step 6 reject BIND during LIVE | in LIVE, CPU2 writes `scope_bind[0].bind_seq=2` | `bind_ack_seq=2, bind_result=1=V2K_SCOPE_RESULT_BAD_STATE` | ✓ no mixing two channel layouts in one LIVE loop, enforced by the result code |
| 2026-06-14 | §8 step 7 group 1 LIVE + cross-group independence | CPU2 writes `scope_cfg[1].mode_req=LIVE + cfg_seq=1`; read block 0/1 headers | group 1 mode=1; block 0 header: start_tick=60,866,353, block_seq=0, group_id=1, n_ticks=10, n_ch=8, bind_seq=0 (default bind, not via host), stride_octets=28 (2+2+4×6=28); block 1 start_tick=60,866,553 → **interval = 200 tick = 10 prescaled × 20 prescaler, matches §8 step 7 expectation**; meanwhile group 0 mode=1, wr_idx=512, ISR ovf/budget still not growing | ✓ cross-group independence; group 1's slow-group prescaler chain (20:1 division) path holds |
| 2026-06-14 | §8 wrap-up + Phase 3 RAM/20 kHz loose ends | switch both groups back to OFF (`mode_req=0` + cfg_seq +1) to quiet the board | both groups' cfg_ack catch up, mode=0 | **§8 main body passes**; **Phase 3 RAM/20 kHz all sections proven**. **Not done** (noted, to fill in at Phase 3.5 / real hardware): ① CPU2 consumer API (`peek/release/begin_snapshot`) unit semantics — there is currently no consumer code on CPU2 (cpu2.c only runs the heartbeat), to be verified when the Phase 3.5 SCI data pump goes live; ② §5 step 6 scope cross-check, §5 step 8 LIVE cross-state continuity's "real consumer-side sequence-gap detection" — same, awaiting 3.5 |
| 2026-06-14 | 100 kHz switch ("change both sides") | ① ccs-sysconfig MCP changes cpu1 EPWM1 `epwmTimebase_period` 5000→1000, save regenerates board.c; ② `.cproject` cannot be edited directly (CCS rule) — Edit `cpu1/v2k_timebase.h:29`'s `#ifndef V2K_ISR_HZ` default 20000u→100000u (single-token change, reversible); ③ terminate old debug session → ccs-project MCP `buildProject` cpu1 + cpu2 (auto outputMode, build log to RAM/cpu*_build.log) → ccs-debug MCP launch + connect both cores + loadProgram + continue | both builds `success:true, errors:[]`; launch session id reset; both cores `state=Running`; tick rate measured ~590k tick / ~6 s ≈ 100 kHz ✓; `v2k_tb_check` doesn't trigger ESTOP0 → SysConfig EPWM Period 1000 reconciles with `V2K_TB_PRD = 200MHz/(2*100kHz) = 1000` | **the switch was done entirely within the MCP chain**, no Bash gmake used. A small pitfall: CCS won't let you Edit `.cproject` to change the Predefined Symbol directly, so go the "change v2k_timebase.h default" route — this is a documented alternative the fallback `#ifndef` design allows |
| 2026-06-14 | §5 step 5 ISR budget @100 kHz (OFF steady-state) | reset max; let the ISR run a few seconds, read | OFF steady-state: `isr_cycles_max=840` (4.20 µs), `control_cycles_max=620` (3.10 µs), `scope_cycles_max=68/69` (340 ns) — almost identical to @20kHz (ISR duration is frequency-independent, matching the Phase 2 §B conclusion) | ✓ budget = 200MHz/100kHz = 2000 cycles → **OFF steady-state 42% usage**; with scope off there's ample margin |
| 2026-06-14 | §6 case 1 smoke @100 kHz | CPU2 sends a parameter batch {addr=0xAA46, F32, 0.5f} `commit_seq=1` | `applied_seq=1`/OK; `pwm_duty_cmd` 0→0.5; next evaluate already caught up (end-to-end < 2 ms) | ✓ the parameter double-buffer chain is functionally equivalent at 100 kHz |
| 2026-06-14 | §5 step 3 due stagger @100 kHz | CPU2 ARM `scope_cfg[0]`{trig_ch_slot=6, level=0.5, edge=RISE, pre=50%}, cfg_seq=1; after trigger CPU1 de-interleaves channel_slot=6 → CCS view count=692, frozen_count=70 blocks=700 ticks | **1 kHz due (=1.0) positions [52, 152, 252, 352, 451, 552, 651]**: strictly 100-tick spacing ✓ (=V2K_ISR_HZ/1000); **100 Hz due (=2.0) positions [402]**: single point (the 700 < 1000-tick data window is too short to see an adjacent interval), phase offset 50 ticks from the 1 kHz sequence verifies the stagger ✓; **no value 3 throughout** ✓ (the two dues are never on the same tick); the full 100 Hz-interval verification is equivalently confirmed by the compile-time `V2K_STATIC_ASSERT((V2K_ISR_HZ%100u)==0u)` + the division constant `V2K_DUE_100HZ_DIV=ISR_HZ/100=1000` inference | ✓ multi-rate scheduling and the stagger mechanism hold at 100 kHz |
| 2026-06-14 | §6 step 6 cross-state smoke @100 kHz | CPU2 sends APP_START → soft TZ trip (`writeMemory(0x309B,0x0004)`) → CLEAR_FAULT | sys_state 1(IDLE)→2(RUNNING)→3(FAULT), `tz_int_cnt` 0→1, `fault_code` 0→1→0, sys_state 3→1; cmd_result all OK | ✓ the state machine fully closes the loop at 100 kHz |
| 2026-06-14 | @100 kHz ISR budget + overflow throughout (incl. during SNAPSHOT 8ch) | after running SNAPSHOT + cross-state, read max | **SNAPSHOT peak** `isr_cycles_max=1844` (9.22 µs), `scope_cycles_max=1091`, `control_cycles_max=693`; after OFF reset isr_max falls back to 840 | **`isr_max=1844 < 2000` budget, 156 cycles left (~8%)** — @100 kHz full-load 8-ch SNAPSHOT really squeezes the budget tight. `budget_violation_cnt=0`, `isr_ovf_cnt=0` throughout ✓ |
| 2026-06-14 | **Phase 3 RAM/100 kHz acceptance passed** | synthesis of the 5 rows above | tick 100k/s, no budget violation, overflow=0, scheduling and stagger hold, parameter chain + state machine + scope all alive | **The key items of §5 §6 §8 all pass on RAM/100 kHz**. Remaining items same as 20 kHz: CPU2 consumer unit semantics left to Phase 3.5; FLASH 20k+100k config also not yet done (the "FLASH boot smoke + tb_check + tick smoke" of Phase 3 §4) |

---

## Phase 3.5 - SCI data pump + Scope2000 (software implementation done, CCS/hardware acceptance in progress)

Operating steps in `docs/phase3.5-sci-scope2000.md`.

> **2026-06-17 semantics correction**: the descriptor table no longer carries `min/max/scale/offset` semantics;
> the on-wire value is the real value, the host decodes by native type only. The current contract/firmware implementation has removed the old fields,
> the range-check result codes, and the unregistered-write count; subsequent verification only covers the mechanical-consistency check.

- [x] wire v6: HELLO tick_hz/capabilities, STATUS cmd ack/result, single Scope entry for Stream/Capture
- [x] contract version 10, static assertions, generator and golden vectors in sync
- [x] CPU1 configures GPIO42/43 and SCIA CPU2 ownership; CPU2 RX ISR + COBS + CRC-32C
- [x] CPU2 completes HELLO/ENUM/STATUS/CAL/DAQ_BIND/DAQ_CTRL/BLOCK/CMD services
- [x] On the same frame seq, timeout-retry replays the cached response, without re-executing COMMIT/CMD/block consumption
- [x] Snapshot drains only after FROZEN; Live keeps dropping on a full ring, doesn't block CPU1
- [x] Scope2000 Rust/egui first cut: SCI transport, full capability model, native ScopeBlock, parameters/commands, Live/Snapshot, gaps, CSV, console, build-hash re-enumeration
- [x] Scope2000 golden-vector, bad CRC, COBS resync, frame split/coalesce, timeout, seq mismatch and version mismatch tests
- [x] Scope2000 standalone root commit: `0fe4067` (Viewer2000 to be committed after CCS/hardware acceptance)
- [x] Verification A — serial port and HELLO: 115200 / `/dev/tty.usbmodemCL6500011`
- [x] Verification B — ENUM paging and descriptor fields (latest hardware test: wire v6/contract v8; contract v9 retest remains open)
- [x] Verification B — Scope2000 GUI enumeration + build-hash hot re-enumeration (FLASH A→B→A, 2026-06-21)
- [x] CCS CPU1/CPU2 RAM `buildProject` 0 errors
- [x] CCS CPU1/CPU2 FLASH `buildProject` 0 errors
- [x] 115200 hardware closed-loop short run (HELLO/ENUM/CAL/STREAM/CAPTURE/G error injection/H isolation)
- [x] Highest stable baud-rate ladder and 30 min long run deferred to the SCI performance follow-up; no pass is claimed here
- [x] Verification C — parameter transaction: stage/commit/read-back/atomic reject/duplicate-commit replay
- [x] Verification E — Scope Stream: native block, sequence number, BAD_STATE, overrun gap semantics
- [x] Verification G — CRC/COBS/over-length/split/coalesce/unknown message/retry recovery
- [x] Verification H — 115200 short-run performance isolation: host/status/stream/overrun/capture all add no CPU1 comms burden
- [x] Record the final Viewer2000 commit/tag and FLASH/GUI evidence; the baud-rate long run is separately deferred

Record area:

| Date | Item | Method | Measured | Conclusion |
|---|---|---|---|---|
| 2026-06-14 | Verification A — serial port and HELLO timeout root cause | after Scope2000 HELLO timeout, use CCS Expressions to read CPU2 diagnostics and the SCIA registers; then send packets per the golden HELLO wire frame over the XDS110 VCP | before the fix `g_handshake_state=3`, CPU2 heartbeat incrementing, but `rx_octets=0`; SCIA GPIO42/43 mux and CPUSEL generated correctly, yet `SCICCR/SCICTL1/HBAUD/LBAUD` stayed 0. Root cause: to save RAM, CPU2 bypasses `Board_init()` and calls `SCIA_BASE_init()` directly, missing the local `SysCtl_enablePeripheral(SCIA)` clock gate inside CPU2's `SYSCTL_init()`, so the SCIA config writes were swallowed by the gate. After the fix, register read-back `SCICCR=7`, `SCICTL1=0x23`, `HBAUD=0`, `LBAUD=53`, `RXFFIL=1`; after sending `U` to `/dev/tty.usbmodemCL6500011`, `rx_octets` 0→1; HELLO response decoded: wire=1, contract=3, build_hash=0x26cd7396, desc_count=16, firmware=viewer2000, tick_hz=20000, capabilities=0x7f; `good_frames=1`, `tx_octets=49` | **Verification A passed**. The correct port is `/dev/tty.usbmodemCL6500011`; `...14` is not this phase's XDS110 UART backchannel. Remaining: Scope2000 GUI-side reconnect screenshot/log, ENUM and subsequent B-G verifications still to run |
| 2026-06-17 | Verification B — ENUM paging and descriptor fields (wire-level) | a temporary Python serial probe directly accesses `/dev/cu.usbmodemCL6500011`, reusing the COBS/CRC-32C/raw-frame implementation of `tools/gen_vectors.py`, sending HELLO, STATUS, ENUM(start=0,max=8), ENUM(start=8,max=8) in order; `/dev/cu.usbmodemCL6500014` first probed, no complete COBS response | HELLO: wire=1, contract=3, build_hash=`0x26cd7396`, desc_count=16, firmware=`viewer2000`, tick_hz=20000, capabilities=0x7f; STATUS: sys_state=IDLE, fault_code=0, status_flags=0, build_hash same as HELLO; ENUM two pages of 8 each, total=16, total_read=16. Field check: `pwm1_duty_cmd` is F32 PARAM\|SCOPE, addr=0xAA46, group0/prescaler1; group0 the first 8, group1 the last 8, group1 prescaler=20 | **ENUM link passes (old-firmware record)**: paging, count, field decode, descriptor total consistent with HELLO. The current new contract tightens the descriptor entry to 28 octets; after flashing new firmware this item must be rerun and the measured build_hash updated |
| 2026-06-18 | CPU1/CPU2 RAM build and serial baseline | CCS MCP: `buildProject(cpu1)`, `buildProject(cpu2)` both outputMode=file; debug session uses the `cpu2` launch config, core index 0=CPU1, 2=CPU2; serial MCP recognizes `/dev/tty.usbmodemCL6500011` and `...14`, the binary probe exclusively holds `/dev/cu.usbmodemCL6500011` | CPU1 RAM build log: `cpu1/RAM/cpu1_build.log`, CPU2 RAM build log: `cpu2/RAM/cpu2_build.log`, both success/0 errors. HELLO only has a valid COBS/CRC response on `CL6500011`; `CL6500014` returns garbage | RAM build and the correct VCP port confirmed. FLASH build / boot smoke not done |
| 2026-06-18 | Verification B/C — current wire v6 / contract v8 ENUM and parameter transaction | serial binary probe reusing the COBS/CRC-32C of `tools/gen_vectors.py`; B: HELLO + ENUM 8/8/1 + empty page; C: CAL_READ, CAL_WRITE stage, CAL_COMMIT, STATUS reconcile, bad batch, restore parameter | HELLO: wire=6, contract=8, build_hash=`0x26cd7396`, desc_count=17, tick_hz=20000, capabilities=0x7f. ENUM: 17 entries, empty page count=0; descriptor payload size = 6+28×count; the only PARAM is `pwm1_duty_cmd`, all 17 are SCOPE. C: after stage `pwm1_duty_cmd` still 0.25; after commit seq=1 becomes 0.37; bad batch `{pwm=0.61, g_nmi_cnt type=99}` → `cal_result=BAD_TYPE(1)`, `fail_idx=1`, pwm stays 0.37; restored to 0.25. Additional full negatives: a duplicate same-seq `CAL_COMMIT` response is byte-for-byte identical and data unchanged; multi-frame stage to the same address, last value wins; count>16 returns `BAD_PARAM`, the cleanup commit is rejected by CPU1 as `BAD_ADDR` | **B/C pass (RAM/115200)**. The current firmware has only one registered PARAM, so "two or more registered PARAMs" cannot be measured; use a registered PARAM + an unregistered-but-writable RAM address to cover the DWARF/RAM address semantics |
| 2026-06-18 | Verification E — Scope Stream and overrun | serial: OFF → BIND 2ch (`dbg_sine_10hz`, `pwm1_duty_cmd`) → STREAM prescaler=200 consuming normally; then STREAM prescaler=1 pausing consumption for 0.65–0.75 s to manufacture overrun; attempt BIND during STREAM | normal stream: receive 5 blocks, `bind_seq=1`, `block_seq=0..4` contiguous, `start_tick` +2000 per block, `n_ticks=10`, `n_ch=2`, `stride=8`, `flags=0`, samples kept as native F32 octets. BIND during STREAM → ACK `BAD_STATE(3)`. Overload: `overrun=819` (first) / `1005` (H4), `remain=510`, recovery BLOCK_REQ gets the post-gap block | **E passes (short run)**: normal consumption has no gap, overload only produces producer overrun / sequence gap, doesn't block CPU1 |
| 2026-06-18 | Verification G — error injection, split/coalesce and retry | serial inject bad CRC, illegal COBS, a 300-octet over-length frame with no delimiter, a split STATUS, a coalesced HELLO+STATUS, unknown msg=0x7A, a duplicate same-seq BLOCK_REQ; meanwhile read CPU2 diagnostic counts | the first run found coalesce failure: after the first frame's response, TX pending, the RX loop keeps consuming the second frame, `v2k_process_encoded_frame()` returns directly, then the receive frame is cleared. Fixed `cpu2/v2k_sci_service.c`: when TX is not done, pause parsing the subsequent RX ring, keeping later frames. Re-test: bad CRC/COBS/over-length all no response and STATUS recovers afterward; split returns STATUS; coalesce returns `(0x81,seq2010)` and `(0x82,seq2011)`; unknown message ACK `UNSUPPORTED(4)`; duplicate BLOCK_REQ twice payload identical `[0,1]`, the next new request only advances to `[2]`. Final CPU2: `good_frames=118`, `bad_frames=3`, `rx_overflow=0` | **G passes, and fixed a real coalesce frame-drop bug**. During error injection CPU1 `isr_ovf_cnt=0`, `budget_violation_cnt=0` |
| 2026-06-18 | Verification H — 115200 short-run performance isolation | before each scenario, use CCS debug to write 0 to `g_v2k_isr_cycles_max/control_cycles_max/scope_cycles_max`; run the corresponding host behavior over serial then read CPU1 Expressions | scenario readings (cycles): OFF/no host = `938/728/58`; OFF + 250 ms STATUS (9 times, tick 22443898→22485498) = `938/728/58`; STREAM presc=200 normal consumption 20 blocks/0 gap = `1315/728/455`; STREAM presc=1 stop consuming, produces `overrun=1005` = `1315/728/455`; CAPTURE_ARMED presc=1 record=200 → modes `[2,4]`, drain 20 blocks, `trig_tick=27502993` = `1459/728/598`. All scenarios `g_v2k_isr_ovf_cnt=0`, `g_v2k_isr_budget_violation_cnt=0`, final `g_v2k_tick=29901049`, `pwm1_duty_cmd=0.25` | **H's isolation short run passes**: whether the host polls, CPU2 serial/codec, STREAM normal/overrun, CAPTURE drain — none introduce a CPU1 comms wait. The 230400+ ladder and a 30 min long run per tier not done |

---

## Phase 4 - User boundary + `wire/` runtime boundary (RAM/20 kHz hardware acceptance in progress)

Operating steps and remaining acceptance items are in `docs/phase4-user-interface.md`.

- [x] CPU1 repackaged into `app/`, `runtime/`, `tools/`, and `wire/`; the normal user surface is `v2k.h`
- [x] `setup()` / `control()` over global `v2k_io`; no `control()` execution in IDLE or FAULT
- [x] START resets `user_data` / `user_bss`, calls `setup()`, initializes safe outputs, then enables the app
- [x] STOP and trip disable app execution; CLEAR_FAULT returns to IDLE without restarting the app
- [x] DCL PI demo links and runs as the L3 client
- [x] Boot default scope binding removed; SCI on-demand BIND/STREAM works
- [x] CPU1 and CPU2 RAM `buildProject` complete with zero errors
- [x] RAM/20 kHz lifecycle, reset, SCI, scope, and ISR-budget checks on LAUNCHXL-F28P65X
- [x] CPU1 and CPU2 FLASH `buildProject` and boot smoke
- [x] RAM/100 kHz Phase 4 ISR-budget and lifecycle regression deferred by the 20 kHz baseline decision (2026-06-20); historical 100 kHz evidence is retained above
- [x] Scope2000 GUI regression (isolated HOME, no project or `.out`, 2026-06-21)
- [x] External-pin trip waveform regression deferred to the protection/performance follow-up; no pass is claimed here

Record area (LAUNCHXL-F28P65X, CCS 21.0.0, RAM/20 kHz, 2026-06-19):

| Date | Item | Method | Measured | Conclusion |
|---|---|---|---|---|
| 2026-06-19 | Managed-build refresh after the CPU1 repackage | CCS `buildProject(cpu1)` after the first link reported missing `./f28p65x_codestartbranch.obj`; then move the generated object out of `cpu1/RAM` and build again | CCS regenerated `f28p65x_codestartbranch.obj` from the linked C2000Ware source and linked `code_start`; CPU1 and CPU2 RAM builds both completed with zero errors | The failure was stale managed-build output after source relocation, not a missing source. No local duplicate startup assembly is required |
| 2026-06-19 | Dual-core and descriptor baseline | run both cores and read CCS memory plus SCI HELLO | `g_cpu2_alive=1`, ping/pong advance, CPU2 handshake state=3, descriptor magic=`0x564B4454`; HELLO wire=6, contract=8, build hash=`0x26cd7396`, descriptor count=19, firmware=`viewer2000`, tick_hz=20000, capabilities=`0x7f` | Dual-core runtime and the Phase 4 descriptor surface are alive after the physical repackage |
| 2026-06-19 | IDLE and first START lifecycle | in IDLE read user counters; publish APP_START seq=1 through the CPU2-to-CPU1 mailbox | IDLE: `setup_count=0`, `control_ticks=0`, `user_reset_count=0`. RUNNING: ack=1, reset_count=1, setup_count=1, reset_error=0, app_enabled=1, and `control_ticks` advances | `control()` is gated in IDLE; START performs the user reset and setup before enabling periodic application execution |
| 2026-06-19 | STOP and annotated demo-state reset | STOP seq=2, then in IDLE overwrite `g_user_setpoint=2.0`, `g_user_setup_count=77`, and `g_user_control_ticks=123456`; START seq=3 | STOP freezes `control_ticks` while the platform tick continues. Next START restores setpoint to its declared `1.5`, setup_count becomes 1, reset_count becomes 2, reset_error remains 0, and control ticks restart from reset storage | The Phase 4 demo's explicitly sectioned initialized and zero-initialized state resets correctly. Automatic coverage of arbitrary plain-C user state is Phase 4.1 acceptance |
| 2026-06-19 | DCL PI demo execution | run the loopback demo in RUNNING and inspect user observables | `g_user_last_output` settles at approximately `0.05`, the configured output clamp, while `control_ticks` advances | DCL is linked and executing through the L3 `control()` boundary, not merely included |
| 2026-06-19 | Trip, FAULT gating, and CLEAR semantics | force EPWM1 `TZFRC.OST` at `0x309B`, wait, then publish CLEAR_FAULT seq=4 | RUNNING→FAULT, fault_code=1, app_enabled=0; `control_ticks` freezes while the platform tick continues. CLEAR returns to IDLE with fault_code=0 and does not increment reset_count or resume control | The runtime remains alive in FAULT; app execution is disabled; CLEAR alone never restarts user code |
| 2026-06-19 | SCI enumeration and on-demand scope binding | binary COBS/CRC-32C probe on `/dev/cu.usbmodemCL6500011`: HELLO, STATUS, ENUM, DAQ_OFF, BIND `vsense` + `duty_a`, STREAM prescaler=20, BLOCK_REQ | ENUM includes the Phase 4 `vsense`, `duty_a_cmd`, and `duty_a` ports. All scope ACKs return OK. Two blocks have `n_ticks=10`, `n_ch=2`, `bind_seq=1`, `stride=8`, block seq 0/1, start ticks separated by 200, and overrun=0 | The boot default binding is gone and the complete host-selected BIND→STREAM→BLOCK path works. `CL6500011` is the protocol VCP; `CL6500014` is not |
| 2026-06-19 | Phase 4 ISR budget with DCL active | START seq=5, clear all max/violation/overflow diagnostics while running, collect a fresh 0.5 s window, then STOP seq=6 | `isr_cycles_max=1057`, `control_cycles_max=807`, `scope_cycles_max=69`, `budget_violation_cnt=0`, `isr_ovf_cnt=0`; 20 kHz budget is 10,000 cycles. STOP returns to IDLE and freezes `control_ticks=554180` | RAM/20 kHz DCL path uses 10.57% of the period at the observed peak, with no deadline or overflow event in the fresh window |

The 2026-06-21 closure record below adds the FLASH/20 kHz build, autonomous
boot, lifecycle, Scope2000, and load-budget acceptance. Further 100 kHz work and
the external Trip waveform are deferred follow-ups.

---

## Phase 4.1 - User-code ownership and reset boundary (20 kHz RAM/FLASH accepted)

Operating steps and remaining acceptance items are in `docs/phase4.1-user-code-boundary.md`.

- [x] Phase 4.1 plan updated in English with the full mutable-user-state reset contract
- [x] CPU1 managed-build hooks generate `v2k_user_boundary.cmd` / normalized manifest before link and run the post-link verifier before `all` completes
- [x] RAM linker ownership: USER_RUN=RAMLS6, USER_CONST_RAM=RAMLS7, USER_GOLDEN_RAM=RAMD5[0:0x800), GS4 excluded
- [x] User data golden uses TI linker `crc_table(..., algorithm = CRC32_PRIME)` and runtime expected/actual CRC diagnostics
- [x] User app no longer uses reset-section pragmas; plain-C state spans two translation units, initialized data, BSS, arrays, structs, function statics, DCL state, and `const`
- [x] Python unit tests for classifier/manifest/verifier failure modes pass
- [x] Host-compiled C CRC vector test locks `v2k_crc32_prime()` to the linker-verified `0xD501B381` user-data image
- [x] CPU1 and CPU2 RAM `buildProject` complete with zero errors
- [x] RAM/20 kHz hardware smoke covers boot CRC, START reset, user-state pollution recovery, RAM golden CRC fail-closed, and recovery after restoring golden
- [x] CPU1 and CPU2 FLASH `buildProject` and boot smoke, including CPU1 FLASH ownership / CPU2-image collision check
- [x] FLASH golden CRC fail-closed test with a controlled expected/actual mismatch
- [x] RAM/100 kHz ISR-budget regression deferred by the 20 kHz baseline decision (2026-06-20)
- [x] 100-cycle START/STOP and FAULT/CLEAR/START endurance run (FLASH/20 kHz, 2026-06-21; debugger-driven command publication, CPU2 halted)
- [x] Scope/parameter/DCL demo regression through Scope2000 GUI and raw CAL transactions

Record area (LAUNCHXL-F28P65X, CCS 21.0.0, RAM/20 kHz, 2026-06-20):

| Date | Item | Method | Measured | Conclusion |
|---|---|---|---|---|
| 2026-06-20 | Unit tests and RAM post-link verifier | `python3 -m unittest discover -s cpu1/tools -p 'test_*.py' -v`; CCS `buildProject(cpu1)` and `buildProject(cpu2)` active RAM configs | 12 unit tests pass. CPU1 RAM build compiles `v2k_crc32_prime.c` and completes with `user boundary verified`; manifest covers 2 user objects, and generated sections are `v2k_user_text`, `v2k_user_const`, `v2k_user_data`, `v2k_user_bss`. CPU2 RAM build also completes with zero errors | Object classification, manifest normalization, linker-fragment generation, verifier core checks, and the extracted CRC helper are active in the managed CCS build |
| 2026-06-20 | C CRC vector test added | host compiler builds `cpu1/runtime/v2k_crc32_prime.c` through `cpu1/tools/test_v2k_crc32_prime.py` | The fixed 32-word linker-verified user-data image returns `0xD501B381` | The runtime CRC byte order and polynomial are now pinned by an automated test independent of the final `.out` |
| 2026-06-20 | Linker layout and CRC record | inspect CPU1 RAM map/linkInfo after `buildProject` | `v2k_user_data` LOAD=`0x20000` in USER_GOLDEN_RAM, RUN=`0xB000` in USER_RUN, size=`0x20`; `v2k_user_const` is in USER_CONST_RAM; `.cinit` has no user data/BSS entries; `V2K_UserDataCrcTable` has one `CRC32_PRIME` record with CRC `0xD501B381` | RAM ownership, non-adjacent golden, `.cinit` exclusion, and build-time CRC table placement match the Phase 4.1 contract |
| 2026-06-20 | Boot reset diagnostics | dual-core RAM debug already running; read CPU1 Expressions before START | `g_v2k_user_reset_error=0`; `g_v2k_user_crc_expected=g_v2k_user_crc_actual=0xD501B381`; `g_v2k_user_reset_count=0`; `g_user_setup_count=0`; `g_v2k_app_enabled=0` | Boot validates layout/CRC and leaves the app disabled; boot init does not call `setup()` |
| 2026-06-20 | START reset and plain-C state recovery | APP_START seq=1; APP_STOP seq=2; while IDLE, overwrite user initialized/BSS state (`g_user_setpoint`, `g_user_initial_offsets[0]`, `g_user_secondary_gain`, `g_user_pi.Kp`, `g_user_setup_count`, `g_user_secondary_ticks`, `g_user_trace.write_index`); APP_START seq=3 | First START: ack=1/result=OK, RUNNING, app enabled, reset_count=1, setup_count=1. After pollution + second START: ack=3/result=OK, RUNNING, reset_count=2, reset_error=0, setup_count=1, setpoint=`1.5`, offset0=`0.100000001`, secondary_gain=`0.25`, PI Kp=`0.349999994`, CRC expected/actual still `0xD501B381` | START restores all writable user static state covered by the generated user sections before calling `setup()`; setup is not responsible for full reset |
| 2026-06-20 | RAM golden CRC fail-closed | APP_STOP seq=4; save USER_GOLDEN_RAM first word (`0x0000 0x3FC0`), write `0x0001` to `0x20000@Data`, then APP_START seq=5 | ack=5, `cmd_result=BAD_STATE(2)`, state stays IDLE, app enabled=0, `g_v2k_user_reset_error=3` (`GOLDEN_CRC`), expected CRC `0xD501B381`, actual CRC `0xA3ACB215`, reset_count remains 2 | Corrupted golden prevents START and keeps the app/output path locked out |
| 2026-06-20 | Recovery after golden restore | restore `0x0000` at `0x20000@Data`; APP_START seq=6; final APP_STOP seq=7 | seq=6 START succeeds with result OK, RUNNING, app enabled=1, reset_error=0, expected/actual CRC both `0xD501B381`, reset_count=3. seq=7 STOP returns to IDLE and app enabled=0 | Fail-closed path is recoverable after the golden image is restored; the board was left running in IDLE with the app disabled |

The 2026-06-21 FLASH/20 kHz closure record below completes the FLASH build,
ownership/collision, cold-boot, golden restore, CRC fail-closed, 100-cycle, and
Scope2000 regression gates. Further 100 kHz work is deferred by policy.

---

## Phase 4.5 - Build-time symbol baking (20 kHz FLASH hardware acceptance complete)

Operating policy and acceptance items are in `docs/phase4.5-symbol-baking.md`.

- [x] `V2K_DESC_MAX=128`, `V2K_USER_DESC_MAX=96`, contract version 10, and GS0 C28x size assertions pass
- [x] CPU1 reserves an exact-size patch blob in RAMD5_FREE for RAM and FLASH_BANK1 for FLASH
- [x] TI `ofd2000 --xml --dwarf` baker scopes variables through Phase 4.1 linker ranges and expands supported scalar leaves
- [x] Baker-generated final-image hash replaces the stale Git-HEAD hash and is stable across repeated patching
- [x] Mutable user leaves are `PARAM|SCOPE`; user const leaves are `SCOPE` only
- [x] Unsupported pointers and other omitted leaves are emitted in the JSON report
- [x] Tracked managed-build hooks run boundary generate, link, boundary verify, bake/patch, and report generation in order
- [x] CPU1 and CPU2 RAM `buildProject` complete with zero errors; CPU1 report contains 30/96 entries and 2 skipped DCL pointer members
- [x] Baker fixture tests and actual RAM `.out` dry-run pass; report roots match `cpu1.map` word addresses
- [x] CPU1 and CPU2 FLASH clean `buildProject` complete with zero errors; CPU1 report contains 30/96 entries and 2 skipped DCL pointer members
- [x] On-target runtime acceptance (RAM/20 kHz, CCS MCP): `desc_error=0`, table/blob headers, baked names+addresses match the report (B), CAL_WRITE tunes a baked PARAM and a const leaf is rejected (D)
- [x] Scope2000 software alignment: exact contract 10 acceptance, golden-vector mirror, stable 128-entry ENUM paging, post-enumeration build-hash confirmation, stale catalog-command rejection, and USER/system classification
- [x] Rebuild both cores and repeat the on-target ENUM/Scope2000 checks with the current contract and user blob version 4
- [x] DAQ bind of a baked var over the link, and ENUM paging returns all entries on hardware (F host side)
- [x] Scope2000 clean-PC ENUM with no `.out` present (C) and build-hash re-enumeration after changing the user variable set (E)

Software verification record (CCS 21.0.0, active RAM configuration, 2026-06-20):

| Item | Method | Result |
|---|---|---|
| Managed build | CCS `buildProject(cpu1)` and `buildProject(cpu2)` | Both RAM builds complete with zero errors; CPU1 prints `user boundary verified` followed by `user descriptors patched: 30/96, skipped=2` |
| CPU1 codestart clean rebuild | delete both generated `RAM/f28p65x_codestartbranch.obj` and `RAM/runtime/f28p65x_codestartbranch.obj`, then run CCS `buildProject(cpu1)` | CCS 21 emits the external startup object under `runtime/` while listing it as a root linker input; the tracked make hook copies it only when missing/different, boundary generation waits for that normalization, one link completes, and the final image remains baked |
| Actual-image bake | `v2k_bake_user_desc.py ... --dry-run` on `cpu1/RAM/cpu1.out` plus map comparison | 30 leaves validate; examples include `setpoint@0xB000`, `pi.Kp@0xB002`, `trace.err[0]@0xB02A`, and const `gain[0]@0xB800`; all are C28x word addresses |
| Kind and skip policy | inspect the contract-9 `v2k_user_desc_report.json` | data/BSS entries had kind 3, const entries had kind 2; `pi.sps` and `pi.css` were omitted with `pointer is unsupported` |
| Contract-10 descriptor origin | baker fixtures, generated vectors, and host C contract compile | new user blobs use version 3 and emit kind 7 (`USER|PARAM|SCOPE`) for mutable leaves or kind 6 (`USER|SCOPE`) for const leaves; platform/system descriptors keep USER clear |
| Final-image hash | rebuild once, then run the phony bake target again without relinking | both reports produce `build_hash=0x19406E08`; the blob section is normalized before hashing, so old patched bytes do not perturb the result |
| Tests | Python baker/boundary/CRC suites, host C contract compile, golden-vector check | All automated checks pass; HELLO vector now carries contract 10 |
| Scope2000 alignment | `cargo test`, `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings`, and exact vector-directory comparison | 32/32 Rust tests pass; the host accepts only wire 6 / contract 10, validates a stable catalog up to 128 entries, refreshes full device information on a build-hash change, rejects queued commands from the previous catalog, classifies descriptors by the USER kind bit, and folds struct/array paths into trees |

Hardware verification record (LAUNCHXL-F28P65X, CCS 21.0.0, RAM/20 kHz, dual-core live debug, CCS MCP, 2026-06-20):

| Date | Item | Method | Measured | Conclusion |
|---|---|---|---|---|
| 2026-06-20 | Runtime blob acceptance | CPU1 read `g_v2k_desc_error` + `desc_table.hdr`; CPU2 read `g_v2k_user_desc_blob` header (blob lives in @Program) | `desc_error=0`; table magic=`0x564B4454` "VKDT", contract_ver=9, entry_stride_words=22, entry_count=52 (22 platform + 30 user); blob magic=`0x564B5544` "VKUD", version=2, count=30, capacity=96; `build_hash=0x19406E08` identical across report/blob/table | Runtime validated the blob header and all 30 user entries and appended them within capacity; the published table carries the baked set |
| 2026-06-20 | Address correctness (check B) | CPU1 CCS Expressions `&var` vs `v2k_user_desc_report.json` word-addresses, plus value sanity | `&setpoint`=0xB000, `&pi`=0xB002, `&pi.Imax`=0xB010, `&gain`=0xB800, `&offset`=0xB018, `&trace`=0xB02A, `&secondary_gain`=0xB01E, `&secondary_ticks`=0xB032 — all exact; `setpoint`=1.5, `gain[0]`=1.0 match the source declarations | Baked word-addresses match CCS exactly, no off-by-word from the 16-bit-char convention; the addresses point at the right variables |
| 2026-06-20 | Baked names in the live table | CPU1 decode `desc_table.entries[i].name/addr/type/kind` over the appended user range (i=22,44,48,51) | entry22=`setpoint`(0xB000,F32,kind3), entry44=`trace.idx`(U16), entry48=`gain[0]`(0xB800,const,kind2), entry51=`gain[3]`; struct/array leaves carry dotted/bracketed names | Names + struct/array expansion are physically present in the GS0 table at the exact path ENUM reads; const leaves are SCOPE-only (kind 2) |
| 2026-06-20 | Check D — tune a baked PARAM | drive GS4 `param_shadow` via CPU2 {addr=0xB000, type=F32, value_bits=0x40200000=2.5f}, count=1, `commit_seq`=1; read back on CPU1 | `setpoint` 1.5→2.5; `param_status.applied_seq=1`, `result=OK(0)` | A baked user PARAM tunes through the param double-buffer with the same handshake as a platform PARAM — no descriptor-membership special-casing |
| 2026-06-20 | Check D — const write rejected | GS4 shadow {addr=0xB800 (`gain[0]`, const), type=F32}, `commit_seq`=2; then restore setpoint via `commit_seq`=3 | `param_status.result=3 (BAD_ADDR)`, `fail_idx=0`, `applied_seq=2`, `gain[0]` unchanged=1.0; restore commit applies setpoint=1.5/result OK | Const baked as SCOPE-only enumerates/observes but is excluded from the write path, matching the implemented kind policy |
| 2026-06-20 | Offline baker suite | `python3 -m unittest test_v2k_bake_user_desc` | 9/9 pass: struct/array/const expansion, C28x wide-char round-trip, capacity+alignment overflow, duplicate/overlong-name reject, pointer report, typedef/const/TI-far resolve | The offline halves of checks A (extract/expand) and F (capacity overflow) hold in the current tree |

The 2026-06-21 closure record below adds FLASH, full standalone ENUM paging,
isolated Scope2000 operation without a project or `.out`, baked-variable bind,
mutable/const CAL policy, and A→B→A build-hash re-enumeration.

---

## Phase 4.6 - Runtime load observability (20 kHz baseline accepted; performance follow-ups deferred)

Operating semantics and acceptance items are in `docs/phase4.6-runtime-load-observability.md`.

- [x] One-second CPU1 ISR windows publish average plus coherent peak-tick control/scope/latency/tick fields
- [x] Five platform descriptors expose the snapshot signal values as system Variables (`load_avg/load_peak/ctrl_at_peak/scope_at_peak/lat_at_peak`); `prof_seq/cycle_budget/peak_tick` ride in STATUS only. STATUS carries the full profiler snapshot for system diagnostics
- [x] Host-compiled profiler test covers window publication, average, coherent peak fields, status publication, and tick wrap
- [x] Scope2000 STATUS reconciliation and System control-cycle-budget UI
- [x] CPU1 and CPU2 RAM builds complete with zero errors
- [x] FLASH/20 kHz Scope OFF/Stream/Capture measurements
- [x] RAM/100 kHz full-load regression deferred by the 20 kHz baseline decision (2026-06-20)
- [x] Host-stop/Scope-overrun isolation regression
- [x] GPIO profiler-overhead comparison deferred to the performance follow-up; no pass is claimed here
- [x] Stream channel-scaling optimization investigation deferred: controlled 1-8 channel matrix, normal/block-boundary timing, GPIO reconciliation, and post-optimization repeat remain separate work
- [x] ADC/EOC terminology documents `lat_at_peak` as trigger-to-ISR-entry hardware latency, not CPU-executed ADC work

Software verification record:

| Date | Item | Method | Result |
|---|---|---|---|
| 2026-06-20 | Automated verification | Viewer2000 `python3 -m unittest discover -s cpu1/tools -p 'test_*.py' -v`, `python3 tools/gen_vectors.py --check`, and host contract compile; Scope2000 `cargo test`, `cargo fmt --check`, and `cargo clippy --all-targets -- -D warnings` | Viewer2000 25/25 host tests pass, vectors report `CHECK OK`, and PC contract assertions compile with `-Werror`; Scope2000 57/57 tests pass with clean formatting and clippy. No CCS target build or hardware acceptance is claimed |

Hardware verification record (LAUNCHXL-F28P65X):

| Date | Rate / mode | Avg / peak / violations | GPIO before / after | Conclusion |
|---|---|---|---|---|
| 2026-06-21 | FLASH/20 kHz, Scope OFF, RUNNING | avg 1776, peak 2167, control 389, scope 118, ADC/EOC latency 277, violations 0, overflow 0 | Deferred | Pass |
| 2026-06-21 | FLASH/20 kHz, 8-channel Stream, F32, prescaler 200 | avg 2151, peak 3018, control 394, scope 1030, ADC/EOC latency 271, 12 contiguous blocks, overrun 0, violations 0, overflow 0 | Deferred | Pass |
| 2026-06-21 | FLASH/20 kHz, 8-channel Capture, F32, prescaler 1 | avg 2151, peak 3309, control 389, scope 1173, ADC/EOC latency 266, 20 contiguous blocks / 195 full-rate ticks, violations 0, overflow 0 | Deferred | Pass; requested 200 points is block-addressed and an in-block trigger left the terminal block at 5 ticks |
| Deferred | RAM/100 kHz, Scope OFF | — | — | Reconsider only after hot-path optimization |
| Deferred | RAM/100 kHz, Stream | — | — | Reconsider only after hot-path optimization |
| Deferred | RAM/100 kHz, 8-channel Capture | — | — | Reconsider only after hot-path optimization |
| 2026-06-20 | Preliminary Stream observation; rate/build/channel types not recorded | Scope-at-total-peak: 1 channel = 376 cycles; 8 channels = 809 cycles; delta = 433 cycles (~61.9 per added channel) | — | **Open** — `scope_at_peak` is not Scope average or Scope-local maximum; run the controlled matrix and boundary-tick measurements in the Phase 4.6 document before drawing an optimization conclusion |

### Phase 4.6 Flash / 20 kHz closure session

Current acceptance policy: 20 kHz is the deployment baseline. The already
recorded 100 kHz experiments remain evidence, but no pending Phase 4.x item is
blocked on a new 100 kHz run. The baud-rate ladder, controlled 1-8-channel
Scope optimization matrix, GPIO profiler-overhead comparison, and external-pin
Trip waveform are separate deferred investigations and are not claimed here.

- [x] CPU1 explicitly assigns Flash Banks 0-2 to CPU1 and Banks 3-4 to CPU2 before releasing CPU2
- [x] CPU1/CPU2 RAM clean builds (recorded 2026-06-20) and CPU1/CPU2 FLASH clean builds (2026-06-21) complete with zero errors
- [x] FLASH map audit: CPU1 entry `0x080000`, CPU2 entry `0x0E0000`, no bank collision, user golden/blob in CPU1 Bank1
- [x] Debugger-assisted CPU2 Flash programming succeeds after the CPU1 Flash Plugin bank map is corrected
- [x] Debugger-assisted first boot reaches the dual-core handshake
- [x] Standalone cold boot reaches IDLE with the application disabled, protection locked, CPU2 alive, VCP online, and no RAM-load CPU2WDRS window
- [x] FLASH user-state restore and 100-cycle lifecycle endurance pass
- [x] Controlled FLASH golden CRC mismatch fails closed; restoring the official image recovers
- [x] Scope2000 firmware-only ENUM, baked-variable bind/tune/reject, and A→B→A build-hash re-enumeration pass
- [x] FLASH/20 kHz Scope OFF, 8-channel Stream, 8-channel Capture, host-stop overrun, VCP reconnect, and CPU2-halt isolation pass
- [x] Official image A is restored, cold-boots into IDLE, and its final build hash is recorded

Pre-session RAM/20 kHz UI snapshot supplied by the user (empty DCL PID client):
ADC/EOC latency 203 cycles, Control 272, Scope 983, Runtime 894, Headroom
7648, Budget 10000. This is a baseline observation, not FLASH acceptance;
the session records a fresh coherent FLASH snapshot for comparison.

2026-06-21 Flash programming note: the initial CPU2 load failure was not a
linker-bank collision. CPU2's FLASH map starts at `0x0E0000`, but the CCS Flash
Plugin still had all banks assigned to CPU1. Correcting the CPU1 Flash Plugin
map to Bank0-2 -> CPU1 and Bank3-4 -> CPU2 allowed CPU2 Flash programming to
complete. A subsequent attempt to program CPU2 while the application was already
running failed while erasing Bank3 (`FMSTAT=65`), matching TI's warning that the
other core must not execute while Flash E/P code uses shared RAM and that CPU2
must not execute from the bank being erased. The supported workflow is now
recorded in `AGENTS.md`, and `tools/ccs/flash_dual_core_f28p65x.sh` was added
to make the Flash Plugin bank map explicit before loading both cores.

Verification record (2026-06-21, LAUNCHXL-F28P65X, CCS 21.0.0,
FLASH/20 kHz):

| Item | Method | Measured | Conclusion |
|---|---|---|---|
| Software gates | Viewer2000 Python suites, golden-vector check, host C contract compile; Scope2000 format, Clippy, and tests | Viewer2000 26/26 tests pass, vectors `CHECK OK`, C compile clean; Scope2000 Clippy clean and 57/57 tests pass. `cargo fmt --check` reports one pre-existing `src/app.rs` formatting delta; the documented `tools/check-brand.py` no longer exists | Firmware-side software gates pass. Scope2000 formatting/documentation cleanup remains open and is not recorded as passing |
| FLASH clean builds | Move both old FLASH output directories to `/tmp`, then CCS MCP `buildProject(cpu1)` and `buildProject(cpu2)` | Both builds return `success=true`, zero errors. CPU1 post-link prints `user boundary verified` and `user descriptors patched: 30/96, skipped=2` | Fresh managed builds, verifier, and baker pass |
| FLASH map and user image | Inspect `cpu1.map`, `cpu2.map`, boundary manifest, descriptor report, and link info | CPU1 codestart `0x080000`, Bank0 used `0x41AC` words, Bank1 used `0x892`, Bank2 unused; CPU2 codestart `0x0E0000`, Bank3 used `0x1C11`, Bank4 unused. User data LOAD `0x0A0000`, RUN `0xB000`, LOAD/RUN size `0x20`; const `0x0A0020`; blob `0x0A0040`; one CRC32_PRIME record `0x57EAE164`; build hash `0x521C2BA6` | CPU1/CPU2 Flash allocations do not collide; user golden, const, and descriptor blob are in CPU1 Bank1; USER_RUN remains RAM and post-link ownership checks pass |
| Dual-image programming | CPU1-only DSS programmer with necessary-sectors-only erase, verify enabled, and temporary all-banks-to-CPU1 programming map | CPU1 Bank0/1 and CPU2 Bank3 erase/program/verify complete; final `BANKMUXSEL & 0x3FF = 0x3C0` | Official image A programmed successfully and deployment ownership restored |
| Debugger-assisted Flash boot | Launch the project-local dual target without loading a program, load symbols only, System Reset, run CPU1, then observe CPU1 and CPU2 | CPU1 IDLE, app disabled, descriptor error 0, CPU2 alive 1, CPU1/CPU2 NMI counts 0, CPU2 handshake 3, user CRC expected=actual=`0x57EAE164`, bank mux `0x3C0` | Both cores execute their Flash images and complete the contract handshake. This is not the standalone power-cycle result |
| FLASH lifecycle endurance | Keep CPU1 running from Flash and CPU2 halted; publish mailbox commands through the debugger. Run 50 START/STOP cycles and 50 START→soft TZ→FAULT→CLEAR→START→STOP cycles | 100/100 cycles pass, 150 STARTs, 350 invariant checks, final ack 302/state IDLE/fault 0/app disabled. Reset count 151 includes one pre-test smoke START; every START has setup count 1, reset error 0, and CRC `0x57EAE164`; every STOP/FAULT has OST latched | Flash golden→RUN restore and lifecycle/protection state transitions remain stable while the comms core is unavailable |
| CPU2-halt isolation | Halt CPU2 for the endurance run and compare CPU1 control state/tick before and after | CPU1 continues advancing ticks and completes all 100 lifecycle cycles; `g_cpu2_alive` transitions 1→0 without changing IDLE/RUNNING/FAULT command behavior | CPU1 does not block on CPU2. Reconnect must be repeated after a standalone reset because connecting CPU2 under the debugger restarts its one-shot IPC rendezvous context |
| Preliminary Scope-OFF profiler read | Read a completed one-second window while CPU2 is halted | Budget 10000, average 1655, peak 1745, control-at-peak 0 in IDLE, scope-at-peak 118, ADC/EOC hardware latency 265, violations 0 | Useful FLASH/20 kHz diagnostic only. The overflow count had already been incremented by a debugger halt, so this is not the required no-halt Scope-OFF acceptance sample |
| Standalone cold boot | Terminate every CCS session, set S3 to Flash boot, physically power-cycle, and use only the XDS110 VCP | HELLO wire 6 / contract 12 / hash `0x521C2BA6` / 57 descriptors / 20 kHz; STATUS IDLE, fault 0, app disabled, CPU1/CPU2 heartbeats advancing; full ENUM 57/57, 30 USER entries; debugger-assisted symbol-only inspection records both NMI counters 0 and the deployment bank map `0x3C0` | Both Flash images autonomously boot and publish the protocol with no RAM-load window. Protection is locked in IDLE; direct OST checks also pass in the debugger-assisted boot and every STOP/FAULT lifecycle invariant |
| FLASH CRC negative | Copy the official CPU1 ELF to `/tmp`, flip one 16-bit word in the Flash golden LOAD image at file offset `0x839C`, and leave both the CRC table and descriptor blob byte-identical; program the negative ELF and cold boot | START is rejected with `BAD_STATE`; app enabled remains 0; reset error is `GOLDEN_CRC`; expected `0x57EAE164`, actual `0x21477B70`; state remains IDLE. Reprogramming official A and cold booting returns expected=actual and normal START behavior | Controlled golden corruption fails closed without changing the advertised catalog; official recovery passes |
| CAL and const rejection | Raw VCP CAL_READ/WRITE/COMMIT against baked entries on official A | Mutable `setpoint` changes `0x40000000`→`0x40200000` with result OK and is restored; writing const `gain` is rejected with `BAD_ADDR`, fail index 0, and value unchanged | Mutable/const policy is enforced on the standalone link |
| Scope2000 isolated enumeration and binding | Run Scope2000 with HOME under `/tmp`, no CCS project binding, and no `.out`; connect via VCP and bind the baked `setpoint` variable | UI reports project `cpu1`, 20 kHz, IDLE, full Variable catalog and control-cycle budget; `setpoint` appears by name and plots its actual 2.0 value in Time Series | Firmware-only full ENUM and baked-variable plotting pass |
| Build-hash re-enumeration | Official A (`0x521C2BA6`, 30 USER entries) → temporary source with `flash_probe` → clean build/program/cold boot B → restore source, rebuild/program/cold boot A | B hash `0xF057F5F0`, 31 USER entries; user confirms Scope2000 automatically refreshes the catalog and shows `flash_probe`. Restored A returns exactly to hash `0x521C2BA6`, 30 USER entries, and no `flash_probe` | A→B invalidates the old catalog; B→A proves deterministic original hash restoration |
| Scope OFF / STATUS-CAL reconciliation | START official A, DAQ OFF, wait for a fresh coherent profiler publication; bracket CAL_READ of all five profiler Variables with raw STATUS reads at one `prof_seq` | seq 367: budget 10000, avg 1776, peak 2167, control 389, scope 118, latency 277, violations 0, overflow 0. Later Stream seq 369 has exact STATUS/CAL values `{2151,3018,394,1030,271}` | Coherent publication and the system-Variable view agree; peak remains below budget |
| 8-channel Stream | Bind eight baked F32 variables, RUNNING, prescaler 200, consume continuously for 1.25 s | 12 contiguous 10-tick blocks, first/last ticks 7,357,220/7,379,220, overrun 0; seq 369 peak 3018 < 10000, violations 0, overflow 0 | Sustainable SCI Stream does not perturb control timing |
| 8-channel full-rate Capture | Under the same binding, arm prescaler 1 / requested 200 points / 50% pretrigger; force a 2.0→2.5 baked setpoint crossing, freeze, and drain | 20 contiguous blocks, 195 full-rate ticks, trigger tick 7,384,055, first/last ticks 7,383,960/7,384,150; seq 370 peak 3309 < 10000, violations 0, overflow 0 | Full-rate capture passes. The block-addressed prehistory plus an in-block trigger makes the terminal block partial (5 ticks), so the honest drained length is 195 rather than pretending it is 200 |
| Host-stop overrun isolation | RUNNING, 8-channel Stream at prescaler 1, issue no BLOCK_REQ for 600 ms, then read STATUS, `control_ticks`, and one block | overrun 1148; CPU1 tick +12,380 and user `control_ticks` +12,360 while the host consumes nothing; violations 0, overflow 0 | Ring saturation drops Scope blocks and never blocks the CPU1 tick or user control |
| VCP close/reopen | Close the serial file descriptor for 500 ms while RUNNING, reopen, send HELLO/STATUS, then STOP | official hash returns; state remains RUNNING; CPU1 tick advances 16,800 while disconnected; STOP returns IDLE/fault 0 | Link loss is isolated and the protocol connection recovers without restarting CPU1 |
| CPU2-halt isolation and recovery qualification | Execute all 100 lifecycle cycles with CPU2 halted; later cold boot restores both cores and VCP. A separate CCS attach/continue experiment confirms that attaching CPU2 after standalone boot restarts its one-shot IPC debug context and is therefore not a valid transparent-resume method | CPU1 completes the lifecycle and keeps advancing with CPU2 unavailable; subsequent deployment cold boot restores HELLO/ENUM. Late CCS attach does not recover VCP until a physical reset | CPU1 non-blocking behavior passes. Recovery acceptance is by deployment cold boot; debugger late-attach behavior is recorded as a tooling constraint, not misreported as transparent CPU2 resume |
| Final deployment cold boot | Terminate CCS, physically power-cycle with S3 in Flash boot, then perform a read-only HELLO/STATUS/full ENUM over VCP | A hash `0x521C2BA6`, wire 6, contract 12, 57 descriptors / 30 USER / no `flash_probe`, 20 kHz; IDLE, fault 0, flags 0, CPU1/CPU2 heartbeats 31,460/30,933 and advancing; coherent profiler seq 31, budget 10000, avg 1655, peak 1748, scope 118, ADC/EOC latency 268, violations 0, overflow 0 | Official A is the final deployed image and the board is left cold-booted in IDLE |
