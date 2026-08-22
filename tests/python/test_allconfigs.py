# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression tests for isolated allconfigs workspaces and child cleanup."""

import os
import signal
import tempfile
import unittest
from unittest import mock

from ove import allconfigs
from ove.kconfig import _setup_workspace
from ove.workspace import WORKSPACE_DIR_ENV, Workspace


class AllconfigsWorkspaceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        os.makedirs(os.path.join(self.root, "output", "active"))
        active_config = os.path.join(self.root, "output", "active", ".config")
        with open(active_config, "w") as f:
            f.write("CONFIG_OVE_RTOS_POSIX=y\n")
        os.symlink(active_config, os.path.join(self.root, ".config"))
        os.symlink("active", os.path.join(self.root, "output", "current"))

    def tearDown(self):
        self.temp.cleanup()

    def test_inactive_workspace_does_not_change_active_links(self):
        config_target = os.readlink(os.path.join(self.root, ".config"))
        current_target = os.readlink(
            os.path.join(self.root, "output", "current"))

        workspace = _setup_workspace(
            self.root, "board", "rtos", "app", activate=False)

        self.assertTrue(os.path.isdir(workspace))
        self.assertEqual(
            os.readlink(os.path.join(self.root, ".config")), config_target)
        self.assertEqual(
            os.readlink(os.path.join(self.root, "output", "current")),
            current_target)

    def test_explicit_workspace_ignores_active_link(self):
        isolated = os.path.join(self.root, "output", "board", "rtos", "app")
        os.makedirs(isolated)
        with open(os.path.join(isolated, ".config"), "w") as f:
            f.write("CONFIG_OVE_RTOS_FREERTOS=y\n")

        with mock.patch.dict(os.environ,
                             {WORKSPACE_DIR_ENV: isolated}, clear=False):
            ws = Workspace(self.root)

        self.assertTrue(ws.is_isolated)
        self.assertEqual(ws.workspace_dir, isolated)
        self.assertEqual(ws.config_path, os.path.join(isolated, ".config"))
        self.assertEqual(ws.rtos, "freertos")

    def test_pipeline_uses_isolated_workspace_after_configuration(self):
        calls = []

        def record(command, env, cwd):
            calls.append((list(command), dict(env), cwd))
            return 0

        with mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(allconfigs, "_run_command",
                                  side_effect=record):
            ok, _elapsed = allconfigs._run_app(
                self.root, "board", "rtos", "app", "ove")

        self.assertTrue(ok)
        self.assertEqual(
            calls[0][0],
            ["ove", "defconfig-fragments", "board.rtos.app",
             "--no-activate"])
        self.assertNotIn(WORKSPACE_DIR_ENV, calls[0][1])
        expected = os.path.join(
            self.root, "output", "board", "rtos", "app")
        self.assertEqual(
            [call[1][WORKSPACE_DIR_ENV] for call in calls[1:]],
            [expected, expected, expected])
        self.assertEqual(
            [call[0][1] for call in calls[1:]],
            ["download", "configure", "build"])
        self.assertTrue(all(call[2] == self.root for call in calls))

    def test_pipeline_stops_at_failed_stage(self):
        with mock.patch.object(allconfigs, "_run_command",
                               side_effect=(0, 1)) as run:
            ok, _elapsed = allconfigs._run_app(
                self.root, "board", "rtos", "app", "ove")

        self.assertFalse(ok)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[-1].args[0], ["ove", "download"])


class AllconfigsChildTest(unittest.TestCase):
    def test_interrupted_command_terminates_process_group(self):
        class InterruptedProcess:
            pid = 4321

            def __init__(self):
                self.waits = 0

            def wait(self, timeout=None):
                self.waits += 1
                if self.waits == 1:
                    raise KeyboardInterrupt
                return -signal.SIGTERM

            @staticmethod
            def poll():
                return None

        proc = InterruptedProcess()
        with mock.patch.object(allconfigs.subprocess, "Popen",
                               return_value=proc), \
                mock.patch.object(allconfigs.os, "killpg") as killpg:
            with self.assertRaises(KeyboardInterrupt):
                allconfigs._run_command(["ove", "build"], {}, "/repo")

        killpg.assert_called_once_with(proc.pid, signal.SIGTERM)
        self.assertEqual(proc.waits, 2)


if __name__ == "__main__":
    unittest.main()
