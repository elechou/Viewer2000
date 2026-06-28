# Board Portability and Variant Workflow

Viewer2000 is structured so that supporting additional target hardware changes
only one layer. This document defines that layer (the board seam), inventories
what is portable versus board-specific, lists the cleanups required to make the
seam airtight, and describes how public board profiles and private downstream
profiles track the same mainline without drifting apart.

This is an architecture/process document. It defines target structure and a
migration path; where it describes a not-yet-final state it says so.

## Why this document exists

The platform premise (see `AGENTS.md`) is that the product is the platform, not
any one motor controller. A second premise follows from it: the platform should
outlive any single board, gate-driver population, position sensor, or C2000
target. The runtime, the shared-memory interface logic, the portable comms
services, and the host application must not need editing to move to a different
hardware target. Only the board-support layer should.

Today that promise is ~90% true and the remaining ~10% is identifiable and
small. This document records exactly where it leaks and how the leaks close.

## Layer terminology

The hardware-support layer (formerly `cpu1/wire/`) is named **`board`**. It is a
board support layer: boot, memory map, peripheral substrate, pin assignment,
device drivers, and the protection wiring all live here.

A selectable **board profile** is the unit that builds. It is not just a bare
PCB name. A profile covers the MCU/core topology, launch board or carrier,
pinmux, PWM/ADC/CMPSS/X-BAR/protection topology, populated sensors, populated
gate-driver interface, SysConfig inputs, linker command files, target
configuration, physical memory map, and calibration defaults. This deliberately
allows one profile to mean "board + sensor + gate driver" when those choices
are inseparable in real hardware.

Shared component code can still live under `board/common/` and be reused by
multiple profiles, but the selected build unit is a profile. Even a common
sensor driver remains profile-bound at the wiring level because the pin
assignment, interrupt routing, sampling cadence, and safety interactions differ
by board.

Public mainline currently carries only the public profile that builds this
repository. Additional public profiles may be added only by an explicit upstream
decision. Private downstream profiles remain in the private downstream
repository only. Public documentation, paths, commit messages, and source
comments must not reveal private board names, device populations, component part
numbers, pin maps, or equipment details; use generic capability descriptions
instead.

The word "wire" is reserved for the **communication protocol** — the serialized
bytes on the link, defined by `docs/wire-spec.md`, `contracts/`, and the golden
vectors. Keeping "board" (hardware) and "wire" (protocol) as distinct words
removes a long-standing overload where "wire" meant both.

The L0–L3 map in `AGENTS.md` should be read with `L0 = board/`.

The target layout should keep the seam stable and the profiles explicit:

```text
cpu1/board/
  include/v2k_board.h
  common/
    sensors/
    gate_drivers/
    protection/
  profiles/
    f28p65x_launchxl_drv8323rs_as5600/

cpu2/board/
  include/v2k_cpu2_board.h
  common/
    pipes/
  profiles/
    f28p65x_launchxl_sci/
```

The exact directory names may change during implementation, but the ownership
rule should not: `common/` holds reusable board-layer components, while
`profiles/` holds concrete public hardware compositions. Private downstream
profiles use the same conceptual structure in the downstream repository, but no
placeholder directory or private profile file is added to public mainline.

Target-wide physical memory maps belong to board/profile ownership, not to
`contracts/`. Public mainline's default board map is
`cpu1/board/v2k_board_memmap.h`; `contracts/v2k_memmap.h` only defines the
logical shared-plane contract and selects that public board header unless a
downstream build overrides `V2K_TARGET_MEMMAP_HEADER`. The default lives under
CPU1 board support because CPU1 is the boot master that assigns shared-memory
ownership; CPU2 consumes the resulting logical macros through the contract.

## The board seam

CPU1 L1 runtime code reaches hardware only through one compile-time substrate
header, `v2k_board.h` (the L0↔L1 seam). It exposes timing and the fixed ISR
fast path, ADC frame acquisition, explicit PWM command application, bounded
background device service, platform variable enumeration, and the protection
lifecycle. Runtime calls these functions; it does not include vendor driver
headers and does not touch registers directly.

CPU2 has the same rule. Portable CPU2 code owns the descriptor enumeration,
parameter service, scope pump, heartbeat policy, and protocol serializers. It
must not own a concrete SCI/MCAN/EtherCAT instance, pinmux, interrupt vector, or
vendor register path directly. Those belong behind a CPU2 board seam such as
`v2k_cpu2_board.h`, which exposes "pipe" capabilities rather than device
registers.

User code (L2/L3) may additionally read completed peripheral results through
documented, non-blocking vendor result/status APIs after the platform-owned EOC
boundary, but it does not configure timing, output, ownership, interrupts, or
protection.

