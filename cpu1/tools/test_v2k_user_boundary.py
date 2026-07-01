#!/usr/bin/env python3
"""Unit tests for v2k_user_boundary.py."""

from __future__ import annotations

import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import v2k_user_boundary as boundary


def section_header(name: str, section_type: str, flags: list[str], size: int = 4) -> ET.Element:
    xml = f"""
    <elf32_shdr>
      <sh_name_string>{name}</sh_name_string>
      <sh_type>{section_type}</sh_type>
      <sh_flags>{''.join(f'<flag>{flag}</flag>' for flag in flags)}</sh_flags>
      <sh_addr>0x0</sh_addr>
      <sh_size>{size:#x}</sh_size>
    </elf32_shdr>
    """
    return ET.fromstring(xml)


def linker_xml(*, run_size: int = 0x20, crc_load_size: int = 0x20) -> ET.Element:
    return ET.fromstring(
        f"""
        <link_info>
          <input_file_list/>
          <object_component_list/>
          <logical_group_list/>
          <placement_map>
            <memory_area><name>USER_RUN</name><origin>0xb000</origin><length>0x800</length></memory_area>
            <memory_area><name>USER_CONST_RAM</name><origin>0xb800</origin><length>0x800</length></memory_area>
            <memory_area><name>USER_GOLDEN_RAM</name><origin>0x20000</origin><length>0x800</length></memory_area>
          </placement_map>
          <crc_table_list>
            <crc_table><name>V2K_UserDataCrcTable</name><crc_rec>
              <name>v2k_user_data</name><alg_name>CRC32_PRIME</alg_name>
              <load_address>0x20000</load_address><load_size>{crc_load_size:#x}</load_size>
              <crc_value>0x12345678</crc_value>
            </crc_rec></crc_table>
          </crc_table_list>
          <symbol_table>
            <symbol><name>V2K_UserDataLoadStart</name><value>0x20000</value></symbol>
            <symbol><name>V2K_UserDataLoadEnd</name><value>{0x20000 + run_size:#x}</value></symbol>
            <symbol><name>V2K_UserDataLoadSize</name><value>{run_size:#x}</value></symbol>
            <symbol><name>V2K_UserDataRunStart</name><value>0xb000</value></symbol>
            <symbol><name>V2K_UserDataRunEnd</name><value>{0xb000 + run_size:#x}</value></symbol>
            <symbol><name>V2K_UserDataRunSize</name><value>{run_size:#x}</value></symbol>
            <symbol><name>V2K_UserBssStart</name><value>{0xb000 + run_size:#x}</value></symbol>
            <symbol><name>V2K_UserBssEnd</name><value>{0xb010 + run_size:#x}</value></symbol>
            <symbol><name>V2K_UserBssSize</name><value>0x10</value></symbol>
            <symbol><name>V2K_UserDataCrcPresent</name><value>0x1</value></symbol>
            <symbol><name>V2K_UserDataCrcTable</name><value>0x200</value></symbol>
            <symbol><name>V2K_UserConstStart</name><value>0xb800</value></symbol>
            <symbol><name>V2K_UserConstEnd</name><value>0xb808</value></symbol>
            <symbol><name>V2K_UserConstSize</name><value>0x8</value></symbol>
          </symbol_table>
        </link_info>
        """
    )


def add_load_group(root: ET.Element, name: str, start: int, size: int) -> None:
    groups = root.find("./logical_group_list")
    assert groups is not None
    groups.append(
        ET.fromstring(
            f"<logical_group><name>{name}</name>"
            f"<load_address>{start:#x}</load_address><size>{size:#x}</size>"
            "</logical_group>"
        )
    )


