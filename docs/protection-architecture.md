# Protection Architecture — Viewer2000

> Cross-cutting reference for the platform's power-stage protection. It collects
> what was previously scattered across `v2k_fault.c` comments,
> `phase2-bringup.md`, `phase5.0-powerstage-interface.md`, and the
> `phase5.1-epwm-debug.md` root-cause note into one model. Read this before
> touching any trip-zone, CMPSS, X-BAR, or DRV8323 code, and before any move
> from DRY_RUN to POWERED.

Authoritative source files:

- `cpu1/runtime/v2k_fault.c` — state machine, TZ ISR, fault latch.
- `cpu1/wire/wire_pwm.c` — ePWM trip-zone / digital-compare / current-trip config.
- `cpu1/wire/wire_f28p65x.c` — power-stage START sequence, DRY_RUN/POWERED mode.
- `cpu1/wire/wire_drv8323rs.c` — gate-driver wake/config/status, nFAULT.
- `contracts/v2k_command.h` — `V2K_STATE_*`, `V2K_FAULT_*` numeric contract.

---

## 0. Principles (from the platform rules)

1. **Protection is a pure-hardware chain.** The thing that actually shuts off
   PWM is the ePWM Trip-Zone, which goes through no CPU. The state machine only
   decides *whether to release or to gate*; the ISR only *records and reports*.
   CPU death does not disable protection (rule 1, rule 2).
2. **Protection must be in place before power is applied**, and proven by
   register read-back, not by assumption (`v2k_fault_init` reads `TZFLG.OST`
   back and `wire_panic_halt()`s if the output is not actually gated).
3. **The debugger is not an e-stop.** A halt can interact with CBC6; an e-stop
   trusts the hardware trip only.

---

## 1. Three layers

```
┌─ L1 state machine — v2k_fault.c (CPU1 background super-loop) ────────────┐
│ Decides release vs gate. g_v2k_sm_state: INIT→IDLE→RUNNING→FAULT.        │
│ Accepts APP_START / APP_STOP / CLEAR_FAULT from the CPU2→CPU1 mailbox.   │
│ NOT in the shutdown path — it cannot be the authority for cutting PWM.   │
└───────────▲──────────────────────────────────────┬──────────────────────┘
     report  │                              release / force OST
             │                                      │
┌────────────┴──────────────────────────────────────▼─────────────────────┐
│ L1' TZ interrupt — v2k_tz_isr (PIE INT_EPWM1_TZ, enabled only in RUNNING)│
│ Trigger = OST latch only. Latches FAULT, picks fault_code, defers DRV    │
│ shutdown to foreground. The hardware already forced the pins low; the    │
│ ISR is a recorder, not the cutoff.                                       │
└────────────────────────────────────▲─────────────────────────────────────┘
                  already forced low  │
┌─────────────────────────────────────┴────────────────────────────────────┐
│ L0 pure hardware — ePWM Trip-Zone (no CPU, fastest, authoritative)        │
│ OST one-shot latch → TZA/TZB force low → all three phases A+B off.        │
│ CBC (TZ6) cycle-by-cycle. DCAEVT1 per-event action (EPWMxA only).         │
└──────────────────────────────────────────────────────────────────────────┘
```

The control ISR never blocks on any of this: a full IPC drops, a full scope
ring overwrites, a dropped block is just dropped. Protection lives underneath
the control domain, not inside it.

---

## 2. State machine timeline

```
[reset]
  │
  ▼ INIT
  │  v2k_fault_arm()   (before Board_init — "gate as early as possible")
  │    enable motor ePWM clocks, then force OST (best-effort; TZCTL still Hi-Z)
  │  v2k_fault_init()  (after Board_init — authoritative)
  │    TZSEL=OSHT1|CBC6, TZA/TZB=force-low configured → force OST latches
  │    install TZ ISR, both interrupt levels DISABLED
  │    read back: output NOT locked → wire_panic_halt()   ← invariant, not trust
  ▼ IDLE   (ready, outputs gated by OST, TZ interrupts disabled)
  │  ── APP_START ──► wire_powerstage_start_begin/poll  (see §5 DRY_RUN/POWERED)
  ▼ RUNNING
  │    control ISR runs; OST interrupt enabled; (POWERED) DCAEVT1 armed
  │  ── APP_STOP ──► disable both TZ int levels, force OST → IDLE
  │  ── hardware trip ──► v2k_tz_isr → FAULT
  ▼ FAULT  (outputs OST-locked; waits for CLEAR_FAULT)
  │    CLEAR_FAULT only succeeds if wire_fault_source_is_released():
  │    trip source still asserted → stay FAULT (accepted, not released)
  ▼ IDLE
```