The named CPU1 seam header is `v2k_board.h` rather than `board.h` because the
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
- **Portable comms services** — descriptor enumeration, parameter service,
  scope pump, heartbeat policy, and explicit protocol serializers. The physical
  communication pipe, interrupt setup, pinmux, peripheral base, and link-local
  init remain board-specific.
- **Host application** — fully shared; it consumes the runtime-enumerated
  descriptor table and the frozen wire protocol, neither of which encodes a
  board.

If a change to any of the above is needed to support a board, that is a design
smell: the board-specific part belongs below the seam, not here.

## Board-specific surface (the layer that changes per target)

- **Boot and core hand-off** — device init, memory/peripheral ownership
  assignment, flash partitioning, and starting the companion core.
- **Build profile artifacts** — SysConfig inputs, generated-board include
  boundaries, linker command files, target configuration, device selection,
  driverlib root, source exclusion rules, and any CCS build metadata that varies
  by target.
- **Physical memory map** — the concrete base addresses and block sizes of
  shared RAM and the message RAM. These differ by device even though the
  *logical* allocation on top of them does not.
- **Peripheral substrate** — the PWM/ADC/trip-zone/comparator configuration,
  the EOC interrupt chain, pin assignment, and the input cross-bar wiring of
  trip sources.
- **CPU2 physical pipe substrate** — concrete SCI/MCAN/EtherCAT instances,
  pinmux, interrupt vectors, baud/link settings, local link watchdogs, and
  peripheral ownership required by the selected profile.
- **Device drivers** — populated gate-driver interfaces and sensor drivers, and
  their start-readiness and fault semantics.
- **Protection wiring** — the trip-source-to-trip-zone bindings and the
  read-back assertions that prove the output is gated before release.

## CCS and SysConfig boundary

The public mainline CCS project pair carries one SysConfig input per project.
Board portability is not implemented by adding extra private SysConfig files or
target configurations to this repository. A downstream target should carry its
own CCS project pair, private build configuration, generated-board files, linker
command files, target configuration, and target memory-map override.

The public C seam supports that downstream split through
`V2K_TARGET_MEMMAP_HEADER`: public mainline defaults it to
`cpu1/board/v2k_board_memmap.h`, and a downstream build may define the macro to
a private board memmap header. `contracts/v2k_memmap.h` keeps the shared
interface logic portable while the downstream repository owns the concrete
physical addresses and any retuned plane capacities.

## Scope data-ring tuning and host compatibility

A board with a tighter RAM budget will want to retune the scope ring. The ring
exposes two kinds of knob, and only one can desynchronize the host (Scope2000),
so the retuning stays mostly free:

**Per-board-free — firmware-internal or self-described on the wire:**

- Ring depth and block cadence are self-described by HELLO v7 as
  `scope_ring_words` and `scope_block_ticks`, so the host can clamp Capture
  settings to the active profile. Ring base, block slot words, and target
  shared-RAM placement remain firmware-internal. Tune freely when HELLO reports
  the resulting resource facts.
- Block geometry — `n_ticks` (N), `n_ch` (M), and `stride_octets` — travels in
  every block header (`contracts/v2k_scope.h`), so a block is self-delimiting and
  a different N or channel count parses with no host change.
- Control rate — HELLO carries `tick_hz`, so a different ISR frequency needs no
  host edit.

**The remaining desync risk — frame payload ceiling the host still mirrors as a
contract constant:** `V2K_WIRE_MAX_PAYLOAD` is the frame-payload size the host
uses for reassembly. The channel ceiling now travels in HELLO v7 as
`scope_max_ch`, so a profile with a different binding limit can be handled at
runtime.

The governing rule is therefore **parameters are negotiated, layout is
versioned**: the block-header and batch-prefix field layout stays pinned to
`V2K_WIRE_VER`, while sizing limits that affect user choices are reported at
runtime. Future changes to frame payload size still require a coordinated
wire-version bump touching the CPU2 serializer, `docs/wire-spec.md`, golden
vectors, and Scope2000 together.

## Cleanups required to make the seam airtight

The seam is mostly clean, but the current implementation still leaks several
board-specific facts into portable code. These are board-neutral improvements
and belong on public mainline regardless of any specific port; doing them before
the private profile exists means doing them once instead of twice.

1. **Rename `cpu1/wire/` → `cpu1/board/`**, `wire.h` → `v2k_board.h`, and the
   old `wire_*` symbol prefix → `v2k_board_*`. The user-facing `v2k_io` object
   keeps its name. After the rename, "wire" means only the serialized protocol.
2. **Move CPU1 boot/board bring-up out of `runtime/`.** Memory/peripheral
   ownership assignment, flash partitioning, pin/interrupt vector setup, and
   companion-core start currently live in a runtime startup file that includes
   vendor headers. They move below the seam behind board-layer entry points so
   the runtime startup path contains no device calls.
