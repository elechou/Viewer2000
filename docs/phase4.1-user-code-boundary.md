# Phase 4.1 - User-code ownership and reset boundary: development plan

> **Document status**: implemented and accepted at the 20 kHz baseline. Phase 4
> proved the `setup()` / `control()` lifecycle with a manually sectioned demo;
> Phase 4.1 turns that prototype into a general build and linker contract for
> arbitrary plain-C user applications. Results are in
> [BRINGUP.md](../BRINGUP.md).
>
> **Position in the roadmap**: Phase 4.1 must complete before Phase 4 is treated
> as production-ready for motor bring-up. [Phase 4.5](phase4.5-symbol-baking.md)
> consumes the same user-code ownership definition for observability, but reset
> correctness does not depend on DWARF, descriptor capacity, or symbol baking.
>
> **Current status, 2026-06-21**: CPU1/CPU2 RAM and FLASH builds, linker/map
> audits, standalone dual-core cold boot, Flash golden-to-RUN restore, controlled
> Flash CRC corruption and recovery, 100-cycle lifecycle endurance, and the
> Scope2000 regression all pass. The current acceptance baseline is 20 kHz;
> further 100 kHz work is deferred until the platform hot path is optimized.

## Goal

Establish a first-class boundary between user-owned code/state and the
Viewer2000 platform:

- the user writes ordinary globals and function-static variables with no
  section pragmas;
- all mutable state owned by the user application is automatically placed in
  resettable sections;
- every START restores that state to its source-declared initial values before
  `setup()`, application execution, or output release;
- runtime, wire, SysConfig, and platform-tool state are never reset by this
  operation;
- section capacity and ownership mistakes fail the build instead of becoming
  delayed runtime surprises;
- Phase 4.5 receives the exact same definition of "user code", so reset and
  observability cannot silently disagree.

This is a **code-ownership and memory-layout feature**, not merely a faster
implementation of `memcpy`.

The safety value is broader than declaration-time initialization. State such
as an integrator, a function-static filter history, or hidden writable state in
a user-owned library changes while `control()` runs and otherwise survives a
STOP or FAULT in RAM. `setup()` runs only after the platform restore; it may
derive configuration or initialize external resources, but it is not required
to know and reset every piece of writable user state.

## Current baseline and gaps

Phase 4 already has the correct lifecycle order and has passed an initial
RAM/20 kHz hardware test, but the current implementation is only a prototype:

| Area | Current baseline | Phase 4.1 requirement |
|---|---|---|
| user-state selection | every demo variable has a manual `#pragma DATA_SECTION` | select all mutable state by user object ownership; no declaration annotations |
| initialized-state golden source | runtime copies into a fixed 256-word RAM snapshot at boot | linker-defined immutable/reserved LOAD image; no fixed software cap |
| FLASH behavior | same boot snapshot mechanism as RAM | restore directly from the FLASH LOAD image |
| RAM behavior | snapshot competes with ordinary runtime RAM | dedicated linker-owned RAM golden region loaded by CCS |
| capacity failure | initialized data may link and then make START fail | static overflow fails at link time |
| coverage audit | a missed pragma silently escapes reset | post-link boundary verifier rejects escaped mutable user state |
| Phase 4.5 scope | independently described object-file rule | one shared user-object manifest/derivation |
| golden integrity | no independent reference | linker-generated build-time CRC, checked before and after each restore |
| large lookup tables | no explicit rule | ordinary `const` objects are never reset; FLASH is the normal large-table build |

The existing fail-closed START behavior remains a useful final guard, but it is
not a substitute for a complete build-time boundary.

## Decisions

### 1. Ownership is defined by build objects, not variable annotations

The primary user domain is every translation unit built from `cpu1/app/`.
Runtime, wire, SysConfig-generated files, startup code, contracts, and tools are
platform-owned.

The canonical ownership specification is
`cpu1/tools/user_boundary.json`. Its default source root is `cpu1/app/`; it may
also name explicit extra objects or archive members for advanced applications.
A build tool generates both the TI linker selector fragment and a normalized
manifest for post-link tools. The linker rules in this phase, the boundary
verifier, and the DWARF baker in Phase 4.5 consume those generated artifacts.
There must not be a second independently maintained glob or allowlist.

The initial supported forms are:

- user sources under `cpu1/app/`;
- additional user/control-library objects explicitly added to the user-object
  set when they own mutable application state.

An external library is not implicitly user-owned merely because `control()`
calls it. This intentionally prevents vendor/runtime library globals from
exploding the user-variable set and from being reset accidentally. Header-only
or inline DCL code needs no special handling when it only operates on user-owned
instances. A header that defines mutable `static` state is different: those
definitions compile into the including `cpu1/app/` object and are user state
under normal C rules. A library with hidden mutable state must be included in
the user domain by explicit object/archive-member ownership, proven read-only
or instance-supplied, or rejected for this lifecycle contract.

