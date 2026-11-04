# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""The native inference gate must exercise arena and admission regressions."""

import signal
import subprocess
import unittest
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

    def test_outer_cleanup_waits_for_inner_case_cleanup(self):
        command = ["fixture-controller"]
        process = mock.Mock(pid=12345)
        process.wait.side_effect = [
            subprocess.TimeoutExpired(command, 1),
            subprocess.TimeoutExpired(command, 15),
            0,
        ]
        with (
            mock.patch.object(
                ove_test.subprocess, "Popen", return_value=process
            ) as popen,
            mock.patch.object(ove_test.os, "killpg") as kill,
        ):
            rc = ove_test._run_owned_test_process(
                command, env={}, timeout=1, cleanup_timeout=15
            )
        self.assertEqual(rc, 124)
        self.assertTrue(popen.call_args.kwargs["start_new_session"])
        self.assertEqual(process.wait.call_args_list[1].kwargs["timeout"], 15)
        self.assertIn(mock.call(12345, signal.SIGTERM), kill.call_args_list)
        self.assertIn(mock.call(12345, signal.SIGKILL), kill.call_args_list)
        self.assertEqual(process.wait.call_args_list[-1], mock.call())

    def test_interrupt_allows_controller_cleanup_before_kill(self):
        process = mock.Mock(pid=12345)
        process.wait.side_effect = [KeyboardInterrupt(), 0, 0]
        with (
            mock.patch.object(ove_test.subprocess, "Popen", return_value=process),
            mock.patch.object(ove_test.os, "killpg") as kill,
            self.assertRaises(KeyboardInterrupt),
        ):
            ove_test._run_owned_test_process(
                ["fixture-controller"], env={}, timeout=1, cleanup_timeout=15
            )
        self.assertEqual(kill.call_args_list[0], mock.call(12345, signal.SIGTERM))
        self.assertEqual(process.wait.call_args_list[1].kwargs["timeout"], 15)
        self.assertEqual(kill.call_args_list[-1], mock.call(12345, signal.SIGKILL))
        self.assertEqual(process.wait.call_args_list[-1], mock.call())


if __name__ == "__main__":
    unittest.main()
