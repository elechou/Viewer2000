# Viewer2000

Rapid Control Prototyping Platform on F28P65x.

This repository contains only the dual-core C2000 firmware. The Rust/egui host
application lives in the independent sibling repository `Scope2000`.

The protocol authority is [docs/wire-spec.md](docs/wire-spec.md) together with
the headers under `contracts/` and the golden vectors under
`contracts/vectors/`. Compatibility with older devices belongs in an external
bridge process and must not alter the native Viewer2000 protocol or data path.
