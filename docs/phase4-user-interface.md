# Phase 4 — User-interface boundary (L1↔user): development plan

> **Document status**: implemented and accepted on the 20 kHz RAM and FLASH
> baselines. The 2026-06-21 FLASH closure includes cold boot, lifecycle,
> protection, Scope2000, and load-budget regression; measured results are in
> [BRINGUP.md](../BRINGUP.md). Further 100 kHz optimization is deferred.
>
> **Scope**: Phase 4 is **firmware boundary work only**. It writes **no control math** (the platform ships none — control math is user-supplied; see [AGENTS.md](../AGENTS.md) Decisions). It turns the user surface into an **Arduino-style `setup()` / `control()`**, draws the **wire↔runtime portability seam**, and proves the reset lifecycle with a manually sectioned demo. [Phase 4.1](phase4.1-user-code-boundary.md) hardens that prototype into an automatic, linker-verified user-code/state boundary. The "user's variables visible by name on any PC" experience remains the separate build-tooling effort [Phase 4.5](phase4.5-symbol-baking.md).
>
> **Constraint**: do **not** disturb verified Phase 1/2/3/3.5 work. Phase 4 includes the guarded physical repackage into `runtime/`, `wire/`, and `app/`.

Phase 3/3.5 proved the platform can schedule, observe, tune, and stream — but every one of those was exercised by a debug sine that ignores its input and writes one duty, and the executor still bakes chip-specific driverlib into the hot path. Phase 4 fixes both **before** Phase 5 applies power, so the *platform-plumbing* failure mode is removed before the *motor* failure mode is introduced.

**Acceptance target = the interface contract, not a converging control loop.** There is no plant on the bench (that is Phase 5). The demo client exercises the seam — named physical I/O, the auto-reset lifecycle, observability — not control correctness.

## Starting state of the boundary

Grounded in the pre-Phase-4 code (`cpu1/app/v2k_platform.h`, `v2k_executor.c`, `v2k_registry.c`, `v2k_scope_runtime.c`):

| Piece | Current (Phase 3 stub) | Gap to close |
|---|---|---|
| old pointer-param callback I/O | `{tick,…,adc_a0_raw,adc_a0_v,…}` / `{pwm1_duty}` | wire/raw ADC leaks; single channel; pointer/struct ceremony unidiomatic for C2000; → global `v2k_io.in/.out` |
| `v2k_executor.c` | `v2k_acquire`→`ADC_readResult`, `v2k_apply`→`EPWM_setCounterCompareValue`, ISR reads `CPUTimer1`/ADC regs | driverlib baked into L1 — blocks the F29x/ARM portability goal |
| count→physical | hard-coded `raw*3.0f/4095f`, one channel | no port table; no general count→physical |
| old weak control callback | weak; debug sine; ignores `in` | renamed `control()`; never driven by real control |
| lifecycle | runs in all states; no reset | **no automatic state reset on start** → a wound-up integrator can restart from its FAULT value (serious hazard) |
| default bind | `v2k_default_bind` binds 8 ch at boot (`v2k_scope_runtime.c:163`) | removed — binding on-demand |

## Decisions (ratified)

