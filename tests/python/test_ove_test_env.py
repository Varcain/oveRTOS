# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""`ove test` runs every target in one process: a target must not leave
environment behind for the next one."""

import os
import unittest
from unittest import mock

from ove import test as ove_test

try:
    import kconfiglib  # noqa: F401

    HAVE_KCONFIGLIB = True
except ImportError:
    HAVE_KCONFIGLIB = False


class KconfigSrctreeTest(unittest.TestCase):
    def test_srctree_is_scoped_to_the_block(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("srctree", None)
            with ove_test._kconfig_srctree("/ove"):
                self.assertEqual(os.environ["srctree"], "/ove")
            self.assertNotIn("srctree", os.environ)

    def test_an_outer_srctree_survives(self):
        with mock.patch.dict(os.environ, {"srctree": "/outer"}):
            with ove_test._kconfig_srctree("/ove"):
                self.assertEqual(os.environ["srctree"], "/ove")
            self.assertEqual(os.environ["srctree"], "/outer")

    def test_restored_when_the_block_raises(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("srctree", None)
            with self.assertRaises(RuntimeError), ove_test._kconfig_srctree("/ove"):
                raise RuntimeError("kconfig failed")
            self.assertNotIn("srctree", os.environ)

    @unittest.skipUnless(HAVE_KCONFIGLIB, "kconfiglib is not installed")
    def test_layout_config_target_leaves_no_srctree(self):
        # The target that used to leak srctree into the nuttx-sim build.
        ove_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("srctree", None)
            result = ove_test.test_lxp_memory_layout_config(ove_dir, None)
            self.assertNotIn("srctree", os.environ)
        self.assertEqual(result.failed, 0)


class TraceTargetTest(unittest.TestCase):
    """`ove test qemu-<rtos>-trace` must configure before it builds and
    must not hand kconfiglib's $srctree to the build (NuttX olddefconfig
    then looks for <ove_dir>/Kconfig)."""

    def run_target(self, env=None):
        from ove import build, configure, kconfig

        calls = []

        def fragments(args):
            # What the real cmd_defconfig_fragments does (kconfig.py).
            os.environ["srctree"] = "/ove"
            calls.append(("fragments", args.spec))

        def configure_(args):
            calls.append(("configure", os.environ.get("srctree")))

        def build_(args):
            calls.append(("build", os.environ.get("srctree")))
            calls.append(("build-ws", os.environ.get("OVE_WORKSPACE_DIR")))

        with mock.patch.dict(os.environ, env or {}, clear=False), \
                mock.patch.object(kconfig, "cmd_defconfig_fragments", fragments), \
                mock.patch.object(configure, "cmd_configure", configure_), \
                mock.patch.object(build, "cmd_build", build_), \
                mock.patch.object(ove_test, "_ensure_rtos_sources", lambda *a: None), \
                mock.patch.object(ove_test, "_sim_invariant_elf", lambda *a: None):
            os.environ.pop("srctree", None)
            result = ove_test.test_qemu_nuttx_trace("/ove", "/ove/output")
            leaked = os.environ.get("srctree")
            self.ws_after = os.environ.get("OVE_WORKSPACE_DIR")
        return calls, result, leaked

    def test_configures_before_building(self):
        calls, _result, _ = self.run_target()
        self.assertEqual([c[0] for c in calls],
                         ["fragments", "configure", "build", "build-ws"])

    def test_build_does_not_inherit_srctree(self):
        calls, _result, leaked = self.run_target()
        self.assertEqual([c for c in calls if c[0] == "build"], [("build", None)])
        self.assertIsNone(leaked)

    def test_builds_the_trace_workspace_not_ove_workspace_dir(self):
        # The ELF is read from the workspace the fragments activate, so an
        # $OVE_WORKSPACE_DIR set by the user must not redirect the build.
        calls, _result, _ = self.run_target({"OVE_WORKSPACE_DIR": "/elsewhere"})
        self.assertIn(("build-ws", None), calls)
        self.assertEqual(self.ws_after, "/elsewhere")

    def test_missing_elf_is_a_failure(self):
        _calls, result, _ = self.run_target()
        self.assertEqual(result.failed, 1)


class ActiveWorkspaceTest(unittest.TestCase):
    """`ove test` puts the user's active workspace back, including the
    CMakeUserPresets.json the targets' `ove configure` rewrites."""

    def test_presets_are_restored(self):
        import tempfile

        with tempfile.TemporaryDirectory() as root:
            presets = os.path.join(root, "CMakeUserPresets.json")
            with open(presets, "w") as f:
                f.write("user")
            saved = ove_test._save_active_workspace(root)
            with open(presets, "w") as f:
                f.write("trace target")
            ove_test._restore_active_workspace(saved)
            with open(presets) as f:
                self.assertEqual(f.read(), "user")

    def test_presets_written_by_a_target_are_removed(self):
        import tempfile

        with tempfile.TemporaryDirectory() as root:
            presets = os.path.join(root, "CMakeUserPresets.json")
            saved = ove_test._save_active_workspace(root)
            with open(presets, "w") as f:
                f.write("trace target")
            ove_test._restore_active_workspace(saved)
            self.assertFalse(os.path.exists(presets))


if __name__ == "__main__":
    unittest.main()
