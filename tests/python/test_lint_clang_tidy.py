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

    def _write_db(self, source="src/foo.c", directory="output/tests/stub", compiler="cc"):
        entry = {"directory": str(self.root), "file": str(self.root / source),
                 "command": f"{compiler} -c {source}"}
        self._db(directory).write_text(json.dumps([entry]), encoding="utf-8")

    def _write_backend_db(self, rtos):
        source = f"backends/{rtos}/foo.c"
        path = self.root / source
        path.parent.mkdir(parents=True)
        path.touch()
        self._write_db(source, f"output/tests/qemu-{rtos}/build", "arm-none-eabi-gcc")
        return str(path)

    def _lint(self, **env):
        with patch.dict(os.environ, {"PATH": str(self.bin), **env}, clear=True):
            return lint._clang_tidy(str(self.root), False)

    def _lint_backends(self):
        with patch.dict(os.environ, {"PATH": str(self.bin)}, clear=True):
            with patch.object(lint, "_run", return_value=(0, "")) as run:
                return lint._clang_tidy_backends(str(self.root), False), run

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
        self.assertIn("--extra-arg=-Werror=thread-safety", args)
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

    def test_empty_backend_database(self):
        self._db("output/tests/qemu-freertos/build").write_text("[]", encoding="utf-8")
        result, run = self._lint_backends()
        run.assert_not_called()
        self.assertEqual(result, ("clang-tidy-backends", "FAIL",
                                  "freertos: no backend files in compile database"))

    def test_backend_database_emptied_by_filtering_beside_valid_database(self):
        self._write_db(directory="output/tests/qemu-freertos/build")
        source = self._write_backend_db("nuttx")
        result, run = self._lint_backends()
        run.assert_called_once()
        self.assertEqual(run.call_args.args[0][-1], source)
        self.assertEqual(result, ("clang-tidy-backends", "FAIL",
                                  "freertos: no backend files in compile database"))

    def test_valid_backend_database(self):
        source = self._write_backend_db("nuttx")
        result, run = self._lint_backends()
        run.assert_called_once()
        self.assertEqual(run.call_args.args[0][-1], source)
        self.assertEqual(result, ("clang-tidy-backends", "OK", "1 backend(s), 1 files"))

    def test_missing_backend_database(self):
        result, run = self._lint_backends()
        run.assert_not_called()
        self.assertEqual(result[:2], ("clang-tidy-backends", "SKIP"))
        self.assertIn("no firmware compile_commands.json", result[2])

    def test_stub_database_in_checkout_under_zephyr_directory(self):
        zephyr = self.root.parent / "zephyr"
        zephyr.mkdir()
        self.root = self.root.rename(zephyr / self.root.name)
        self._write_db()
        result, run = self._lint_backends()
        run.assert_not_called()
        self.assertEqual(result, ("clang-tidy-backends", "SKIP",
                                  "no RTOS-tagged compile_commands.json found"))