1. **User surface = Arduino-style `setup()` / `control()`**, `void`, accessing I/O through a named global `v2k_io.in / v2k_io.out` (not passed params — matches the C2000/DCL/SDK idiom and is simpler for non-programmers). `loop()` is rejected (it implies a free-running loop and hides the platform's deterministic-ISR identity); `on_start()` is eliminated (the auto-reset replaces it). Names are bare (no `v2k_` prefix) — sketch-level, like Arduino; platform internals keep `v2k_*`, and board/chip wiring uses `wire_*`.
2. **Stop/start = unconditional full reset of all user state.** On every IDLE→RUNNING the platform re-copies the entire user data section to its declared initial values. This is a **safety guarantee** (the wound-up-integrator restart is structurally impossible), not opt-in convenience. Tuned parameters reset too (they need not persist — each run starts reproducibly from the source-declared values). Phase 4 proves the sequence on explicitly sectioned state; Phase 4.1 completes automatic coverage.
3. **wire↔runtime compile-time seam**: count→physical + the port table + all driverlib-facing hot-path access live in `wire/`, behind a zero-cost inline seam (`wire_acquire/apply/cycle_count/...`, FreeRTOS-port-layer style). The executor runtime becomes chip-agnostic.
4. **Port table is wire/board-owned** (routing an input to an ADC SOC / output to an EPWM is hardware; rule 4 keeps runtime/user code off registers).
5. **First client = a C2000Ware DCL control block** (float PI), loopback-fed, no motor.
6. **Remove the boot default binding** — on-demand only.

## Division of labor

Phase 4 is **mostly C**. **SysConfig** adds no new static hardware for the minimal cut (reuse Phase 2/3). **C** does the global I/O surface, the wire seam, the reset lifecycle, and the demo. The baseline uses dedicated user data/BSS sections plus a boot-time RAM snapshot. Automatic object-based section ownership, linker-owned RAM/FLASH golden images, and build-time escape detection are the Phase 4.1 closure.

---

## 1. The wire↔runtime seam (the portability cut)

The runtime executor stops calling driverlib directly; it calls a small **compile-time** seam that `wire/` implements per chip/board, the hot ones `inline` where needed so the generated ISR code stays equivalent to today's — zero added cycles, **not** a runtime function-pointer HAL (the FreeRTOS port-layer model).

```c
// wire implements (chip/board), runtime calls — the only layer rewritten on a port.
void     wire_acquire(v2k_io_in_t *in);    // read input ports → physical quantities
void     wire_apply(const v2k_io_out_t *out); // physical commands → actuators
uint32_t wire_cycle_count(void);            // free-running cycle counter
uint16_t wire_isr_ack(void);                // clear ADC int / overflow / PIE ack
void     wire_register_ports(uint16_t fast_prescaler); // register port names into the descriptor table
```

The runtime owns the `__interrupt` vector (off ADCA1 EOC) but delegates board/chip access to `wire`:

```c
void v2k_executor_tick(void)              // chip-agnostic core
{
    uint32_t t0 = wire_cycle_count();
    wire_acquire(&v2k_io.in);             // wire: read ADCs + count→physical
    v2k_io.in.due_mask = v2k_schedule(&param_due);
    if (param_due && !v2k_user_reset_is_active()) v2k_param_apply_ready();
    if (state == RUNNING) v2k_user_control_tick(); // L3 control() through lifecycle gate
    wire_apply(&v2k_io.out);              // wire: route to PWM
    v2k_scope_sample_all(v2k_io.in.tick);
    g_v2k_tick++;
    /* control/scope/isr cycles + budget via wire_cycle_count() diffs */
}
```

## 2. The user surface: `setup()` / `control()` + global `v2k_io`

The I/O is a single named global the platform fills/applies; the user reads/writes it directly — no params, no pointers. Fields come from the wire/board port table (a motor build gets `.ia/.theta/.vbus` in, `.duty_a/b/c` out; the minimal demo gets `.vsense` in, `.duty_a` out).

```c
// the platform's global (filled by wire_acquire, applied by wire_apply)
extern v2k_io_t v2k_io;     // v2k_io.in.<port>, v2k_io.out.<port>

// the user's whole file — declarations at top, then control()
float kp = 1.5f, ki = 50.0f;          // tunable; auto-reset on start (no need to persist)
float integrator = 0.0f;              // state; auto-reset on start

void control(void)                    // every control tick (deterministic ISR rate)
{
    float e = ref - v2k_io.in.vsense;
    integrator += ki * e;
    v2k_io.out.duty_a = kp * e + integrator;
}
```

The port table (wire-owned) is the single source for acquisition routing, scaling, naming, and descriptor registration; `wire_acquire`/`wire_apply` walk it. A motor thin-view header exposes named motor fields; **Simulink forward-compat**: each port = a root inport/outport (an adapter can map `v2k_io` ↔ `rtU/rtY` later).

## 3. Lifecycle: automatic full reset on START (the safety core)

**The hazard**: a PI integrator (or PLL, ramp, observer) winds up to an extreme value during a FAULT. If stop→start does not reset it, the controller restarts from that extreme value and commands a huge output on the first tick — a serious safety hazard. Relying on the user to reset it (an opt-in `on_start()`) is unsafe: forget it once, and the board is at risk.

**The mechanism**: on every IDLE→RUNNING transition, on CPU1, **before output is enabled**, the platform re-initializes the **entire user data section** to its declared initial values:

```
v2k_user_reset():  memcpy(user_data_init → user_data)   // restore declared initializers
                   memset(user_bss, 0, user_bss_size)    // zero the uninitialized data
```

- The Phase 4 demo explicitly places its tested globals in `user_data` / `user_bss`. Phase 4.1 removes those annotations, routes all user-object writable storage automatically, and replaces the fixed boot snapshot with linker-owned RAM/FLASH golden LOAD images.
- It runs at the START transition (not in the hot loop) — a few KB memcpy, negligible.
- **The lifecycle operation is unconditional and does not depend on a user reset hook.** Phase 4 verifies this for the explicitly sectioned demo state. Phase 4.1 makes every user-object `static`/global enter those sections automatically, which completes the general safety claim.

**Sequence**: `IDLE → (APP_START) → v2k_user_reset() → setup() [optional] → enable output → control() each tick`. `CLEAR_FAULT` only returns to IDLE; the next START resets again.

**`setup()` is optional**, runs each start **after** the reset, only for init that a declared initial value cannot express (a vendor init call, a computed table). The simplest user writes only declarations + `control()` and gets the full reset for free.

**Target contract after Phase 4.1 — why auto-reset and not `on_start()`**:
robustness to user mistakes (none of these is a safety hazard once automatic
user-object section ownership is active, because the section reset restores
declared initial values before `control()`):

| User mistake | Outcome |
|---|---|
| declares a var in `setup()`, uses it in `control()` | `control()` won't compile (undeclared) → self-correcting |
| `float integrator = 0;` (non-`static`) inside `control()` | resets every tick → integral action dead (broken, but **not** a wind-up hazard) |
| forgets to reset some state | nothing — the section reset already restored declared initials |
| where they put a `static`/global declaration | all covered — the reset is section-based |

The only silent footgun (non-`static` local in `control()`) is the *safer* kind of broken (no windup). Mitigation: the `examples/` template declares variables at file scope (Arduino convention); non-programmers copy it.

## 4. Three-layer packaging (the Arduino-style seam)

| Arduino | Viewer2000 | user must understand? |
|---|---|---|
| core (`wiring.c`) | **`runtime/` + `wire/`** = platform runtime + board/chip wiring + four shared interfaces + scope/scheduler/protection | no — invisible |
| libraries | **`examples/`** = readable reference apps; control math from C2000Ware, wiring is ours and readable | optional |
| sketch | **L3** = `setup()` / `control()`, accesses `v2k_io`, includes one header | Arduino-simple |

Phase 4 draws the logical seam and performs the physical repackage (user faces one header + writes `setup`/`control`; executor calls `wire_*`). The runtime is invisible, yet the user keeps full observability of their own code (Phase 4.5 / CCS).

## 5. First real client (no motor)

`control()` wires a C2000Ware DCL float PI (`DCL_runPI_C1`) to the ports: read `v2k_io.in.vsense`, error vs a tunable `setpoint`, run the PI with tunable `kp/ki`, write `v2k_io.out.duty_a`. All declared at file scope → auto-reset on start. The input is the **DACA→ADCA0 loopback** ([phase2 §1.3](phase2-bringup.md)). **No plant → convergence is meaningless**; this verifies plumbing + the reset. The math is vendor DCL — the demo only wires it.

## 6. SysConfig / build prerequisites

Minimal cut: **no new static hardware** (reuse Phase 2/3). C2000Ware **DCL** added to the cpu1 include path. The Phase 4 baseline adds dedicated `user_data`/`user_bss` sections and a bounded RAM snapshot sufficient to prove the lifecycle. The complete linker/build contract is specified by Phase 4.1. Build via CCS `buildProject`, RAM + FLASH, both cores, 0 errors.

## 7. Verification (on-target, CCS + Scope2000)

The current acceptance baseline is 20 kHz in both RAM and FLASH. The historical
100 kHz measurements remain recorded, but further 100 kHz Phase 4 acceptance is
deferred until the platform hot path is optimized.

| # | Verification | Method | Pass criterion |
|---|---|---|---|
| A | named I/O + count→physical | DACA → known codes; read `v2k_io.in.vsense` | tracks DACA with the wire port-table scale/offset (no raw-count leak) |
| B | wire↔runtime seam zero-cost | `isr_cycles_max` before/after the seam refactor | within measurement noise of today's inline version |
| C | descriptor exposes ports | ENUM via Scope2000 | each in/out port enumerated by name/type |
| D | **auto-reset safety** | START → run integrator up → soft TZ → FAULT → wind integrator to an extreme via CCS → CLEAR_FAULT → START | on the new START, `integrator` reads its declared initial value (0) **before** the first `control()` tick; output never commands the wound-up value; verified without any `on_start` code |
| E | reset covers initialized + zero state | overwrite the explicitly sectioned demo data/BSS, STOP, START | initialized values return to declarations and BSS returns to zero |
| F | observability of user state | CCS Expressions/Graph on `integrator` + `v2k_io.out` | sampled every tick (rule 7); Scope2000-by-name is Phase 4.5 |
| G | ISR budget with a real client | reset max; PI + scope at 20 kHz | `isr_cycles_max < 10000`; `budget_violation=0`, `ovf=0` |

## 8. Acceptance and exit

| Acceptance item | Pass condition |
|---|---|
| user surface | `setup()`/`control()` (`void`) + global `v2k_io.in/.out`; old callback hooks gone |
| wire↔runtime seam | `v2k_executor.c` driverlib-free; wire implements `wire_*`; zero-cost confirmed (B) |
| reset lifecycle prototype | the explicitly sectioned Phase 4 demo state is reset to declared initials on every START, before output enable; the wind-up-restart test (D) passes |
| port contract | generic named ports; count→physical in wire |
| default bind removed | on-demand binding works |
| first client | a DCL block runs in `control()`; no control math written by us |
| four-config build | CPU1/CPU2 RAM/FLASH 0 errors; `v2k_tb_check`/layout assert clean |
| verifications A–G | pass at the current 20 kHz baseline |
| regression | Phase 2/3/3.5 still pass |

Record into BRINGUP.md Phase 4 area: date/board/CCS, RAM/FLASH,
`V2K_ISR_HZ`, build hash; the wire port table; A–G results including the seam
before/after (B), the wind-up-restart trace (D), and the 20 kHz load snapshot.
Tag `phase4-user-interface` after all of the above.

The broader claim that arbitrary plain-C user state is automatically covered is
reserved for Phase 4.1 acceptance and its own tag.

## 9. What Phase 4 deliberately does NOT do

- **No control library, no control math** — the PI is vendor DCL; we wire it. (DCL for generic primitives, MOTORCONTROL-SDK for PMSM, hand-written for SRM-specific control; the platform ships none.)
- **No motor, no plant, no closed-loop correctness** — Phase 5, which reuses this contract.
- **No user-variable-name baking** — that is [Phase 4.5](phase4.5-symbol-baking.md); Phase 4 verifies observability via CCS.
- **No general user-object ownership enforcement** — Phase 4 proves the reset lifecycle on an annotated demo; [Phase 4.1](phase4.1-user-code-boundary.md) makes the boundary automatic and build-verified.
- **No L2 directory** — control math remains user/vendor-owned; Phase 4 only wires a DCL PI demo.
- **No PC simulation / SimSource, no Simulink binding** — only the contract is kept forward-compatible.
- **Note (unresolved)**: whether to let advanced students drop below the `control()` boundary to touch registers (rule 4 loosening / "embedded teaching bench" mode) was raised but not pursued — the boundary stays; hardware learning is served by SysConfig-by-hand + the readable open `wire/`.
