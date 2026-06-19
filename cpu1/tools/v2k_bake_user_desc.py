#!/usr/bin/env python3
"""Bake DWARF-derived CPU1 user descriptors into the linked firmware image."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass
from pathlib import Path


NAME_LEN = 16
USER_CAPACITY = 96
USER_MAGIC = 0x564B5544
USER_VERSION = 2
BLOB_HEADER_FORMAT = "<IHHHHI"
BLOB_HEADER_SIZE = struct.calcsize(BLOB_HEADER_FORMAT)
ENTRY_SIZE = 44
KIND_PARAM = 1
KIND_SCOPE = 2

TYPE_I16 = 0
TYPE_U16 = 1
TYPE_I32 = 2
TYPE_U32 = 3
TYPE_F32 = 4
TYPE_NAMES = ("I16", "U16", "I32", "U32", "F32")

WRAPPER_TAGS = {
    "DW_TAG_typedef",
    "DW_TAG_const_type",
    "DW_TAG_volatile_type",
    "DW_TAG_restrict_type",
    "DW_TAG_TI_far_type",
}
ADDR_RE = re.compile(r"^DW_OP_addr\s+(0x[0-9a-fA-F]+)$")
MEMBER_RE = re.compile(r"^DW_OP_plus_uconst\s+(0x[0-9a-fA-F]+|[0-9]+)$")


class BakeError(RuntimeError):
    pass


@dataclass(frozen=True)
class AddressRange:
    name: str
    start: int
    end: int
    mutable: bool

    def contains(self, address: int, words: int = 1) -> bool:
        return self.start <= address and address + words <= self.end


@dataclass(frozen=True)
class Descriptor:
    name: str
    type: int
    kind: int
    addr: int
    prescaler: int = 1
    reserved: int = 0
    storage: str = ""


@dataclass(frozen=True)
class Skipped:
    name: str
    reason: str


def parse_int(text: str | None) -> int:
    if text is None:
        raise BakeError("missing integer value")
    return int(text.strip(), 0)


def child_text(node: ET.Element, name: str) -> str | None:
    child = node.find(name)
    return child.text.strip() if child is not None and child.text else None


def find_ofd() -> Path:
    candidates: list[Path] = []
    cg_root = os.environ.get("CG_TOOL_ROOT")
    if cg_root:
        candidates.append(Path(cg_root) / "bin" / "ofd2000")
    candidates.extend(
        sorted(
            Path("/Applications/ti/ccs2100/ccs/tools/compiler").glob(
                "ti-cgt-c2000_*/bin/ofd2000"
            ),
            reverse=True,
        )
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise BakeError("ofd2000 not found; set CG_TOOL_ROOT to the C2000 compiler")


def run_ofd(elf: Path, ofd: Path) -> tuple[ET.Element, str]:
    with tempfile.NamedTemporaryFile(suffix=".xml") as output:
        result = subprocess.run(
            [str(ofd), "--xml", "--dwarf", f"--output={output.name}", str(elf)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode != 0:
            message = result.stderr.decode("utf-8", errors="replace").strip()
            raise BakeError(f"ofd2000 failed for {elf}: {message}")
        try:
            root = ET.parse(output.name).getroot()
        except (OSError, ET.ParseError) as exc:
            raise BakeError(f"invalid ofd2000 DWARF XML for {elf}: {exc}") from exc
    banner = child_text(root, "banner") or "ofd2000"
    if root.find("./object_file/dwarf") is None:
        raise BakeError(f"{elf}: no DWARF data found")
    return root, banner


def load_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="ascii"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BakeError(f"cannot read boundary manifest {path}: {exc}") from exc
    if manifest.get("version") != 1 or not manifest.get("ownership_sha256"):
        raise BakeError(f"{path}: unsupported or incomplete boundary manifest")
    return manifest


def load_ranges(path: Path) -> list[AddressRange]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        raise BakeError(f"cannot parse linker XML {path}: {exc}") from exc
    symbols: dict[str, int] = {}
    for symbol in root.findall("./symbol_table/symbol"):
        name = child_text(symbol, "name")
        value = child_text(symbol, "value")
        if name and value:
            symbols[name] = parse_int(value)
    pairs = (
        ("data", "V2K_UserDataRunStart", "V2K_UserDataRunEnd", True),
        ("bss", "V2K_UserBssStart", "V2K_UserBssEnd", True),
        ("const", "V2K_UserConstStart", "V2K_UserConstEnd", False),
    )
    missing = [key for _, start, end, _ in pairs for key in (start, end) if key not in symbols]
    if missing:
        raise BakeError("linker XML is missing symbols: " + ", ".join(missing))
    ranges = [AddressRange(name, symbols[start], symbols[end], mutable) for name, start, end, mutable in pairs]
    for item in ranges:
        if item.end < item.start:
            raise BakeError(f"invalid user {item.name} range")
    return ranges


def attr_node(die: ET.Element, name: str) -> ET.Element | None:
    for attribute in die.findall("attribute"):
        if child_text(attribute, "type") == name:
            return attribute
    return None


def attr_text(die: ET.Element, name: str, value_tag: str) -> str | None:
    attribute = attr_node(die, name)
    return child_text(attribute, f"value/{value_tag}") if attribute is not None else None


def attr_string(die: ET.Element, name: str) -> str | None:
    return attr_text(die, name, "string")


def attr_const(die: ET.Element, name: str, default: int | None = None) -> int | None:
    text = attr_text(die, name, "const")
    return default if text is None else parse_int(text)


def attr_ref(die: ET.Element, name: str) -> str | None:
    attribute = attr_node(die, name)
    if attribute is None:
        return None
    ref = attribute.find("value/ref")
    return ref.get("idref") if ref is not None else None


def attr_expr(die: ET.Element, name: str) -> str | None:
    return attr_text(die, name, "exprloc")


def die_tag(die: ET.Element) -> str:
    return child_text(die, "tag") or ""


def index_dies(root: ET.Element) -> dict[str, ET.Element]:
    dies = {die.get("id"): die for die in root.findall(".//die") if die.get("id")}
    if not dies:
        raise BakeError("DWARF XML contains no DIE records")
    return dies


def referenced_type(die: ET.Element, dies: dict[str, ET.Element]) -> ET.Element:
    reference = attr_ref(die, "DW_AT_type")
    if not reference or reference not in dies:
        raise BakeError(f"{die.get('id', '<unknown>')}: missing referenced type DIE")
    return dies[reference]


def resolve_type(die: ET.Element, dies: dict[str, ET.Element]) -> ET.Element:
    visited: set[str] = set()
    while die_tag(die) in WRAPPER_TAGS:
        identifier = die.get("id", "")
        if identifier in visited:
            raise BakeError("recursive DWARF type wrapper")
        visited.add(identifier)
        die = referenced_type(die, dies)
    return die


def type_size(die: ET.Element, dies: dict[str, ET.Element]) -> int:
    resolved = resolve_type(die, dies)
    size = attr_const(resolved, "DW_AT_byte_size")
    if size is None:
        raise BakeError(f"{resolved.get('id', '<unknown>')}: type has no size")
    return size


def scalar_type(die: ET.Element, dies: dict[str, ET.Element]) -> tuple[int, int] | None:
    resolved = resolve_type(die, dies)
    if die_tag(resolved) != "DW_TAG_base_type":
        return None
    size = attr_const(resolved, "DW_AT_byte_size")
    encoding = attr_const(resolved, "DW_AT_encoding")
    if encoding == 0x4 and size == 2:
        return TYPE_F32, 2
    if encoding in (0x5, 0x6) and size in (1, 2):
        return (TYPE_I16 if size == 1 else TYPE_I32), size
    if encoding in (0x7, 0x8) and size in (1, 2):
        return (TYPE_U16 if size == 1 else TYPE_U32), size
    return None


def member_offset(die: ET.Element) -> int:
    expression = attr_expr(die, "DW_AT_data_member_location")
    if expression is None:
        return 0
    match = MEMBER_RE.fullmatch(expression)
    if not match:
        raise BakeError(f"unsupported member location expression: {expression}")
    return parse_int(match.group(1))


def range_for(ranges: list[AddressRange], address: int, words: int = 1) -> AddressRange | None:
    return next((item for item in ranges if item.contains(address, words)), None)


def expand_type(
    name: str,
    address: int,
    type_die: ET.Element,
    dies: dict[str, ET.Element],
    storage: AddressRange,
    entries: list[Descriptor],
    skipped: list[Skipped],
) -> None:
    resolved = resolve_type(type_die, dies)
    scalar = scalar_type(resolved, dies)
    if scalar is not None:
        type_code, words = scalar
        if not storage.contains(address, words):
            raise BakeError(f"{name}: leaf crosses the {storage.name} boundary")
        if words == 2 and address & 1:
            raise BakeError(f"{name}: 32-bit leaf has odd C28x word address {address:#x}")
        entries.append(
            Descriptor(
                name=name,
                type=type_code,
                kind=(KIND_PARAM | KIND_SCOPE) if storage.mutable else KIND_SCOPE,
                addr=address,
                storage=storage.name,
            )
        )
        return

    tag = die_tag(resolved)
    if tag == "DW_TAG_array_type":
        element_type = referenced_type(resolved, dies)
        dimensions = [
            attr_const(child, "DW_AT_upper_bound")
            for child in resolved.findall("die")
            if die_tag(child) == "DW_TAG_subrange_type"
        ]
        if not dimensions or any(bound is None for bound in dimensions):
            skipped.append(Skipped(name, "array has no fixed upper bound"))
            return
        element_words = type_size(element_type, dies)

        def visit_dimension(prefix: str, base: int, dimension: int) -> None:
            count = int(dimensions[dimension]) + 1
            stride = element_words
            for later in dimensions[dimension + 1 :]:
                stride *= int(later) + 1
            for index in range(count):
                child_name = f"{prefix}[{index}]"
                child_addr = base + index * stride
                if dimension + 1 == len(dimensions):
                    expand_type(child_name, child_addr, element_type, dies, storage, entries, skipped)
                else:
                    visit_dimension(child_name, child_addr, dimension + 1)

        visit_dimension(name, address, 0)
        return

    if tag == "DW_TAG_structure_type":
        for member in resolved.findall("die"):
            if die_tag(member) != "DW_TAG_member":
                continue
            member_name = attr_string(member, "DW_AT_name")
            if not member_name:
                skipped.append(Skipped(name, "anonymous struct member"))
                continue
            leaf_name = f"{name}.{member_name}"
            if any(
                attr_node(member, attribute) is not None
                for attribute in ("DW_AT_bit_size", "DW_AT_bit_offset", "DW_AT_data_bit_offset")
            ):
                skipped.append(Skipped(leaf_name, "bitfield is unsupported"))
                continue
            try:
                offset = member_offset(member)
                member_type = referenced_type(member, dies)
            except BakeError as exc:
                skipped.append(Skipped(leaf_name, str(exc)))
                continue
            expand_type(leaf_name, address + offset, member_type, dies, storage, entries, skipped)
        return

    reasons = {
        "DW_TAG_pointer_type": "pointer is unsupported",
        "DW_TAG_union_type": "union is unsupported",
        "DW_TAG_enumeration_type": "enum is unsupported",
    }
    skipped.append(Skipped(name, reasons.get(tag, f"unsupported type {tag or '<unknown>'}")))


def collect_entries(root: ET.Element, ranges: list[AddressRange], capacity: int = USER_CAPACITY) -> tuple[list[Descriptor], list[Skipped]]:
    dies = index_dies(root)
    variables: list[tuple[int, str, ET.Element, AddressRange]] = []
    seen_variables: set[tuple[int, str, str]] = set()
    for die in root.findall(".//die"):
        if die_tag(die) != "DW_TAG_variable":
            continue
        location = attr_expr(die, "DW_AT_location") or ""
        match = ADDR_RE.fullmatch(location)
        if not match:
            continue
        address = parse_int(match.group(1))
        storage = range_for(ranges, address)
        if storage is None:
            continue
        name = attr_string(die, "DW_AT_name")
        type_ref = attr_ref(die, "DW_AT_type")
        if not name or not type_ref or type_ref not in dies:
            raise BakeError(f"user variable at {address:#x} has incomplete DWARF")
        key = (address, name, type_ref)
        if key not in seen_variables:
            seen_variables.add(key)
            variables.append((address, name, dies[type_ref], storage))

    entries: list[Descriptor] = []
    skipped: list[Skipped] = []
    for address, name, type_die, storage in sorted(variables, key=lambda item: (item[0], item[1])):
        expand_type(name, address, type_die, dies, storage, entries, skipped)
    entries.sort(key=lambda item: (item.addr, item.name))

    names: set[str] = set()
    for entry in entries:
        try:
            encoded = entry.name.encode("ascii")
        except UnicodeEncodeError as exc:
            raise BakeError(f"{entry.name!r}: descriptor name must be ASCII") from exc
        if len(encoded) >= NAME_LEN:
            raise BakeError(f"{entry.name}: descriptor name exceeds 15 visible characters")
        if entry.name in names:
            raise BakeError(f"duplicate descriptor name: {entry.name}")
        names.add(entry.name)
    if len(entries) > capacity:
        raise BakeError(f"user descriptor capacity exceeded: {len(entries)} > {capacity}")
    return entries, skipped


def encode_blob(
    entries: list[Descriptor],
    build_hash: int = 0,
    capacity: int = USER_CAPACITY,
) -> bytes:
    if len(entries) > capacity:
        raise BakeError(f"user descriptor capacity exceeded: {len(entries)} > {capacity}")
    output = bytearray(
        struct.pack(
            BLOB_HEADER_FORMAT,
            USER_MAGIC,
            USER_VERSION,
            len(entries),
            capacity,
            0,
            build_hash,
        )
    )
    for index in range(capacity):
        if index < len(entries):
            entry = entries[index]
            encoded = entry.name.encode("ascii")
            output.extend(b"".join(struct.pack("<H", value) for value in encoded.ljust(NAME_LEN, b"\0")))
            output.extend(struct.pack("<HHIHH", entry.type, entry.kind, entry.addr, entry.prescaler, entry.reserved))
        else:
            output.extend(b"\0" * ENTRY_SIZE)
    return bytes(output)


def decode_blob(data: bytes) -> tuple[int, list[Descriptor]]:
    expected_size = BLOB_HEADER_SIZE + USER_CAPACITY * ENTRY_SIZE
    if len(data) != expected_size:
        raise BakeError(f"user descriptor section size is {len(data)}, expected {expected_size}")
    magic, version, count, capacity, reserved, build_hash = struct.unpack_from(
        BLOB_HEADER_FORMAT, data
    )
    if (magic, version, capacity, reserved) != (USER_MAGIC, USER_VERSION, USER_CAPACITY, 0):
        raise BakeError("invalid user descriptor blob header")
    if count > capacity:
        raise BakeError("user descriptor blob count exceeds capacity")
    result: list[Descriptor] = []
    offset = BLOB_HEADER_SIZE
    for index in range(capacity):
        name_words = struct.unpack_from("<16H", data, offset)
        name = bytes(value & 0xFF for value in name_words).split(b"\0", 1)[0].decode("ascii")
        type_code, kind, address, prescaler, entry_reserved = struct.unpack_from("<HHIHH", data, offset + 32)
        if index < count:
            result.append(Descriptor(name, type_code, kind, address, prescaler, entry_reserved))
        offset += ENTRY_SIZE
    return build_hash, result


def canonical_build_hash(
    image: bytes,
    section_offset: int,
    section_size: int,
    entries: list[Descriptor],
) -> int:
    if section_offset + section_size > len(image):
        raise BakeError("descriptor section lies outside the ELF image")
    canonical = bytearray(image)
    canonical[section_offset : section_offset + section_size] = b"\0" * section_size
    digest = hashlib.sha256()
    digest.update(canonical)
    digest.update(encode_blob(entries, build_hash=0))
    value = int.from_bytes(digest.digest()[:4], "little")
    return value if value != 0 else 1


def elf_section(path: Path, name: str) -> tuple[int, int]:
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise BakeError(f"{path}: not an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise BakeError(f"{path}: expected ELF32 little-endian format")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    section_offset, section_size, section_count, strings_index = header[6], header[11], header[12], header[13]
    if section_size != 40 or strings_index >= section_count:
        raise BakeError(f"{path}: invalid ELF section table")
    if section_offset + section_count * section_size > len(data):
        raise BakeError(f"{path}: truncated ELF section table")

    def section_header(index: int) -> tuple[int, ...]:
        return struct.unpack_from("<IIIIIIIIII", data, section_offset + index * section_size)

    string_header = section_header(strings_index)
    string_data = data[string_header[4] : string_header[4] + string_header[5]]
    for index in range(section_count):
        header_values = section_header(index)
        name_offset = header_values[0]
        end = string_data.find(b"\0", name_offset)
        section_name = string_data[name_offset:end].decode("ascii") if end >= 0 else ""
        if section_name == name:
            section_type, file_offset, size = header_values[1], header_values[4], header_values[5]
            if section_type != 1 or file_offset + size > len(data):
                raise BakeError(f"{path}: {name} is not patchable PROGBITS")
            return file_offset, size
    raise BakeError(f"{path}: section {name!r} not found")


def prepare_blob(path: Path, entries: list[Descriptor]) -> tuple[bytes, int]:
    offset, size = elf_section(path, "v2k_user_desc")
    image = path.read_bytes()
    expected_size = len(encode_blob([]))
    if size != expected_size:
        raise BakeError(f"{path}: v2k_user_desc size is {size}, expected {expected_size}")
    current = image[offset : offset + size]
    decode_blob(current)
    build_hash = canonical_build_hash(image, offset, size, entries)
    return encode_blob(entries, build_hash=build_hash), build_hash


def patch_elf(path: Path, blob: bytes, dry_run: bool) -> None:
    offset, size = elf_section(path, "v2k_user_desc")
    if size != len(blob):
        raise BakeError(f"{path}: v2k_user_desc size is {size}, expected {len(blob)}")
    if not dry_run:
        with path.open("r+b") as stream:
            stream.seek(offset)
            stream.write(blob)
    actual = blob if dry_run else path.read_bytes()[offset : offset + size]
    if decode_blob(actual) != decode_blob(blob):
        raise BakeError(f"{path}: decoded descriptor verification failed")


def report_entry(entry: Descriptor) -> dict:
    result = asdict(entry)
    result["type_name"] = TYPE_NAMES[entry.type]
    return result


def command_bake(args: argparse.Namespace) -> None:
    manifest = load_manifest(args.manifest)
    ranges = load_ranges(args.link_info)
    ofd = args.ofd or find_ofd()
    root, banner = run_ofd(args.elf, ofd)
    entries, skipped = collect_entries(root, ranges)
    blob, build_hash = prepare_blob(args.elf, entries)
    patch_elf(args.elf, blob, args.dry_run)
    report = {
        "version": 1,
        "configuration": manifest.get("configuration"),
        "ownership_sha256": manifest["ownership_sha256"],
        "toolchain": {"ofd2000": str(ofd.resolve()), "version": banner},
        "elf": str(args.elf.resolve()),
        "patched": not args.dry_run,
        "build_hash": build_hash,
        "build_hash_hex": f"0x{build_hash:08X}",
        "entry_count": len(entries),
        "capacity": USER_CAPACITY,
        "platform_reserved": 32,
        "entries": [report_entry(entry) for entry in entries],
        "skipped": [asdict(item) for item in skipped],
    }
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="ascii")
    action = "validated" if args.dry_run else "patched"
    print(f"user descriptors {action}: {len(entries)}/{USER_CAPACITY}, skipped={len(skipped)}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    bake = subparsers.add_parser("bake")
    bake.add_argument("--manifest", type=Path, required=True)
    bake.add_argument("--link-info", type=Path, required=True)
    bake.add_argument("--elf", type=Path, required=True)
    bake.add_argument("--report", type=Path, required=True)
    bake.add_argument("--ofd", type=Path)
    bake.add_argument("--dry-run", action="store_true")
    bake.set_defaults(func=command_bake)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        args.func(args)
    except (BakeError, OSError, subprocess.SubprocessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