### 2. Mutable user state is reset; code and constants are only classified

The linker classifies the complete user domain:

| Class | Examples | Lifecycle |
|---|---|---|
| user text | `setup`, `control`, helper functions | executable; not copied on START |
| user const | lookup tables declared `const` | immutable; not copied on START |
| user data | initialized globals/statics | copied from the golden LOAD image on START |
| user BSS | zero-initialized globals/statics | cleared on START |

Classifying text and constants makes ownership visible in the map file and
gives Phase 4.5 a stable scope, even though only mutable sections participate
in reset.

This classification follows normal C semantics and never infers lifecycle from
object size. A `const` lookup table is immutable and therefore needs no reset.
A writable table is mutable application state and must reset, regardless of
size. Casting away `const` and writing the object is undefined behavior, as in
ordinary C.

Large tables should have one definition in a `.c` file and an `extern const`
declaration in a header. Defining a `static const` table in a header creates one
copy per translation unit under normal C rules; the platform does not hide that
cost.

User-owned mutable `NOINIT`, persistent, shared-memory, CLA, or absolute-address
state is not supported in the initial contract. The boundary verifier rejects
it unless a later platform API explicitly defines its lifecycle.

### 3. The linker owns capacity and the golden source

Reserve explicit memory regions:

- `USER_RUN`: the combined run capacity for user data and user BSS;
- `USER_CONST_RAM`: immutable user objects in the constrained RAM build;
- `USER_GOLDEN_RAM`: the RAM-build LOAD image for initialized user data;
- a FLASH LOAD allocation for initialized user data in FLASH builds.

The initial target allocation is:

- `RAMLS6` (`0x800` words) as `USER_RUN`;
- `RAMLS7` (`0x800` words) as `USER_CONST_RAM`;
- the first `0x800` words of CPU1-owned `RAMD5` as `USER_GOLDEN_RAM`;
- an aligned allocation in the CPU1 FLASH image as the FLASH golden source.

The RAM golden image is deliberately non-adjacent to `USER_RUN`. `RAMGS4` is
not available capacity: CPU1 assigns it to CPU2 before booting the comms core.
The names, not physical bank numbers, are the runtime contract, so a later
build may move or enlarge these regions over a larger contiguous CPU1 range
without changing user code or the reset API.

Ordinary platform `.const`, `.data`, and `.bss` must not spill into any user
region. User text and constants are separately classified even though they do
not participate in reset.

Required link-time constraints:

```text
sizeof(user_data) + sizeof(user_bss) <= sizeof(USER_RUN)
sizeof(user_data) <= sizeof(USER_GOLDEN_RAM)    # RAM build
sizeof(user_const) <= sizeof(USER_CONST_RAM)    # RAM build
FLASH LOAD allocation must fit its flash region # FLASH build
LOAD_SIZE(user_data) == RUN_SIZE(user_data)
```

The cost model is intentional:

| User object | RAM build | FLASH build | START action |
|---|---|---|---|
| initialized writable | RUN RAM + golden RAM | RUN RAM + golden FLASH | copy |
| zero-initialized writable | RUN RAM | RUN RAM | clear |
| `const` | `USER_CONST_RAM` | FLASH | none |

FLASH is the normal deployment configuration. A large `const` table that does
not fit the limited RAM bring-up configuration fails at link time with guidance
to use FLASH; it does not require a pragma or a Viewer2000-specific declaration.

The linker emits LOAD/RUN start, end, and size symbols. Runtime code uses only
those symbols; there is no `V2K_USER_DATA_SNAPSHOT_WORDS` constant and no
runtime-owned snapshot array.

### 4. START reset remains section-based and independent of DWARF

User initialization has one owner. Initialized user data has a direct,
uncompressed LOAD image and an equal-size RUN image. User BSS is platform-
cleared. Neither class may appear in `.cinit`; the normal C runtime continues
to initialize platform-owned `.data` and `.bss`.

The linker attaches `crc_table(V2K_UserDataCrcTable, algorithm =
CRC32_PRIME)` to the golden image. The generated record is the independent,
build-time expected CRC for both RAM and FLASH configurations. Runtime validates
the table fields and uses the same byte ordering and polynomial as the TI
linker. A zero-length user-data section has no CRC record and no copy.

The runtime sequence is:

