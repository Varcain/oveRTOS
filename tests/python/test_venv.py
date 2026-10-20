# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import sys
import tempfile
import unittest
from pathlib import Path

from ove import venv


class VenvVersionTest(unittest.TestCase):
    def test_version_of_running_python(self):
        self.assertEqual(venv.python_version(sys.executable), tuple(sys.version_info[:3]))

    def test_version_missing_interpreter(self):
        self.assertIsNone(venv.python_version("/nonexistent/python"))
        self.assertIsNone(venv.python_version(None))


class ZephyrPythonTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def _fake_python(self, version):
        exe = Path(venv.python_path(self.tmp.name))
        exe.parent.mkdir(parents=True)
        exe.write_text(f"#!/bin/sh\necho '{version}'\n")
        exe.chmod(0o755)

    def test_missing_venv(self):
        self.assertIn("make setup", venv.zephyr_error(self.tmp.name))

    def test_old_python(self):
        self._fake_python("3 11 2")
        error = venv.zephyr_error(self.tmp.name)
        self.assertIsNotNone(error)
        self.assertIn("3.11.2", error)
        self.assertIn("3.12", error)
        self.assertIn(venv.path(self.tmp.name), error)
        self.assertIn("make setup", error)

    def test_supported_python(self):
        self._fake_python("3 12 15")
        self.assertIsNone(venv.zephyr_error(self.tmp.name))
