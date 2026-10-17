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

    def test_layout_config_target_leaves_no_srctree(self):
        # The target that used to leak srctree into the nuttx-sim build.
        ove_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("srctree", None)
            result = ove_test.test_lxp_memory_layout_config(ove_dir, None)
            self.assertNotIn("srctree", os.environ)
        self.assertEqual(result.failed, 0)


if __name__ == "__main__":
    unittest.main()