```text
remain in IDLE with the hardware OST output lock asserted
disable application execution and publish reset-active
validate linker symbols
validate the golden image against the linker-generated CRC
copy user_data LOAD -> RUN
validate the RUN image against the same CRC
clear user_bss
run optional setup()
re-assert safe initial outputs
return success to the state machine
apply safe output, then release the hardware output lock and enter RUNNING
```

`control()` remains gated to RUNNING. `CLEAR_FAULT` returns only to IDLE; the
next START performs the same reset again.

The reset operates on complete sections, not a list of symbols. It therefore
also covers arrays, structs, function-static variables, unsupported descriptor
types, and variables omitted from Phase 4.5 because the descriptor table is
full.

The boot path performs the same validation, copy, and clear once before user
code is available, making the IDLE image well-defined. Every START repeats it.
Any layout, CRC, or copy failure leaves the application disabled, the safe
output selected, the OST lock asserted, and the state machine in IDLE.

The ISR continues running while the foreground reset executes. Parameter apply
is already suppressed by `reset-active`; scope may observe intermediate or torn
user values during the short reset window. Those samples are diagnostic only
and cannot reach the power output, so Phase 4.1 adds no permanent ISR branch to
suppress them.

### 5. Build-time audit closes silent escape paths

Add a post-link boundary check that fails the build when:

- a mutable symbol defined by a user object is outside user data/BSS;
- a platform object contributes writable storage to a user reset section;
- a user object contributes unsupported writable sections such as `NOINIT`;
- linker-generated LOAD/RUN symbols are missing, reversed, overlapping, or
  outside their declared MEMORY regions;
- LOAD/RUN sizes differ, the linker CRC record is missing or inconsistent, or
  user data/BSS is still represented in `.cinit`;
- RAM and FLASH configurations derive different user ownership rules.

The generator classifies allocatable object sections using the TI toolchain's
`ofd2000 --xml` output. The post-link verifier consumes the normalized manifest,
linker XML information, map, and final ELF. Both tools use only Python's
standard library; DWARF and Phase 4.5 are not correctness dependencies.

## Implementation plan

### 1. Freeze the user-object contract

- Define `cpu1/app/` as the default user source root.
- Add `cpu1/tools/user_boundary.json` as the canonical ownership specification.
- Generate a TI linker selector fragment plus a normalized object manifest in
  the active build-output directory.
- Document how a multi-file application and a stateful vendor archive member
  join that manifest.
- Make Phase 4.5 import this same manifest instead of maintaining its own
  `user.obj` / `examples/*.obj` rule.

### 2. Prove the TI linker selection syntax

Before changing the production scripts, build a small linker spike against the
actual cl2000 EABI output and map file. Confirm collection of:

- `.data` and symbol subsections from initialized globals/statics;
- `.bss`, `.bss:output`, COMMON, and symbol subsections from zero-initialized
  globals/statics;
- function-static variables;
- multiple objects under `cpu1/app/`;
- relevant archive members if explicitly user-owned.

The spike must also confirm C28x word-address units for linker size symbols,
direct LOAD/RUN images, linker CRC byte ordering, and absence of user `.cinit`
records.
Record the exact accepted syntax in the implementation commit and remove the
spike afterward.

### 3. Implement RAM and FLASH linker layouts

- Create named MEMORY regions for `USER_RUN`, `USER_CONST_RAM`, and the RAM
  golden image.
- Collect user text/const/data/BSS by object ownership.
- Give initialized user data separate LOAD and RUN addresses.
- Export the linker symbols consumed by `v2k_user_runtime.c`.
- Remove `.const` and platform writable-section fallback into user regions.
- Place the linker-generated `.TI.crctab` as platform-owned read-only data.
- Add linker assertions or deliberately bounded MEMORY placement so every
  static overflow is a linker error.

RAM and FLASH scripts must expose the same runtime symbol contract even though
their LOAD regions differ.

### 4. Replace the runtime snapshot prototype

- Remove the fixed 256-word snapshot array and snapshot-ready state.
- Validate the linker CRC table and golden image at boot and before each START.
- Copy from the linker-provided LOAD address and CRC-check RUN after each copy.
- Clear the complete user BSS range.
- Keep address/order validation and fail closed if the linker contract is
  invalid.
- Preserve the existing reset counter/error diagnostics.

The platform makes no claim against arbitrary C undefined behavior or a wild
pointer that corrupts both a golden image and its independent CRC record. The
guarantee is that all correctly classified user static storage is restored
under normal memory integrity, with accidental golden drift detected.

### 5. Remove user declaration annotations

- Delete all `#pragma DATA_SECTION(..., "v2k_user_*")` directives from the demo.
- Add ordinary initialized and zero-initialized globals, arrays/struct members,
  a function-static test variable, and a `const` lookup table.
