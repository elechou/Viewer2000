#!/usr/bin/env python3
"""Unit tests for v2k_bake_user_desc.py."""

from __future__ import annotations

import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import v2k_bake_user_desc as bake


def attribute(name: str, tag: str, value: str) -> str:
    if tag == "ref":
        payload = f'<ref idref="{value}"/>'
    else:
        payload = f"<{tag}>{value}</{tag}>"
    return f"<attribute><type>{name}</type><value>{payload}</value></attribute>"


def die(identifier: str, tag: str, attrs: str = "", children: str = "") -> str:
    return f'<die id="{identifier}"><tag>{tag}</tag>{attrs}{children}</die>'


def base(identifier: str, name: str, size: int, encoding: int) -> str:
    return die(
        identifier,
        "DW_TAG_base_type",
        attribute("DW_AT_name", "string", name)
        + attribute("DW_AT_byte_size", "const", hex(size))
        + attribute("DW_AT_encoding", "const", hex(encoding)),
    )


def variable(identifier: str, name: str, address: int, type_ref: str) -> str:
    return die(
        identifier,
        "DW_TAG_variable",
        attribute("DW_AT_name", "string", name)
        + attribute("DW_AT_location", "exprloc", f"DW_OP_addr {address:#x}")
        + attribute("DW_AT_type", "ref", type_ref),
    )


def root(*records: str) -> ET.Element:
    return ET.fromstring("<ofd><object_file><dwarf><section>" + "".join(records) + "</section></dwarf></object_file></ofd>")


RANGES = [
    bake.AddressRange("data", 0x1000, 0x1100, True),
    bake.AddressRange("bss", 0x1100, 0x1200, True),
    bake.AddressRange("const", 0x2000, 0x2100, False),
]


class CollectionTests(unittest.TestCase):
    def test_scalar_and_function_static_use_source_names(self) -> None:
        xml = root(
            base("f32", "float", 2, 4),
            variable("v1", "setpoint", 0x1000, "f32"),
            die("fn", "DW_TAG_subprogram", children=variable("v2", "history", 0x1100, "f32")),
        )
        entries, skipped = bake.collect_entries(xml, RANGES)
        self.assertEqual([entry.name for entry in entries], ["setpoint", "history"])
        self.assertEqual(skipped, [])

    def test_typedef_const_and_ti_far_wrappers_resolve(self) -> None:
        wrapped = die("far", "DW_TAG_TI_far_type", attribute("DW_AT_type", "ref", "f32"))
        wrapped = wrapped + die("const", "DW_TAG_const_type", attribute("DW_AT_type", "ref", "far"))
        wrapped = wrapped + die("alias", "DW_TAG_typedef", attribute("DW_AT_type", "ref", "const"))
        entries, _ = bake.collect_entries(
            root(base("f32", "float", 2, 4), wrapped, variable("v", "gain", 0x2000, "alias")),
            RANGES,
        )
        self.assertEqual(entries[0].type, bake.TYPE_F32)
        self.assertEqual(entries[0].kind, bake.KIND_USER | bake.KIND_SCOPE)

    def test_array_struct_nested_and_const(self) -> None:
        array = die(
            "array",
            "DW_TAG_array_type",
            attribute("DW_AT_byte_size", "const", "0x4") + attribute("DW_AT_type", "ref", "f32"),
            die("sub", "DW_TAG_subrange_type", attribute("DW_AT_upper_bound", "const", "1")),
        )
        inner = die(
            "inner",
            "DW_TAG_structure_type",
            attribute("DW_AT_byte_size", "const", "0x4"),
            die("m1", "DW_TAG_member", attribute("DW_AT_name", "string", "err") + attribute("DW_AT_type", "ref", "array")),
        )
        outer = die(
            "outer",
            "DW_TAG_structure_type",
            attribute("DW_AT_byte_size", "const", "0x6"),
            die("m2", "DW_TAG_member", attribute("DW_AT_name", "string", "trace") + attribute("DW_AT_type", "ref", "inner"))
            + die("m3", "DW_TAG_member", attribute("DW_AT_name", "string", "idx") + attribute("DW_AT_data_member_location", "exprloc", "DW_OP_plus_uconst 0x4") + attribute("DW_AT_type", "ref", "u16")),
        )
        xml = root(
            base("f32", "float", 2, 4),
            base("u16", "unsigned int", 1, 7),
            array,
            inner,
            outer,
            variable("v1", "s", 0x1000, "outer"),
            variable("v2", "offset", 0x2000, "array"),
        )
        entries, _ = bake.collect_entries(xml, RANGES)
        self.assertEqual(
            [entry.name for entry in entries],
            ["s.trace.err[0]", "s.trace.err[1]", "s.idx", "offset[0]", "offset[1]"],
        )
        self.assertEqual(
            entries[0].kind, bake.KIND_USER | bake.KIND_PARAM | bake.KIND_SCOPE
        )
        self.assertEqual(entries[-1].kind, bake.KIND_USER | bake.KIND_SCOPE)

    def test_pointer_member_is_reported(self) -> None:
        pointer = die("ptr", "DW_TAG_pointer_type", attribute("DW_AT_type", "ref", "f32"))
        struct_type = die(
            "struct",
            "DW_TAG_structure_type",
            attribute("DW_AT_byte_size", "const", "0x2"),
            die("member", "DW_TAG_member", attribute("DW_AT_name", "string", "next") + attribute("DW_AT_type", "ref", "ptr")),
        )
        entries, skipped = bake.collect_entries(
            root(base("f32", "float", 2, 4), pointer, struct_type, variable("v", "node", 0x1000, "struct")),
            RANGES,
        )
        self.assertEqual(entries, [])
        self.assertEqual(skipped[0].name, "node.next")
        self.assertIn("pointer", skipped[0].reason)

    def test_duplicate_and_overlong_names_fail(self) -> None:
        scalar = base("u16", "unsigned int", 1, 7)
        with self.assertRaisesRegex(bake.BakeError, "duplicate"):
            bake.collect_entries(
                root(scalar, variable("v1", "same", 0x1000, "u16"), variable("v2", "same", 0x1001, "u16")),
                RANGES,
            )
        with self.assertRaisesRegex(bake.BakeError, "15 visible"):
            bake.collect_entries(root(scalar, variable("v3", "sixteen_char_len", 0x1000, "u16")), RANGES)

    def test_capacity_and_alignment_fail(self) -> None:
        records = [base("u16", "unsigned int", 1, 7)]
        records.extend(variable(f"v{i}", f"x{i}", 0x1000 + i, "u16") for i in range(3))
        with self.assertRaisesRegex(bake.BakeError, "capacity"):
            bake.collect_entries(root(*records), RANGES, capacity=2)
        with self.assertRaisesRegex(bake.BakeError, "odd"):
            bake.collect_entries(root(base("f32", "float", 2, 4), variable("v", "bad", 0x1001, "f32")), RANGES)


