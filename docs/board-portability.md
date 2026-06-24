# Board Portability and Variant Workflow

Viewer2000 is structured so that supporting an additional target board changes
only one layer. This document defines that layer (the board seam), inventories
what is portable versus board-specific, lists the cleanups required to make the
seam airtight, and describes how a downstream board variant tracks this
mainline without the two drifting apart.

This is an architecture/process document. It defines target structure and a
migration path; where it describes a not-yet-final state it says so.

## Why this document exists

The platform premise (see `AGENTS.md`) is that the product is the platform, not
any one motor controller. A second premise follows from it: the platform should
outlive any single board. The runtime, the shared-memory interface logic, the
comms core, and the host application must not need editing to move to a
different C2000 target. Only the board-support layer should.

Today that promise is ~90% true and the remaining ~10% is identifiable and
small. This document records exactly where it leaks and how the leaks close.

## Layer terminology

The hardware-support layer (formerly `cpu1/wire/`) is named **`board`**. It is a
board support layer: boot, memory map, peripheral substrate, pin assignment,
device drivers, and the protection wiring all live here.

The word "wire" is reserved for the **communication protocol** — the serialized
bytes on the link, defined by `docs/wire-spec.md`, `contracts/`, and the golden
vectors. Keeping "board" (hardware) and "wire" (protocol) as distinct words
removes a long-standing overload where "wire" meant both.

The L0–L3 map in `AGENTS.md` should be read with `L0 = board/`.

## The board seam

L1 runtime code reaches hardware only through one compile-time substrate header,
`v2k_board.h` (the L0↔L1 seam). It exposes timing and the fixed ISR fast path,
ADC frame acquisition, explicit PWM command application, bounded background
device service, platform variable enumeration, and the protection lifecycle.
Runtime calls these functions; it does not include vendor driver headers and
does not touch registers directly.

User code (L2/L3) may additionally read completed peripheral results through
documented, non-blocking vendor result/status APIs after the platform-owned EOC
boundary, but it does not configure timing, output, ownership, interrupts, or
protection.

The named seam header is `v2k_board.h` rather than `board.h` because the
configuration generator already emits a `board.h`; that generated file stays
board-private and is included only by board-layer implementation, never by
runtime.

## Portable surface (must not diverge between boards)

These carry no board-physical detail and are shared verbatim by every target:

- **`runtime/`** — ISR executor, time base, multi-rate scheduling, RAM scope,
  parameter double-buffer, descriptor registry, fault-latch state machine,
  load profiling. Includes only the board seam, `v2k.h`, `contracts/`, and
  `common/`.
- **Logical interface contracts** — the descriptor/parameter/scope/command
  structures and the allocation policy (which structure lives in which plane,
  write-owner rules, ring-depth reasoning, section names).
- **Comms core** — the descriptor enumeration, parameter service, scope pump,
  heartbeat, and protocol serializers.
- **Host application** — fully shared; it consumes the runtime-enumerated
  descriptor table and the frozen wire protocol, neither of which encodes a
  board.

If a change to any of the above is needed to support a board, that is a design
smell: the board-specific part belongs below the seam, not here.

## Board-specific surface (the layer that changes per target)

- **Boot and core hand-off** — device init, memory/peripheral ownership
  assignment, flash partitioning, and starting the companion core.
- **Physical memory map** — the concrete base addresses and block sizes of
  shared RAM and the message RAM. These differ by device even though the
  *logical* allocation on top of them does not.
- **Peripheral substrate** — the PWM/ADC/trip-zone/comparator configuration,
  the EOC interrupt chain, pin assignment, and the input cross-bar wiring of
  trip sources.
- **Device drivers** — the gate driver and position sensor drivers, and their
  start-readiness and fault semantics.
- **Protection wiring** — the trip-source-to-trip-zone bindings and the
  read-back assertions that prove the output is gated before release.

## Cleanups required to make the seam airtight

The seam is mostly clean. Three identifiable leaks currently cross it, plus the
naming change. All four are board-neutral improvements and belong on mainline
regardless of any specific port; doing them before a variant exists means doing
them once instead of twice.

1. **Rename `wire/` → `board/`**, `wire.h` → `v2k_board.h`, and the `wire_*`
   symbol prefix → `board_*`. The user-facing `v2k_io` object keeps its name.
2. **Move boot/board bring-up out of `runtime/`.** Memory/peripheral ownership
   assignment, flash partitioning, pin/interrupt vector setup, and companion-
   core start currently live in a runtime startup file that includes vendor
   headers. They move below the seam behind board-layer entry points so the
   runtime startup path contains no device calls.
3. **Remove the device-driver name from the descriptor registry.** The registry
   currently includes a specific sensor's internal header and registers that
   sensor's diagnostics by name. Replace this with a board-provided
   "register diagnostics" hook so the portable registry names no specific
   device.
4. **Split the memory-map header.** Separate the portable *logical* allocation
   (structure-to-plane mapping, section names, ring-depth reasoning) from the
   *physical* base addresses and block sizes. The logical part stays in the
   shared contracts; the physical part moves to a board header.

After these, the per-target divergence collapses to a small set of clearly
bounded board files plus the physical memory header. The portable surface above
then merges between targets with effectively no conflicts.

## Variant workflow

A board variant is maintained as a **downstream repository that tracks this
mainline as an `upstream` remote**. The relationship is deliberately one-way for
content and asymmetric for changes.

### Tracking mainline (downstream pulls)

The variant routinely fetches and merges `upstream/main`. Because the portable
surface is identical and the board-specific files live in paths the mainline
does not define, these merges touch only shared portable code and never collide
with variant-specific files.

### Contributing back (downstream → mainline)

Improvements that are genuinely board-neutral can flow back to mainline, but
**only as reviewed, cherry-picked commits**, never as a branch push. Each such
contribution is reviewed to confirm it contains only portable, board-neutral
material before it lands on mainline. The mainline repository is not configured
as a push target of the variant; upstreaming is an explicit, per-commit action.

### Path isolation

Variant-specific files live in directories the mainline does not define. Two
consequences follow automatically:

- mainline merges never modify or conflict with variant-only files, and
- variant-only files cannot be carried upstream by an ordinary merge, because
  upstreaming is cherry-pick-and-review rather than branch-push.

This keeps each repository's content cleanly scoped to its own concern with no
manual bookkeeping in the common case.

### Recommended sequence

1. Land the four cleanups above on mainline (all board-neutral).
2. Make the board layer selectable by build configuration.
3. Create the variant repository from mainline history and add mainline as the
   `upstream` remote.
4. Add the variant's board layer and physical memory header; switch the build.
5. Track mainline by routine merge; contribute board-neutral fixes back by
   reviewed cherry-pick.

## Notes on target capability differences

Board variants need not be feature-identical. A target may lack a peripheral
that mainline uses for a later-phase link; in that case the affected
later-phase feature simply does not build for that target, while everything up
to the board seam remains shared. Such capability gaps are a property of the
board layer and its build configuration, not of the portable surface.