3. **Add a CPU2 board seam.** CPU2 portable code must not include generated
   board headers or driverlib/device headers directly for physical link setup.
   Concrete SCI/MCAN/EtherCAT instances, pinmux, interrupts, baud/link
   settings, and peripheral ownership move behind a CPU2 board profile.
4. **Remove device-driver names from the descriptor registry.** The registry
   currently includes a specific sensor's internal header and registers that
   sensor's diagnostics by name. Replace this with a board-provided
   "register diagnostics" hook so the portable registry names no specific
   device.
5. **Split the memory-map header.** Separate the portable *logical* allocation
   (structure-to-plane mapping, section names, ring-depth reasoning) from the
   *physical* base addresses and block sizes. The logical contract selects a
   target memmap header through `V2K_TARGET_MEMMAP_HEADER`; public mainline
   defaults to `cpu1/board/v2k_board_memmap.h`, while downstream repositories
   provide their own private board header without upstreaming it.
6. **Replace smart-driver-shaped lifecycle bits with capability-based
   gate-driver semantics.** The board seam may expose generic power-stage
   lifecycle state, start blockers, fault inputs, optional fault-clear/reset
   outputs, and diagnostic descriptors. It must not assume that every gate
   driver has SPI registers, a configuration transaction, or a status-register
   model. Public names should describe capabilities such as "gate fault",
   "fault clear", "desat/uvlo class fault", or "driver ready", not private
   component identities.
7. **Add a board profile manifest/API version.** Each profile should declare
   the board API version, CPU topology, supported physical pipes, power-stage
   capabilities, sensor capabilities, and required generated/build artifacts.
   Public mainline changes that alter the seam should fail downstream profile
   builds clearly rather than producing silent mismatches.

After these, the per-target divergence collapses to explicit board profiles,
their physical memory headers, and their CCS/SysConfig/build artifacts. The
portable surface above then merges between targets with minimal conflicts.

## Public mainline and private downstream workflow

Mainline may support additional public board profiles only when they are meant
to be public upstream artifacts. The path isolation rule applies to private
profiles: private-only board paths, SysConfig files, linker command files, target
configurations, and target memory-map headers are not defined by mainline and
never appear in the public repository.

The private device repository should be an independent private repository,
initialized from public mainline history and configured with public mainline as
an `upstream` remote. It should not be treated as a GitHub fork workflow whose
branches are routinely pushed back to public.

### Tracking mainline from the private repository

The private repository routinely fetches and merges `upstream/main`, preferably
through a short-lived integration branch:

1. Fetch `upstream/main`.
2. Merge it into a private integration branch.
3. Resolve only real seam/build conflicts.
4. Build the selected private profile.
5. Run bench acceptance appropriate for the changed seam.
6. Merge the integration branch into the private repository's main branch.

Because the portable surface is identical and private board files live in
private-only paths, routine upstream merges should primarily touch shared
portable code and public profiles. If a merge repeatedly collides with private
profile files, that is evidence that private hardware detail has leaked into
public paths or the seam is too narrow.

### Contributing back from private to public

Improvements that are genuinely board-neutral can flow back to public mainline,
but **only as reviewed, cherry-picked or reimplemented commits**, never as a
branch push from the private repository. Each such contribution is reviewed to
confirm that it contains only portable, board-neutral material and no private
hardware identifiers, pin maps, calibration values, target configs, private
device topology, or commit-message clues.

The public repository should not be configured as a normal push target of the
private repository. Upstreaming is an explicit, per-change action.

### Path isolation

Private profile files live in directories the public mainline does not define.
Two consequences follow automatically:

- public mainline merges do not modify private-only files, and
- private-only files cannot be carried upstream by an ordinary merge, because
  upstreaming is cherry-pick/reimplement-and-review rather than branch-push.

This keeps each repository's content cleanly scoped to its own concern with no
manual bookkeeping in the common case.

### Recommended sequence

1. Land the seam cleanups above on public mainline.
2. Make the board profile selectable by build configuration. Treat the selected
   profile as more than a C macro: device, SysConfig inputs, generated headers,
   linker command files, target configuration, driverlib root, and source
   inclusion/exclusion may all vary.
3. Add at least one public board profile in mainline to prove that public
   multi-board support is real.
4. Create the private repository from mainline history and add public mainline
   as the `upstream` remote.
5. Add the private board profile, physical memory header, generated/build
   artifacts, and private calibration defaults only in the private repository.
6. Track mainline by routine merge; contribute board-neutral fixes back only by
   reviewed cherry-pick or reimplementation after a private-information review.

### Current public profile contract

Public mainline carries a small machine-readable profile contract in each core's
board layer:

- CPU1 selects a profile through `V2K_BOARD_PROFILE_HEADER`, defaulting to
  `cpu1/board/profiles/f28p65x_launchxl_drv8323rs_as5600/v2k_board_profile.h`.
- CPU2 selects a profile through `V2K_CPU2_BOARD_PROFILE_HEADER`, defaulting to
  `cpu2/board/profiles/f28p65x_launchxl_sci/v2k_cpu2_board_profile.h`.
- Each public profile has a hand-written `manifest.toml` declaring the board API version,
  CPU topology, capability set, memory-map header, SysConfig input, linker
  command files, target configuration, and board-owned sources. TOML is used
  here because these files are maintained by people and benefit from comments.

The seam between the portable surface and the board layer is validated without a
dedicated second build configuration. The public FLASH build links the portable
runtime against the real board through the seam headers only, and
`tools/check_board_seams.py` enforces that no portable file (CPU1 runtime, or the
portable CPU2 files including `cpu2.c`) reaches a vendor register outside that
seam. A private downstream profile drops into the same `profiles/<id>/` shape with
its own `manifest.toml`, board sources, and `V2K_*_BOARD_PROFILE_HEADER`
selection; because the portable surface references the board only through the
seam, accepting that profile needs no edits above the board layer.

(A throwaway `null_loopback` profile with a no-op board implementation was
prototyped as an explicit link-level acid test. It was dropped: the CCS build
tooling forces the active configuration back to `FLASH`, and the green FLASH build
plus the static seam check already cover the same risk. The selection mechanism,
manifest schema, and checker remain — they are what a real downstream profile
plugs into.)

Run `python3 tools/check_board_seams.py` before splitting or merging board-seam
changes. The check rejects vendor register access from the CPU1 runtime and the
portable CPU2 files (including `cpu2.c`, the super-loop orchestration), requires
every public profile manifest to declare a non-empty `board_sources` (a profile
must bring its own board implementation), and verifies referenced artifacts exist
— treating `"none"` as an artifact a build-only profile deliberately omits.

## CPU1-only student deliverable

A CPU1-only deliverable for student user-code writing and flashing is not a
board profile. It changes the product topology: no CPU2 command/status service,
no CPU2 heartbeat consumer, no CPU2 scope pump, different boot/flash workflow,
different shared-memory ownership assumptions, and likely a different host or
tooling contract.

Treat it as a separate **product profile** or template repository, not as a
normal board-portability variant. The preferred shape is a small student-facing
repository or release artifact created from a known public tag, with only the
CPU1 user-code surface and the minimum flashing workflow exposed. If it must
stay in the main repository, it should be an explicit product profile with
stubbed capabilities and clear acceptance criteria, not an implicit single-core
fallback path inside the normal dual-core platform.

## Notes on target capability differences

Board variants need not be feature-identical. A target may lack a peripheral
that mainline uses for a later-phase link; in that case the affected
later-phase feature simply does not build for that target, while everything up
to the board seam remains shared. Such capability gaps are a property of the
board layer and its build configuration, not of the portable surface.

### Protection posture without a smart gate driver

A target may populate a plain gate driver with no fault pin, no autonomous
overcurrent/desaturation shutdown, and no way to command the driver into a safe
state. This removes the external one-shot trip chain (a smart driver's fault
output feeding a trip-zone input) and the driver-autonomous cutoff, but it does
**not** remove MCU protection: the ePWM trip-zone forces the gate *signals* to
their off state through no CPU, so it works for any driver. The portable runtime
and the fault state machine are unchanged; only the board layer differs, and the
seam already expresses this (the gate-fault/comm/config/status start blockers
collapse to no-ops while the on-chip current-trip arming stays).

The board layer must re-create the lost defense in hardware, not software:

- The on-chip current trip (comparator → input cross-bar → trip-zone) becomes
  the front-line cutoff. Prefer a self-contained latching shunt comparator
  against a fixed reference wired into a spare trip-zone input, so it does not
  depend on MCU comparator-reference setup or the current-sense front end staying
  powered.
- The ePWM dead-band becomes safety-critical, since a plain driver enforces no
  interlock of its own.
- Matching the trip-zone force action to "gate signal off" through the specific
  driver's input polarity is the mandatory bring-up check; the wrong polarity
  turns the cutoff into a shoot-through.

Coarser, slower levers sit beneath the trip-zone and complement it, never
replace it:

- A CPU1-GPIO contactor/relay is a software-gated, millisecond-scale lever for
  connect-when-ready sequencing and de-energizing after a latched fault. It is
  not a microsecond cutoff and is generally not rated to break fault current.
- A fuse is the non-electronic backstop below everything else.

The board profile manifest (cleanup item 7) should declare this posture ("no
autonomous gate fault; primary protection = on-chip current window") and the
profile should register only diagnostics that physically exist (no SPI status
descriptors for a driver with no SPI).
