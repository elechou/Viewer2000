# Phase 4 — User-interface boundary (L1↔user): development plan

> **Document status**: a **forward-looking development plan** — nothing here is implemented yet (unlike the phase1–3.5 docs, which were bring-up checklists for already-written firmware). As pieces land, the verification sections become the acceptance checklist; measured results go into [BRINGUP.md](../BRINGUP.md) Phase 4 area; tag `phase4-user-interface` after acceptance.
>
> **Scope**: Phase 4 is **firmware boundary work only**. It writes **no control math** (the platform ships none — control math is user-supplied; see [AGENTS.md](../AGENTS.md) Decisions). It turns the user surface into an **Arduino-style `setup()` / `control()`** with auto-observable globals, draws the **L0↔L1 portability seam**, and makes **stop/start an unconditional full reset of all user state** (a safety guarantee). The "user's variables visible by name on any PC" experience is a separate build-tooling effort, [Phase 4.5](phase4.5-symbol-baking.md).
>
> **Constraint**: do **not** disturb verified Phase 1/2/3/3.5 work. The physical file reorg (`runtime/` folder) is deferred to a guarded follow-up.

Phase 3/3.5 proved the platform can schedule, observe, tune, and stream — but every one of those was exercised by a debug sine that ignores its input and writes one duty, and the executor still bakes chip-specific driverlib into the hot path. Phase 4 fixes both **before** Phase 5 applies power, so the *platform-plumbing* failure mode is removed before the *motor* failure mode is introduced.

**Acceptance target = the interface contract, not a converging control loop.** There is no plant on the bench (that is Phase 5). The demo client exercises the seam — named physical I/O, the auto-reset lifecycle, observability — not control correctness.

## Current state of the boundary (where we start)

Grounded in the actual code (`cpu1/v2k_platform.h`, `v2k_executor.c`, `v2k_registry.c`, `v2k_scope_runtime.c`):

| Piece | Current (Phase 3 stub) | Gap to close |
|---|---|---|
| `plat_in_t / plat_out_t` passed to `user_step(in,out)` | `{tick,…,adc_a0_raw,adc_a0_v,…}` / `{pwm1_duty}` | L0 (raw ADC) leaks; single channel; pointer/struct ceremony unidiomatic for C2000; → global `v2k_io.in/.out` |
| `v2k_executor.c` | `v2k_acquire`→`ADC_readResult`, `v2k_apply`→`EPWM_setCounterCompareValue`, ISR reads `CPUTimer1`/ADC regs | driverlib baked into L1 — blocks the F29x/ARM portability goal |
| count→physical | hard-coded `raw*3.0f/4095f`, one channel | no port table; no general count→physical |
| `user_step()` | weak; debug sine; ignores `in` | renamed `control()`; never driven by real control |
| lifecycle | runs in all states; no reset | **no automatic state reset on start** → a wound-up integrator can restart from its FAULT value (serious hazard) |
| default bind | `v2k_default_bind` binds 8 ch at boot (`v2k_scope_runtime.c:163`) | removed — binding on-demand |

## Decisions (ratified)