Two hard-won ordering rules live here (Phase 2, `v2k_fault.c` comments):

- **TZ interrupt levels are enabled only in RUNNING** (PIE level + EPWM-level
  `TZEINT.OST`). Leaving them enabled during IDLE/FAULT lets a latched OST keep
  re-setting `TZFLG.INT` into `PIEIFR`; the next START then fires a *spurious*
  trip. Enable on START, disable on STOP, disable when the ISR enters FAULT.
- **APP_START sets RUNNING before enabling the interrupt; APP_STOP disables the
  interrupt before forcing OST.** Both orderings exist so a real trip racing the
  command is never silently overwritten (rule 7).

---

## 3. The Trip-Zone funnel (why so many names)

These are not competing protections — they are stages of one C2000 pipeline.

| Name | What it is | Stage |
|---|---|---|
| `TZ1..TZ6` | six external digital trip inputs (from Input X-BAR) | source |
| `DCTRIPSEL` | mux: which TRIPIN line feeds the digital comparator's DCAH | selector |
| `DCAEVT1` | the digital-compare event derived from DCAH | derived source |
| `TZSEL` | per-source enable into the OST latch or the CBC latch | the OR funnel |
| `OST` / `CBC` | one-shot (sticky) / cycle-by-cycle (auto-reset) latches | latch |
| `TZCTL.TZA/TZB` | action applied to EPWMxA / EPWMxB when OST or CBC latches | action |
| `TZCTL.DCAEVTx` | **separate** per-event action, EPWMxA only | action (side) |

```
 sources           TZSEL (OR)        latch              action            pin
 ───────           ─────────         ─────              ──────            ───
 TZ1 (nFAULT)─┐
 TZ2..TZ5     ├─►[OSHT bits]──OR──►┌ OST ┐──► TZA=low ─► EPWMxA
 TZ6         ─┤                    │      │──► TZB=low ─► EPWMxB   (A+B both off)
 DCAEVT1(I)  ─┘                    └──┬───┘
                                      └──► TZFLG.OST ──► TZ interrupt (RUNNING)

 TZ1..TZ6    ───►[CBC bits] ──OR──► CBC (per-cycle) ──► TZA/TZB

 ★ side path (NOT through TZSEL/OST): DCAEVT1 ─► TZCTL.DCAEVT1 action ─► EPWMxA only
```

**Convergence point = the OST latch + TZA/TZB.** Every source enabled into OST
collapses into one `TZFLG.OST`, one TZ interrupt, one both-sides-low action.

This platform's actual OST membership: `TZSEL=0x0120` = `OSHT1 | CBC6`, plus
`DCAEVT1` (bit `0x4000`) added only when the current trip is armed in POWERED.

---

## 4. The three hardware trip chains

### Chain ① External one-shot — DRV8323 nFAULT (authoritative, A+B)

```
DRV8323 internal fault (OCP / UVLO / OT)
  → nFAULT pin low → GPIO82 (code name TZ_EXT)
  → INPUT X-BAR INPUT1  (wire_pwm.c: XBAR_setInputPin)
  → ePWM TZ1, selected as OSHT1
  → OST latch → TZA/TZB force LOW → all three EPWMxA+xB low
  → (RUNNING) INT_EPWM1_TZ → v2k_tz_isr → fault_code = V2K_FAULT_TZ1_EXT (1)
```

Both-sides shutdown + interrupt + report. This is the real e-stop path, and it
plus the DRV's own internal OCP are the **authoritative cutoffs**.

### Chain ② On-chip current window — CMPSS/ADC → X-BAR TRIP7 → DCAEVT1

```
phase A current → CMPSS7 (high+low window)  ┐
phase B current → CMPSS8 (high+low window)  ├─ OR → EPWM X-BAR TRIP7 → TRIPIN7
phase C current → ADCC PPB1 (low only)      ┘
  → DCTRIPSEL DCAH=TRIPIN7 ; TZDCSEL DCAEVT1=DCAH-high → DCAEVT1 asserted
  → when armed (TZSEL.DCAEVT1): OST latch → TZA/TZB → A+B off  ✔ symmetric path
  → always (per-event): TZCTL.DCAEVT1 action → EPWMxA only      ⚠ see §6
```

