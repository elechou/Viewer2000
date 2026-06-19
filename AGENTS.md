<!-- DO NOT EDIT - This part is automatically generated. -->

# Agent Guidelines

## CCStudio IDE Installation Directory

CCStudio IDE is installed at `/Applications/ti/ccs2100`. Save it as `{ccs-install-dir}` for the session — scripts and tools will need it.

## MANDATORY Pre-Task Requirement (DO NOT SKIP)

**CRITICAL - NO EXCEPTIONS**: Before ANY CCS/Texas Instruments-related task (even simple ones), you MUST read `/Applications/ti/ccs2100/ccs/Code Composer Studio.app/Contents/Resources/ai/CCS.md`. This file includes information on how to interact with CCS as well as device-specific information (UART backchannel pins, LED setup, transmit best practices, etc.). 

Do NOT call any ccs-project, ccs-debug, ccs-sysconfig, or ccs-serial MCP tools until CCS.md has been read.


<!-- DO NOT EDIT - This part is automatically generated. -->

<!-- User instructions should be added below this line -->

# Commit & Language Rule

Going forward, write **code comments in English**. Existing Chinese comments are kept as-is and translated opportunistically when a file is next edited. **Commit messages and documentation (`.md`) are English.** (History: the repo was bilingual — Chinese comments/docs, English identifiers — until the 2026-06-19 switch that makes the whole repo referenceable by non-native-Chinese readers.)

# AGENTS.md — Viewer2000 (C2000 RCP platform)

## Project positioning

A rapid control prototyping (RCP) platform built from scratch on the TI C2000 **F28P65x (dual C28x core, 200 MHz)**, targeting general power-electronics rapid control prototyping.

**Core premise: the product of this project is the platform itself, not a motor controller.** Platform = deterministic control-task scheduling + peripheral abstraction + parameter tuning / data scoping + protection. FOC motor control is merely the first example application running on the platform, doubling as the platform acceptance test.

**Performance anchor: 20–100 kHz (TBD) × 8ch × f32 lossless, 0.64–3.2 MB/s.** This number drives the physical-layer choice by elimination (see "Communication architecture").

**Repo boundary: this repo is firmware only** (the part flashed into the F28P65x). The host side is a sibling standalone repo **Scope2000** (Rust + egui); the native transport implementation is `V2kSource`. A future `SimSource` is a long-term/optional path (see "Language strategy") — note the platform no longer ships a portable L2 control library, so PC simulation, where wanted, rides Simulink SIL/PIL or the portable subset a given demo happens to use, rather than an FFI binding to a platform L2. Legacy-device compatibility is handled by a separate bridge process that presents normalized Viewer2000 message semantics to Scope2000 over a generic local byte-stream transport, staying out of the `V2kSource` native hot path.

Design principles: **protection first, observability first, platform/application separation.**

## Hardware

| Item | Content | Status |
|---|---|---|
| Board | LAUNCHXL-F28P65X | in hand |
| MCU | TMS320F28P650DK9, dual C28x @ 200 MHz, both cores ISA-identical (shared structs → no ABI issues); dual CLA (one per core) | confirmed |
| Debugger | onboard XDS110 (with virtual COM port VCP → zero extra hardware for the early SCI link) | primary host interface for the early/mid phases |
| On-chip comms | SCI / MCAN (onboard CAN transceiver) / FSI / **EtherCAT ESC (onboard dual PHY + RJ45)**. **No on-chip USB, no Ethernet MAC** | confirmed |

Early parameter tuning, state-machine commands, and variable observation are done via CCS real-time mode (read/write memory without halting the CPU) + Expressions / Graph windows. But unlike the predecessor project: **the wire protocol and the four shared-memory interfaces are frozen together in Phase 0** (host requirements and front-end basics are known), and the minimal SCI data pump is pulled forward to Phase 3.5 — no throwaway ASCII parser.

## Communication architecture

The physical layer is decided by elimination from the full-rate 100 kHz × 8ch requirement:

| Link | Realistic throughput | Verdict |
|---|---|---|
| SCI (XDS110 VCP) | a few Mbps | bring-up dumb-pump only |
| CAN-FD | effective 2–3 Mbps | excluded |
| W5500/SPI Ethernet bridge | single-digit Mbps | excluded |
| FSI | 50–100 Mbps but the PC doesn't speak it, needs a custom bridge | excluded |
| **EtherCAT** | 100 Mbps line rate, ~8–10 MB/s practical | **meets the requirement, adopted** |

**Only two rungs: SCI dumb-pump (Phase 3.5, validates the interfaces and architecture) → EtherCAT (Phase 6, final link).** The protocol (blocks, descriptors) lives inside the pipe; swapping the physical layer only swaps the pipe.

EtherCAT key points:

- **Minimal usable set**: free-run mode (**no DC** — the timestamp is the ISR tick in the block header, unrelated to the EtherCAT clock system), no EoE/FoE, CoE trimmed to the bare minimum. The SSC stack / ESI / state machine is **one-time work**: once OP + PDO mapping is up it is never touched again; later channel additions and protocol changes all happen inside the pipe.
- **Design point**: block = 50 tick × 8ch × int16 = **800 B**; the master loops at 2 kHz, and each PDO cycle carries **0–2 blocks + a count field** (grab more when there's data, absorbing the ppm crystal mismatch at both ends, see the scope plane); single standard-frame payload (process data ceiling ~1486 B); average cable utilization ~15%, peak ~30%. A triple-buffered SyncManager at a 2-block PDO is 3 × 1.6 KB = 4.8 KB ESC RAM — approaching the 8 KB-class budget, so **the ESC RAM verification TODO rises in priority**.
- **Headroom**: 8→16ch is basically free (2 kHz × 1.6 KB or 4 kHz × 800 B); 24–32ch is the engineering-limit zone (ESC RAM and master jitter tighten simultaneously). More nominal channels come from **multi-rate channel groups** and **snapshot mode**, not brute force. If the CPU1 producer side gets tight, quantize/pack can be moved to CPU2 (double the on-chip ring in exchange for ISR cycles).
- **PC master in pure software + an ordinary NIC**: ethercrab preferred (pure Rust, lives directly inside the host), with SOEM as the C reference. Linux + an RT-priority thread runs the 2 kHz loop; running the master on Windows is misery — know this up front.
- All of it lives on CPU2, in parallel with control-core development — EtherCAT bring-up tunes "does the pipe carry"; "are the interfaces right" was already validated by the SCI pump in Phase 3.5, so the two failure modes don't stack.

## Architecture

### Dual-core division of labor

- **CPU1 = control core**: boot master, responsible for peripheral/memory ownership assignment, booting CPU2, NMI/trip configuration. Owns all power-stage peripherals (ePWM / ADC / eQEP / CMPSS / SDFM). Runs the ISR executor + control application. **No printf, no comms stack.**
- **CPU2 = comms core**: owns the comms peripherals (SCI / MCAN / EtherCAT ESC). Runs the parameter service, scope data pump, heartbeat monitor; from Phase 3.5 it runs the SCI dumb-pump, and from Phase 6 the EtherCAT link and firmware update.

The essence of core separation is **isolating fault domains + isolating time domains**: the control domain's determinism is not polluted by the comms domain's jitter/disconnects; protection is the pure-hardware layer beneath both domains.

The drive models are symmetric: **CPU1 is time-driven, CPU2 is event-driven.** CPU1 holds the chip's single control time base: the ePWM master timer (sync-group phase-locked) → ADC SOC → **EOC interrupt** into the control ISR (the interrupt source hangs off EOC, not the period event, so data is already ready on ISR entry), with slow loops software-divided; the ISR tick is **the platform's single control/sampling time**, from which scope timestamps, rate division, and heartbeat epochs are all derived. CPU2 samples nothing and produces no control time — it consumes only data already timestamped by CPU1; its load (the ring has a new block, an SM was read by the master, a command arrived in the mailbox, the SSC main loop) is all events, so its structure = ISR collects events + super-loop does the work. CPU2 may have a low-rate local diagnostic heartbeat/timeout to prove the comms core is itself alive or to judge link timeout, but that local time must not enter sampling, block timestamps, or control scheduling. Its real job is an **elastic coupling** between two external clock domains — the CPU1 crystal and the PC clock; putting a control metronome on the coupling is pointless, and so is an RTOS (rule 6 leaves no opening).

### Layering

| Layer | Content | Owner |
|---|---|---|
| L3 | User control application (FOC demo, etc.) | User (on CPU1) |
| L2 | Control math (**user-supplied**): C2000Ware MOTORCONTROL-SDK / DCL / hand-written / Simulink-generated C — **not provided by the platform; plugged in through the `user_step` boundary** | User (runs on CPU1) |
| L1 | Platform core: ISR scheduling executor, protection manager, parameter/descriptor registry, RAM scope | CPU1 (consumer side may be on CPU2) |
| L0 | Driver layer, on C2000Ware driverlib | Each core separately |

**ISR ownership belongs to L1, not the user.** User code is invoked by the executor as a `user_step(in, out)` callback: in = this tick's named input ports (physical quantities), out = this tick's output ports. The fixed ISR sequence = `plat_acquire → user_step → plat_apply → scope_sample_all (walk the descriptor table) → trigger_eval → g_tick++`; scope sampling and trigger evaluation are done by the platform in the epilogue — the user only registers at init, so **there is no un-sampled path** (rule 7, mechanized). Attached constraint: observables must be addressable static storage (not stack locals); budget ~5 cycles/tick per fast channel into the ISR. This mirrors rule 4 in the time dimension: **L3 touches no registers, and no clock either.**

### Language strategy

**Target-side firmware (both cores, all of L0–L3) is pure C, C99 + stdint fixed-width types, no C++.** Reasons:

- The primary HMI is CCS Expressions/Graph: flat C structs are directly readable; C++ name mangling, templates, and private members are all friction in the map file and watch window (cl2000's C++ support is itself a second-class citizen);
- The CLA compiler accepts only a C subset: keeping control code in plain C preserves the option of later moving the fast loop onto the CLA (one CLA per core). Since L2 control math is now user-supplied, this is the user's option to keep — the platform only guarantees its own L1 boundary stays C;
- Shared-memory structs must be fixed-layout PODs readable by all three parties (CPU1 / CPU2 / host); C is the greatest common denominator;
- C2000Ware / MotorControl SDK / DCL are all C, and the platform's target users (beginners) speak C too; ODrive's heavy C++ is part of its learning barrier — not replicated;
- Avoids bare-metal C++ pitfalls like global-object construction order and implicit constructors.

Three coding conventions that make up for the absence of C++ (aligned with TI DCL style): multiple instances = struct instance + operation functions (`pi_update(&pi_vel, err)`); namespaces = module prefixes; no dynamic allocation after init completes.

**The host side is uniformly Rust + egui**: Scope2000 uses a `DataSource` boundary; `V2kSource` serves native SCI/EtherCAT. A `SimSource` is a long-term/optional path — and since the platform no longer ships a portable L2 control library, PC simulation does not FFI-bind to a platform L2; if wanted later it rides Simulink SIL/PIL (which subsumes it) or whatever portable subset a specific demo uses. Legacy devices are adapted to the same message semantics by a separate bridge process; Scope2000 adds no dedicated compatibility data source.

### The four shared-memory interfaces (frozen before any code)

1. **Descriptor table**: at control-core startup, tunable parameters / observable signals are registered into a table `{name, type, address, kind, default decimation ratio, default group}` written to shared RAM; the comms core and the host **enumerate** this table and know nothing about any variable in advance. Key points:
   - **The on-wire value is the real value**: the firmware does not put `min/max/scale/offset` display or guard-rail metadata on the wire, and does no float→int compression, quantization, or physical-quantity reconstruction; the host interprets the bit pattern by the variable's native type — F32 is F32, I16/U16/I32/U32 are the corresponding integer values;
   - **The decimation-ratio field implements multi-rate channel groups**: 8 fast channels (100 kHz) + N slow channels (1 kHz temperature/bus/status, a rounding error of bandwidth);
   - The table header carries a **firmware build hash**: when the host reconnects and detects a change it forces re-enumeration, preventing an old table from reading new firmware.
2. **Parameter plane** (host → control): double-buffered. Any write first lands in the shadow region → sets a commit flag → the control ISR swaps the whole set at a fixed safe point each cycle. Solves the non-atomic multi-parameter write problem; XDS100/CCS tuning also pokes the shadow region.
3. **Scope plane** (control → host): lock-free SPSC ring buffer. The control ISR is the sole producer (**sampling is in ISR context, so all channels are naturally same-tick**), and the comms core / background loop is the sole consumer. **Dual-mode + blocked**:
   - **Live mode**: decimated continuous stream, for online monitoring / watching trends while tuning;
   - **Snapshot mode**: sampled into the ring at full rate, frozen on trigger, drained slowly — the ring structure naturally supports **pre-trigger** (the key to catching transient faults). Trigger evaluation (variable crosses threshold, state-machine event) is done inside the control ISR. CCS Graph is the zeroth consumer of snapshot mode;
   - **Frame = block**: N tick × M ch + header (start tick, sequence number, channel-group id). N is a parameter: SCI uses a small N, EtherCAT uses N=50 (=800 B). The host detects dropped blocks by sequence number — a dropped block draws a gap, the control core keeps running;
   - **Each PDO cycle carries 0–2 blocks + a count field**: the master grabs more when there's data. The two crystals always differ by tens of ppm, so the ring buffer slowly fills/empties and a long recording session is bound to collide — the bandwidth headroom is spent exactly here; the sequence mechanism incidentally covers dedup and gap detection;
   - The ring-buffer size is the master-side jitter-absorption headroom (tens of KB ≈ tens of ms @ 1.6 MB/s).
4. **Command/status plane**: state-machine requests (start/stop/clear-fault) go over an IPC mailbox; the two cores exchange heartbeats.

**The wire protocol = the serialized view of the four shared interfaces**, frozen together with the interfaces in Phase 0: enumerating the descriptor table = one request, a parameter commit = one transaction, the scope stream = blocks. The memory layout and the wire format are the same data model — they are not allowed to drift apart.

User-code API draft:

```c
// The user does not declare monitored variables; the platform auto-registers
// the available parameter/observable table.
// in/out are generic named-port structs (typed physical-quantity ports);
// a motor demo is one concrete port mapping over them.

// The only user-owned slot, called by the L1 executor every tick
void user_step(const plat_in_t *in, plat_out_t *out);
```

### Cross-repo interface management (firmware ↔ host)

- **No struct memcpy on the wire** (16-bit char + endianness); each side writes its own explicit serializer;
- The single source of truth = the wire-spec document + **golden test vectors** (hex frame samples): the firmware serializer compiles and runs unit tests on the PC, the Rust parser runs conformance tests, both ends against the same set of vectors;
- The descriptor table is enumerated at runtime → the two repos need no codegen, and are naturally decoupled.

### Architecture diagram

```
            ┌────────────────────────────────────────────────┐
            │  Scope2000 = Rust + egui front-end             │
            │  DataSource: V2k (native) / Sim (long-term)    │
            └────┬─────────────────────┬────────────────┬────┘
                 │ JTAG/CCS            │ SCI (XDS110    │ EtherCAT
                 │ (Phase 1–)          │  VCP, Ph 3.5)  │ (ethercrab, Ph 6)
   ┌─────────────┴─────────────────┐  ┌┴────────────────┴─────────────┐
   │ CPU1 — control core           │  │ CPU2 — comms core             │
   │  L3 user control app          │  │  EtherCAT data pump (Phase 6) │
   │  L2 control math              │  │  SCI dumb-pump (Phase 3.5)    │
   │     (user: SDK / Simulink)    │  │  param service / heartbeat    │
   │  L1 executor: ISR sched+prot  │  │  / firmware update            │
   │  L0 ePWM ADC eQEP CMPSS       │  │  L0: SCI / MCAN / ESC(PDI)    │
   └───────┬───────────────────────┘  └──────────────────────┬────────┘
           │   shared-memory interfaces (GSx RAM + MSGRAM)   │
           │  ┌──────────────────────────────────────────┐   │
           └──┤ 1. descriptor table (param/ch/build hash)├───┘
              │ 2. param plane: double-buf + commit      │
              │ 3. scope plane: SPSC ring + dual-mode    │
              │ 4. command/status: IPC mailbox + hb      │
              └──────────────────────────────────────────┘
   Hardware protection chain (CMPSS → ePWM X-BAR → Trip Zone): goes through
   no CPU at all, let alone across cores
```

## Basic rules (all code must obey)

1. **The control core never blocks waiting on the comms core, on any path.** IPC full → drop, scope buffer full → overwrite or stop sampling, link drops a block → drop the block; the control ISR keeps running. CPU2 dies → the motor keeps running stably, just "out of contact"; CPU1 dies → PWM is shut off by hardware trip and each core's independent watchdog, not by CPU2.
2. **Protection is a pure-hardware chain**: CMPSS → ePWM X-BAR → Trip Zone shuts off PWM, going through no CPU. **Protection must be in place before power is applied.**
3. **The platform always runs dual-core**: Viewer2000 is permanently targeted at the F28P65x dual-C28x architecture. The CPU1/CPU2 division of labor is a platform boundary; there is no single-core build fallback. Debugging problems are solved by narrowing functionality, disabling peripheral consumers, or swapping the host data source — not by moving comms-core logic back onto CPU1.
4. **L2/L3 touch no registers**: the platform exposes a **generic named-port table** to user code through `user_step` — named, typed physical-quantity input ports plus output ports the user writes back; count↔physical-quantity conversion is done by the platform in L0/L1. The FOC demo is one concrete port mapping (phase currents [A] / angle [rad] / bus voltage [V] → three-phase duty). This port table is the boundary the platform exposes to user code.
5. **Control-time ownership belongs to CPU1**: the PWM time base, ISR tick, sample timestamps, and block time are published by CPU1; CPU2 may have a local diagnostic heartbeat/timeout, but must not write its local time into control scheduling or scope timestamps.
6. **No RTOS**: a classic bare-metal foreground/background architecture — control in the ISR, state machine / chores in the background loop.
7. **Observability in place from day 0**, no "add tooling once you hit a problem you can't inspect" (the predecessor project's biggest lesson).

## C2000-specific pitfalls

- **Configure EPWM TBCTL.FREE_SOFT on day one**: it decides PWM behavior when the debugger halts. Under the default config, hitting a breakpoint may keep PWM driving → blown transistors while the motor is energized. The debugger is the primary tool, so this has the highest priority.
- **C28x char is 16-bit**: any byte-packed code (memcpy byte streams, packed structs, byte buffers) has a trap. The wire protocol confronts this head-on with **explicit serializers + golden vectors**; no byte-wise memcpy on the wire.
- **CCS real-time mode writes multi-field parameters non-atomically** → must go through the parameter double-buffer commit (see v2k_param.h).
- **The debugger is not an e-stop**: the XDS110 + USB link can hang; an e-stop trusts hardware trip only.
- TODO, pending TRM verification: the actual size of ESC process-data RAM and the SyncManager configuration ceiling (decides the block/PDO size ceiling); the mapping of ePWM/eQEP resources to LaunchPad pins; flash bank partitioning and where the CPU2 image is stored.

## Roadmap

- **Phase 0 — Interface + protocol freeze**: the memory map + the data structures of the four shared-memory interfaces (fields, memory layout, indexing protocol) written as header files; **the wire spec (block frame format, enumeration/transaction protocol) + the first cut of golden test vectors are frozen in lockstep**. This is the interface baseline for all code that follows.
- **Phase 1 — Dual-core skeleton**: two CCS projects, two linker .cmd files, GSx RAM ownership assignment, CPU1 boots CPU2, IPC ping-pong, shared-RAM handshake, a CCS dual-core debug session. Done = each core blinks its own LED + handshake succeeds. (Mostly toolchain grunt work, but it decides the memory map, so it must be done early.)
- **Phase 2 — Time-base proof + protection**: the EPWM → ADC SOC → EOC ISR chain is up, GPIO toggle + scope-measured interrupt latency and jitter; configure FREE_SOFT; CMPSS hardware trip + fault-latch state machine.
- **Phase 3 — Executor + observability**: the ISR multi-rate scheduling framework (software division + **phase staggering** — slow loops don't all bunch onto the same `k%N==0` tick, flattening WCET; whether slow loops run inline or are handed to a low-priority soft interrupt is decided here), the dual-mode RAM scope (snapshot first, consumed by CCS Graph), the parameter double-buffer, the descriptor table.
- **Phase 3.5 — SCI dumb data pump**: CPU2 runs a minimal protocol subset over the XDS110 VCP (enumerate the descriptor table + Live small-N blocks + snapshot drain). **Significance: the first real consumer of the descriptor table, scope plane, and command plane** — CCS Graph reads memory directly over JTAG, bypassing CPU2 / the SPSC consumer side / IPC, so it doesn't count; the biggest architectural risk point, the dual-core split, is validated early here rather than left to Phase 6. Scope2000 lands the first cut of `V2kSource` in parallel; the compatibility bridge only reserves the transport/capability boundary.
- **Phase 4 — User-interface boundary (L1↔user)** ([plan](docs/phase4-user-interface.md)): firmware boundary work — the user surface becomes Arduino-style **`setup()` / `control()`** (`void`) over a global `v2k_io.in/.out`; the port table + count↔physical + all driverlib move behind the chip/board **`wire`↔runtime compile-time seam** (`wire_acquire/apply/cycle_count/...`, zero ISR cost); the physical package becomes `runtime/`, `wire/`, and `app/`; remove the boot default binding. The first real client is one C2000Ware DCL block, loopback-fed, with no motor or platform-owned control math. Phase 4 proves the safe reset lifecycle on the demo.
- **Phase 4.1 — User-code ownership and reset boundary** ([plan](docs/phase4.1-user-code-boundary.md)): turn the Phase 4 reset prototype into a general build/linker contract. All writable state owned by plain-C user objects is collected automatically, restored from linker-owned RAM/FLASH golden images on every START, and audited post-link; user pragmas and the fixed RAM snapshot disappear; overflow or escaped state fails the build. This phase defines the single user-object scope later consumed by Phase 4.5.
- **Phase 4.5 — Build-time symbol baking** ([plan](docs/phase4.5-symbol-baking.md)): a build tool reads the firmware `.out` DWARF and bakes user plain-C variable `name→addr→type` into the descriptor table, so the names travel with the device and Scope2000 shows them by name on any PC with no `.out` present (host unchanged; no registration macro, no mandated declaration style). It consumes Phase 4.1's user-object scope but does not participate in runtime reset correctness.
- **Phase 5 — Motor bring-up (platform acceptance)**: open-loop V/f → current-sense calibration → current loop → encoder → speed loop → position loop. Each step is a small L3 application, validating the platform interface design along the way.
- **Phase 6 — EtherCAT link maturity**: SSC port, ESI/EEPROM, state machine to OP, PDO mapping; ethercrab master @ 2 kHz loop. **Acceptance = 100 kHz × 8ch lossless continuous stream (zero sequence loss × long duration) + record-and-replay.**
- **(Long-term) portability option**: the shared interfaces + host are chip-agnostic, and the **L1 control core is chip-agnostic** (it touches no registers — the L0↔L1 seam quarantines driverlib in L0). When swapping chips (F29x / AM26x) later, the rewrite is **L0 + the board substrate** (linker/memory map/dual-core/IPC), not L1. The platform's accumulated value lives in the interface definitions and the L1 core, not the chip.

## Workflow conventions

- **commit**: small steps, each commit corresponding to one actually-verified node; when fixing a bug, write the root cause into the message (a good habit carried over from the predecessor project).
- **tag**: git-tag the nodes that have been hardware-verified.
- **BRINGUP.md**: record what each step verified on real hardware and how (scope-measured values, CCS Graph screenshots, etc.). Verification knowledge must not live only in commit messages.
- Code comments in English going forward; identifiers English; existing Chinese comments are kept and translated when their file is next edited. Docs (`.md`) and commit messages are English.

## Decisions / open questions

- [x] Platform name: **Viewer2000**
- [x] Chip and board: **TMS320F28P650DK9 / LAUNCHXL-F28P65X** (onboard XDS110 + VCP; no on-chip USB)
- [x] Host-link physical layer: **SCI dumb-pump (3.5) → EtherCAT (6)**, decided by elimination from the 100 kHz × 8ch requirement; CAN-FD/W5500 do not meet it
- [x] CLA ownership: one per core; **whether to use it** is still open (keeping control code in plain C preserves the option)
- [x] **L2 ownership: the platform ships no L2 control library; control math is user-supplied in L3** (C2000Ware MOTORCONTROL-SDK / DCL / hand-written / Simulink-generated C). The platform only defines the `setup()`/`control()` boundary. Rationale: duplicating a documented vendor SDK is a maintenance/pedagogical liability, and PC-SIL is low-value for this single-developer + strong on-target-observability context. (Decided 2026-06-19.)
- [x] **User API = Arduino-style `setup()` / `control()` (`void`) over a global `v2k_io.in/.out`** named-port struct (a motor build is one thin view; ports come from the L0 port table). Chosen over `user_step(in,out)` params for the C2000/DCL idiom + non-programmer simplicity; over a motor-hardcoded struct for general-RCP scope + Simulink root-port binding. `loop()` rejected (hides the deterministic-ISR identity); `on_start()` eliminated (the auto-reset replaces it). (Decided 2026-06-19.)
- [x] **Stop/start = unconditional full reset of all user state**: on every IDLE→RUNNING the platform restores user-owned mutable storage to declared initial values before output is enabled — a safety guarantee (a wound-up integrator/PLL can never restart from its FAULT value) that does not depend on the user writing a reset hook. Tuned parameters reset too; each run starts reproducibly from the source-declared values. Phase 4 proved the lifecycle on explicitly sectioned demo state; Phase 4.1 completes automatic object ownership, linker-owned RAM/FLASH golden images, and build-time escape detection. (Decided 2026-06-19.)
- [x] **L0↔L1 portability seam**: the executor (L1 control core) calls a thin compile-time HAL (`l0_acquire/l0_apply/l0_cycle_count/...`); all driverlib + the port table + count↔physical live in L0. Zero ISR cost (direct/inline calls, not a runtime function-pointer HAL — the FreeRTOS-port-layer model). (Decided 2026-06-19.)
- [x] **Observability of user variables = build-time symbol baking (Phase 4.5)**, not host `.out` parsing (would tie Scope2000 to the project + risk a stale/wrong ELF) and not a registration macro (mandated declaration style). The student writes plain C; names are baked into the descriptor table from DWARF at build time and travel with the device; the host is unchanged. (Decided 2026-06-19.)
- [x] **Control-math sourcing**: generic primitives (PID/LPF/ramp) from C2000Ware DCL; PMSM transforms from MOTORCONTROL-SDK; SRM-specific control hand-written (the standard FOC SDK has no SRM equivalent — not wheel-reinvention). The platform ships none; demos/examples reach these via the include path. (Decided 2026-06-19.)
- [ ] ESC process-data RAM size and SM configuration ceiling (TRM verification, decides the block ceiling)
- [ ] The specific tiers for scope channel groups and decimation ratios
- [ ] Flash bank partitioning and where the CPU2 image is stored

# Debug Hint

When using CCS MCP to start dubug, if you want to debug for example "targetConfigs/TMS320F28P650DK9.ccxml",
please request "targetConfigs/TMS320F28P650DK9", MCP will Auto-Complete ".ccxml".
If request "targetConfigs/TMS320F28P650DK9.ccxml", CCS MCP will incorrectly request "targetConfigs/TMS320F28P650DK9.ccxml.ccxml",
and debug will not start.
