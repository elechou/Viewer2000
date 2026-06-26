# Commit & Language Rule

Going forward, write **code comments in English**. Existing Chinese comments are kept as-is and translated opportunistically when a file is next edited. **Commit messages and documentation (`.md`) are English.** (History: the repo was bilingual — Chinese comments/docs, English identifiers — until the 2026-06-19 switch that makes the whole repo referenceable by non-native-Chinese readers.)

# Debug / Flash Hint

Do not use CCS MCP `launchTargetConfiguration` for this repo's project-local
target configurations. In this environment that tool resolves names relative to
`/Users/shou/ti/CCSTargetConfigurations` and appends `.ccxml`, so both
`targetConfigs/TMS320F28P650DK9.ccxml` and absolute project-local paths can be
misresolved. For project-local debug, use CCS GUI/debugProject/DSS instead.

F28P65x Flash programming requires the CPU1 Flash Plugin bank map to be set
before either core is loaded:

- Bank0, Bank1, Bank2 -> CPU1
- Bank3, Bank4 -> CPU2
- click/configure the CPU1 Flash Plugin clock before CPU2-only Flash operations
- use "Necessary Sectors Only (for Program Load)" unless deliberately erasing
  selected banks

Do not program CPU2 while the application is running from Bank3/Bank4. Enter a
programming state with both C28x cores halted, or halt CPU1 before it executes
`Device_bootCPU2()`. The repo Flash tool uses
`tools/ccs/flash_dual_core_f28p65x.sh` on macOS and
`tools/ccs/flash_dual_core_f28p65x.cmd` on Windows. Both launchers
set the bank map explicitly and then load `cpu1/FLASH/cpu1.out` and
`cpu2/FLASH/cpu2.out`. Terminate any CCS GUI debug session first because the
XDS110 probe can have only one owner, then run the platform launcher from the
repo root:

```sh
tools/ccs/flash_dual_core_f28p65x.sh
```

```bat
tools\ccs\flash_dual_core_f28p65x.cmd
```

The tool is program-only: it disables CCS run-to-main and post-load debug
breakpoints and connects only CPU1. During programming it temporarily assigns
Bank0-4 to CPU1, loads both `cpu1.out` and `cpu2.out` through the CPU1 Flash
Plugin with necessary-sectors-only erase, then restores the deployment map
(Bank0-2 -> CPU1, Bank3-4 -> CPU2). CPU2 remains in the Boot-ROM wait state and
is never connected during programming. This avoids the stale CPU2 debug context
that caused XDS110 `-1041`/`-1044` during an earlier two-session DSS attempt. If
an active CCS GUI debug session owns the probe, CPU1 acquisition fails before
any erase/program operation. Application start and cold-boot acceptance still
require a physical power cycle with S3 set to Flash boot.

After that cold boot, do not use an "attach-only" DSS/CCS session as a
non-invasive health probe. Connecting through this target configuration runs
GEL connect hooks that can place CPU2 back into the Boot-ROM wait state after
CPU1 has already executed its one-time `Device_bootCPU2()`. The visible symptom
is a stopped CPU2 LED and a dead SCI service while CPU1 continues ticking. Use
the isolated SCI link for live acceptance. If JTAG attachment is unavoidable,
restore the deployment state with a physical power cycle afterward.

# Build Configuration Policy

From Phase 5.0 onward the only supported Viewer2000 firmware build
configuration is **FLASH** for both CPU1 and CPU2. The old RAM configuration is
deprecated and kept only for historical Phase 1-4 bring-up references. Do not
use RAM builds for powered commissioning, current acceptance evidence, quick
start instructions, capacity fixes, or build-matrix gates. The RAM linker
command files may remain in the tree temporarily so old records and tooling can
be interpreted, but they are not a supported product surface.

# AGENTS.md — Viewer2000 (C2000 RCP platform)

## Project positioning

A rapid control prototyping (RCP) platform built from scratch on the TI C2000 **F28P65x (dual C28x core, 200 MHz)**, targeting general power-electronics rapid control prototyping.

