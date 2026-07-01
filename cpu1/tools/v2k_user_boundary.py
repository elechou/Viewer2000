#!/usr/bin/env python3
"""Generate and verify the CPU1 user-code linker boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


OUTPUT_SECTIONS = {
    "text": "v2k_user_text",
    "const": "v2k_user_const",
    "data": "v2k_user_data",
    "bss": "v2k_user_bss",
}

UNSUPPORTED_NAME_RE = re.compile(
    r"(^|[.:_])(noinit|persistent|retain|shared|cla)([.:_]|$)", re.IGNORECASE
)


class BoundaryError(RuntimeError):
    pass


@dataclass(frozen=True)
class InputSection:
    name: str
    kind: str
    size_bytes: int


@dataclass(frozen=True)
class ObjectInfo:
    selector: str
    path: Path
    sections: tuple[InputSection, ...]


def parse_int(text: str | None) -> int:
    if text is None:
        return 0
    return int(text, 0)


def child_text(node: ET.Element, name: str) -> str | None:
    child = node.find(name)
    return child.text.strip() if child is not None and child.text else None


def find_ofd() -> Path:
    cg_root = os.environ.get("CG_TOOL_ROOT")
    candidates: list[Path] = []
    executable_names = ("ofd2000.exe", "ofd2000")
    if cg_root:
        candidates.extend(Path(cg_root) / "bin" / name for name in executable_names)
    compiler_roots = [
        Path("C:/ti/ccs2100/ccs/tools/compiler"),
        Path("/Applications/ti/ccs2100/ccs/tools/compiler"),
    ]
    ccs_install_dir = os.environ.get("CCS_INSTALL_DIR")
    if ccs_install_dir:
        compiler_roots.insert(0, Path(ccs_install_dir) / "ccs" / "tools" / "compiler")
    for compiler_root in compiler_roots:
        for compiler_dir in sorted(
            compiler_root.glob("ti-cgt-c2000_*"), reverse=True
        ):
            candidates.extend(compiler_dir / "bin" / name for name in executable_names)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise BoundaryError("ofd2000 not found; set CG_TOOL_ROOT to the C2000 compiler")


def run_ofd(path: Path) -> ET.Element:
    result = subprocess.run(
        [str(find_ofd()), "--xml", str(path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise BoundaryError(f"ofd2000 failed for {path}: {message}")
    try:
        return ET.fromstring(result.stdout)
    except ET.ParseError as exc:
        raise BoundaryError(f"invalid ofd2000 XML for {path}: {exc}") from exc


def classify_section(header: ET.Element, object_name: str) -> InputSection | None:
    name = child_text(header, "sh_name_string") or ""
    section_type = child_text(header, "sh_type") or ""
    flags = {flag.text for flag in header.findall("./sh_flags/flag") if flag.text}
    size_bytes = parse_int(child_text(header, "sh_size"))
    address = parse_int(child_text(header, "sh_addr"))

    if "SHF_ALLOC" not in flags or size_bytes == 0:
        return None
    if address != 0:
        raise BoundaryError(
            f"{object_name}:{name}: absolute-address user section is unsupported"
        )
    if UNSUPPORTED_NAME_RE.search(name):
        raise BoundaryError(
            f"{object_name}:{name}: retained/NOINIT/shared/CLA user state is unsupported"
        )
    if "SHF_EXECINSTR" in flags:
        kind = "text"
    elif "SHF_WRITE" in flags:
        if section_type == "SHT_NOBITS":
            if not (name.startswith(".bss") or name in {"COMMON", ".common"}):
                raise BoundaryError(
                    f"{object_name}:{name}: unsupported custom uninitialized user section"
                )
            kind = "bss"
        else:
            if not name.startswith(".data"):
                raise BoundaryError(
                    f"{object_name}:{name}: unsupported custom writable user section"
                )
            kind = "data"
    else:
        kind = "const"
    return InputSection(name=name, kind=kind, size_bytes=size_bytes)


def inspect_object(path: Path, selector: str, archive_member: str | None = None) -> ObjectInfo:
    root = run_ofd(path)
    object_files = root.findall("./object_file")
    if archive_member is not None:
        object_files = [
            node
            for node in object_files
            if Path(child_text(node, "name") or "").name == archive_member
        ]
        if not object_files:
            raise BoundaryError(f"{path}: archive member {archive_member!r} not found")
    elif len(object_files) != 1:
        raise BoundaryError(f"{path}: expected one object, found {len(object_files)}")

    sections: list[InputSection] = []
    for object_file in object_files:
        for section in object_file.findall("./elf/section_table/section"):
            header = section.find("elf32_shdr")
            if header is None:
                continue
            classified = classify_section(header, selector)
            if classified is not None:
                sections.append(classified)

    deduplicated = {(item.name, item.kind): item for item in sections}
    return ObjectInfo(
        selector=selector,
        path=path,
        sections=tuple(sorted(deduplicated.values(), key=lambda item: (item.kind, item.name))),
    )


def load_spec(path: Path) -> dict:
    try:
        spec = json.loads(path.read_text(encoding="ascii"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BoundaryError(f"cannot read {path}: {exc}") from exc
    if spec.get("version") != 1:
        raise BoundaryError(f"{path}: unsupported boundary version")
    for key in ("source_roots", "extra_objects", "archive_members"):
        if not isinstance(spec.get(key), list):
            raise BoundaryError(f"{path}: {key} must be a list")
    return spec


def discover_objects(spec: dict, project_root: Path, build_dir: Path) -> list[ObjectInfo]:
    selectors: set[str] = set()
    for source_root in spec["source_roots"]:
        source_path = project_root / source_root
        if not source_path.is_dir():
            raise BoundaryError(f"user source root does not exist: {source_path}")
        for source in source_path.rglob("*.c"):
            relative = source.relative_to(project_root).with_suffix(".obj")
            selectors.add(relative.as_posix())
    selectors.update(str(value) for value in spec["extra_objects"])

    objects: list[ObjectInfo] = []
    for selector in sorted(selectors):
        path = build_dir / selector
        if not path.is_file():
            raise BoundaryError(f"user object is missing from this build: {selector}")
        objects.append(inspect_object(path, selector))

    for entry in spec["archive_members"]:
        if not isinstance(entry, dict) or set(entry) != {"archive", "member"}:
            raise BoundaryError("archive_members entries require archive and member")
        archive_text = str(entry["archive"])
        archive = Path(archive_text)
        if not archive.is_absolute():
            archive = project_root / archive
        member = str(entry["member"])
        if not archive.is_file():
            raise BoundaryError(f"user archive does not exist: {archive}")
        selector = f"{archive_text}<{member}>"
        objects.append(inspect_object(archive, selector, member))

    if not objects:
        raise BoundaryError("the user-object set is empty")
    return objects


def selector_line(obj: ObjectInfo, section: InputSection) -> str:
    selector = obj.selector if "<" in obj.selector else f'"./{obj.selector}"'
    return f"        {selector}({section.name})"


def section_lines(objects: Iterable[ObjectInfo], kind: str) -> list[str]:
    lines: list[str] = []
    for obj in objects:
        patterns: set[str] = set()
        names = {section.name for section in obj.sections if section.kind == kind}
        for section in obj.sections:
            if section.kind != kind:
                continue
            if ":" in section.name:
                base = section.name.split(":", 1)[0]
                if base not in names:
                    patterns.add(base + ":*")
            else:
                patterns.add(section.name)
        for pattern in sorted(patterns):
            lines.append(selector_line(obj, InputSection(pattern, kind, 0)))
    return lines


def render_fragment(objects: list[ObjectInfo], config: str) -> tuple[str, bool]:
    config_upper = config.upper()
    if config_upper not in {"RAM", "FLASH"}:
        raise BoundaryError(f"unsupported CPU1 configuration: {config}")

    groups = {kind: section_lines(objects, kind) for kind in OUTPUT_SECTIONS}
    has_data = bool(groups["data"])
    if config_upper == "RAM":
        text_place = ">> RAMD0 | RAMD1 | RAMLS0 | RAMLS1 | RAMLS2 | RAMLS3"
        const_place = "> USER_CONST_RAM"
        data_load = "USER_GOLDEN_RAM"
    else:
        text_place = ">> FLASH_BANK0 | FLASH_BANK1, ALIGN(8)"
        # START/END/SIZE remain undefined for a section split with >> across
        # multiple FLASH regions. Keep user const in one CPU1-owned bank so
        # the verifier and descriptor baker receive an authoritative range.
        const_place = "> FLASH_BANK1, ALIGN(8)"
        # Keep the reset golden image with the other user-owned flash assets.
        data_load = "FLASH_BANK1"

    def body(kind: str) -> str:
        return "\n".join(groups[kind])

    crc_clause = (
        ",\n        crc_table(V2K_UserDataCrcTable, algorithm = CRC32_PRIME)"
        if has_data
        else ""
    )
    absent_crc_symbol = "" if has_data else "V2K_UserDataCrcTable = 0;\n"
    fragment = f"""/* Generated by v2k_user_boundary.py; do not edit. */