Provisional window `DACL=512 / DACH=3584` raw counts (`wire_adc.h`,
`WIRE_CURRENT_LIMIT_LOW/HIGH_COUNTS`) — TI bring-up values, **not** calibrated
amperes. Note "asymmetric" in the Phase 5.0 docs refers to **this window**:
phase C can only trip on the low side (C5 feeds only the CMPSS2 low comparator).
That is a *different axis* from the high-side/low-side gate question in §6.

### Chain ③ Cycle-by-cycle — CBC (TZ6)

Auto-resets each PWM cycle. **Debugger interaction:** a CPU halt with CBC6
configured can latch `TZCBCFLG` and force outputs low — so PWM waveforms must be
judged with the CPU *running*, never at a breakpoint.

---

## 5. DRY_RUN vs POWERED

Mode is a compile-time predefine (`wire_f28p65x.c`); checked-in default is
`WIRE_POWERSTAGE_MODE_DRY_RUN`. POWERED requires **both**
`WIRE_POWERSTAGE_MODE=0` **and** `WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED=1`.

| | DRY_RUN (default) | POWERED |
|---|---|---|
| Gate driver | stays asleep (ENABLE low) | sleep→wake→configure→verify |
| START path | straight to READY | SLEEP_WAIT → WAKE_WAIT → checks → arm |
| Current trip | **disarmed** (DCAEVT1 not in TZSEL) | armed after readiness |
| Purpose | scope MCU PWM logic with FETs unpowered | real energized operation |

POWERED START (`wire_powerstage_start_poll`, see also phase5.0 §7) fails closed:
nFAULT must be released, DRV config must read back, all three current samples
must be in-window, and the current trip is armed only if its register read-back
passes — with a re-check of the DCAEVT1 source *across* the OST-release boundary.

---

## 6. DCAEVT1's dual identity (the Phase 5.1 root cause)

The single most important non-obvious fact in this whole architecture:

> **DCAEVT1 has two independent outputs.** ① When selected in `TZSEL`, it feeds
> the OST latch (→ TZA/TZB → both sides). ② It *also* has a per-event
> `TZCTL.DCAEVT1` action that drives **EPWMxA only**, and that action is applied
> **whenever the DC event is asserted, independent of TZSEL arming**.

Consequences that actually bit us:

- **DRY_RUN high-side float (fixed in `cbdf1da`).** `TZCTL.DCAEVT1` reset value
  is `0 = High-Impedance`. In DRY_RUN the DRV sleeps, the current-sense
  amplifiers are unpowered, the CMPSS low comparator asserts TRIPIN7 with no
  real current, and the per-event action floated all three EPWMxA (high side)
  while EPWMxB kept switching — even though the trip was "disarmed."
  *Fix:* manage the action with the arm state — `DISABLE` when disarmed,
  force `LOW` when armed.
- **Silent force, no fault.** The per-event action fires **without latching OST**
  and the TZ interrupt is wired to OST only — so the high side was forced with
  zero interrupt, zero `fault_code`, zero report. A protection-style output
  action with no observability is the worst possible combination.
- **Armed-transient asymmetry (open, §8).** Because the overcurrent uses DCAEVT1
  (EPWMxA) with no matching DCBEVT1, the symmetry of a real armed trip relies on
  the OST→TZA/TZB path; the per-event side covers only the high side. Needs
  POWERED hardware verification before sustained runs.

**Rule going forward:** never assume "DCAEVT1 is gated by arming." Its A-only
action must be explicitly set (`DISABLE`/`LOW`) for every arm state.

---

## 7. Distinguishing multiple faults

OST is an OR of several sources, but hardware preserves per-source identity:

- **Which class →** `fault_code`: the ISR reads `TZOSTFLG`
  (`EPWM_getOneShotTripZoneFlagStatus`, `wire_pwm_current_trip_was_active`). If
  the DCAEVT1 one-shot bit is set → `V2K_FAULT_OVERCURRENT (2)`, else
  `V2K_FAULT_TZ1_EXT (1)`.
