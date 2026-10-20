# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for venv path discovery and the Zephyr Python requirement."""

import os
import re
import stat
import sys
import tempfile
import unittest
from pathlib import Path

from ove import venv

MAKEFILE = Path(__file__).resolve().parents[2] / "Makefile"


class VenvPathTest(unittest.TestCase):
    def test_layout(self):
        self.assertEqual(venv.path("/repo"), "/repo/.venv")
        self.assertEqual(venv.bin_path("/repo"), "/repo/.venv/bin")
        self.assertEqual(venv.bin_path("/repo", "west"),
                         "/repo/.venv/bin/west")
        self.assertEqual(venv.python_path("/repo"), "/repo/.venv/bin/python")

    def test_version_of_running_python(self):
        self.assertEqual(venv.python_version(sys.executable),
                         tuple(sys.version_info[:3]))

    def test_version_missing_interpreter(self):
        self.assertIsNone(venv.python_version("/nonexistent/python"))
        self.assertIsNone(venv.python_version(None))


class ZephyrPythonTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def _fake_python(self, version):
        exe = venv.python_path(self.tmp.name)
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        with open(exe, "w") as f:
            f.write("#!/bin/sh\necho '" + version + "'\n")
        os.chmod(exe, os.stat(exe).st_mode | stat.S_IXUSR)
        return exe

    def test_missing_venv(self):
        self.assertIn("make setup", venv.zephyr_error(self.tmp.name))

    def test_old_python(self):
        self._fake_python("3 11 2")
        error = venv.zephyr_error(self.tmp.name)
        self.assertIn("3.11.2", error)
        self.assertIn("3.12", error)
        self.assertIn(venv.path(self.tmp.name), error)
        self.assertIn("make setup", error)

    def test_supported_python(self):
        self._fake_python("3 12 15")
        self.assertIsNone(venv.zephyr_error(self.tmp.name))

    def test_makefile_uses_the_same_minimum(self):
        """The Makefile's auto-detection and version gate must require
        the version the CLI checks for."""
        text = MAKEFILE.read_text()
        found = re.findall(r"sys\.version_info\s*[<>]=?\s*\((\d+),\s*(\d+)\)",
                           text)
        self.assertGreaterEqual(len(found), 2, "no version check found")
        for major, minor in found:
            self.assertEqual((int(major), int(minor)), venv.ZEPHYR_MIN_PYTHON)


if __name__ == "__main__":
    unittest.main()