SECTIONS
{{
    v2k_user_text :
    {{
{body('text')}
    }} {text_place}

    v2k_user_const :
    {{
{body('const')}
    }} {const_place},
        START(V2K_UserConstStart),
        END(V2K_UserConstEnd),
        SIZE(V2K_UserConstSize)

    v2k_user_data :
    {{
{body('data')}
    }} LOAD = {data_load},
        RUN = USER_RUN,
        ALIGN(2),
        LOAD_START(V2K_UserDataLoadStart),
        LOAD_END(V2K_UserDataLoadEnd),
        LOAD_SIZE(V2K_UserDataLoadSize),
        RUN_START(V2K_UserDataRunStart),
        RUN_END(V2K_UserDataRunEnd),
        RUN_SIZE(V2K_UserDataRunSize){crc_clause}

    v2k_user_bss :
    {{
{body('bss')}
    }} > USER_RUN, TYPE = NOINIT, ALIGN(2),
        START(V2K_UserBssStart),
        END(V2K_UserBssEnd),
        SIZE(V2K_UserBssSize)
}}

V2K_UserDataCrcPresent = {1 if has_data else 0};
V2K_UserDataStart = V2K_UserDataRunStart;
V2K_UserDataEnd = V2K_UserDataRunEnd;
{absent_crc_symbol}"""
    return fragment, has_data


def manifest_dict(spec_path: Path, objects: list[ObjectInfo], config: str, has_data: bool) -> dict:
    object_entries = []
    for obj in objects:
        section_map = {kind: [] for kind in OUTPUT_SECTIONS}
        for section in obj.sections:
            section_map[section.kind].append(section.name)
        object_entries.append({"selector": obj.selector, "sections": section_map})
    ownership = json.dumps(object_entries, sort_keys=True, separators=(",", ":"))
    return {
        "version": 1,
        "configuration": config.upper(),
        "source": str(spec_path),
        "source_sha256": hashlib.sha256(spec_path.read_bytes()).hexdigest(),
        "ownership_sha256": hashlib.sha256(ownership.encode("ascii")).hexdigest(),
        "has_user_data": has_data,
        "objects": object_entries,
    }


def write_always(path: Path, content: str) -> None:
    # These files are make targets. Refresh timestamps even when classification
    # is unchanged so newer objects/tools do not rerun generation indefinitely.
    path.write_text(content, encoding="ascii")


def command_generate(args: argparse.Namespace) -> None:
    project_root = args.project_root.resolve()
    build_dir = args.build_dir.resolve()
    spec_path = args.spec.resolve()
    spec = load_spec(spec_path)
    objects = discover_objects(spec, project_root, build_dir)
    fragment, has_data = render_fragment(objects, args.config)
    manifest = manifest_dict(spec_path, objects, args.config, has_data)
    write_always(args.output, fragment)
    write_always(
        args.manifest,
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
    )
    counts = {
        kind: sum(1 for obj in objects for section in obj.sections if section.kind == kind)
        for kind in OUTPUT_SECTIONS
    }
    print(
        f"user boundary: {len(objects)} objects; "
        + ", ".join(f"{kind}={counts[kind]}" for kind in OUTPUT_SECTIONS)
    )


def parse_link_info(path: Path) -> ET.Element:
    try:
        return ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        raise BoundaryError(f"cannot parse linker XML {path}: {exc}") from exc


def link_symbols(root: ET.Element) -> dict[str, int]:
    result: dict[str, int] = {}
    for symbol in root.findall("./symbol_table/symbol"):
        name = child_text(symbol, "name")
        value = child_text(symbol, "value")
        if name and value:
            result[name] = parse_int(value)
    return result


def memory_areas(root: ET.Element) -> dict[str, tuple[int, int]]:
    result: dict[str, tuple[int, int]] = {}
    for area in root.findall("./placement_map/memory_area"):
        name = child_text(area, "name")
        if name:
            result[name] = (
                parse_int(child_text(area, "origin")),
                parse_int(child_text(area, "length")),
            )
    return result


def in_area(start: int, size: int, area: tuple[int, int]) -> bool:
    origin, length = area
    return start >= origin and start + size <= origin + length


def logical_group_ranges(root: ET.Element) -> dict[str, tuple[int, int]]:
    result: dict[str, tuple[int, int]] = {}
    for group in root.findall("./logical_group_list/logical_group"):
        name = child_text(group, "name")
        address = child_text(group, "load_address")
        size = child_text(group, "size")
        if name and address and size:
            result[name] = (parse_int(address), parse_int(size))
    return result


def ranges_overlap(first: tuple[int, int], second: tuple[int, int]) -> bool:
    first_start, first_size = first
    second_start, second_size = second
    return (
        first_size > 0
        and second_size > 0
        and first_start < second_start + second_size
        and second_start < first_start + first_size
    )


def component_outputs(root: ET.Element) -> tuple[dict[str, str], dict[str, dict[str, str]]]:
    components: dict[str, dict[str, str]] = {}
    for component in root.findall("./object_component_list/object_component"):
        component_id = component.get("id")
        if component_id:
            components[component_id] = {
                "name": child_text(component, "name") or "",
                "file": (component.find("input_file_ref").get("idref")
                         if component.find("input_file_ref") is not None else ""),
            }

    outputs: dict[str, str] = {}
    for group in root.findall("./logical_group_list/logical_group"):
        output_name = child_text(group, "name") or ""
        for ref in group.findall("./contents/object_component_ref"):
            component_id = ref.get("idref")
            if component_id:
                outputs[component_id] = output_name
    return outputs, components


def input_files(root: ET.Element) -> dict[str, str]:
    result: dict[str, str] = {}
    for input_file in root.findall("./input_file_list/input_file"):
        file_id = input_file.get("id")
        path = child_text(input_file, "path") or ""
        file_name = child_text(input_file, "file") or child_text(input_file, "name") or ""
        kind = child_text(input_file, "kind") or "object"
        member = child_text(input_file, "name") or ""
        if file_id:
            actual = (Path(path) / file_name).as_posix()
            if kind == "archive":
                actual += f"<{member}>"
            result[file_id] = actual
    return result


def normalize_selector_path(path: str) -> str:
    normalized = path.replace("\\", "/")
    while normalized.startswith("./"):
        normalized = normalized[2:]
    return normalized


def split_archive_selector(selector: str) -> tuple[str, str | None]:
    path, separator, member = selector.partition("<")
    if not separator:
        return path, None
    return path, member.rstrip(">")


def path_tail_matches(selector_path: str, actual_path: str) -> bool:
    selector_norm = normalize_selector_path(selector_path)
    actual_norm = normalize_selector_path(actual_path)
    if actual_norm == selector_norm:
        return True
    return actual_norm.endswith("/" + selector_norm)


def selector_matches(selector: str, actual_path: str) -> bool:
    selector_path, selector_member = split_archive_selector(selector)
    actual_file, actual_member = split_archive_selector(actual_path)
    if selector_member is not None or actual_member is not None:
        return selector_member == actual_member and path_tail_matches(selector_path, actual_file)
    return path_tail_matches(selector_path, actual_path)


def verify_ownership(root: ET.Element, manifest: dict) -> list[str]:
    errors: list[str] = []
    outputs, components = component_outputs(root)
    files = input_files(root)
    expected: dict[str, dict[str, str]] = {}
    for obj in manifest["objects"]:
        expected[obj["selector"]] = {
            section_name: OUTPUT_SECTIONS[kind]
            for kind, names in obj["sections"].items()
            for section_name in names
        }

    for component_id, component in components.items():
        actual_file = files.get(component["file"], "")
        selector = next(
            (candidate for candidate in expected if selector_matches(candidate, actual_file)),
            None,
        )
        output = outputs.get(component_id, "")
        if selector is not None and component["name"] in expected[selector]:
            wanted = expected[selector][component["name"]]
            if not output.startswith(wanted):
                errors.append(
                    f"{selector}:{component['name']} linked into {output or '<discarded>'}; expected {wanted}"
                )
        elif any(output.startswith(section) for section in OUTPUT_SECTIONS.values()) and selector is None:
            errors.append(
                f"platform object {actual_file}:{component['name']} contaminates {output}"
            )
    return errors


def verify_layout(root: ET.Element, manifest: dict, map_text: str) -> list[str]:
    errors: list[str] = []
    symbols = link_symbols(root)
    areas = memory_areas(root)
    groups = logical_group_ranges(root)
    required = (
        "V2K_UserDataLoadStart",
        "V2K_UserDataLoadEnd",
        "V2K_UserDataLoadSize",
        "V2K_UserDataRunStart",
        "V2K_UserDataRunEnd",
        "V2K_UserDataRunSize",
        "V2K_UserBssStart",
        "V2K_UserBssEnd",
        "V2K_UserBssSize",
        "V2K_UserDataCrcPresent",
        "V2K_UserConstStart",
        "V2K_UserConstEnd",
        "V2K_UserConstSize",
    )
    missing = [name for name in required if name not in symbols]
    if missing:
        return ["missing linker symbols: " + ", ".join(missing)]

    load_start = symbols["V2K_UserDataLoadStart"]
    load_end = symbols["V2K_UserDataLoadEnd"]
    load_size = symbols["V2K_UserDataLoadSize"]
    run_start = symbols["V2K_UserDataRunStart"]
    run_end = symbols["V2K_UserDataRunEnd"]
    run_size = symbols["V2K_UserDataRunSize"]
    bss_start = symbols["V2K_UserBssStart"]
    bss_end = symbols["V2K_UserBssEnd"]
    bss_size = symbols["V2K_UserBssSize"]
    const_start = symbols["V2K_UserConstStart"]
    const_end = symbols["V2K_UserConstEnd"]
    const_size = symbols["V2K_UserConstSize"]

    if load_size != run_size or load_end - load_start != load_size:
        errors.append("user data LOAD range/size does not match RUN size")
    if run_end - run_start != run_size:
        errors.append("user data RUN range does not match RUN size")
    if bss_end - bss_start != bss_size:
        errors.append("user BSS range does not match BSS size")
    if const_end - const_start != const_size:
        errors.append("user const range does not match const size")
    if run_size + bss_size > areas.get("USER_RUN", (0, 0))[1]:
        errors.append("user data+BSS exceeds USER_RUN")
    if run_size and not in_area(run_start, run_size, areas.get("USER_RUN", (0, 0))):
        errors.append("user data RUN lies outside USER_RUN")
    if bss_size and not in_area(bss_start, bss_size, areas.get("USER_RUN", (0, 0))):
        errors.append("user BSS lies outside USER_RUN")

    config = manifest["configuration"]
    load_area_name = "USER_GOLDEN_RAM" if config == "RAM" else "FLASH_BANK1"
    if load_size and not in_area(load_start, load_size, areas.get(load_area_name, (0, 0))):
        errors.append(f"user data LOAD lies outside {load_area_name}")
    if run_size and not (load_end <= run_start or run_end <= load_start):
        errors.append("user data LOAD and RUN overlap")
    if run_size and bss_size and not (run_end <= bss_start or bss_end <= run_start):
        errors.append("user data RUN and BSS overlap")
    if config == "RAM" and const_size and not in_area(
        const_start, const_size, areas.get("USER_CONST_RAM", (0, 0))
    ):
        errors.append("user const lies outside USER_CONST_RAM")

    crc_present = symbols["V2K_UserDataCrcPresent"]
    if bool(crc_present) != bool(manifest["has_user_data"]):
        errors.append("CRC-present symbol disagrees with the generated manifest")
    if manifest["has_user_data"] and "V2K_UserDataCrcTable" not in symbols:
        errors.append("linker-generated user-data CRC table is missing")
    crc_tables = [
        table
        for table in root.findall("./crc_table_list/crc_table")
        if child_text(table, "name") == "V2K_UserDataCrcTable"
    ]
    if manifest["has_user_data"]:
        records = crc_tables[0].findall("crc_rec") if len(crc_tables) == 1 else []
        if len(records) != 1:
            errors.append("user data must have exactly one linker CRC record")
        else:
            record = records[0]
            if (
                child_text(record, "alg_name") != "CRC32_PRIME"
                or parse_int(child_text(record, "load_address")) != load_start
                or parse_int(child_text(record, "load_size")) != load_size
            ):
                errors.append("linker CRC record does not match user data LOAD")
    elif crc_tables:
        errors.append("zero-length user data unexpectedly has a CRC table")

    if config == "FLASH":
        flash_area_name = "FLASH_BANK1"
        flash_area = areas.get(flash_area_name, (0, 0))
        protected_ranges = {"user data golden": (load_start, load_size)}
        for group_name, label in (
            ("v2k_user_desc", "user descriptor blob"),
            (".TI.crctab", "user CRC table"),
        ):
            group_range = groups.get(group_name)
            if group_range is None:
                errors.append(f"missing {label} load range")
            else:
                protected_ranges[label] = group_range
                if not in_area(group_range[0], group_range[1], flash_area):
                    errors.append(f"{label} lies outside {flash_area_name}")
        crc_range = groups.get(".TI.crctab")
        if crc_range is not None and symbols.get("V2K_UserDataCrcTable") != crc_range[0]:
            errors.append("user CRC table symbol does not match its load range")
        labels = tuple(protected_ranges)
        for index, first_label in enumerate(labels):
            for second_label in labels[index + 1 :]:
                if ranges_overlap(
                    protected_ranges[first_label], protected_ranges[second_label]
                ):
                    errors.append(f"{first_label} overlaps {second_label}")
    if re.search(r"\.cinit\.v2k_user_(data|bss)\.load", map_text):
        errors.append("user data/BSS is still initialized through .cinit")
    return errors


def command_verify(args: argparse.Namespace) -> None:
    manifest = json.loads(args.manifest.read_text(encoding="ascii"))
    source_path = Path(manifest["source"])
    if not source_path.is_file() or hashlib.sha256(source_path.read_bytes()).hexdigest() != manifest.get(
        "source_sha256"
    ):
        raise BoundaryError("normalized manifest is stale relative to user_boundary.json")
    root = parse_link_info(args.link_info)
    map_text = args.map.read_text(encoding="ascii", errors="replace")
    if not args.elf.is_file():
        raise BoundaryError(f"final ELF is missing: {args.elf}")
    errors = verify_ownership(root, manifest)
    errors.extend(verify_layout(root, manifest, map_text))
    if errors:
        raise BoundaryError("boundary verification failed:\n  " + "\n  ".join(errors))
    print(
        "user boundary verified: "
        f"{len(manifest['objects'])} objects, {manifest['configuration']}, "
        f"ownership={manifest['ownership_sha256'][:12]}"
    )


def command_normalize_codestart(args: argparse.Namespace) -> None:
    source = args.source
    output = args.output
    if source.is_file() and (
        not output.is_file() or source.read_bytes() != output.read_bytes()
    ):
        shutil.copy2(source, output)
    if not output.is_file():
        raise BoundaryError(
            f"code-start object is missing from both {source} and {output}"
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser("generate")
    generate.add_argument("--project-root", type=Path, required=True)
    generate.add_argument("--build-dir", type=Path, required=True)
    generate.add_argument("--config", required=True)
    generate.add_argument("--spec", type=Path, required=True)
    generate.add_argument("--output", type=Path, required=True)
    generate.add_argument("--manifest", type=Path, required=True)
    generate.set_defaults(func=command_generate)

    normalize_codestart = subparsers.add_parser("normalize-codestart")
    normalize_codestart.add_argument("--source", type=Path, required=True)
    normalize_codestart.add_argument("--output", type=Path, required=True)
    normalize_codestart.set_defaults(func=command_normalize_codestart)

    verify = subparsers.add_parser("verify")
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--link-info", type=Path, required=True)
    verify.add_argument("--map", type=Path, required=True)
    verify.add_argument("--elf", type=Path, required=True)
    verify.set_defaults(func=command_verify)
    return parser


def main() -> int:
    try:
        args = build_parser().parse_args()
        args.func(args)
        return 0
    except BoundaryError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