class ClassificationTests(unittest.TestCase):
    def test_classifies_standard_allocated_sections(self) -> None:
        cases = (
            (".text:control", "SHT_PROGBITS", ["SHF_ALLOC", "SHF_EXECINSTR"], "text"),
            (".const:table", "SHT_PROGBITS", ["SHF_ALLOC"], "const"),
            (".data:value", "SHT_PROGBITS", ["SHF_ALLOC", "SHF_WRITE"], "data"),
            (".bss:value", "SHT_NOBITS", ["SHF_ALLOC", "SHF_WRITE"], "bss"),
        )
        for name, section_type, flags, expected in cases:
            with self.subTest(name=name):
                actual = boundary.classify_section(
                    section_header(name, section_type, flags), "user.obj"
                )
                self.assertIsNotNone(actual)
                self.assertEqual(actual.kind, expected)

    def test_rejects_unsupported_mutable_lifecycle(self) -> None:
        with self.assertRaises(boundary.BoundaryError):
            boundary.classify_section(
                section_header(
                    ".TI.noinit:retained",
                    "SHT_NOBITS",
                    ["SHF_ALLOC", "SHF_WRITE"],
                ),
                "user.obj",
            )

    def test_rejects_unknown_writable_section(self) -> None:
        with self.assertRaises(boundary.BoundaryError):
            boundary.classify_section(
                section_header(
                    ".vendor_state",
                    "SHT_PROGBITS",
                    ["SHF_ALLOC", "SHF_WRITE"],
                ),
                "vendor.obj",
            )


class FragmentTests(unittest.TestCase):
    def test_ram_fragment_uses_named_regions_and_crc(self) -> None:
        obj = boundary.ObjectInfo(
            selector="app/user.obj",
            path=Path("app/user.obj"),
            sections=(
                boundary.InputSection(".text:control", "text", 4),
                boundary.InputSection(".const:table", "const", 4),
                boundary.InputSection(".data:value", "data", 4),
                boundary.InputSection(".bss:state", "bss", 4),
            ),
        )
        fragment, has_data = boundary.render_fragment([obj], "RAM")
        self.assertTrue(has_data)
        self.assertIn('"./app/user.obj"(.data:*)', fragment)
        self.assertIn("LOAD = USER_GOLDEN_RAM", fragment)
        self.assertIn("> USER_CONST_RAM", fragment)
        self.assertIn("crc_table(V2K_UserDataCrcTable", fragment)
        self.assertIn("TYPE = NOINIT", fragment)

    def test_flash_const_uses_one_bank_for_authoritative_range_symbols(self) -> None:
        obj = boundary.ObjectInfo(
            selector="app/user.obj",
            path=Path("app/user.obj"),
            sections=(
                boundary.InputSection(".const:table", "const", 4),
            ),
        )
        fragment, _ = boundary.render_fragment([obj], "FLASH")
        self.assertIn("> FLASH_BANK1, ALIGN(8)", fragment)
        self.assertNotIn(">> FLASH_BANK0 | FLASH_BANK1, ALIGN(8),\n        START", fragment)

    def test_flash_data_load_uses_user_flash_bank(self) -> None:
        obj = boundary.ObjectInfo(
            selector="app/user.obj",
            path=Path("app/user.obj"),
            sections=(
                boundary.InputSection(".data:value", "data", 4),
            ),
        )
        fragment, _ = boundary.render_fragment([obj], "FLASH")
        self.assertIn("LOAD = FLASH_BANK1", fragment)
        self.assertNotIn("LOAD = FLASH_BANK0", fragment)


class LinkerScriptTests(unittest.TestCase):
    def test_user_descriptor_catalog_uses_user_flash_bank(self) -> None:
        linker_script = (
            Path(__file__).resolve().parents[1]
            / "28p65x_generic_flash_lnk_cpu1.cmd"
        )
        text = linker_script.read_text(encoding="ascii")
        self.assertIn(".TI.crctab       : > FLASH_BANK1", text)
        self.assertNotIn(".TI.crctab       : > FLASH_BANK0", text)
        self.assertIn("v2k_user_desc   : > FLASH_BANK1", text)
        self.assertNotIn("v2k_user_desc   : > FLASH_BANK0", text)


