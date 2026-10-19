# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""A NuttX build starts from a clean CMake tree when its defconfig changes.

NuttX's CMake regenerates include/nuttx/config.h only when .config differs
from .config.prev, a copy of the first .config that it never refreshes, so
going back to the first configuration would keep the previous config.h.
"""

import os
import tempfile
import unittest
from unittest import mock

from ove.build import build_nuttx


class _Workspace:
    def __init__(self, root):
        self.ove_dir = os.path.join(root, "ove")
        self.app_dir = os.path.join(root, "app")
        self.gen_dir = os.path.join(root, "generated")
        self.ws_dl_dir = os.path.join(root, "dl")
        self.board_dir = os.path.join(root, "board")
        self.build_dir = os.path.join(root, "build")
        self.images_dir = os.path.join(root, "images")
        self.build_log = os.path.join(root, "build.log")
        self.rtos_config_path = os.path.join(root, "rtos.config")
        self.board_name = "qemu-mps2-an500"
        for path in (self.ove_dir, self.app_dir, self.gen_dir,
                     self.ws_dl_dir, self.board_dir, self.build_dir):
            os.makedirs(path)

    @staticmethod
    def toolchain_env():
        return {}


class NuttXBuildTreeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.ws = _Workspace(self.temp.name)
        self.defconfig = os.path.join(self.ws.build_dir, "nuttx-defconfig")
        self.cmake_build = os.path.join(self.ws.build_dir, "nuttx-cmake")
        self.stale = os.path.join(self.cmake_build, "include", "nuttx",
                                  "config.h")
        self.content = "CONFIG_SCHED_BACKTRACE=y\n"
        # Whether the stale header existed when the build first ran a
        # command (the CMake configure): the clean must come before it.
        self.stale_at_configure = []

        def write_defconfig(*_args, **_kwargs):
            with open(self.defconfig, "w") as f:
                f.write(self.content)
            return self.defconfig

        patches = [
            mock.patch("ove.build._find_cmake", return_value="cmake"),
            mock.patch("ove.build._setup_nuttx_build_tree",
                       return_value=("nuttx", "apps")),
            mock.patch("ove.build.nuttx_board_config",
                       return_value="mps2-an500:nsh"),
            mock.patch("ove.build._write_nuttx_defconfig",
                       side_effect=write_defconfig),
            mock.patch("ove.build.run", side_effect=self._record_run),
            mock.patch("ove.build._check_config_landed"),
            mock.patch("ove.build._check_source_trees_unchanged"),
            mock.patch("ove.build._create_run_or_flash_script"),
        ]
        for patcher in patches:
            patcher.start()
            self.addCleanup(patcher.stop)

    def _record_run(self, *_args, **_kwargs):
        self.stale_at_configure.append(os.path.exists(self.stale))

    def _build(self):
        self.stale_at_configure.clear()
        build_nuttx(self.ws)
        return self.stale_at_configure[0]

    def tearDown(self):
        self.temp.cleanup()

    def _build_with_stale_header(self):
        build_nuttx(self.ws)
        os.makedirs(os.path.dirname(self.stale), exist_ok=True)
        with open(self.stale, "w") as f:
            f.write("#define CONFIG_SCHED_BACKTRACE 1\n")

    def test_changed_defconfig_cleans_the_tree(self):
        self._build_with_stale_header()
        self.content = "# CONFIG_SCHED_BACKTRACE is not set\n"
        self.assertFalse(self._build(), "configure ran on the stale tree")
        self.assertFalse(os.path.exists(self.stale))

    def test_unchanged_defconfig_keeps_the_tree(self):
        self._build_with_stale_header()
        self.assertTrue(self._build())
        self.assertTrue(os.path.exists(self.stale))

    def test_tree_from_an_older_ove_is_cleaned_once(self):
        """A tree recorded under the old name may hold a stale config.h."""
        self._build_with_stale_header()
        os.rename(os.path.join(self.cmake_build, ".ove_defconfig_sha256"),
                  os.path.join(self.cmake_build, ".ove_defconfig_hash"))
        self.assertFalse(self._build())
        self._build_with_stale_header()
        self.assertTrue(self._build())


if __name__ == "__main__":
    unittest.main()
