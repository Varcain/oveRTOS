# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression tests for workspace configuration and matrix-build cleanup."""

import contextlib
import io
import json
import os
import signal
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

from ove import allconfigs
from ove.kconfig import (
    _savedefconfig_destination,
    _write_workspace_config,
    parse_defconfig_name,
)
from ove.workspace import APP_PATH_FILE, WORKSPACE_DIR_ENV, Workspace


class _Config:
    def __init__(self, contents="CONFIG_TEST=y\n", error=None):
        self.contents = contents
        self.error = error

    def write_config(self, path):
        if self.error:
            raise self.error
        with open(path, "w") as f:
            f.write(self.contents)


class WorkspaceConfigTest(unittest.TestCase):
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

        workspace, config = _write_workspace_config(
            _Config(), self.root, "board", "rtos", "app", activate=False)

        self.assertTrue(os.path.isdir(workspace))
        self.assertTrue(os.path.isfile(config))
        self.assertEqual(
            os.readlink(os.path.join(self.root, ".config")), config_target)
        self.assertEqual(
            os.readlink(os.path.join(self.root, "output", "current")),
            current_target)

    def test_active_workspace_updates_both_links_after_write(self):
        workspace, config = _write_workspace_config(
            _Config(), self.root, "board", "rtos", "app")

        self.assertEqual(os.path.realpath(os.path.join(self.root, ".config")),
                         config)
        self.assertEqual(
            os.readlink(os.path.join(self.root, "output", "current")),
            os.path.join("board", "rtos", "app"))
        self.assertEqual(workspace, os.path.dirname(config))

    def test_external_workspace_uses_external_output_and_absolute_link(self):
        external = os.path.join(self.root, "external")
        os.makedirs(external)

        workspace, config = _write_workspace_config(
            _Config('CONFIG_OVE_BOARD_NAME="board"\n'
                    'CONFIG_OVE_RTOS_POSIX=y\n'
                    'CONFIG_OVE_APP_NAME="app"\n'), self.root,
            "board", "rtos", "app", external)

        expected = os.path.join(
            external, "output", "board", "rtos", "app")
        self.assertEqual(workspace, expected)
        self.assertEqual(config, os.path.join(expected, ".config"))
        self.assertEqual(
            os.readlink(os.path.join(self.root, "output", "current")),
            expected)
        with open(os.path.join(expected, APP_PATH_FILE)) as f:
            self.assertEqual(
                os.path.realpath(os.path.join(expected, f.read().strip())),
                external)

        kconfig = os.path.join(self.root, "output", "kconfig")
        os.makedirs(kconfig)
        with open(os.path.join(kconfig, "app_paths.json"), "w") as f:
            json.dump({"app": "/wrong/global/path"}, f)
        with mock.patch.dict(os.environ,
                             {WORKSPACE_DIR_ENV: expected}, clear=False):
            ws = Workspace(self.root)
            self.assertTrue(ws.is_external_app)
            self.assertEqual(ws.app_dir, external)
            self.assertEqual(
                _savedefconfig_destination(ws),
                os.path.join(external, "defconfigs",
                             "board_posix_app_defconfig"))

    def test_failed_config_write_preserves_active_links(self):
        config_target = os.readlink(os.path.join(self.root, ".config"))
        current_target = os.readlink(
            os.path.join(self.root, "output", "current"))

        with self.assertRaisesRegex(OSError, "write failed"):
            _write_workspace_config(
                _Config(error=OSError("write failed")), self.root,
                "board", "rtos", "app")

        self.assertEqual(
            os.readlink(os.path.join(self.root, ".config")), config_target)
        self.assertEqual(
            os.readlink(os.path.join(self.root, "output", "current")),
            current_target)

    def test_workspace_links_available_downloaded_toolchain(self):
        toolchains = os.path.join(self.root, "output", "toolchains")
        toolchain = os.path.join(toolchains, "arm-toolchain")
        os.makedirs(toolchain)
        with open(os.path.join(toolchains, "path.txt"), "w") as f:
            f.write(os.path.join("ignored", "arm-toolchain"))

        workspace, _config = _write_workspace_config(
            _Config(), self.root, "board", "rtos", "app", activate=False)

        self.assertEqual(
            os.path.realpath(os.path.join(workspace, "toolchain")),
            toolchain)

    def test_saved_defconfig_identity_preserves_app_underscores(self):
        self.assertEqual(
            parse_defconfig_name(
                "stm32f746g-discovery_freertos_my_app_defconfig"),
            ("stm32f746g-discovery_freertos_my_app_defconfig",
             "stm32f746g-discovery", "freertos", "my_app"))

    def test_saved_defconfig_identity_adds_suffix(self):
        self.assertEqual(
            parse_defconfig_name("host_posix_example_c"),
            ("host_posix_example_c_defconfig",
             "host", "posix", "example_c"))

    def test_invalid_saved_defconfig_identity_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "cannot parse"):
            parse_defconfig_name("missing_rtos_defconfig")

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

    def test_saved_defconfig_pipeline_uses_external_workspace(self):
        calls = []

        def record(command, env, cwd):
            calls.append((list(command), dict(env), cwd))
            return 0

        external = os.path.join(self.root, "external")
        path = os.path.join(
            external, "defconfigs", "host_posix_my_app_defconfig")
        with mock.patch.dict(os.environ,
                             {"OVE_WORKSPACE_DIR": "/stale"}, clear=True), \
                mock.patch.object(allconfigs, "_run_command",
                                  side_effect=record):
            ok, _elapsed = allconfigs._run_defconfig(
                self.root, external, path, "ove")

        self.assertTrue(ok)
        self.assertEqual(
            calls[0][0],
            ["ove", "defconfig", "host_posix_my_app_defconfig",
             "--no-activate"])
        self.assertNotIn(WORKSPACE_DIR_ENV, calls[0][1])
        self.assertEqual(calls[0][1]["OVE_EXTERNAL_APPS"], external)
        expected = os.path.join(
            external, "output", "host", "posix", "my_app")
        self.assertEqual(
            [call[1][WORKSPACE_DIR_ENV] for call in calls[1:]],
            [expected, expected, expected])

    def test_saved_defconfig_discovery_rejects_duplicate_names(self):
        external = os.path.join(self.root, "external")
        first = os.path.join(external, "defconfigs", "one")
        second = os.path.join(external, "defconfigs", "two")
        os.makedirs(first)
        os.makedirs(second)
        name = "host_posix_my_app_defconfig"
        for directory in (first, second):
            with open(os.path.join(directory, name), "w") as f:
                f.write("CONFIG_TEST=y\n")

        with self.assertRaisesRegex(ValueError, "duplicate defconfig"):
            allconfigs._discover_defconfigs(external)

    def test_saved_defconfig_matrix_continues_then_reports_failure(self):
        external = os.path.join(self.root, "external")
        configs = os.path.join(external, "defconfigs")
        os.makedirs(configs)
        with open(os.path.join(external, "app.yaml"), "w") as f:
            f.write("lang: c\n")
        for name in ("host_posix_first_defconfig",
                     "host_posix_second_defconfig"):
            with open(os.path.join(configs, name), "w") as f:
                f.write("CONFIG_TEST=y\n")

        workspace = SimpleNamespace(
            ove_dir=self.root, venv_dir=os.path.join(self.root, ".venv"))
        args = SimpleNamespace(app_dir=external, json=False)
        with mock.patch.object(allconfigs, "Workspace",
                               return_value=workspace), \
                mock.patch.object(allconfigs, "_run_defconfig",
                                  side_effect=((False, 1.0), (True, 2.0))) \
                as run, contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(SystemExit, "1"):
                allconfigs._cmd_alldefconfigs(args)

        self.assertEqual(run.call_count, 2)


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
