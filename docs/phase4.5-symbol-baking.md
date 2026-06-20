# Phase 4.5 — Build-time symbol baking (auto-observability)

> **Document status**: implemented and accepted on RAM/20 kHz and
> FLASH/20 kHz. On 2026-06-21 an isolated Scope2000 instance with no project or
> `.out` enumerated all 57 entries, bound and plotted a baked variable, enforced
> mutable/const CAL policy, and refreshed its catalog across deterministic
> A (`0x521C2BA6`) → B (`0xF057F5F0`, `flash_probe`) → A transitions. See
> [BRINGUP.md](../BRINGUP.md).
>
> **Why it's "4.5"**: it serves the same user-facing layer as [Phase 4](phase4-user-interface.md) — the L2/L3 experience — but it is a **build-tooling** effort (host/build side). It consumes the user-object ownership contract established by [Phase 4.1](phase4.1-user-code-boundary.md), while remaining independent of the runtime reset mechanism.

## Goal

> Flash the firmware, then on **any** computer — even one without the original project — open Scope2000 and see the user's variables **by name**, with no `.out` present.

…while the student writes **plain C** (no mandated declaration style, no registration macro). This is the Arduino soul of the platform: declare a variable normally, and it is automatically observable/tunable by name.

## The idea: read DWARF at build time, bake the table into firmware

The compiler already emits every variable's `{name, address, type}` into the `.out` DWARF. The only question is *where and when* that information is read:

- **Rejected — host reads `.out` at runtime**: requires the `.out` to be present on the user's PC, and risks a stale/wrong `.out` (compiled at a different time than what is flashed) → writes to the wrong address. Ties Scope2000 to the project directory.
- **Rejected — registration macro**: forces the student into a mandated declaration style.
- **Adopted — read DWARF at build time, on the build machine, and bake a compact `name→addr→type` table into the firmware image**, served over the existing ENUM. The names travel with the device; any PC sees them; the addresses come from the *same build that is flashed* (no stale-ELF class of bug; `build_hash` still guards the host cache); the student writes plain C.

This is the same pattern professional RCP toolchains use (A2L generated from ELF post-build), except the result is baked into the device instead of shipped as a side file.

| | host DWARF (rejected) | macro (rejected) | **build-time baking (this phase)** |
|---|---|---|---|
| student writes | plain C | mandated macro | **plain C** |
| names travel with device | no (PC needs `.out`) | yes | **yes (in flash, via ENUM)** |
| stale/wrong ELF risk | yes | n/a | **none (same build)** |
| struct / array | auto-expand | awkward | **auto-expand to per-member scalars** |
| host (Scope2000) changes | a whole DWARF parser | none | **none — it already enumerates the descriptor table** |
| cost | host parser + `.out` shipping | trivial | **a build tool + a link/patch step (the price)** |

The decisive win: Scope2000 needs no DWARF parser or project file. It reads user-visible names from the existing ENUM service; descriptor `kind` bit 2 marks baked user entries so the UI can keep platform/system diagnostics outside its main "All Variables" tree.

## Build flow

```
① Phase 4.1 generates the authoritative user-object manifest and linker ranges.
② the linker reserves v2k_user_desc:
     RAM build   → spare CPU1 RAM (RAMD5_FREE)
     FLASH build → CPU1 flash (FLASH_BANK1)
③ CCS links cpu1.out with DWARF and the zero-entry reserved blob.
④ Phase 4.1 verifies ownership and reset ranges.
⑤ v2k_bake_user_desc.py runs TI ofd2000 --xml --dwarf:
     - accepts only absolute DW_OP_addr variables inside user data/BSS/const;
     - resolves typedef/const/volatile/TI-far wrappers;
     - expands supported arrays and structs to scalar leaves;
     - hashes the final ELF with the blob normalized, plus the generated records;
     - reads CPU1's CCS/Eclipse `cpu1/.project` `<name>` and bakes the human-readable project name/build time;
     - patches v2k_user_desc in cpu1.out and decodes it back for verification;
     - emits v2k_user_desc_report.json.
⑥ v2k_registry_init registers platform descriptors, appends the baked records,
   then publishes V2K_DESC_MAGIC. ENUM and Scope2000 remain unchanged.
```

The tracked `makefile.init` and `makefile.targets` hooks implement this order without editing CCS `.cproject`. Binary patching is deliberate: the fixed-size section does not move any linked symbol and needs no second link.

## Implemented policy

