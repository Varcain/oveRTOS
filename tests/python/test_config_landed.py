# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Requested RTOS config lines are checked against the resolved .config."""

import os
import tempfile
import textwrap
import unittest

from ove.utils import config_assignments, unlanded_assignments


class ConfigLandedTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.temp.cleanup()

    def _file(self, name, text):
        path = os.path.join(self.temp.name, name)
        with open(path, "w") as f:
            f.write(textwrap.dedent(text).lstrip())
        return path

    def test_later_layer_wins(self):
        a = self._file("a", "CONFIG_FOO=1\n# CONFIG_BAR is not set\n")
        b = self._file("b", "CONFIG_FOO=2\n")
        self.assertEqual(config_assignments(a, b), {"CONFIG_FOO": "2", "CONFIG_BAR": "n"})

    def test_optional_lines_are_not_required(self):
        conf = self._file("conf", """
            # ove:optional
            CONFIG_LOG_MODE_MINIMAL=y
            CONFIG_FOO=1
            # ove:optional
            # CONFIG_BAR is not set
            """)
        self.assertEqual(config_assignments(conf), {"CONFIG_FOO": "1"})

    def test_everything_landed(self):
        final = self._file(".config", """
            CONFIG_FOO=2
            CONFIG_NAME="ove"
            # CONFIG_BAR is not set
            """)
        requested = {"CONFIG_FOO": "2", "CONFIG_NAME": '"ove"', "CONFIG_BAR": "n",
                     "CONFIG_GONE": "n"}
        self.assertEqual(unlanded_assignments(requested, final), [])

    def test_dropped_and_changed_lines_are_reported(self):
        final = self._file(".config", "CONFIG_FOO=3\nCONFIG_BAR=y\n")
        requested = {"CONFIG_FOO": "2", "CONFIG_BAR": "n", "CONFIG_RENAMED": "8"}
        self.assertEqual(
            unlanded_assignments(requested, final),
            [("CONFIG_FOO", "2", "3"), ("CONFIG_BAR", "n", "y"), ("CONFIG_RENAMED", "8", None)],
        )


if __name__ == "__main__":
    unittest.main()
