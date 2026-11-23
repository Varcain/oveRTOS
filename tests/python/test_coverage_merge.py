# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAKE = shutil.which("make") or shutil.which("gmake")


@unittest.skipUnless(
    MAKE and shutil.which("lcov") and shutil.which("genhtml"),
    "make, lcov and genhtml are required",
)
class CoverageMergeTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.output = root / "out"
        self.output.mkdir()
        self.make_args = [f"COVERAGE_OUTPUT_DIR={self.output}"]
        self.tracefiles = {}
        for backend in ("STUB", "CPP", "RUST", "QEMU_FREERTOS"):
            directory = root / backend.lower()
            self.make_args.append(f"COVERAGE_{backend}_DIR={directory}")
            if backend in ("STUB", "CPP"):
                directory /= "coverage"
            directory.mkdir(parents=True)
            self.tracefiles[backend] = directory / "coverage.filtered.info"
        source = root / "source.c"
        source.write_text("int covered;\n")
        self.trace = f"SF:{source}\nDA:1,1\nend_of_record\n"
        self.tracefiles["STUB"].write_text(self.trace)

    def _make(self, *args):
        env = os.environ.copy()
        for name in ("MAKEFLAGS", "MFLAGS", "COVERAGE_REQUIRE_ALL"):
            env.pop(name, None)
        return subprocess.run(
            [MAKE, "-s", "-C", str(ROOT), "coverage-merge", *self.make_args, *args],
            env=env, capture_output=True, text=True, timeout=60,
        )

    def _write_all_tracefiles(self):
        for path in self.tracefiles.values():
            path.write_text(self.trace)

    def test_partial_merge(self):
        result = self._make()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.output / "html/index.html").is_file())

    def test_strict_merge_rejects_missing_tracefiles(self):
        result = self._make("COVERAGE_REQUIRE_ALL=1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.output / "html/index.html").is_file())
        for backend in ("CPP", "RUST", "QEMU_FREERTOS"):
            self.assertIn(f"missing or empty coverage tracefile: {self.tracefiles[backend]}", result.stderr)

    def test_strict_merge_rejects_empty_tracefile(self):
        self._write_all_tracefiles()
        self.tracefiles["CPP"].write_text("")
        result = self._make("COVERAGE_REQUIRE_ALL=1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"missing or empty coverage tracefile: {self.tracefiles['CPP']}", result.stderr)

    def test_strict_merge_with_all_tracefiles(self):
        self._write_all_tracefiles()
        result = self._make("COVERAGE_REQUIRE_ALL=1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