class LayoutTests(unittest.TestCase):
    manifest = {"configuration": "RAM", "has_user_data": True}

    def test_accepts_valid_layout(self) -> None:
        self.assertEqual(
            boundary.verify_layout(linker_xml(), self.manifest, ".cinit platform only"),
            [],
        )

    def test_rejects_crc_size_mismatch(self) -> None:
        errors = boundary.verify_layout(
            linker_xml(crc_load_size=0x1f), self.manifest, ".cinit platform only"
        )
        self.assertIn("linker CRC record does not match user data LOAD", errors)

    def test_rejects_user_cinit(self) -> None:
        errors = boundary.verify_layout(
            linker_xml(), self.manifest, ".cinit.v2k_user_data.load"
        )
        self.assertIn("user data/BSS is still initialized through .cinit", errors)

    def test_rejects_user_run_overflow(self) -> None:
        errors = boundary.verify_layout(
            linker_xml(run_size=0x801, crc_load_size=0x801),
            self.manifest,
            ".cinit platform only",
        )
        self.assertIn("user data+BSS exceeds USER_RUN", errors)

    def test_zero_user_data_does_not_overlap_bss(self) -> None:
        manifest = {"configuration": "RAM", "has_user_data": False}
        root = linker_xml(run_size=0, crc_load_size=0)
        root.find("./symbol_table/symbol[name='V2K_UserDataCrcPresent']/value").text = "0x0"
        root.find("./crc_table_list").clear()
        self.assertNotIn(
            "user data RUN and BSS overlap",
            boundary.verify_layout(root, manifest, ".cinit platform only"),
        )

    def test_flash_layout_accepts_user_bank_data_load(self) -> None:
        root = linker_xml()
        manifest = {"configuration": "FLASH", "has_user_data": True}
        placement = root.find("./placement_map")
        assert placement is not None
        placement.append(
            ET.fromstring(
                "<memory_area><name>FLASH_BANK1</name>"
                "<origin>0x20000</origin><length>0x800</length></memory_area>"
            )
        )
        root.find("./crc_table_list/crc_table/crc_rec/load_address").text = "0x20000"
        root.find("./symbol_table/symbol[name='V2K_UserDataCrcTable']/value").text = (
            "0x20200"
        )
        add_load_group(root, "v2k_user_desc", 0x20100, 0x80)
        add_load_group(root, ".TI.crctab", 0x20200, 0x0A)
        self.assertEqual(
            boundary.verify_layout(root, manifest, ".cinit platform only"),
            [],
        )

    def test_flash_layout_rejects_post_link_range_overlap(self) -> None:
        root = linker_xml()
        manifest = {"configuration": "FLASH", "has_user_data": True}
        placement = root.find("./placement_map")
        assert placement is not None
        placement.append(
            ET.fromstring(
                "<memory_area><name>FLASH_BANK1</name>"
                "<origin>0x20000</origin><length>0x800</length></memory_area>"
            )
        )
        root.find("./symbol_table/symbol[name='V2K_UserDataCrcTable']/value").text = (
            "0x20200"
        )
        add_load_group(root, "v2k_user_desc", 0x20010, 0x80)
        add_load_group(root, ".TI.crctab", 0x20200, 0x0A)
        errors = boundary.verify_layout(root, manifest, ".cinit platform only")
        self.assertIn("user data golden overlaps user descriptor blob", errors)


class OwnershipTests(unittest.TestCase):
    def test_selector_match_requires_path_boundary(self) -> None:
        self.assertTrue(
            boundary.selector_matches(
                "app/user.obj",
                "/tmp/project/RAM/app/user.obj",
            )
        )
        self.assertFalse(
            boundary.selector_matches(
                "app/user.obj",
                "/tmp/project/RAM/myapp/user.obj",
            )
        )

    def test_rejects_platform_contamination(self) -> None:
        root = ET.fromstring(
            """
            <link_info>
              <input_file_list><input_file id="f1"><path>/build/runtime</path><file>platform.obj</file></input_file></input_file_list>
              <object_component_list><object_component id="c1"><name>.data</name><input_file_ref idref="f1"/></object_component></object_component_list>
              <logical_group_list><logical_group><name>v2k_user_data</name><contents><object_component_ref idref="c1"/></contents></logical_group></logical_group_list>
            </link_info>
            """
        )
        manifest = {"objects": [{"selector": "app/user.obj", "sections": {}}]}
        errors = boundary.verify_ownership(root, manifest)
        self.assertEqual(len(errors), 1)
        self.assertIn("platform object", errors[0])

    def test_allows_platform_reserved_descriptor_section(self) -> None:
        root = ET.fromstring(
            """
            <link_info>
              <input_file_list><input_file id="f1"><path>/build/runtime</path><file>registry.obj</file></input_file></input_file_list>
              <object_component_list><object_component id="c1"><name>v2k_user_desc</name><input_file_ref idref="f1"/></object_component></object_component_list>
              <logical_group_list><logical_group><name>v2k_user_desc</name><contents><object_component_ref idref="c1"/></contents></logical_group></logical_group_list>
            </link_info>
            """
        )
        manifest = {"objects": [{"selector": "app/user.obj", "sections": {}}]}
        self.assertEqual(boundary.verify_ownership(root, manifest), [])


if __name__ == "__main__":
    unittest.main()