class BlobTests(unittest.TestCase):
    def test_round_trip_uses_c28x_wide_chars(self) -> None:
        entries = [bake.Descriptor("pi.Kp", bake.TYPE_F32, 7, 0x1000)]
        firmware_info = bake.FirmwareInfo("phase4-demo", 0x6A4DE800)
        blob = bake.encode_blob(entries, build_hash=0x12345678, firmware_info=firmware_info)
        self.assertEqual(
            len(blob),
            bake.BLOB_HEADER_SIZE + bake.FIRMWARE_INFO_SIZE + bake.USER_CAPACITY * 44,
        )
        self.assertEqual(
            blob[bake.BLOB_HEADER_SIZE : bake.BLOB_HEADER_SIZE + 22],
            b"p\0h\0a\0s\0e\0" b"4\0-\0d\0e\0m\0o\0",
        )
        entry_offset = bake.BLOB_HEADER_SIZE + bake.FIRMWARE_INFO_SIZE
        self.assertEqual(
            blob[entry_offset : entry_offset + 12],
            b"p\0i\0.\0K\0p\0\0\0",
        )
        self.assertEqual(bake.decode_blob(blob), (0x12345678, firmware_info, entries))

    def test_rejects_bad_blob_version(self) -> None:
        blob = bytearray(bake.encode_blob([]))
        blob[4:6] = b"\x03\x00"
        with self.assertRaisesRegex(bake.BakeError, "header"):
            bake.decode_blob(bytes(blob))

    def test_final_image_hash_ignores_previous_blob_contents_and_project_info(self) -> None:
        entries = [bake.Descriptor("setpoint", bake.TYPE_F32, 7, 0x1000)]
        blank = bake.encode_blob([])
        patched = bake.encode_blob(
            entries,
            build_hash=0xAABBCCDD,
            firmware_info=bake.FirmwareInfo("alpha", 1_781_913_600),
        )
        repatched = bake.encode_blob(
            entries,
            build_hash=0x11223344,
            firmware_info=bake.FirmwareInfo("beta", 1_781_999_999),
        )
        prefix = b"ELF-prefix"
        suffix = b"ELF-suffix"
        offset = len(prefix)
        first = bake.canonical_build_hash(
            prefix + blank + suffix, offset, len(blank), entries
        )
        second = bake.canonical_build_hash(
            prefix + patched + suffix, offset, len(patched), entries
        )
        third = bake.canonical_build_hash(
            prefix + repatched + suffix, offset, len(repatched), entries
        )
        changed = bake.canonical_build_hash(
            prefix + patched + suffix,
            offset,
            len(patched),
            [bake.Descriptor("setpoint2", bake.TYPE_F32, 7, 0x1002)],
        )
        self.assertEqual(first, second)
        self.assertEqual(first, third)
        self.assertNotEqual(first, changed)
        self.assertNotEqual(first, 0)

    def test_project_name_defaults_and_validation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / ".project"
            with self.assertRaisesRegex(bake.BakeError, "cannot read CCS project file"):
                bake.load_project_name(missing)

            blank = Path(tmp) / "blank.project"
            blank.write_text(
                "<projectDescription><name>   </name></projectDescription>",
                encoding="utf-8",
            )
            self.assertEqual(bake.load_project_name(blank), bake.DEFAULT_PROJECT_NAME)

            named = Path(tmp) / "named.project"
            named.write_text(
                "<projectDescription><name> demo-01 </name></projectDescription>",
                encoding="utf-8",
            )
            self.assertEqual(bake.load_project_name(named), "demo-01")

        with self.assertRaisesRegex(bake.BakeError, "exceeds"):
            bake.validate_project_name("x" * (bake.PROJECT_NAME_LEN + 1))
        with self.assertRaisesRegex(bake.BakeError, "printable ASCII"):
            bake.validate_project_name("电机")

    def test_repeated_bake_preserves_build_time_for_same_hash(self) -> None:
        previous = bake.FirmwareInfo("old", 1234)
        self.assertEqual(bake.select_build_time(0xAA, previous, 0xAA, "old", 5678), 1234)
        self.assertEqual(bake.select_build_time(0xAA, previous, 0xBB, "old", 5678), 5678)
        self.assertEqual(bake.select_build_time(0xAA, previous, 0xAA, "new", 5678), 5678)
        self.assertEqual(
            bake.select_build_time(0xAA, bake.FirmwareInfo("old", 0), 0xAA, "old", 5678),
            5678,
        )


if __name__ == "__main__":
    unittest.main()
