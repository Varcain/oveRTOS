# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import json
import os
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

from ove import lint


class ClangTidyTest(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "checkout"
        self.bin = Path(tmp.name) / "bin"
        self.bin.mkdir()
        (self.root / "src").mkdir(parents=True)
        (self.root / "src/foo.c").touch()
        self._tool("clang-tidy", "printf '%s\\n' \"$@\" > tidy-args\nexit 0")

    def _tool(self, name, script):
        tool = self.bin / name
        tool.write_text(f"#!/bin/sh\n{script}\n", encoding="utf-8")
        tool.chmod(0o755)

    def _db(self, directory="output/tests/stub"):
        path = self.root / directory / "compile_commands.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        return path

    def _write_db(self, source="src/foo.c", directory="output/tests/stub"):
        entry = {"directory": str(self.root), "file": str(self.root / source),
                 "command": f"cc -c {source}"}
        self._db(directory).write_text(json.dumps([entry]), encoding="utf-8")

    def _lint(self, **env):
        with patch.dict(os.environ, {"PATH": str(self.bin), **env}, clear=True):
            return lint._clang_tidy(str(self.root), False)

    def test_configure_failure_includes_diagnostic(self):
        self._tool("cmake", "printf '%250s' output\necho 'configure error' >&2\nexit 1")
        step, status, detail = self._lint()
        self.assertEqual((step, status), ("clang-tidy", "FAIL"))
        self.assertIn("configure error", detail)

    def test_stray_database_cannot_replace_stub_database(self):
        self._write_db(directory="output")
        self._tool("cmake", "exit 0")
        self.assertEqual(self._lint()[1], "FAIL")
        self.assertFalse((self.root / "tidy-args").exists())

    def test_configure_succeeds_without_database(self):
        self._tool("cmake", "exit 0")
        self.assertEqual(self._lint()[1], "FAIL")

    def test_invalid_database(self):
        for content in (b"{", b"\xff", b"null", b"[null]"):
            with self.subTest(content=content):
                self._db().write_bytes(content)
                self.assertEqual(self._lint()[1], "FAIL")

    def test_database_without_in_scope_sources(self):
        source = "src/build/generated.c"
        (self.root / source).parent.mkdir()
        (self.root / source).touch()
        self._write_db(source)
        self.assertEqual(self._lint()[1], "FAIL")

    def test_checkout_under_build_directory(self):
        build = self.root.parent / "build"
        build.mkdir()
        self.root = self.root.rename(build / self.root.name)
        self._write_db()
        self.assertEqual(self._lint(), ("clang-tidy", "OK", "1 files"))
        args = (self.root / "tidy-args").read_text(encoding="utf-8").splitlines()
        self.assertEqual(args[:3], ["--quiet", "-p", str(self.root / "output/tests/stub")])
        self.assertEqual(args[-1], str(self.root / "src/foo.c"))

    def test_valid_stub_database(self):
        self._write_db()
        self.assertEqual(self._lint(), ("clang-tidy", "OK", "1 files"))

    def test_missing_clang_tidy_locally(self):
        (self.bin / "clang-tidy").unlink()
        self.assertEqual(self._lint()[1], "SKIP")

    def test_missing_clang_tidy_in_ci(self):
        (self.bin / "clang-tidy").unlink()
        self.assertEqual(self._lint(CI="true")[1], "FAIL")

    def test_missing_cmake_when_stub_database_is_absent(self):
        self.assertEqual(self._lint()[1], "SKIP")
        self.assertEqual(self._lint(CI="true")[1], "FAIL")
