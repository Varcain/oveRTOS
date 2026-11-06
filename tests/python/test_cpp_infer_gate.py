# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep embedded inference construction and cleanup in the registered gate."""

import unittest
from unittest import mock

from ove import test as ove_test


class CppInferenceGateTest(unittest.TestCase):
    def run_gate(self, sanitize=False, fail_case=None):
        with (
            mock.patch.object(ove_test, "_cmake_build") as build,
            mock.patch.object(
                ove_test,
                "_run_owned_test_process",
                side_effect=lambda command, **_kwargs: int(command[-1] == fail_case),
            ) as run,
        ):
            name = "cpp-infer-sanitize" if sanitize else "cpp-infer"
            result = ove_test.TEST_TARGETS[name]("/ove", "/out")
        return result, build.call_args_list, run.call_args_list

    def test_every_mode_and_assertion_policy_is_registered(self):
        for sanitize in (False, True):
            with self.subTest(sanitize=sanitize):
                result, builds, calls = self.run_gate(sanitize)
                self.assertEqual((result.passed, result.failed), (38, 0))
                self.assertEqual(len(builds), 1)
                self.assertIn(
                    "-DOVE_SANITIZE=" + ("ON" if sanitize else "OFF"),
                    builds[0].kwargs["extra_args"],
                )
                for mode in ("heap", "zero"):
                    for policy in (
                        "hook",
                        "hook_release",
                        "default_release",
                        "default_debug",
                    ):
                        selected = [
                            c
                            for c in calls
                            if c.args[0][0].endswith("infer_" + mode + "_" + policy)
                        ]
                        self.assertEqual(
                            len(selected), 4 if policy == "default_debug" else 5
                        )
                        self.assertIn("lifetime", {c.args[0][-1] for c in selected})
                if sanitize:
                    self.assertEqual(
                        calls[0].kwargs["env"]["ASAN_OPTIONS"],
                        "detect_leaks=1:halt_on_error=1",
                    )
                    self.assertEqual(
                        calls[0].kwargs["env"]["UBSAN_OPTIONS"], "halt_on_error=1"
                    )

    def test_each_regression_failure_reaches_the_gate(self):
        for case in ("capacity", "lifetime"):
            for sanitize in (False, True):
                with self.subTest(case=case, sanitize=sanitize):
                    result, _, _ = self.run_gate(sanitize, fail_case=case)
                    self.assertEqual((result.passed, result.failed), (30, 8))


if __name__ == "__main__":
    unittest.main()
