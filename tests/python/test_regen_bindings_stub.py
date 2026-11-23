# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import os
import shutil
import subprocess
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/regen-bindings-stub.sh"


class RegenBindingsStubTest(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.script = self.root / "scripts" / SCRIPT.name
        self.script.parent.mkdir()
        shutil.copyfile(SCRIPT, self.script)
        sizes = self.root / "output/tests/rust_stub/ove_storage_sizes.env"
        self.missing_sizes = f"rust_stub build not present at {sizes}"

    def _add_lvgl_header(self):
        header = self.root / "dl/lvgl/lvgl.h"
        header.parent.mkdir(parents=True)
        header.touch()

    def _assert_check(self, status, diagnostic, *, ci=False):
        env = {"PATH": os.environ["PATH"]}
        if "TMPDIR" in os.environ:
            env["TMPDIR"] = os.environ["TMPDIR"]
        if ci:
            env["CI"] = "true"
        result = subprocess.run(
            ["bash", str(self.script), "--check"],
            env=env,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, status, result.stderr)
        action = "cannot run the" if ci else "skipping"
        self.assertEqual(
            result.stderr, f"{diagnostic} — {action} bindings_stub.rs drift check\n"
        )

    def test_missing_lvgl_warns_locally(self):
        self._assert_check(0, "warning: dl/lvgl not present")

    def test_missing_lvgl_fails_in_ci(self):
        self._assert_check(1, "error: dl/lvgl not present", ci=True)

    def test_missing_storage_sizes_warns_locally(self):
        self._add_lvgl_header()
        self._assert_check(0, f"warning: {self.missing_sizes}")

    def test_missing_storage_sizes_fails_in_ci(self):
        self._add_lvgl_header()
        self._assert_check(1, f"error: {self.missing_sizes}", ci=True)