- **Which phase / side →** `s_current_trip_last`: read back the CMPSS `COMPSTS`
  filter-latch bits (`wire_pwm_capture_current_sources`) into the
  `PHASE_A_HIGH/_A_LOW/_B_HIGH/_B_LOW` bitfield, exposed as the `curr_trip_last`
  scope variable.

**Known limitations (carry these forward):**

1. `fault_code` is only `{NONE, TZ1_EXT, OVERCURRENT}`. If nFAULT and overcurrent
   assert in the same cycle, both `TZOSTFLG` bits latch (no information lost) but
   `fault_code` is labeled OVERCURRENT. For exact attribution read `TZOSTFLG` +
   `curr_trip_last`, not just `fault_code`.
2. `TZ1_EXT` is the *else* bucket. Today only TZ1 feeds OST besides DCAEVT1, so it
   is unambiguous — **but adding any TZ2..TZ5 source into OST would have it
   mislabeled `TZ1_EXT`.** Extend the discrimination when you extend OST.

---

## 8. Safe states on overcurrent

Two legitimate safe states exist; "all off, current freewheels through the body
diodes" is real but is **not** what an asymmetric (A-only) trip produces.

| Safe state | How | Cost |
|---|---|---|
| All-off coast | A+B all off; current freewheels through body diodes | DC-bus pump-up on a stiff/weak supply; body-diode Vf loss/heat under sustained or repetitive trips |
| Low-side brake | high off, **low all on** (a defined state) | lower loss (FET channel, not diode); sustained short = dynamic braking, evaluate at high current |

The danger of the A-only state is not "failed to dissipate current" — it is that
"high off + low *still switching per the commanded duty*" is **neither** safe
state, but a half-driven, undefined bridge. The authoritative both-off state is
delivered by Chain ① (nFAULT → TZ1 → OST → TZA/TZB) and the DRV's own OCP.

**Open item:** make Chain ② symmetric before sustained POWERED runs — add a
`DCBEVT1` force-low, or route the current trip through a TZ1..6 digital input so
it uses TZA/TZB. (Tracked from `phase5.1-epwm-debug.md` §11.7.)

---

## 9. How the regression slipped through (verification gap)

A short post-mortem, because the *process* gap matters as much as the bug:

- The healthy six-pin waveform (A+B each ~48% duty, 1 µs dead time, all phases
  together) was scoped **early**, in `BRINGUP.md` Phase 5.0, *before* the
  CMPSS→X-BAR→DCAEVT1 current substrate was added. It was genuinely correct.
- The current substrate was added **later**. Its acceptance verified the trip at
  the **register level** only — `fault_code=2`, the DCAEVT1 one-shot flag on all
  three ePWMs, `curr_trip_last` — never re-scoping the six gate outputs.
- The six-pin re-scope under a real trip was **explicitly deferred** to school
  equipment ("the home logic analyzer cannot perform the required edge-triggered
  acquisition", `BRINGUP.md` Phase 5.0). DCAEVT1's A-only effect is visible only
  on the high-side pin level — exactly the deferred observation.

Design blind spot (DCAEVT1's dual identity) + verification gap (deferred six-pin
scope) aligned, and the high-side float fell straight through.

---

## 10. Invariants and POWERED gates

Hard requirements before any move from DRY_RUN to POWERED:

- [ ] **All-six-output scope/edge capture is a mandatory gate, not a deferral.**
  A register-level OST/DCAEVT1 flag is *not* a substitute for observing that all
  six gate outputs actually reach the intended state on a real trip.
- [ ] **Make Chain ② symmetric** (add `DCBEVT1`, or route through a TZ digital
  input) so an armed overcurrent forces A+B, not A-only.
- [ ] **Calibrated ampere limits** replace the provisional `512/3584` raw counts.
- [ ] **nFAULT-edge → all-six-PWM shutdown-latency** capture.
- [ ] `WIRE_POWERSTAGE_POWERED_CONFIG_APPROVED=1` only after the DRV register
  image, current-limit behavior, pin mapping, and bench-supply procedure are
  physically verified.

Standing invariants (already enforced, do not regress):

- Protection state is verified by register read-back (`v2k_fault_init` →
  `wire_panic_halt` on failure), never assumed.
- TZ interrupt levels enabled only in RUNNING.
- The control ISR never blocks on the comms core or on protection bookkeeping.
- Every DCAEVT1 arm state explicitly sets its A-only `TZCTL.DCAEVT1` action.