**Core premise: the product of this project is the platform itself, not a motor controller.** Platform = deterministic control-task scheduling + peripheral abstraction + parameter tuning / data scoping + protection. FOC motor control is merely the first example application running on the platform, doubling as the platform acceptance test.

**Performance anchor: 20–100 kHz (TBD) × 8ch × f32 lossless, 0.64–3.2 MB/s.** This number drives the physical-layer choice by elimination (see "Communication architecture").

**Repo boundary: this repo is firmware only** (the part flashed into the F28P65x). The host side is a sibling standalone repo **Scope2000** (Rust + egui); the native transport implementation is `V2kSource`. A future `SimSource` is a long-term/optional path (see "Language strategy") — note the platform no longer ships a portable control-math library, so PC simulation, where wanted, rides Simulink SIL/PIL or the portable subset a given demo happens to use, rather than an FFI binding to a platform-owned control layer. Legacy-device compatibility is handled by a separate bridge process that presents normalized Viewer2000 message semantics to Scope2000 over a generic local byte-stream transport, staying out of the `V2kSource` native hot path.

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

### Layering And Vocabulary

The numbered layers are the internal development map. User-facing C identifiers should still use physical semantics (`duty_a`, `ia_raw`, `wire_as5600_sample_t`) rather than abstract layer names.

| Layer | Package / area | Content | Owner |
|---|---|---|---|
| L3 | `app/` | `setup()` / `control()`, application state, demo orchestration | User (on CPU1) |
| L2 | app-owned control modules | Control math and motor semantics: DCL, C2000Ware MotorControl SDK, hand-written control, Simulink-generated C | User (runs on CPU1) |
| L1 | `runtime/` | ISR executor, state machine, protection policy, parameter/descriptor registry, RAM scope | CPU1 (consumer side may be on CPU2) |
| L0 | `wire/` | Board wiring and device drivers: ePWM/ADC/TZ/CMPSS substrate, DRV8323RS, non-blocking AS5600 service, on C2000Ware driverlib | Platform by default; safe read-only hooks may be user-visible |

**ISR ownership belongs to L1, not the user.** The platform owns the ePWM→ADC SOC→EOC chain and calls the user's `control()` only after the configured control ADC frame is complete. User code may read those completed result registers through documented DriverLib result/status APIs and owns count→physical conversion; it may not retrigger or reconfigure ADC, PWM, interrupts, ownership, or protection. The fixed ISR sequence = `publish platform state/schedule → control → wire_apply → scope_sample_all (walk the descriptor table) → trigger_eval → g_tick++`. Scope sampling and trigger evaluation remain in the platform epilogue, so addressable user static variables discovered by symbol baking have no unsampled path (rule 7). No blocking peripheral transaction is permitted in `control()`.

L0 is not a sealed black box. Motor users need a clear path to the physical world. The rule is narrower: L0 configuration, timing, ownership, output release, and protection remain platform-owned, while selected read-only or cache-only L0 APIs may be public when they express real hardware semantics and do not perform blocking or safety-changing work. `wire_as5600_get_latest()` is public for that reason; `wire_as5600_service()` and DRV8323RS register writes are not.

### Language strategy

**Target-side firmware (both cores, all of L0-L3) is pure C, C99 + stdint fixed-width types, no C++.** Reasons:

- The primary HMI is CCS Expressions/Graph: flat C structs are directly readable; C++ name mangling, templates, and private members are all friction in the map file and watch window (cl2000's C++ support is itself a second-class citizen);
- The CLA compiler accepts only a C subset: keeping control code in plain C preserves the option of later moving the fast loop onto the CLA (one CLA per core). Since L2 control math is user-supplied, this is the user's option to keep — the platform only guarantees its own L1 boundary stays C;
- Shared-memory structs must be fixed-layout PODs readable by all three parties (CPU1 / CPU2 / host); C is the greatest common denominator;
- C2000Ware / MotorControl SDK / DCL are all C, and the platform's target users (beginners) speak C too; ODrive's heavy C++ is part of its learning barrier — not replicated;
- Avoids bare-metal C++ pitfalls like global-object construction order and implicit constructors.

Three coding conventions that make up for the absence of C++ (aligned with TI DCL style): multiple instances = struct instance + operation functions (`pi_update(&pi_vel, err)`); namespaces = module prefixes; no dynamic allocation after init completes.

**The host side is uniformly Rust + egui**: Scope2000 uses a `DataSource` boundary; `V2kSource` serves native SCI/EtherCAT. A `SimSource` is a long-term/optional path — and since the platform no longer ships a portable control-math library, PC simulation does not FFI-bind to a platform-owned control layer; if wanted later it rides Simulink SIL/PIL (which subsumes it) or whatever portable subset a specific demo uses. Legacy devices are adapted to the same message semantics by a separate bridge process; Scope2000 adds no dedicated compatibility data source.

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
// User static variables are discovered and registered at build time.
// The platform has already completed the configured ADC frame here.
uint16_t ia_raw;
float ia_A;

void setup(void);
void control(void)
{
    ia_raw = v2k_io.adc.ia_raw;
    ia_A = user_current_convert(ia_raw);
    v2k_pwm_apply(user_control_step_a(ia_A),
                  user_control_step_b(ia_A),
                  user_control_step_c(ia_A));
}
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
   │  L2 control math/modules      │  │  SCI dumb-pump (Phase 3.5)    │
   │     (user: SDK / Simulink)    │  │  param service / heartbeat    │
   │  L1 runtime: ISR sched+prot   │  │  / firmware update            │
   │  L0 wire: ePWM ADC TZ CMPSS   │  │  wire: SCI / MCAN / ESC(PDI)  │
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
4. **L2/L3 may read, but may not configure, platform peripherals**: user `control()` may call documented, non-blocking DriverLib result/status APIs such as `ADC_readResult()` after the platform-owned EOC boundary. Users own count↔physical conversion and application sensor semantics. Peripheral clocks, pinmux, ownership, ADC SOC/interrupt timing, ePWM synchronization/output, X-BAR, CMPSS, TZ, and fault clearing remain platform-owned. Slow or transactional devices without a vendor device driver, such as AS5600 over I2C, use a project non-blocking driver; user control code reads only its cached sample through `wire/wire_as5600.h`.
5. **Control-time ownership belongs to CPU1**: the PWM time base, ISR tick, sample timestamps, and block time are published by CPU1; CPU2 may have a local diagnostic heartbeat/timeout, but must not write its local time into control scheduling or scope timestamps.
6. **No RTOS**: a classic bare-metal foreground/background architecture — control in the ISR, state machine / chores in the background loop.
7. **Observability in place from day 0**, no "add tooling once you hit a problem you can't inspect" (the predecessor project's biggest lesson).

## C2000-specific pitfalls

- **Configure EPWM TBCTL.FREE_SOFT on day one**: it decides PWM behavior when the debugger halts. Under the default config, hitting a breakpoint may keep PWM driving → blown transistors while the motor is energized. The debugger is the primary tool, so this has the highest priority.
- **C28x char is 16-bit**: any byte-packed code (memcpy byte streams, packed structs, byte buffers) has a trap. The wire protocol confronts this head-on with **explicit serializers + golden vectors**; no byte-wise memcpy on the wire.
- **CCS real-time mode writes multi-field parameters non-atomically** → must go through the parameter double-buffer commit (see v2k_param.h).
- **The debugger is not an e-stop**: the XDS110 + USB link can hang; an e-stop trusts hardware trip only.
- TODO, pending TRM verification: the actual size of ESC process-data RAM and the SyncManager configuration ceiling (decides the block/PDO size ceiling); the mapping of ePWM/eQEP resources to LaunchPad pins.

## Roadmap

- **Phase 0 — Interface + protocol freeze**: the memory map + the data structures of the four shared-memory interfaces (fields, memory layout, indexing protocol) written as header files; **the wire spec (block frame format, enumeration/transaction protocol) + the first cut of golden test vectors are frozen in lockstep**. This is the interface baseline for all code that follows.
- **Phase 1 — Dual-core skeleton**: two CCS projects, two linker .cmd files, GSx RAM ownership assignment, CPU1 boots CPU2, IPC ping-pong, shared-RAM handshake, a CCS dual-core debug session. Done = each core blinks its own LED + handshake succeeds. (Mostly toolchain grunt work, but it decides the memory map, so it must be done early.)
- **Phase 2 — Time-base proof + protection**: the EPWM → ADC SOC → EOC ISR chain is up, GPIO toggle + scope-measured interrupt latency and jitter; configure FREE_SOFT; CMPSS hardware trip + fault-latch state machine.
- **Phase 3 — Executor + observability**: the ISR multi-rate scheduling framework (software division + **phase staggering** — slow loops don't all bunch onto the same `k%N==0` tick, flattening WCET; whether slow loops run inline or are handed to a low-priority soft interrupt is decided here), the dual-mode RAM scope (snapshot first, consumed by CCS Graph), the parameter double-buffer, the descriptor table.
- **Phase 3.5 — SCI dumb data pump**: CPU2 runs a minimal protocol subset over the XDS110 VCP (enumerate the descriptor table + Live small-N blocks + snapshot drain). **Significance: the first real consumer of the descriptor table, scope plane, and command plane** — CCS Graph reads memory directly over JTAG, bypassing CPU2 / the SPSC consumer side / IPC, so it doesn't count; the biggest architectural risk point, the dual-core split, is validated early here rather than left to Phase 6. Scope2000 lands the first cut of `V2kSource` in parallel; the compatibility bridge only reserves the transport/capability boundary.
- **Phase 4 — User-interface boundary (L1↔L3)** ([plan](docs/phase4-user-interface.md)): firmware boundary work — the user surface becomes Arduino-style **`setup()` / `control()`** (`void`) over a global `v2k_io`; the physical package becomes `runtime/`, `wire/`, and `app/`; remove the boot default binding. Its single-channel `wire_acquire` + platform count→physical path was a deliberate demo boundary and was superseded for motor acquisition by Phase 5.0 and the 2026-06-24 grouped interface (`v2k_io.sys`, `v2k_io.adc`, explicit `v2k_pwm_apply()`). Phase 4's lasting contracts are ISR ownership, safe output submission, reset lifecycle, and package separation.
- **Phase 4.1 — User-code ownership and reset boundary** ([plan](docs/phase4.1-user-code-boundary.md)): turn the Phase 4 reset prototype into a general build/linker contract. All writable state owned by plain-C user objects is collected automatically, restored from linker-owned RAM/FLASH golden images on every START, and audited post-link; user pragmas and the fixed RAM snapshot disappear; overflow or escaped state fails the build. This phase defines the single user-object scope later consumed by Phase 4.5.
- **Phase 4.5 — Build-time symbol baking** ([plan](docs/phase4.5-symbol-baking.md)): a build tool reads the firmware `.out` DWARF and bakes user plain-C variable `name→addr→type` into the descriptor table, so the names travel with the device and Scope2000 shows them by name on any PC with no `.out` present (no host DWARF parser, registration macro, or mandated declaration style). Baked entries carry the USER kind bit so Scope2000 can separate them from platform/system diagnostics. It consumes Phase 4.1's user-object scope but does not participate in runtime reset correctness.
- **Phase 4.6 — Runtime load observability** ([plan](docs/phase4.6-runtime-load-observability.md)): aggregate one-second CPU1 ISR load windows with coherent peak-tick ADC/Control/Scope/Runtime breakdown, expose the values as system Variables and in STATUS diagnostics, and render the control-cycle budget in Scope2000. The current firmware acceptance baseline is 20 kHz; 100 kHz is deferred until the platform hot path is optimized. GPIO timing remains the authority for profiler overhead.
- **Phase 5.0 — Power-stage hardware interface** ([plan](docs/phase5.0-powerstage-interface.md), accepted 2026-06-22): the three-phase PWM/ADC/protection substrate, DRV8323RS, and non-blocking AS5600 driver passed every verification executable with the available bench equipment. The production interface contains no bench-only diagnostic commands. Its closure baseline was DRY_RUN; Phase 5.2 deliberately advances the tracked firmware to a POWERED but neutral-only commissioning application. No sustained V/f or FOC runs belong to Phase 5.0. The cross-cutting power-stage protection model (trip-zone funnel, the three hardware trip chains, DCAEVT1's dual identity, fault discrimination, and the POWERED gates) is consolidated in [protection-architecture.md](docs/protection-architecture.md); read it before touching any trip-zone/CMPSS/X-BAR/DRV code or moving from DRY_RUN to POWERED.
- **Phase 5.2 — POWERED neutral commissioning** ([plan](docs/phase5.2-minimum-powered-commissioning.md)): the active baseline selects POWERED with approval enabled but hard-locks user output to the three-phase neutral vector. With the currently available supply, multimeter, and logic analyzer, acceptance is one motor-disconnected START/STOP, one motor-connected neutral offset/noise capture, and one functional powered nFAULT lifecycle. Oscilloscope-only gate/switch-node timing and calibrated-current shutdown are explicitly deferred until before higher-energy, sustained, loaded, or closed-loop operation; they do not block the first tightly limited Phase 5.5 rotation.
- **Phase 5.5 — Powered user application and first rotation** ([plan](docs/phase5.5-powered-user-application.md)): replace the neutral smoke client with app-owned current conversion, readiness/interlock state, explicit enable, bounded open-loop V/f, and the first short low-energy motor rotation. APP_START remains neutral; FOC and sustained/load operation remain later work.
- **Phase 5 family — Motor bring-up (platform acceptance)**: Phase 5.0 establishes the substrate, Phase 5.2 authorizes low-energy powered motion, and Phase 5.5 proves the first user V/f application. Continue with current loop → encoder commutation → speed loop → position loop as small user applications that validate the platform boundary without moving control math into the platform.
- **Phase 6 — EtherCAT link maturity**: SSC port, ESI/EEPROM, state machine to OP, PDO mapping; ethercrab master @ 2 kHz loop. **Acceptance = 100 kHz × 8ch lossless continuous stream (zero sequence loss × long duration) + record-and-replay.**
- **(Long-term) portability option**: the shared interfaces, host, and **L1 runtime remain chip-agnostic**; L1 touches no registers. The platform port is L0 `wire/` plus board support (linker/memory map/dual-core/IPC). A user application that directly reads DriverLib ADC results is intentionally board-bound and must adapt its read sites when moved; applications that require source portability may add their own thin acquisition adapter without imposing that abstraction on every user.

## Workflow conventions

- **commit**: small steps, each commit corresponding to one actually-verified node; when fixing a bug, write the root cause into the message (a good habit carried over from the predecessor project).
- **tag**: git-tag the nodes that have been hardware-verified.
- **BRINGUP.md**: record what each step verified on real hardware and how (scope-measured values, CCS Graph screenshots, etc.). Verification knowledge must not live only in commit messages.
- **serial handoff**: after any serial-console debugging session, proactively disconnect the serial port and hand it back to the user. If the target VCP is busy, first disconnect any existing MCP serial connection; if the OS port is still held by another process, identify and kill that stale serial owner before retrying.
- Code comments in English going forward; identifiers English; existing Chinese comments are kept and translated when their file is next edited. Docs (`.md`) and commit messages are English.

## Decisions / open questions

- [x] Platform name: **Viewer2000**
- [x] Chip and board: **TMS320F28P650DK9 / LAUNCHXL-F28P65X** (onboard XDS110 + VCP; no on-chip USB)
- [x] Host-link physical layer: **SCI dumb-pump (3.5) → EtherCAT (6)**, decided by elimination from the 100 kHz × 8ch requirement; CAN-FD/W5500 do not meet it
- [x] CLA ownership: one per core; **whether to use it** is still open (keeping control code in plain C preserves the option)
- [x] **L2 Control-code ownership: the platform ships no control-math library; control math is user-supplied inside app-owned modules** (C2000Ware MOTORCONTROL-SDK / DCL / hand-written / Simulink-generated C). The platform only defines the `setup()`/`control()` boundary plus safe output and cache APIs. Rationale: duplicating a documented vendor SDK is a maintenance/pedagogical liability, and PC-SIL is low-value for this single-developer + strong on-target-observability context. (Revised 2026-06-21.)
- [x] **User API = Arduino-style `setup()` / `control()` (`void`) plus grouped `v2k_io` and explicit PWM submission**. `v2k_io.sys` carries platform state/schedule, `v2k_io.adc` carries the completed semantic raw ADC frame, and `v2k_pwm_apply(a,b,c)` submits the recommended duty command path. Advanced users may directly call native TI read/write APIs; runtime does not implicitly apply PWM after `control()` returns. `loop()` remains rejected because it hides deterministic ISR identity; `on_start()` remains unnecessary because automatic reset precedes `setup()`. (Revised 2026-06-24.)
- [x] **Stop/start = unconditional full reset of all user state**: on every IDLE→RUNNING the platform restores user-owned mutable storage to declared initial values before output is enabled — a safety guarantee (a wound-up integrator/PLL can never restart from its FAULT value) that does not depend on the user writing a reset hook. Tuned parameters reset too; each run starts reproducibly from the source-declared values. Phase 4 proved the lifecycle on explicitly sectioned demo state; Phase 4.1 completes automatic object ownership, linker-owned RAM/FLASH golden images, and build-time escape detection. (Decided 2026-06-19.)
- [x] **L0↔L1 portability seam**: L1 (v2k runtime) calls a small compile-time substrate for ADC frame acquisition, explicit PWM command application, timing, interrupt acknowledgement, foreground device service, and protection. It keeps the recommended app path readable without hiding native TI APIs. Transactional devices without a suitable vendor driver may still have project drivers, but their control-facing API is cache-only and non-blocking. Safe read-only L0 APIs may be public when they preserve physical semantics and do not change timing/safety configuration. (Revised 2026-06-24.)
- [x] **ADC acquisition boundary**: the platform owns pinmux, SOC trigger schedule, EOC validity, and protection; the default app reads semantic raw counts from `v2k_io.adc` and owns physical conversion. Advanced users may directly read platform-configured, completed ADC results through DriverLib. At the current 20 kHz motor baseline all seven configured motor analog SOCs form one ePWM-triggered frame. A slow control loop consumes this frame less often; it does not manually trigger a "slow ADC." (Revised 2026-06-24.)
- [x] **Observability of user variables = build-time symbol baking (Phase 4.5)**, not host `.out` parsing (would tie Scope2000 to the project + risk a stale/wrong ELF) and not a registration macro (mandated declaration style). The student writes plain C; names are baked into the descriptor table from DWARF at build time and travel with the device; the host is unchanged. (Decided 2026-06-19.)
- [x] **Control-math sourcing**: generic primitives (PID/LPF/ramp) from C2000Ware DCL; PMSM transforms from MOTORCONTROL-SDK; SRM-specific control hand-written (the standard FOC SDK has no SRM equivalent — not wheel-reinvention). The platform ships none; demos/examples reach these via the include path. (Decided 2026-06-19.)
- [x] **Current control-rate acceptance baseline = 20 kHz**. The already-recorded 100 kHz experiments remain useful evidence, and the communication architecture retains 100 kHz transport headroom, but 100 kHz is not a Phase 4.x acceptance gate. It may be reconsidered after profiling-driven platform optimization and/or CLA partitioning. (Decided 2026-06-20.)
- [x] **Flash partition = CPU1 Banks 0-2, CPU2 Banks 3-4; CPU2 image entry = Bank 3 Sector 0.** CPU1 assigns all five bank owners before releasing CPU2. Bank 2 is currently reserved headroom in the CPU1 allocation. (Decided 2026-06-20.)
- [ ] ESC process-data RAM size and SM configuration ceiling (TRM verification, decides the block ceiling)
- [ ] The specific tiers for scope channel groups and decimation ratios
