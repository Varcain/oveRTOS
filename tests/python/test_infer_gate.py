# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""The native inference gate must exercise arena and admission regressions."""

import json
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ove import test as ove_test


class InferenceGateTest(unittest.TestCase):
    def run_gate(self, sanitize=False, runner=None):
        with (
            mock.patch("ove.download.download_tflm", return_value=True),
            mock.patch.object(ove_test, "load_manifest", return_value={}),
            mock.patch.object(ove_test, "_cmake_build") as build,
            mock.patch.object(
                ove_test, "_run_owned_test_process", side_effect=runner, return_value=0
            ) as execute,
        ):
            result = ove_test._test_infer_admission("/ove", "/out", sanitize=sanitize)
        return result, build.call_args_list, execute.call_args_list

    def test_allocation_and_sanitizer_modes_run_both_suites(self):
        for sanitize in (False, True):
            with self.subTest(sanitize=sanitize):
                result, builds, calls = self.run_gate(sanitize)
                self.assertEqual((result.passed, result.failed), (4, 0))
                self.assertEqual(len(builds), 2)
                for call in builds:
                    self.assertIn("-DOVE_ARENA_MUTATIONS=ON", call.kwargs["extra_args"])
                self.assertEqual(len(calls), 4)
                for zero in (False, True):
                    selected = [
                        c for c in calls if ("--zero-heap" in c.args[0]) == zero
                    ]
                    self.assertEqual(len(selected), 2)
                    self.assertTrue(
                        all(("--sanitize" in c.args[0]) == sanitize for c in selected)
                    )
                    self.assertEqual(
                        {c.args[0][1] for c in selected},
                        {
                            "/ove/tests/infer_admission/run.py",
                            "/ove/tests/infer_arena/run.py",
                        },
                    )
                    arena = next(c for c in selected if "/infer_arena/" in c.args[0][1])
                    self.assertIn("--mutations", arena.args[0])
                    self.assertGreater(arena.kwargs["cleanup_timeout"], 5)

    def test_arena_failure_fails_the_gate(self):
        def run(command, **_kwargs):
            return int("/infer_arena/" in command[1])

        result, _, calls = self.run_gate(runner=run)
        self.assertGreater(result.failed, 0)
        self.assertTrue(any("/infer_arena/" in c.args[0][1] for c in calls))

    def test_descriptor_failure_stops_each_admission_runner_mode(self):
        runner = Path(__file__).resolve().parents[1] / "infer_admission/run.py"
        for zero in (False, True):
            for sanitize in (False, True):
                with (
                    self.subTest(zero=zero, sanitize=sanitize),
                    tempfile.TemporaryDirectory() as tmp,
                ):
                    seen = []

                    def execute(command, seen=seen, **kwargs):
                        binary = Path(command[0]).name
                        seen.append((binary, kwargs["env"]))
                        return subprocess.CompletedProcess(
                            command, int(binary == "infer_descriptors")
                        )

                    command = [str(runner), tmp]
                    if zero:
                        command.append("--zero-heap")
                    if sanitize:
                        command.append("--sanitize")
                    with (
                        mock.patch.object(sys, "argv", command),
                        mock.patch("subprocess.run", side_effect=execute),
                        self.assertRaises(SystemExit) as stopped,
                    ):
                        runpy.run_path(str(runner), run_name="__main__")
                    self.assertEqual(stopped.exception.code, 1)
                    rows = json.loads(
                        (Path(tmp) / "admission-results.json").read_text()
                    )
                    self.assertEqual(rows[-1]["binary"], "infer_descriptors")
                    self.assertEqual(rows[-1]["case"], "matrix")
                    self.assertFalse(rows[-1]["intentional_retention"])
                    self.assertEqual(rows[-1]["rc"], 1)
                    if sanitize:
                        self.assertIn("detect_leaks=1", seen[-1][1]["ASAN_OPTIONS"])




if __name__ == "__main__":
    unittest.main()
