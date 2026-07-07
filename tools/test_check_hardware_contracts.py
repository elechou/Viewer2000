from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import check_hardware_contracts as gate


MANIFEST = """
schema = "viewer2000.board-profile.v2"
id = "test_profile"
path_base = "repository"
hardware_contract = "profile/hardware_contracts.toml"
"""

CONTRACT = """
schema = "viewer2000.hardware-contract.v1"
profile = "test_profile"

[sysconfig]
file = "cpu1/test.syscfg"

[sysconfig.instance_counts]
epwm = 2

[sysconfig.required]
"epwm*.mode" = "UP_DOWN"
"epwm*.clockDiv" = "DIV_1"

[sysconfig.defaults]
"epwm*.clockDiv" = "DIV_1"

[linker.required]
"cpu1/test.cmd" = ['scope_ring\\s*:\\s*>\\s*RAM']

[linker.forbidden]
"cpu1/test.cmd" = ["PRIVATE_BANK"]
"""

SYSCONFIG = """
const epwm = scripting.addModule("/driverlib/epwm.js", {}, false);
const epwm1 = epwm.addInstance();
const epwm2 = epwm.addInstance();
epwm1.mode = "UP_DOWN";
epwm2.mode = "UP_DOWN";
"""


class HardwareContractTests(unittest.TestCase):
    def make_tree(self) -> tuple[tempfile.TemporaryDirectory[str], Path, Path]:
        temp = tempfile.TemporaryDirectory()
        root = Path(temp.name)
        (root / "profile").mkdir()
        (root / "cpu1").mkdir()
        manifest = root / "profile/manifest.toml"
        manifest.write_text(MANIFEST, encoding="utf-8")
        (root / "profile/hardware_contracts.toml").write_text(
            CONTRACT, encoding="utf-8"
        )
        (root / "cpu1/test.syscfg").write_text(SYSCONFIG, encoding="utf-8")
        (root / "cpu1/test.cmd").write_text(
            "scope_ring : > RAM\n", encoding="utf-8"
        )
        return temp, root, manifest

    def test_accepts_wildcards_reviewed_defaults_and_linker_rules(self) -> None:
        temp, root, manifest = self.make_tree()
        self.addCleanup(temp.cleanup)
        self.assertEqual(gate.check_contract(manifest, root), [])

    def test_reports_sysconfig_drift(self) -> None:
        temp, root, manifest = self.make_tree()
        self.addCleanup(temp.cleanup)
        path = root / "cpu1/test.syscfg"
        path.write_text(
            SYSCONFIG.replace('epwm2.mode = "UP_DOWN"', 'epwm2.mode = "UP"'),
            encoding="utf-8",
        )
        errors = gate.check_contract(manifest, root)
        self.assertTrue(any("epwm2.mode" in error for error in errors))

    def test_reports_instance_count_and_forbidden_linker_pattern(self) -> None:
        temp, root, manifest = self.make_tree()
        self.addCleanup(temp.cleanup)
        path = root / "cpu1/test.syscfg"
        path.write_text(
            SYSCONFIG.replace("const epwm2 = epwm.addInstance();\n", ""),
            encoding="utf-8",
        )
        (root / "cpu1/test.cmd").write_text(
            "scope_ring : > PRIVATE_BANK\n", encoding="utf-8"
        )
        errors = gate.check_contract(manifest, root)
        self.assertTrue(any("instance count" in error for error in errors))
        self.assertTrue(any("forbidden pattern" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
