# Phase 4.5 — Build-time symbol baking (auto-observability): development plan

> **Document status**: a forward-looking development plan. As pieces land, the verification section becomes the acceptance checklist; results go into [BRINGUP.md](../BRINGUP.md). Tag `phase4.5-symbol-baking` after acceptance.
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

The decisive win: **Scope2000 needs zero changes** — it already reads user-visible names from the descriptor table over ENUM. Baking user variables into that table makes them appear with no new host feature.

## Build flow

```
① reserve a fixed-size const blob in firmware:
     const v2k_reg_rec_t g_v2k_user_reg[V2K_USER_REG_MAX];   // zero-init at link, known section
② CCS builds normally → cpu1.out (ELF + DWARF, addresses assigned)
③ post-build: python tools/bake_user_desc.py cpu1.out
     - pyelftools parses DWARF; collects globals/statics defined in the user object(s)
       (v2k_user.o / examples/*.o — scoped by a "which files are user code" rule)
     - extracts {name, word-address, type}; expands struct members / array elements
       into named scalar entries (e.g. motor.ia, ramp[3])
     - encodes records and patches the bytes of g_v2k_user_reg in cpu1.out
       (fixed location + size → no symbol moves → no relink needed)
④ flash the patched cpu1.out
⑤ at boot, v2k_registry_init walks g_v2k_user_reg[0..count] → v2k_desc_add(...) → publish
   → ENUM serves user names alongside platform names. Host: unchanged.
```

Two implementation flavors:
- **Binary-patch the reserved blob** (above): most robust — fixed location/size means no address shift and no relink.
- **Generate a `.c` + relink**: easier to wire into CCS's managed build; relies on the linker keeping `.const` (flash) independent of `.bss/.data` (RAM) so the baked RAM addresses stay valid after the relink.

Firmware runtime is **unchanged** in shape: `v2k_registry_init` already walks a registration source and calls `v2k_desc_add`; Phase 4.5 only changes that source from hand-coded calls to a tool-filled blob. The wire protocol and host are unchanged.

## Constraints and open points

- **Capacity**: `V2K_DESC_MAX` is 64 (≈17 platform entries used). Bump it (96/128) to fit platform + user, and re-check the GS0 RAM budget (each entry = 22 C28x words; 128 entries ≈ 2.8 K words) against `v2k_memmap.h`. ENUM already pages 8/req, so more entries just means more pages.
- **Name length**: `V2K_NAME_LEN` is 16; longer DWARF names truncate (or the tool errors). Expanded names like `motor.ia` must fit.
- **Scoping rule**: consume Phase 4.1's machine-readable user-object set. The baker must not maintain an independent `user.obj` / `examples/*.obj` glob that can drift from the reset boundary.
- **C28x address convention**: DWARF expresses addresses; the descriptor `addr` is a CPU1 data-space **word** address. Confirm the mapping (cl2000 EABI ELF, 16-bit char) — the one real parsing wrinkle.
- **ELF format**: cl2000 EABI emits standard ELF+DWARF; `pyelftools` handles it. Verify against the actual `.out`.
- **CCS integration**: a post-build step (patch flavor) or a generate-then-relink step. The repo already runs a pre-build (`gen_build_hash.py`), so the hook infrastructure exists; a post-build-then-flash path is the new bring-up.

## Verification

| # | Verification | Method | Pass criterion |
|---|---|---|---|
| A | tool extracts symbols | run `bake_user_desc.py` on a known `.out` | emits correct `{name, addr, type}` for the demo's plain-C variables; struct members expanded |
| B | addresses correct | compare baked addresses against the `.map` / CCS Expressions `&var` | exact match; no off-by-word from the 16-bit-char convention |
| C | names travel | flash the patched `.out`; on a **clean PC without the project**, open Scope2000, ENUM | the user's plain-C variable names appear by name; bind + plot one |
| D | tune a baked var | CAL_WRITE to a baked PARAM var | takes effect; `applied_seq` reconciles (no descriptor-table-membership special-casing needed) |
| E | stale guard | flash a different build (changed vars) | `build_hash` changes → Scope2000 re-enumerates; old names gone, new names present |
| F | capacity | bake a near-full table | within `V2K_DESC_MAX`; ENUM paging returns all; GS0 RAM fits |

## Acceptance and exit

| Item | Pass condition |
|---|---|
| build tool | `bake_user_desc.py` parses cl2000 EABI DWARF, scopes to user objects, expands structs/arrays |
| baking | reserved blob patched (or generated + relinked) with correct word-addresses; no symbol shift |
| host unchanged | Scope2000 shows user names via the existing ENUM, no new host code |
| travels with device | clean-PC test (C) passes — names visible without the project or any `.out` |
| guards | `build_hash` re-enumeration (E); capacity within budget (F) |

Record into BRINGUP.md: tool version, the `.out` parsed, the scoping rule, capacity used vs `V2K_DESC_MAX`, the clean-PC screenshot, and the build_hash re-enumeration trace.

## Relationship to the other layers

- Phase 4.5 changes only **how the descriptor table gets filled** (build-tool-baked user vars, alongside wire/runtime-registered platform and port names). The wire, the host, and the firmware runtime shape are untouched.
- Phase 4.1 remains authoritative for which mutable storage resets. A variable still resets if it is not bakeable, not visible, unsupported by the descriptor type system, or omitted because descriptor capacity is exhausted.
- It supersedes the earlier "application variables are discovered host-side via `.out` (DWARF)" wording in `wire-spec.md` / `contracts/v2k_descriptor.h`: discovery is now **build-time baking into the descriptor table**, so the host needs no `.out`.
