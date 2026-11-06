# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep linked assets and inference lifecycle in the registered Zig gate."""

import os
import tempfile
import unittest
from unittest import mock

from ove import test as ove_test


class ZigInferenceGateTest(unittest.TestCase):
    def run_gate(self, *, canary=False, fail_case=None):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        binaries = [
            (zero, opt, kind, f"{directory.name}/{zero}/{opt}/{kind}", {})
            for zero in (False, True)
            for opt in ("Debug", "ReleaseSafe", "ReleaseFast", "ReleaseSmall")
            for kind in ("contracts", "assets")
        ]

        for _, _, _, binary, _ in binaries:
            os.makedirs(os.path.dirname(binary), exist_ok=True)

        def execute(command, **kwargs):
            if command[-1] == "canary":
                if "stdout" in kwargs:
                    if fail_case == "erased":
                        kwargs["stdout"].write(b"canary: 0 failed checks\n")
                        return 0
                    kwargs["stdout"].write(
                        b"failed: explicit fixture failure canary\n"
                        b"canary: 1 failed checks\n"
                    )
                return 1
            return int(
                command[-1] == fail_case
                or (fail_case == "assets" and command[0].endswith("/assets"))
            )

        with (
            mock.patch.dict(
                os.environ, {"OVE_ZIG_INFER_CANARY": "1" if canary else "0"}
            ),
            mock.patch.object(
                ove_test, "_build_zig_infer_binaries", return_value=binaries
            ),
            mock.patch.object(
                ove_test, "_run_owned_test_process", side_effect=execute
            ) as run,
        ):
            result = ove_test.TEST_TARGETS["zig-infer"]("/ove", "/out")
        return result, run.call_args_list

    def test_both_modes_and_four_optimizations_cover_real_closed_objects(self):
        result, calls = self.run_gate()
        self.assertEqual((result.passed, result.failed), (60, 0))
        self.assertEqual(sum(c.args[0][0].endswith("/assets") for c in calls), 8)
        for zero in (False, True):
            for opt in ("Debug", "ReleaseSafe", "ReleaseFast", "ReleaseSmall"):
                selected = [c for c in calls if f"/{zero}/{opt}/" in c.args[0][0]]
                cases = {
                    c.args[0][-1]
                    for c in selected
                    if c.args[0][0].endswith("/contracts")
                }
                self.assertTrue(
                    {"forward", "extent", "errors", "busy_live", "closed"} <= cases
                )
                self.assertEqual("busy_candidate" in cases, zero)
                self.assertNotIn("closed_observer", cases)

    def test_each_boundary_failure_reaches_the_gate(self):
        for case in ("extent", "closed", "assets"):
            with self.subTest(case=case):
                result, _ = self.run_gate(fail_case=case)
                self.assertEqual((result.passed, result.failed), (52, 8))

    def test_erased_canary_cannot_pass_the_normal_gate(self):
        result, _ = self.run_gate(fail_case="erased")
        self.assertEqual((result.passed, result.failed), (52, 8))

    def test_explicit_canary_runs_each_real_contract_binary(self):
        result, calls = self.run_gate(canary=True, fail_case="canary")
        self.assertEqual((result.passed, result.failed), (0, 8))
        self.assertEqual(len(calls), 8)
        self.assertTrue(all(c.args[0][-1] == "canary" for c in calls))


if __name__ == "__main__":
    unittest.main()
