# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for the dashboard build manifest's workspace detection.

The bridge points --build-dir at the CMake build tree. Some RTOS trees
keep their own .config there (NuttX does), which must not shadow the
oveRTOS workspace .config that describes the enabled features."""

import importlib.util
import os
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MANIFEST = os.path.join(ROOT, "config", "scripts", "ove_manifest.py")

WORKSPACE_CONFIG = """\
CONFIG_OVE_APP_NAME="example_c"
CONFIG_OVE_BOARD_NAME="qemu-mps2-an500"
CONFIG_OVE_RTOS_NUTTX=y
CONFIG_OVE_TRACE_STREAM=y
CONFIG_OVE_PROFILER=y
CONFIG_OVE_PROFILER_HZ=250
"""

NUTTX_CONFIG = """\
CONFIG_SCHED_INSTRUMENTATION=y
CONFIG_SCHED_INSTRUMENTATION_SWITCH=y
CONFIG_DRIVERS_NOTE=y
"""


def load_manifest():
    spec = importlib.util.spec_from_file_location("ove_manifest", MANIFEST)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write(path, contents):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(contents)


class WorkspaceSearchTest(unittest.TestCase):
    def setUp(self):
        self.manifest = load_manifest()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.ws = self.tmp.name
        write(os.path.join(self.ws, ".config"), WORKSPACE_CONFIG)

    def test_freertos_and_zephyr_build_dir(self):
        build = os.path.join(self.ws, "build", "firmware")
        os.makedirs(build)
        self.assertEqual(self.manifest.find_workspace(build), self.ws)

    def test_nuttx_vendored_config_does_not_shadow_workspace(self):
        build = os.path.join(self.ws, "build", "nuttx-cmake")
        write(os.path.join(build, ".config"), NUTTX_CONFIG)
        self.assertEqual(self.manifest.find_workspace(build), self.ws)

    def test_no_workspace_config(self):
        with tempfile.TemporaryDirectory() as other:
            build = os.path.join(other, "build", "nuttx-cmake")
            write(os.path.join(build, ".config"), NUTTX_CONFIG)
            self.assertIsNone(self.manifest.find_workspace(build))

    def test_manifest_reads_workspace_features(self):
        ws = self.manifest.find_workspace(
            os.path.join(self.ws, "build", "nuttx-cmake"))
        m = self.manifest.build(ws, ROOT, debugger=True, console=False)
        self.assertEqual(m["app"], "example_c")
        self.assertEqual(m["rtos"], "nuttx")
        self.assertTrue(m["features"]["trace"])
        self.assertTrue(m["features"]["profiler"])
        self.assertEqual(m["profiler_hz"], 250)
        self.assertEqual(m["board"]["name"], "qemu-mps2-an500")
        self.assertTrue(m["features"]["debugger"])
        self.assertFalse(m["features"]["console"])


if __name__ == "__main__":
    unittest.main()
