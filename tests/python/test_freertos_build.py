# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression tests for FreeRTOS kernel source selection."""

import os
import tempfile
import unittest
from unittest import mock

from ove.build import build_freertos


class _Workspace:
    def __init__(self, root):
        self.ove_dir = os.path.join(root, "ove")
        self.app_dir = os.path.join(root, "app")
        self.gen_dir = os.path.join(root, "generated")
        self.ws_dl_dir = os.path.join(root, "dl")
        self.board_dir = os.path.join(root, "board")
        self.build_dir = os.path.join(root, "build")
        self.build_log = os.path.join(root, "build.log")
        self.arm_float_abi = "hard"
        self.toolchain_dir = None
        self.config = {}

        for path in (self.ove_dir, self.app_dir, self.gen_dir,
                     self.ws_dl_dir, self.board_dir):
            os.makedirs(path)

    @staticmethod
    def toolchain_env():
        return {}


class FreeRTOSBuildSourceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.ws = _Workspace(self.temp.name)
        self.run = mock.Mock()
        self.patches = [
            mock.patch("ove.build._find_cmake", return_value="cmake"),
            mock.patch("ove.build.run", self.run),
            mock.patch("ove.build._copy_images"),
            mock.patch("ove.build._create_run_or_flash_script"),
        ]
        for patcher in self.patches:
            patcher.start()

    def tearDown(self):
        for patcher in reversed(self.patches):
            patcher.stop()
        self.temp.cleanup()

    def _configure_args(self):
        build_freertos(self.ws)
        return self.run.call_args_list[0].args[0]

    def test_cube_kernel_fallback_clears_stale_cache_value(self):
        args = self._configure_args()

        self.assertIn("-UFREERTOS_PATH", args)
        self.assertFalse(any(arg.startswith("-DFREERTOS_PATH=")
                             for arg in args))

    def test_standalone_kernel_uses_prepared_worktree(self):
        os.makedirs(os.path.join(self.ws.ws_dl_dir, "FreeRTOS-Kernel"))
        prepared = os.path.join(self.temp.name, "prepared-kernel")
        with mock.patch("ove.build._prepare_patched_git_tree",
                        return_value=prepared):
            args = self._configure_args()

        self.assertIn("-DFREERTOS_PATH=" + prepared, args)
        self.assertNotIn("-UFREERTOS_PATH", args)


if __name__ == "__main__":
    unittest.main()
