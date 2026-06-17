# Workspace Layout and Shared Knowledge

Viewer2000 and Scope2000 are separate public repositories that normally live
under the same local workspace parent:

```text
20260610_Viewer2000/
  Viewer2000/   public firmware repo
  Scope2000/    public host application repo
  _private/     local-only private notes and historical reference material
```

The workspace parent is not a source repository. It is a local coordination
folder.

## Public Repositories

`Viewer2000/` owns firmware and the protocol authority:

- `docs/wire-spec.md`
- `contracts/`
- `contracts/vectors/`
- phase bring-up and hardware verification documents

`Scope2000/` owns the PC-side Rust/egui application:

- `V2kSource` service, codec, and transport implementation
- GUI panels, waveform display, CSV export, and local configuration
- conformance tests that mirror `Viewer2000/contracts/vectors/`

Neither public repo should contain private reference code, private brand
constraints, old project history, or compatibility-only implementation details.

## Private Reference Material

Private reference material belongs under the workspace-level `_private/`
directory, outside both public repositories:

```text
20260610_Viewer2000/_private/
  brand-constraints.md
  V2KScope.reference/
```

This directory is local-only. It is not a Git dependency, not a submodule, not
used by public CI, and not required to build either public repository.

When a useful idea is extracted from private reference material, rewrite it into
neutral Viewer2000/Scope2000 terminology before adding it to public source or
docs. Public artifacts should contain only current project names and current
architecture terms.

## Shared Knowledge Boundary

The two public repositories share knowledge through explicit artifacts, not by
copying implementation details between repos.

| Knowledge | Authority | Consumer |
|---|---|---|
| Wire message layout | `Viewer2000/docs/wire-spec.md` | Firmware, Scope2000 codec |
| Shared interface constants and structs | `Viewer2000/contracts/` | CPU1, CPU2, tests, Scope2000 model |
| Protocol examples | `Viewer2000/contracts/vectors/` | Scope2000 `tests/vectors/` |
| Bring-up procedure | Viewer2000 `docs/phase*.md` | Firmware work, Scope2000 hardware validation |
| Host UX and source abstraction | Scope2000 README/source docs | Host application work |
| Private constraints and reference notes | `_private/` | Local human context only |

If a fact affects protocol behavior, it belongs in `wire-spec.md`, contracts,
or vectors. If a fact affects hardware bring-up, it belongs in a Viewer2000
phase document. If a fact affects host UI behavior without changing the wire
contract, it belongs in Scope2000.

## Update Flow

1. Change the Viewer2000 wire spec or contracts first.
2. Regenerate Viewer2000 golden vectors.
3. Run Viewer2000 contract/vector checks.
4. Sync vectors into Scope2000.
5. Update Scope2000 codec/source behavior.
6. Run Scope2000 format, Clippy, tests, and brand scan.

Compatibility with older firmware remains outside the native hot path. A future
`LegacyBridge` may translate older devices into Viewer2000 message semantics,
but the bridge must live out of process and must not define or weaken the
native protocol.