- **Capacity**: `V2K_DESC_MAX=128`; 32 slots are reserved for platform descriptors and the baked blob holds at most 96 user leaves. The GS0 plane is 2950 C28x words, below its 4096-word allocation.
- **Names**: `V2K_NAME_LEN` remains 16. The tool uses source-level names, never linker-name fallback, and fails rather than truncating names longer than 15 visible ASCII characters. Function statics use the source variable name and therefore collide explicitly when ambiguous.
- **Types**: I16/U16/I32/U32/F32 are accepted. Pointers, unions, enums, doubles, 64-bit leaves, bitfields, and other unsupported forms are omitted and listed in the JSON report.
- **Kinds**: user data/BSS leaves are `PARAM|SCOPE`; user const leaves are `SCOPE` only. Runtime write validation excludes const while read/scope validation includes it.
- **Addresses**: TI OFD reports C28x word addresses. A 32-bit leaf must be even-aligned and fully contained in one authoritative user range.
- **ELF**: the patcher requires ELF32 little-endian, an exact-size `SHT_PROGBITS` section, matching blob magic/version/capacity, and a successful decode after patching.
- **Build hash**: the blob carries a nonzero 32-bit hash derived from the normalized final ELF and baked records. Re-baking an unchanged image is stable; changing code, linked addresses, or the variable set changes the hash even before commit.
- **Project info**: the project name comes from CPU1's CCS/Eclipse `.project` `<name>` as-is. Empty names become `untitled`; `untitled` only emits a post-link warning. Overlong or non-printable names fail because HELLO carries a fixed 32-octet printable ASCII field. Project name and build time are excluded from `build_hash` and exist only for human identification in HELLO.
- **Runtime diagnostics**: the platform reserves exactly 32 descriptors and appends the user set only after validating every entry and total capacity. The platform descriptor `desc_error` reports 0=OK, 1=bad blob header, 2=platform capacity, 3=combined table capacity, or 4=invalid user entry.

## Verification

| # | Verification | Method | Pass criterion |
|---|---|---|---|
| A | tool extracts symbols | run `v2k_bake_user_desc.py` on a known `.out` | emits correct `{name, addr, type}` for the demo's plain-C variables; struct members expanded |
| B | addresses correct | compare baked addresses against the `.map` / CCS Expressions `&var` | exact match; no off-by-word from the 16-bit-char convention |
| C | names travel | flash the patched `.out`; on a **clean PC without the project**, open Scope2000, ENUM | the user's plain-C variable names appear by name; bind + plot one |
| D | tune a baked var | CAL_WRITE to a baked PARAM var | takes effect; `applied_seq` reconciles (no descriptor-table-membership special-casing needed) |
| E | stale guard | flash a different build (changed vars) | `build_hash` changes → Scope2000 re-enumerates; old names gone, new names present |
| F | capacity | unit-test overflow and build the 128-entry shared layout | overflow fails; GS0 RAM fits; ENUM paging returns all on hardware |

## Acceptance and exit

| Item | Pass condition |
|---|---|
| build tool | `v2k_bake_user_desc.py` parses TI OFD DWARF XML, scopes by Phase 4.1 linker ranges, expands structs/arrays |
| baking | reserved blob patched with correct word-addresses; no symbol shift or second link |
| host remains project-free | Scope2000 shows user names via ENUM and classifies them by the USER kind bit; no `.out` or DWARF parser |
| travels with device | clean-PC test (C) passes — names visible without the project or any `.out` |
| guards | `build_hash` re-enumeration (E); capacity within budget (F) |

Record into BRINGUP.md: tool version, the `.out` parsed, the scoping rule, capacity used vs `V2K_DESC_MAX`, the clean-PC screenshot, and the build_hash re-enumeration trace.

## Relationship to the other layers

- Phase 4.5 changes **how the descriptor table gets filled** and increases its shared-memory capacity. The 28-octet descriptor wire layout is unchanged; contract version 11 introduced descriptor `kind` bit 2 as the USER origin flag and CPU1-baked HELLO project metadata.
- Phase 4.1 remains authoritative for which mutable storage resets. A variable still resets if it is not bakeable, not visible, or unsupported by the descriptor type system; descriptor capacity overflow fails the build instead of weakening reset coverage.
- It supersedes the earlier "application variables are discovered host-side via `.out` (DWARF)" wording in `wire-spec.md` / `contracts/v2k_descriptor.h`: discovery is now **build-time baking into the descriptor table**, so the host needs no `.out`.
