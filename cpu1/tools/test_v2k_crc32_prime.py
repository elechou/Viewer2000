#!/usr/bin/env python3
"""Host-compiled tests for the target CRC32_PRIME implementation."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


CPU1_ROOT = Path(__file__).resolve().parents[1]
RUNTIME_DIR = CPU1_ROOT / "runtime"


class Crc32PrimeTests(unittest.TestCase):
    def test_matches_linker_verified_user_data_vector(self) -> None:
        cc = shutil.which("cc") or shutil.which("clang")
        self.assertIsNotNone(cc, "host C compiler not found")
        source = textwrap.dedent(
            r'''
            #include "v2k_crc32_prime.h"

            #include <stdint.h>
            #include <stdio.h>

            int main(void)
            {
                static const uint16_t user_data_golden[32] = {
                    0x0000u, 0x3fc0u, 0x3333u, 0x3eb3u,
                    0xc28fu, 0x3c75u, 0x0000u, 0x0000u,
                    0xcccdu, 0x3f4cu, 0xcccdu, 0x3d4cu,
                    0x0000u, 0x3f80u, 0x0000u, 0x0000u,
                    0xcccdu, 0x3f4cu, 0xcccdu, 0x3d4cu,
                    0x0000u, 0x0000u, 0x0000u, 0x0000u,
                    0xcccdu, 0x3dccu, 0xcccdu, 0xbd4cu,
                    0xcccdu, 0x3cccu, 0x0000u, 0x3e80u,
                };
                uint32_t actual = v2k_crc32_prime(user_data_golden, 32u);
                if (actual != 0xd501b381uL)
                {
                    printf("expected 0xd501b381, got 0x%08lx\n", (unsigned long)actual);
                    return 1;
                }
                return 0;
            }
            '''
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_source = temp_path / "test_crc.c"
            executable = temp_path / "test_crc"
            test_source.write_text(source, encoding="ascii")
            compile_result = subprocess.run(
                [
                    cc,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-I",
                    str(RUNTIME_DIR),
                    str(RUNTIME_DIR / "v2k_crc32_prime.c"),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stdout + compile_result.stderr,
            )
            run_result = subprocess.run(
                [str(executable)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)


if __name__ == "__main__":
    unittest.main()
