# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAKE = shutil.which("make") or shutil.which("gmake")


@unittest.skipUnless(MAKE, "make is not available")
class MakefilePythonResolutionTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.venv = self.root / "venv"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for tool in ("touch", "true"):
            (self.bin / tool).symlink_to(shutil.which(tool))

    def _python(self, name, minor, *, in_venv=False):
        exe = self.bin / name
        exe.parent.mkdir(parents=True, exist_ok=True)
        state = shlex.quote(
            f"import sys, venv; sys.version_info = (3, {minor}, 8); "
            f"sys.prefix = {'/venv' if in_venv else '/base'!r}; sys.base_prefix = '/base'; "
        )
        exe.write_text(
            '#!/bin/sh\ncase "$1" in\n'
            f'  -c) exec {shlex.quote(sys.executable)} -c {state}"$2";;\n'
            f'  --version) echo "Python 3.{minor}.8";;\n'
            '  -m) /bin/mkdir -p "${4:-$3}" && printf "%s %s\\n" "$0" "$*";;\n'
            'esac\n'
        )
        exe.chmod(0o755)
        return exe

    def _make(self, *args):
        env = dict(os.environ, PATH=f"{self.root}:{self.bin}")
        for name in ("OVE_PYTHON", "MAKEFLAGS", "MFLAGS"):
            env.pop(name, None)
        return subprocess.run(
            [MAKE, "--no-print-directory", f"VENV_DIR={self.venv}", "VENV_PIP=true",
             str(self.venv / ".stamp"), *args],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=15,
        )

    def _assert_refuses(self, result, exe):
        output = result.stdout + result.stderr
        self.assertIn("Python >= 3.12 is required", output)
        self.assertIn(f"{exe} is Python 3.11.8", output)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.venv / ".stamp").exists())

    def _assert_creates(self, result, exe, *flags):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        command = " ".join((str(exe), "-m", "venv", *flags, str(self.venv)))
        self.assertIn(command, result.stdout.splitlines())

    def test_explicit_old_python_is_rejected(self):
        old = self._python("python3", 11)
        self._python("python3.14", 14)
        self._assert_refuses(self._make(f"OVE_PYTHON={old}"), old)
        self.assertFalse(self.venv.exists())

    def test_auto_detect_chooses_first_supported_python(self):
        self._python("python3.14", 11)
        supported = self._python("python3.13", 13)
        self._python("python3", 14)
        self._assert_creates(self._make(), supported, "--clear")

    def test_auto_detect_reports_old_python3(self):
        old = self._python("python3", 11)
        self._python(self.venv / "bin" / "python3", 11, in_venv=True)
        self._assert_refuses(self._make(), old)

    def test_auto_detect_skips_venv_python(self):
        self._python(self.root / "python3.14", 14, in_venv=True)
        supported = self._python("python3.14", 14)
        self._python("python3", 11)
        self._assert_creates(self._make(), supported, "--clear")

    def test_auto_detect_clears_old_venv(self):
        self._python(self.venv / "bin" / "python", 11, in_venv=True)
        supported = self._python("python3.12", 12)
        self._assert_creates(self._make(), supported, "--clear")

    def test_auto_detect_reuses_supported_venv(self):
        existing = self._python(self.venv / "bin" / "python", 12, in_venv=True)
        existing.with_name("python3").symlink_to(existing.name)
        self._python("python3", 11)
        self._assert_creates(self._make(), existing)
