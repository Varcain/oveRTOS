# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep ownership, context and invalid-metadata cases in the actual Rust gate."""

import unittest
from unittest import mock

from ove import test as ove_test


class RustInferenceGateTest(unittest.TestCase):
    def run_gate(self, sanitize, fail_case=None):
        def execute(command, **_kwargs):
            return int(command[-1] == fail_case)

        with (
            mock.patch.object(ove_test, "_cmake_build") as build,
            mock.patch.object(
                ove_test, "_run_owned_test_process", side_effect=execute
            ) as run,
        ):
            result = ove_test._test_rust_infer("/ove", "/out", sanitize=sanitize)
        return result, build.call_args_list, run.call_args_list

    def test_both_modes_and_sanitizers_keep_the_full_contract_suite(self):
        for sanitize in (False, True):
            with self.subTest(sanitize=sanitize):
                result, builds, calls = self.run_gate(sanitize)
                self.assertEqual(result.failed, 0)
                self.assertEqual(len(builds), 2)
                for zero in (False, True):
                    selected = [
                        c
                        for c in calls
                        if ("_zh" in c.kwargs["env"]["OVE_GEN_DIR"]) == zero
                    ]
                    cases = [
                        c.args[0][-1]
                        for c in selected
                        if c.args[0][0].endswith("infer-contracts")
                    ]
                    self.assertEqual(len(cases), 20 if zero else 24)
                    self.assertTrue(
                        {
                            "normal",
                            "null",
                            "misaligned",
                            "too_large",
                            "straddle",
                            "constant_input",
                            "empty",
                            "drop_isr",
                            "forget",
                            "construct_isr",
                        }
                        <= set(cases)
                    )
                    self.assertEqual("heap_construct_isr" in cases, not zero)
                    types = [
                        c for c in selected if c.args[0][1].endswith("type_checks.py")
                    ]
                    self.assertEqual(len(types), 0 if sanitize else 1)
                    if sanitize:
                        self.assertIn(
                            "-Zsanitizer=address",
                            selected[0].kwargs["env"]["RUSTFLAGS"],
                        )
                        self.assertEqual(
                            selected[0].kwargs["env"]["ASAN_OPTIONS"],
                            "detect_leaks=1:halt_on_error=1",
                        )

    def test_rejected_span_regression_fails_the_gate(self):
        for sanitize in (False, True):
            with self.subTest(sanitize=sanitize):
                result, _, _ = self.run_gate(sanitize, fail_case="straddle")
                self.assertEqual(result.failed, 2)


if __name__ == "__main__":
    unittest.main()