- Split at least one test variable into a second application translation unit
  to prove the boundary is not tied to one `user.obj`.

The final demo must look like normal plain C.

### 6. Add the boundary verifier

- Inspect the final CPU1 `.out` and map for both RAM and FLASH configurations.
- Emit concise diagnostics naming the escaped symbol, owning object, actual
  section, and expected section.
- Integrate generation and verification through tracked `makefile.init` and
  `makefile.targets` hooks that CCS generated makefiles already include. Do not
  edit `.cproject`, and run formal builds only through CCS `buildProject`.
- Add negative fixtures for escaped user state, platform contamination, and
  section overflow.

### 7. Hand the ownership contract to Phase 4.5

- Make the symbol baker consume the Phase 4.1 user-object set.
- Add a Phase 4.5 validation that every baked mutable variable lies inside
  user data/BSS.
- Do not make reset depend on whether a variable is bakeable, visible, or
  registered.

## Verification

| # | Verification | Method | Pass criterion |
|---|---|---|---|
| A | plain-C initialized state | declare globals/statics with no pragmas; mutate in IDLE; START | all return to declared initial values before first `control()` |
| B | plain-C zero state | mutate BSS globals, arrays, structs, and a function-static | all return to zero |
| C | multi-file ownership | place state in two `cpu1/app/` translation units | both objects are classified and reset |
| D | platform isolation | observe runtime tick, reset counters, wire diagnostics across START | platform state is not reset or placed in user sections |
| E | repeated lifecycle | START/STOP and FAULT/CLEAR/START for at least 100 cycles | identical initialized state every run; no golden-image drift |
| F | RAM golden source | inspect map and mutate RUN storage repeatedly | LOAD image is in `USER_GOLDEN_RAM`, remains unchanged, and restores RUN |
| G | FLASH golden source | inspect map, flash, mutate RUN storage, restart | RUN restores directly from the FLASH LOAD image |
| H | golden CRC failure | alter RAM golden; force a controlled FLASH expected/actual mismatch | START is rejected, diagnostics identify CRC failure, and OST stays locked |
| I | capacity failure | exceed `USER_RUN`, RAM golden, and RAM const capacity | link fails with a clear placement/assertion error; large const FLASH build still passes |
| J | escaped-state failure | add a deliberately unsupported/misplaced user writable section | boundary verifier fails the build and names it |
| K | user pragma independence | search final user sources and inspect map | no user reset-section pragmas; complete user writable coverage |
| L | single initializer | inspect `.cinit`, linker XML, and CRC table | no user `.cinit`; one valid CRC record when user data is non-empty; LOAD size equals RUN size |
| M | four-config build | CCS `buildProject` for CPU1/CPU2 RAM/FLASH | zero errors; boundary check runs for both CPU1 configs |
| N | Phase 2-4 regression | trip/state lifecycle, scope bind/stream, DCL demo, 20 kHz budget | previous behavior passes; no ISR budget regression |
| O | C CRC vector | host-compile the same `v2k_crc32_prime.c` used by firmware | the 32-word linker-verified user-data image produces `0xD501B381` |

For the hardware reset tests, halt or instrument immediately after section
restore and before the first `control()` tick at least once, so a later control
assignment cannot mask an incomplete reset.

## Acceptance and exit

Phase 4.1 is complete only when:

- ordinary user globals and function statics require no pragmas;
- all user mutable static storage is reset from a linker-owned golden source;
- RAM and FLASH use verified, distinct LOAD mechanisms;
- both builds validate their golden source against a linker-generated CRC;
- the fixed runtime snapshot and its size cap are gone;
- static capacity overflow and ownership escape fail the build;
- runtime/wire/platform state is proven outside the reset boundary;
- Phase 4.5 consumes the same user-object ownership definition;
- RAM/FLASH builds and on-target lifecycle regressions pass;
- memory usage and hardware results are recorded in `BRINGUP.md`.

The RAM milestone may be committed before the FLASH gates above, but it must
remain documented as a RAM milestone rather than the final Phase 4.1 tag. The
FLASH follow-up must prove that CPU1's FLASH golden allocation is CPU1-owned,
does not collide with the CPU2 image, validates the same linker CRC record at
boot and START, and rejects START on a controlled expected/actual CRC mismatch.

## Non-goals

- Baking names or descriptors into firmware; that is Phase 4.5.
- Resetting automatic stack locals; they have invocation lifetime, not
  application-run lifetime.
- Preserving tuned parameters across START. User mutable state intentionally
  returns to source-declared values.
- Supporting arbitrary persistent/retained user sections in this phase.
- Protecting against arbitrary memory corruption caused by undefined behavior.
- Adding control math or changing the `setup()` / `control()` API.
