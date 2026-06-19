# Phase 4.1 - User-code ownership and reset boundary: development plan

> **Document status**: forward-looking implementation plan. Phase 4 proved the
> `setup()` / `control()` lifecycle with a manually sectioned demo. Phase 4.1
> turns that prototype into a general build and linker contract for arbitrary
> plain-C user applications. Results go into [BRINGUP.md](../BRINGUP.md); tag
> `phase4.1-user-code-boundary` after acceptance.
>
> **Position in the roadmap**: Phase 4.1 must complete before Phase 4 is treated
> as production-ready for motor bring-up. [Phase 4.5](phase4.5-symbol-baking.md)
> consumes the same user-code ownership definition for observability, but reset
> correctness does not depend on DWARF, descriptor capacity, or symbol baking.

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
calls it. Header-only or inline DCL code needs no special handling. A library
with hidden mutable state must be included in the user domain or rejected for
this lifecycle contract.

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

User-owned mutable `NOINIT`, persistent, shared-memory, CLA, or absolute-address
state is not supported in the initial contract. The boundary verifier rejects
it unless a later platform API explicitly defines its lifecycle.

### 3. The linker owns capacity and the golden source

Reserve explicit memory regions:

- `USER_RUN`: the combined run capacity for user data and user BSS;
- `USER_GOLDEN_RAM`: the RAM-build LOAD image for initialized user data;
- a FLASH LOAD allocation for initialized user data in FLASH builds.

The initial target allocation is:

- `RAMLS6` as `USER_RUN`;
- `RAMLS7` as `USER_GOLDEN_RAM`;
- an aligned allocation in the CPU1 FLASH image as the FLASH golden source.

Ordinary `.const`, runtime `.data`, and runtime `.bss` must not spill into
`USER_RUN` or `USER_GOLDEN_RAM`.

Required link-time constraints:

```text
sizeof(user_data) + sizeof(user_bss) <= sizeof(USER_RUN)
sizeof(user_data) <= sizeof(USER_GOLDEN_RAM)    # RAM build
FLASH LOAD allocation must fit its flash region # FLASH build
```

The linker emits LOAD/RUN start, end, and size symbols. Runtime code uses only
those symbols; there is no `V2K_USER_DATA_SNAPSHOT_WORDS` constant and no
runtime-owned snapshot array.

### 4. START reset remains section-based and independent of DWARF

The runtime sequence is:

```text
lock safe output
disable application execution
validate linker symbols
copy user_data LOAD -> RUN
clear user_bss
run optional setup()
re-assert safe initial outputs
release the hardware output lock
enter RUNNING
```

`control()` remains gated to RUNNING. `CLEAR_FAULT` returns only to IDLE; the
next START performs the same reset again.

The reset operates on complete sections, not a list of symbols. It therefore
also covers arrays, structs, function-static variables, unsupported descriptor
types, and variables omitted from Phase 4.5 because the descriptor table is
full.

### 5. Build-time audit closes silent escape paths

Add a post-link boundary check that fails the build when:

- a mutable symbol defined by a user object is outside user data/BSS;
- a platform object contributes writable storage to a user reset section;
- a user object contributes unsupported writable sections such as `NOINIT`;
- linker-generated LOAD/RUN symbols are missing, reversed, overlapping, or
  outside their declared MEMORY regions;
- RAM and FLASH configurations derive different user ownership rules.

The implementation may inspect the map file plus ELF section/symbol metadata.
DWARF can improve diagnostics, but the correctness check must not require
Phase 4.5's descriptor baking or a runtime symbol table.

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

The spike must also confirm C28x word-address units for linker size symbols.
Record the exact accepted syntax in the implementation commit and remove the
spike afterward.

### 3. Implement RAM and FLASH linker layouts

- Create named MEMORY regions for `USER_RUN` and the RAM golden image.
- Collect user text/const/data/BSS by object ownership.
- Give initialized user data separate LOAD and RUN addresses.
- Export the linker symbols consumed by `v2k_user_runtime.c`.
- Remove `.const` and platform writable-section fallback into user regions.
- Add linker assertions or deliberately bounded MEMORY placement so every
  static overflow is a linker error.

RAM and FLASH scripts must expose the same runtime symbol contract even though
their LOAD regions differ.

### 4. Replace the runtime snapshot prototype

- Remove the fixed 256-word snapshot array and snapshot-ready state.
- Copy from the linker-provided LOAD address on every START.
- Clear the complete user BSS range.
- Keep address/order validation and fail closed if the linker contract is
  invalid.
- Preserve the existing reset counter/error diagnostics.

The platform makes no claim against arbitrary C undefined behavior or a wild
pointer corrupting the golden region. The guarantee is that all correctly
classified user static storage is restored under normal memory integrity.

### 5. Remove user declaration annotations

- Delete all `#pragma DATA_SECTION(..., "v2k_user_*")` directives from the demo.
- Add ordinary initialized and zero-initialized globals, arrays/struct members,
  and a function-static test variable.
- Split at least one test variable into a second application translation unit
  to prove the boundary is not tied to one `user.obj`.

The final demo must look like normal plain C.

### 6. Add the boundary verifier

- Inspect the final CPU1 `.out` and map for both RAM and FLASH configurations.
- Emit concise diagnostics naming the escaped symbol, owning object, actual
  section, and expected section.
- Integrate it as a build step through CCS-supported project tooling.
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
| H | capacity failure | temporarily exceed `USER_RUN` and RAM golden capacity | link fails with a clear placement/assertion error |
| I | escaped-state failure | add a deliberately unsupported/misplaced user writable section | boundary verifier fails the build and names it |
| J | user pragma independence | search final user sources and inspect map | no user reset-section pragmas; complete user writable coverage |
| K | four-config build | CCS `buildProject` for CPU1/CPU2 RAM/FLASH | zero errors; boundary check runs for both CPU1 configs |
| L | Phase 2-4 regression | trip/state lifecycle, scope bind/stream, DCL demo, 20/100 kHz budgets | previous behavior passes; no ISR budget regression |

For the hardware reset tests, halt or instrument immediately after section
restore and before the first `control()` tick at least once, so a later control
assignment cannot mask an incomplete reset.

## Acceptance and exit

Phase 4.1 is complete only when:

- ordinary user globals and function statics require no pragmas;
- all user mutable static storage is reset from a linker-owned golden source;
- RAM and FLASH use verified, distinct LOAD mechanisms;
- the fixed runtime snapshot and its size cap are gone;
- static capacity overflow and ownership escape fail the build;
- runtime/wire/platform state is proven outside the reset boundary;
- Phase 4.5 consumes the same user-object ownership definition;
- RAM/FLASH builds and on-target lifecycle regressions pass;
- memory usage and hardware results are recorded in `BRINGUP.md`.

## Non-goals

- Baking names or descriptors into firmware; that is Phase 4.5.
- Resetting automatic stack locals; they have invocation lifetime, not
  application-run lifetime.
- Preserving tuned parameters across START. User mutable state intentionally
  returns to source-declared values.
- Supporting arbitrary persistent/retained user sections in this phase.
- Protecting against arbitrary memory corruption caused by undefined behavior.
- Adding control math or changing the `setup()` / `control()` API.