1. **User surface = Arduino-style `setup()` / `control()`**, `void`, accessing I/O through a named global `v2k_io.in / v2k_io.out` (not passed params — matches the C2000/DCL/SDK idiom and is simpler for non-programmers). `loop()` is rejected (it implies a free-running loop and hides the platform's deterministic-ISR identity); `on_start()` is eliminated (the auto-reset replaces it). Names are bare (no `v2k_` prefix) — sketch-level, like Arduino; platform internals keep `v2k_*` / `l0_*`.
2. **Stop/start = unconditional full reset of all user state.** On every IDLE→RUNNING the platform re-copies the entire user data section to its declared initial values. This is a **safety guarantee** (the wound-up-integrator restart is structurally impossible), not opt-in convenience. Tuned parameters reset too (they need not persist — each run starts reproducibly from the source-declared values).
3. **L0↔L1 compile-time seam**: count→physical + the port table + all driverlib live in L0, behind a zero-cost inline seam (`l0_acquire/apply/cycle_count/...`, FreeRTOS-port-layer style). The executor (L1) becomes chip-agnostic.
4. **Port table is L0/board-owned** (routing an input to an ADC SOC / output to an EPWM is hardware; rule 4 keeps L1/L3 off registers).
5. **First client = a C2000Ware DCL control block** (float PI), loopback-fed, no motor.
6. **Remove the boot default binding** — on-demand only.

## Division of labor

Phase 4 is **mostly C**. **SysConfig** adds no new static hardware for the minimal cut (reuse Phase 2/3). **C** does the global I/O surface, the L0 seam, the auto-reset, and the demo. **The build/linker** gains the one non-trivial piece: a dedicated user data section + a flash "golden image" / RAM snapshot for the reset (§3).

---

## 1. The L0↔L1 seam (the portability cut)

L1 stops calling driverlib; it calls a small **compile-time** seam that L0 implements per chip (`l0_<chip>.{c,h}`), the hot ones `inline` so the generated ISR code is byte-identical to today's — zero added cycles, **not** a runtime function-pointer HAL (the FreeRTOS port-layer model).

```c
// L0 implements (chip/board), L1 calls — the only layer rewritten on a port.
void     l0_acquire(v2k_io_in_t *in);    // read input ports → physical quantities
void     l0_apply(const v2k_io_out_t *out); // physical commands → actuators
uint32_t l0_cycle_count(void);            // free-running cycle counter
void     l0_isr_ack(void);                // clear ADC int / overflow / PIE ack
void     l0_register_ports(void);         // register port names into the descriptor table
```

L0 owns the `__interrupt` vector (off ADCA1 EOC) and calls the chip-agnostic tick:

```c
void v2k_executor_tick(void)              // chip-agnostic core
{
    uint32_t t0 = l0_cycle_count();
    l0_acquire(&v2k_io.in);               // L0: read ADCs + count→physical
    v2k_io.in.due_mask = v2k_schedule(&param_due);
    if (param_due) v2k_param_apply_ready();
    control();                            // L3 (reads v2k_io.in, writes v2k_io.out)
    l0_apply(&v2k_io.out);                // L0: route to PWM
    v2k_scope_sample_all(v2k_io.in.tick);
    g_v2k_tick++;
    /* control/scope/isr cycles + budget via l0_cycle_count() diffs */
}
```

## 2. The user surface: `setup()` / `control()` + global `v2k_io`

The I/O is a single named global the platform fills/applies; the user reads/writes it directly — no params, no pointers. Fields come from the L0/board port table (a motor build gets `.ia/.theta/.vbus` in, `.duty_a/b/c` out; the minimal demo gets `.vsense` in, `.duty_a` out).

```c
// the platform's global (filled by l0_acquire, applied by l0_apply)
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

The port table (L0-owned) is the single source for acquisition routing, scaling, naming, and descriptor registration; `l0_acquire`/`l0_apply` walk it. A motor thin-view header exposes named motor fields; **Simulink forward-compat**: each port = a root inport/outport (an adapter can map `v2k_io` ↔ `rtU/rtY` later).

## 3. Lifecycle: automatic full reset on START (the safety core)

**The hazard**: a PI integrator (or PLL, ramp, observer) winds up to an extreme value during a FAULT. If stop→start does not reset it, the controller restarts from that extreme value and commands a huge output on the first tick — a serious safety hazard. Relying on the user to reset it (an opt-in `on_start()`) is unsafe: forget it once, and the board is at risk.

**The mechanism**: on every IDLE→RUNNING transition, on CPU1, **before output is enabled**, the platform re-initializes the **entire user data section** to its declared initial values:

```
v2k_user_reset():  memcpy(user_data_init → user_data)   // restore declared initializers
                   memset(user_bss, 0, user_bss_size)    // zero the uninitialized data
```

- The user's globals + function-`static` locals land in `user.obj`'s `.data`/`.bss`, routed by the `.cmd` into `user_data` / `user_bss`. The flash "golden image" is the LOAD copy the cl2000 copy-table already keeps (RUN=RAM, LOAD=flash); RAM-only builds snapshot the section into a reserved golden region at first boot, then re-copy from it.
- It runs at the START transition (not in the hot loop) — a few KB memcpy, negligible.
- **It is unconditional and does not depend on the user.** Every user `static`/global is back at its declared initial value before `control()`'s first tick, regardless of what the user did or didn't write. The wound-up-restart hazard is structurally impossible.

**Sequence**: `IDLE → (APP_START) → v2k_user_reset() → setup() [optional] → enable output → control() each tick`. `CLEAR_FAULT` only returns to IDLE; the next START resets again.

**`setup()` is optional**, runs each start **after** the reset, only for init that a declared initial value cannot express (a vendor init call, a computed table). The simplest user writes only declarations + `control()` and gets the full reset for free.

**Why auto-reset and not `on_start()`** — robustness to user mistakes (none of these is a safety hazard, because the section reset already restored declared initial values before `control()`):

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
| core (`wiring.c`) | **`runtime/`** = L1 core + L0 HAL + four shared interfaces + scope/scheduler/protection | no — invisible |
| libraries | **`examples/`** = readable reference apps; control math from C2000Ware, wiring is ours and readable | optional |
| sketch | **L3** = `setup()` / `control()`, accesses `v2k_io`, includes one header | Arduino-simple |

Two-step: **Phase 4 draws the logical seam** (user faces one header + writes `setup`/`control`; executor driverlib-free); the **physical `runtime/` move** is a guarded follow-up with a build + dual-core smoke regression. The runtime is invisible, yet the user keeps full observability of their own code (Phase 4.5 / CCS).

## 5. First real client (no motor)

`control()` wires a C2000Ware DCL float PI (`DCL_runPI_C1`) to the ports: read `v2k_io.in.vsense`, error vs a tunable `setpoint`, run the PI with tunable `kp/ki`, write `v2k_io.out.duty_a`. All declared at file scope → auto-reset on start. The input is the **DACA→ADCA0 loopback** ([phase2 §1.3](phase2-bringup.md)). **No plant → convergence is meaningless**; this verifies plumbing + the reset. The math is vendor DCL — the demo only wires it.

## 6. SysConfig / build prerequisites

Minimal cut: **no new static hardware** (reuse Phase 2/3). C2000Ware **DCL** added to the cpu1 include path. **New linker/build work**: a dedicated `user_data`/`user_bss` section routing `user.obj`'s data, with a LOAD(flash)/RUN(RAM) copy table for the golden image (RAM build: a reserved snapshot region). Build via CCS `buildProject`, RAM + FLASH, both cores, 0 errors.

## 7. Verification (on-target, CCS + Scope2000)

RAM/20 kHz baseline, then RAM/100 kHz for budget; Phase 2 TZ/state-machine regressed.

| # | Verification | Method | Pass criterion |
|---|---|---|---|
| A | named I/O + count→physical | DACA → known codes; read `v2k_io.in.vsense` | tracks DACA with the L0 port-table scale/offset (no raw-count leak) |
| B | L0↔L1 seam zero-cost | `isr_cycles_max` before/after the seam refactor | within measurement noise of today's inline version |
| C | descriptor exposes ports | ENUM via Scope2000 | each in/out port enumerated by name/type |
| D | **auto-reset safety** | START → run integrator up → soft TZ → FAULT → wind integrator to an extreme via CCS → CLEAR_FAULT → START | on the new START, `integrator` reads its declared initial value (0) **before** the first `control()` tick; output never commands the wound-up value; verified without any `on_start` code |
| E | reset covers params + statics | tune `kp` live, STOP, START | `kp` is back at its declared value; a `static` local in `control()` also reset |
| F | observability of user state | CCS Expressions/Graph on `integrator` + `v2k_io.out` | sampled every tick (rule 7); Scope2000-by-name is Phase 4.5 |
| G | ISR budget with a real client | reset max; PI + scope at 20k/100k | `isr_cycles_max < 200MHz/V2K_ISR_HZ`; `budget_violation=0`, `ovf=0` |

## 8. Acceptance and exit

| Acceptance item | Pass condition |
|---|---|
| user surface | `setup()`/`control()` (`void`) + global `v2k_io.in/.out`; `user_step`/`on_start` gone |
| L0↔L1 seam | `v2k_executor.c` driverlib-free; L0 implements `l0_*`; zero-cost confirmed (B) |
| auto-reset | the user data section is reset to declared initials on every START, before output enable; the wind-up-restart test (D) passes; params + statics reset (E) |
| port contract | generic named ports; count→physical in L0 |
| default bind removed | on-demand binding works |
| first client | a DCL block runs in `control()`; no control math written by us |
| four-config build | CPU1/CPU2 RAM/FLASH 0 errors; `v2k_tb_check`/layout assert clean |
| verifications A–G | pass at 20 kHz; budget/overflow re-checked at 100 kHz |
| regression | Phase 2/3/3.5 still pass |

Record into BRINGUP.md Phase 4 area: date/board/CCS, RAM/FLASH, `V2K_ISR_HZ`, build hash; the L0 port table; A–G results incl. the seam before/after (B) and the wind-up-restart trace (D); one `isr_cycles_max` set each at 20/100 kHz. Tag `phase4-user-interface` after all of the above.

## 9. What Phase 4 deliberately does NOT do

- **No control library, no control math** — the PI is vendor DCL; we wire it. (DCL for generic primitives, MOTORCONTROL-SDK for PMSM, hand-written for SRM-specific control; the platform ships none.)
- **No motor, no plant, no closed-loop correctness** — Phase 5, which reuses this contract.
- **No user-variable-name baking** — that is [Phase 4.5](phase4.5-symbol-baking.md); Phase 4 verifies observability via CCS.
- **No physical `runtime/` folder move** — logical seam only.
- **No PC simulation / SimSource, no Simulink binding** — only the contract is kept forward-compatible.
- **Note (unresolved)**: whether to let advanced students drop below the `control()` boundary to touch registers (rule 4 loosening / "embedded teaching bench" mode) was raised but not pursued — the boundary stays; hardware learning is served by SysConfig-by-hand + the readable open L0.
