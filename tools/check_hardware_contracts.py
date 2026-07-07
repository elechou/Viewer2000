#!/usr/bin/env python3
"""Validate profile-owned hardware contracts against build inputs.

The checker is target-neutral. Every device fact, instance count, SysConfig
value, and linker invariant comes from the selected profile's TOML files.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_SCHEMA = "viewer2000.board-profile.v2"
CONTRACT_SCHEMA = "viewer2000.hardware-contract.v1"

INSTANCE_RE = re.compile(
    r"^\s*const\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\.addInstance\(\);\s*$"
)
ASSIGNMENT_RE = re.compile(
    r"^\s*([A-Za-z_$][A-Za-z0-9_.$]*)\s*=\s*(.+);\s*$"
)


def load_toml(path: Path) -> dict[str, Any]:
    try:
        return tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc


def repo_path(root: Path, value: str) -> Path:
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError(f"contract paths must be repository-relative: {value}")
    return root / path


def canonical(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, list):
        return json.dumps(value, separators=(",", ":"), ensure_ascii=True)
    if isinstance(value, str):
        return re.sub(r"\s+", "", value)
    raise ValueError(f"unsupported contract value: {value!r}")


def canonical_js(raw: str) -> str:
    text = raw.strip()
    if text.startswith(('"', "[")):
        try:
            return canonical(json.loads(text))
        except json.JSONDecodeError:
            pass
    return re.sub(r"\s+", "", text)


def parse_sysconfig(path: Path) -> tuple[dict[str, str], dict[str, list[str]]]:
    assignments: dict[str, str] = {}
    instances: dict[str, list[str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        instance = INSTANCE_RE.match(line)
        if instance:
            name, module = instance.groups()
            instances.setdefault(module, []).append(name)
            continue
        assignment = ASSIGNMENT_RE.match(line)
        if assignment:
            key, value = assignment.groups()
            assignments[key] = canonical_js(value)
    for names in instances.values():
        names.sort()
    return assignments, instances


def expanded_keys(pattern: str, instances: dict[str, list[str]]) -> list[str]:
    marker = "*."
    if marker not in pattern:
        return [pattern]
    module, suffix = pattern.split(marker, 1)
    names = instances.get(module, [])
    if not names:
        raise ValueError(f"{pattern}: no {module} instances exist")
    return [f"{name}.{suffix}" for name in names]


def default_for(key: str, pattern: str, defaults: dict[str, Any]) -> Any | None:
    if key in defaults:
        return defaults[key]
    return defaults.get(pattern)


def check_sysconfig(
    root: Path,
    table: dict[str, Any],
    errors: list[str],
) -> None:
    path_value = table.get("file")
    if not isinstance(path_value, str):
        errors.append("hardware contract: sysconfig.file must be a string")
        return
    try:
        path = repo_path(root, path_value)
    except ValueError as exc:
        errors.append(str(exc))
        return
    if not path.is_file():
        errors.append(f"hardware contract: missing SysConfig input {path_value}")
        return

    assignments, instances = parse_sysconfig(path)
    counts = table.get("instance_counts", {})
    for module, expected in counts.items():
        actual = len(instances.get(module, []))
        if actual != expected:
            errors.append(
                f"{path_value}: {module} instance count is {actual}; expected {expected}"
            )

    required = table.get("required", {})
    defaults = table.get("defaults", {})
    for pattern, expected_value in required.items():
        try:
            keys = expanded_keys(pattern, instances)
        except ValueError as exc:
            errors.append(f"{path_value}: {exc}")
            continue
        expected = canonical(expected_value)
        for key in keys:
            actual = assignments.get(key)
            source = "SysConfig"
            if actual is None:
                default = default_for(key, pattern, defaults)
                if default is not None:
                    actual = canonical(default)
                    source = "reviewed default"
            if actual is None:
                errors.append(f"{path_value}: missing required assignment {key}")
            elif actual != expected:
                errors.append(
                    f"{path_value}: {key} is {actual!r} from {source}; "
                    f"expected {expected!r}"
                )


def check_pattern_table(
    root: Path,
    table: dict[str, Any],
    required: bool,
    errors: list[str],
) -> None:
    for path_value, patterns in table.items():
        try:
            path = repo_path(root, path_value)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        if not path.is_file():
            errors.append(f"hardware contract: missing input {path_value}")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in patterns:
            found = re.search(pattern, text, re.MULTILINE) is not None
            if required and not found:
                errors.append(f"{path_value}: required pattern not found: {pattern}")
            if not required and found:
                errors.append(f"{path_value}: forbidden pattern found: {pattern}")


def check_contract(manifest_path: Path, root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    try:
        manifest = load_toml(manifest_path)
    except ValueError as exc:
        return [str(exc)]
    if manifest.get("schema") != MANIFEST_SCHEMA:
        return [
            f"{manifest_path}: hardware contracts require manifest schema "
            f"{MANIFEST_SCHEMA}"
        ]
    if manifest.get("path_base") != "repository":
        return [f"{manifest_path}: path_base must be repository"]
    contract_value = manifest.get("hardware_contract")
    if not isinstance(contract_value, str):
        return [f"{manifest_path}: hardware_contract must be a string"]
    try:
        contract_path = repo_path(root, contract_value)
        contract = load_toml(contract_path)
    except ValueError as exc:
        return [str(exc)]

    if contract.get("schema") != CONTRACT_SCHEMA:
        errors.append(f"{contract_value}: unsupported hardware contract schema")
    if contract.get("profile") != manifest.get("id"):
        errors.append(
            f"{contract_value}: profile does not match manifest id "
            f"{manifest.get('id')!r}"
        )

    sysconfig = contract.get("sysconfig")
    if isinstance(sysconfig, dict):
        check_sysconfig(root, sysconfig, errors)
    else:
        errors.append(f"{contract_value}: missing [sysconfig] table")

    linker = contract.get("linker", {})
    if isinstance(linker, dict):
        check_pattern_table(root, linker.get("required", {}), True, errors)
        check_pattern_table(root, linker.get("forbidden", {}), False, errors)
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args(argv)
    manifest = args.manifest.resolve()
    errors = check_contract(manifest)
    if errors:
        print("Hardware contract check failed:")
        for error in errors:
            print(f"  {error}")
        return 1
    print(f"Hardware contract check passed: {manifest.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
