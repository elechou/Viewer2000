#!/usr/bin/env python3
"""Check board-portability seams and public profile manifests."""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

from check_hardware_contracts import check_contract


ROOT = Path(__file__).resolve().parents[1]

CPU1_RUNTIME_GLOBS = [
    "cpu1/runtime/*.c",
    "cpu1/runtime/*.h",
]

# cpu2.c is the portable CPU2 super-loop orchestration: it owns the heartbeat/IPC
# policy and drives v2k_sci_service, while all vendor register access (NMI, IPC,
# SCI pipe) lives behind the CPU2 board seam. It must stay vendor-free, symmetric
# with cpu1/runtime/*.
CPU2_PORTABLE_FILES = [
    "cpu2/cpu2.c",
    "cpu2/v2k_sci_service.c",
    "cpu2/v2k_sci_service.h",
    "common/v2k_planes.h",
    "common/v2k_scope_consumer.h",
]

VENDOR_PATTERNS = [
    re.compile(r'#\s*include\s+[<"](?:driverlib|device|board)\.h[">]'),
    re.compile(
        r"\b(?:Device|SysCtl|Interrupt|IPC|MemCfg|GPIO|SCI|ADC|EPWM|CMPSS|"
        r"XBAR|CPUTimer)_[A-Za-z0-9_]+\s*\("
    ),
    re.compile(
        r"\b(?:ESTOP0|EINT|ERTM|HWREG|HWREGH|SCIA_BASE|INT_SCIA_RX|"
        r"DEVICE_DELAY_US|BOOTMODE_[A-Z0-9_]+)\b"
    ),
]

REQUIRED_MANIFEST_FIELDS = {
    "schema",
    "id",
    "visibility",
    "core",
    "api_version",
    "cpu_topology",
    "device",
    "board",
    "path_base",
    "hardware_contract",
    "profile_header",
    "memory_map_header",
    "capabilities",
    "artifacts",
}


def strip_comments(text: str) -> str:
    out: list[str] = []
    i = 0
    in_block = False
    while i < len(text):
        if in_block:
            if text.startswith("*/", i):
                in_block = False
                i += 2
            else:
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            continue
        if text.startswith("/*", i):
            in_block = True
            i += 2
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            if j == -1:
                break
            out.append("\n")
            i = j + 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def check_no_vendor_access(path: Path, errors: list[str]) -> None:
    text = strip_comments(path.read_text(encoding="utf-8"))
    for lineno, line in enumerate(text.splitlines(), start=1):
        for pattern in VENDOR_PATTERNS:
            if pattern.search(line):
                errors.append(f"{path.relative_to(ROOT)}:{lineno}: {line.strip()}")


def existing_path(value: str, manifest: Path, errors: list[str]) -> None:
    # "none" marks an artifact a build-only profile deliberately omits (a
    # null-loopback build generates no board.c and is never flashed/debugged).
    if value == "none":
        return
    path = ROOT / value
    if not path.exists():
        errors.append(
            f"{manifest.relative_to(ROOT)}: referenced path does not exist: {value}"
        )


def check_manifest(path: Path, errors: list[str]) -> None:
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # pragma: no cover - diagnostic path
        errors.append(f"{path.relative_to(ROOT)}: invalid TOML: {exc}")
        return

    missing = sorted(REQUIRED_MANIFEST_FIELDS - set(data))
    if missing:
        errors.append(f"{path.relative_to(ROOT)}: missing fields: {', '.join(missing)}")
        return

    if data["schema"] != "viewer2000.board-profile.v2":
        errors.append(f"{path.relative_to(ROOT)}: unsupported schema {data['schema']}")
    if data["visibility"] != "public":
        errors.append(f"{path.relative_to(ROOT)}: public mainline profiles must be public")
    if data["core"] not in {"cpu1", "cpu2"}:
        errors.append(f"{path.relative_to(ROOT)}: core must be cpu1 or cpu2")
    expected_api = {"cpu1": 2, "cpu2": 1}.get(data["core"])
    if data["api_version"] != expected_api:
        errors.append(
            f"{path.relative_to(ROOT)}: api_version must be {expected_api} "
            f"for {data['core']}"
        )
    if data["path_base"] != "repository":
        errors.append(f"{path.relative_to(ROOT)}: path_base must be repository")

    existing_path(data["profile_header"], path, errors)
    existing_path(data["memory_map_header"], path, errors)
    existing_path(data["hardware_contract"], path, errors)

    artifacts = data["artifacts"]
    existing_path(artifacts["sysconfig"], path, errors)
    existing_path(artifacts["target_configuration"], path, errors)
    for linker in artifacts["linker_command_files"]:
        existing_path(linker, path, errors)
    if not artifacts["board_sources"]:
        errors.append(
            f"{path.relative_to(ROOT)}: board_sources must not be empty "
            "(a profile must bring its own board implementation)"
        )
    for source in artifacts["board_sources"]:
        existing_path(source, path, errors)
    for source in artifacts.get("app_sources", []):
        existing_path(source, path, errors)

    errors.extend(check_contract(path, ROOT))


def main() -> int:
    errors: list[str] = []

    for pattern in CPU1_RUNTIME_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            check_no_vendor_access(path, errors)

    for rel in CPU2_PORTABLE_FILES:
        check_no_vendor_access(ROOT / rel, errors)

    manifests = sorted(ROOT.glob("cpu*/board/profiles/*/manifest.toml"))
    if not manifests:
        errors.append("no board profile manifests found")
    for manifest in manifests:
        check_manifest(manifest, errors)

    if errors:
        print("Board seam check failed:")
        for error in errors:
            print(f"  {error}")
        return 1
    print("Board seam check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
