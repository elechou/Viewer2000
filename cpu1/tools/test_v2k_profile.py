#!/usr/bin/env python3
"""Host-compiled tests for the CPU1 one-second load profiler."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


CPU1_ROOT = Path(__file__).resolve().parents[1]
RUNTIME_DIR = CPU1_ROOT / "runtime"


class ProfileTests(unittest.TestCase):
    def test_window_peak_average_publish_and_tick_wrap(self) -> None:
        cc = shutil.which("cc") or shutil.which("clang")
        self.assertIsNotNone(cc, "host C compiler not found")
        source = textwrap.dedent(
            r'''
            #include "v2k_profile.h"

            #include <stdint.h>
            #include <stdio.h>

            volatile uint32_t g_v2k_isr_budget_violation_cnt;
            volatile uint32_t g_v2k_isr_ovf_cnt;

            static int expect_u32(const char *name, uint32_t actual, uint32_t expected)
            {
                if (actual == expected) return 0;
                printf("%s: expected %lu, got %lu\n", name,
                       (unsigned long)expected, (unsigned long)actual);
                return 1;
            }

            static int expect_u16(const char *name, uint16_t actual, uint16_t expected)
            {
                if (actual == expected) return 0;
                printf("%s: expected %u, got %u\n", name,
                       (unsigned)expected, (unsigned)actual);
                return 1;
            }

            int main(void)
            {
                int failed = 0;
                v2k_profile_init();
                v2k_profile_sample(100u, 60u, 20u, 3u, 0u);
                v2k_profile_sample(150u, 70u, 40u, 4u, 1u);
                v2k_profile_sample(120u, 65u, 25u, 5u, 2u);
                v2k_profile_sample(90u, 55u, 15u, 6u, 3u);
                failed |= expect_u32("seq before service", g_v2k_prof_seq, 0u);
                v2k_profile_service();
                failed |= expect_u32("seq", g_v2k_prof_seq, 1u);
                failed |= expect_u32("average", g_v2k_load_avg, 119u);
                failed |= expect_u32("peak", g_v2k_load_peak, 154u);
                failed |= expect_u32("control", g_v2k_ctrl_at_peak, 70u);
                failed |= expect_u32("scope", g_v2k_scope_at_peak, 40u);
                failed |= expect_u32("latency", g_v2k_lat_at_peak, 4u);
                failed |= expect_u32("peak tick", g_v2k_peak_tick, 1u);

                g_v2k_isr_budget_violation_cnt = 2u;
                g_v2k_isr_ovf_cnt = 3u;
                v2k_cpu1_status_t status = {0};
                v2k_profile_publish_status(&status);
                failed |= expect_u32("status seq", status.prof_seq, 1u);
                failed |= expect_u32("status budget", status.cycle_budget, g_v2k_cycle_budget);
                failed |= expect_u32("status avg", status.load_avg, 119u);
                failed |= expect_u32("status peak", status.load_peak, 154u);
                failed |= expect_u32("status control", status.ctrl_at_peak, 70u);
                failed |= expect_u32("status scope", status.scope_at_peak, 40u);
                failed |= expect_u16("status latency", status.lat_at_peak, 4u);
                failed |= expect_u32("status tick", status.peak_tick, 1u);
                failed |= expect_u32("status violations", status.budget_violations, 2u);
                failed |= expect_u32("status overflows", status.isr_overflows, 3u);

                /* A second publish with no newly-serviced window must be skipped,
                   so CPU2 always reads an immutable, coherent snapshot. */
                status.load_peak = 0xBEEFu;
                v2k_profile_publish_status(&status);
                failed |= expect_u32("publish skip when unchanged", status.load_peak, 0xBEEFu);

                v2k_profile_sample(80u, 50u, 10u, 7u, 0xFFFFFFFEu);
                v2k_profile_sample(90u, 55u, 15u, 8u, 0xFFFFFFFFu);
                v2k_profile_sample(110u, 60u, 20u, 9u, 0u);
                v2k_profile_sample(100u, 58u, 18u, 10u, 1u);
                v2k_profile_service();
                failed |= expect_u32("wrapped seq", g_v2k_prof_seq, 2u);
                failed |= expect_u32("wrapped average", g_v2k_load_avg, 103u);
                failed |= expect_u32("wrapped peak", g_v2k_load_peak, 119u);
                failed |= expect_u32("wrapped peak tick", g_v2k_peak_tick, 0u);
                return failed;
            }
            '''
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_source = temp_path / "test_profile.c"
            executable = temp_path / "test_profile"
            test_source.write_text(source, encoding="ascii")
            compile_result = subprocess.run(
                [
                    cc,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-DV2K_PROFILE_WINDOW_SAMPLES=4",
                    "-I",
                    str(RUNTIME_DIR),
                    str(RUNTIME_DIR / "v2k_profile.c"),
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
