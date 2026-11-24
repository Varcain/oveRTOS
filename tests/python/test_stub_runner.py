# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import os
import re
import unittest
from pathlib import Path
from shlex import quote
from tempfile import TemporaryDirectory
from unittest import mock

from ove import test as ove_test
from ove.test import NUTTX_SANITIZER_REPORT


_PASSING_OUTPUT = "[  PASSED  ] 3 test(s).\n=== Summary: 0 test group(s) had failures ===\n"


def _run_fake_binary(output, returncode, **kwargs):
    with TemporaryDirectory() as directory:
        binary = Path(directory) / "test-suite"
        binary.write_text(
            f"#!/bin/sh\nprintf '%s' {quote(output)}\nexit {returncode}\n",
            encoding="utf-8",
        )
        binary.chmod(0o700)
        return ove_test._run_test_binary([binary], "stub", **kwargs)


class StubRunnerTest(unittest.TestCase):
    def test_missing_clang_skips_msan_locally(self):
        with (
            TemporaryDirectory() as directory,
            mock.patch.dict(os.environ, clear=True),
            mock.patch.object(ove_test.shutil, "which", return_value=None),
            mock.patch.object(ove_test, "_cmake_build") as build,
        ):
            results = ove_test.test_stub_msan(directory, Path(directory) / "output")

        self.assertEqual(results, ove_test.TestResults(suite="stub-msan", skipped=1))
        build.assert_not_called()

    def test_missing_clang_fails_msan_in_ci(self):
        with (
            TemporaryDirectory() as directory,
            mock.patch.dict(os.environ, {"CI": "true"}),
            mock.patch.object(ove_test.shutil, "which", return_value=None),
            mock.patch.object(ove_test, "_cmake_build") as build,
        ):
            results = ove_test.test_stub_msan(directory, Path(directory) / "output")

        self.assertEqual(results, ove_test.TestResults(
            suite="stub-msan", failed=1, failed_names=["<clang not installed>"],
        ))
        build.assert_not_called()

    def test_missing_extra_binary_is_a_failure(self):
        results = ove_test.TestResults(suite="stub")
        with TemporaryDirectory() as build:
            ove_test._run_extra_binaries(build, ("ove_test_nvs_full",), "stub", results)

        self.assertEqual(results.failed, 1)
        self.assertEqual(results.failed_names, ["<missing ove_test_nvs_full>"])

    def test_silent_binary_with_required_summary_fails(self):
        results = _run_fake_binary("", 0, summary=True)
        self.assertEqual(results.passed, 0)
        self.assertEqual(results.failed, 1)
        self.assertIn("<no summary line>", results.failed_names)

    def test_partial_cmocka_output_with_required_summary_fails(self):
        results = _run_fake_binary("[  PASSED  ] 3 test(s).\n", 0, summary=True)
        self.assertEqual(results.passed, 3)
        self.assertEqual(results.failed, 1)

    def test_c_summary_completes_run(self):
        results = _run_fake_binary(
            "[  PASSED  ] 3 test(s).\n\n=== Summary: 0 test group(s) had failures ===\n",
            0, summary=True,
        )
        self.assertEqual(results.failed, 0)

    def test_rust_zig_summary_completes_run(self):
        results = _run_fake_binary("=== Summary: 5 passed, 0 failed ===\n", 0, summary=True)
        self.assertEqual(results.failed, 0)

    def test_kasan_report_fails_despite_successful_summary(self):
        results = _run_fake_binary(
            "kasan_report: kasan detected a read access error\n" + _PASSING_OUTPUT,
            0, summary=True, fail_on=NUTTX_SANITIZER_REPORT,
        )
        self.assertEqual(results.failed, 1)
        self.assertIn("<sanitizer report>", results.failed_names)

    def test_ubsan_report_fails_despite_successful_summary(self):
        results = _run_fake_binary(
            "UBSAN: add-overflow in foo\n" + _PASSING_OUTPUT,
            0, summary=True, fail_on=re.compile(NUTTX_SANITIZER_REPORT),
        )
        self.assertEqual(results.failed, 1)
        self.assertIn("<sanitizer report>", results.failed_names)

    def test_clean_output_passes_with_sanitizer_check(self):
        results = _run_fake_binary(
            _PASSING_OUTPUT, 0, summary=True, fail_on=NUTTX_SANITIZER_REPORT,
        )
        self.assertEqual(results.failed, 0)

    def test_silent_binary_without_required_summary_passes(self):
        results = _run_fake_binary("", 0)
        self.assertEqual(results.passed, 1)
        self.assertEqual(results.failed, 0)
